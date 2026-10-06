/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s).
 *
 * The stored shape of an assistant turn's blocks: the text written with its
 * message row, and the validating read that rebuilds the blocks on reload.
 */

#include <json-c/json.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm_turn_blocks.h"
#include "logging.h"

/* Envelope, block list, block, native object and what a vendor nests inside
 * it (Anthropic server-tool results nest a few levels). */
#define STORED_JSON_DEPTH 64

/* Serialized as received: no "\/" escapes, and no reformatting. */
#define STORED_JSON_FLAGS (JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE)

char *llm_turn_blocks_to_stored(struct json_object *blocks) {
   if (!blocks || !json_object_is_type(blocks, json_type_array) ||
       json_object_array_length(blocks) == 0) {
      return NULL;
   }
   struct json_object *envelope = json_object_new_object();
   if (!envelope) {
      return NULL;
   }
   json_object_object_add(envelope, "v", json_object_new_int(LLM_TURN_BLOCKS_STORED_VERSION));
   json_object_object_add(envelope, "blocks", json_object_get(blocks));

   size_t len = 0;
   const char *text = json_object_to_json_string_length(envelope, STORED_JSON_FLAGS, &len);
   char *out = NULL;
   if (text && len <= LLM_TURN_BLOCKS_STORED_MAX) {
      out = strdup(text);
   } else if (text) {
      OLOG_WARNING("turn blocks: %zu bytes, over the %d-byte stored cap; not stored", len,
                   LLM_TURN_BLOCKS_STORED_MAX);
   }
   json_object_put(envelope);
   return out;
}

static bool is_string(struct json_object *obj, const char *key) {
   struct json_object *v = NULL;
   return json_object_object_get_ex(obj, key, &v) && json_object_is_type(v, json_type_string);
}

/* An optional string field: absent, null or a string. */
static bool optional_string(struct json_object *obj, const char *key) {
   struct json_object *v = NULL;
   return !json_object_object_get_ex(obj, key, &v) || v == NULL ||
          json_object_is_type(v, json_type_string);
}

static const char *type_of(struct json_object *block) {
   struct json_object *t = NULL;
   if (!json_object_is_type(block, json_type_object) ||
       !json_object_object_get_ex(block, "type", &t) || !json_object_is_type(t, json_type_string)) {
      return NULL;
   }
   return json_object_get_string(t);
}

/* Vendor data names who issued it: a carrier and format, and maybe a model. */
static bool valid_origin(struct json_object *obj) {
   return is_string(obj, "carrier") && is_string(obj, "format") && optional_string(obj, "model");
}

/* Why @p block isn't a valid stored block, or NULL when it is. */
static const char *block_problem(struct json_object *block) {
   const char *type = type_of(block);
   if (!type) {
      return "block without a type";
   }
   if (strcmp(type, "text") == 0) {
      return is_string(block, "text") ? NULL : "text block without text";
   }
   if (strcmp(type, "tool_call") == 0) {
      if (!is_string(block, "id") || !is_string(block, "name") || !is_string(block, "arguments")) {
         return "tool call without id, name or arguments";
      }
      struct json_object *sig = NULL;
      if (json_object_object_get_ex(block, "sig", &sig) &&
          (!json_object_is_type(sig, json_type_object) || !valid_origin(sig) ||
           !is_string(sig, "value"))) {
         return "tool call with an invalid signature";
      }
      return NULL;
   }
   if (strcmp(type, "reasoning") == 0 || strcmp(type, "opaque") == 0) {
      struct json_object *native = NULL;
      if (!valid_origin(block) || !json_object_object_get_ex(block, "native", &native) ||
          !json_object_is_type(native, json_type_object)) {
         return "vendor block without its origin or content";
      }
      return NULL;
   }
   return "unknown block type";
}

/* The blocks of a parsed envelope, or NULL with @p why set. */
static struct json_object *envelope_blocks(struct json_object *envelope, const char **why) {
   struct json_object *v = NULL, *blocks = NULL;
   if (!json_object_is_type(envelope, json_type_object) ||
       !json_object_object_get_ex(envelope, "v", &v) || !json_object_is_type(v, json_type_int)) {
      *why = "not an envelope";
      return NULL;
   }
   if (json_object_get_int(v) != LLM_TURN_BLOCKS_STORED_VERSION) {
      *why = "unknown version";
      return NULL;
   }
   if (!json_object_object_get_ex(envelope, "blocks", &blocks) ||
       !json_object_is_type(blocks, json_type_array)) {
      *why = "no block list";
      return NULL;
   }
   const size_t n = json_object_array_length(blocks);
   if (n > LLM_TURN_BLOCKS_STORED_ENTRIES_MAX) {
      *why = "too many blocks";
      return NULL;
   }
   for (size_t i = 0; i < n; i++) {
      const char *problem = block_problem(json_object_array_get_idx(blocks, i));
      if (problem) {
         *why = problem;
         return NULL;
      }
   }
   return blocks;
}

struct json_object *llm_turn_blocks_from_stored(const char *text, size_t len, long long row_id) {
   if (!text || len == 0) {
      return NULL;
   }
   const char *why = NULL;
   struct json_object *out = NULL;
   if (len > LLM_TURN_BLOCKS_STORED_MAX) {
      why = "over the stored cap";
   } else {
      json_tokener *tok = json_tokener_new_ex(STORED_JSON_DEPTH);
      if (!tok) {
         return NULL;
      }
      struct json_object *envelope = json_tokener_parse_ex(tok, text, (int)len);
      const enum json_tokener_error err = json_tokener_get_error(tok);
      const bool whole = err == json_tokener_success && json_tokener_get_parse_end(tok) == len;
      json_tokener_free(tok);
      if (!whole) {
         why = err == json_tokener_error_depth ? "nested too deeply" : "not valid JSON";
      } else {
         out = envelope_blocks(envelope, &why);
         if (out) {
            json_object_get(out);
         }
      }
      json_object_put(envelope);
   }
   if (!out) {
      OLOG_WARNING("turn blocks: row %lld loads without its blocks (%s)", row_id, why);
   }
   return out;
}

/* The next tool call id in @p blocks from *pos, or NULL at the end. */
static const char *next_block_call(struct json_object *blocks, size_t *pos) {
   const size_t n = blocks ? json_object_array_length(blocks) : 0;
   while (*pos < n) {
      struct json_object *b = json_object_array_get_idx(blocks, (*pos)++);
      const char *type = type_of(b);
      struct json_object *id = NULL;
      if (type && strcmp(type, "tool_call") == 0 && json_object_object_get_ex(b, "id", &id)) {
         return json_object_get_string(id);
      }
   }
   return NULL;
}

bool llm_turn_blocks_calls_match(struct json_object *blocks, struct json_object *tool_calls) {
   if (tool_calls && !json_object_is_type(tool_calls, json_type_array)) {
      return false;
   }
   size_t pos = 0;
   const size_t n = tool_calls ? json_object_array_length(tool_calls) : 0;
   for (size_t i = 0; i < n; i++) {
      struct json_object *id = NULL;
      const char *want = json_object_object_get_ex(json_object_array_get_idx(tool_calls, i), "id",
                                                   &id)
                             ? json_object_get_string(id)
                             : NULL;
      const char *have = next_block_call(blocks, &pos);
      if (!want || !have || strcmp(want, have) != 0) {
         return false;
      }
   }
   return next_block_call(blocks, &pos) == NULL;
}

char *llm_turn_blocks_answer_stored(struct json_object *blocks) {
   if (!blocks) {
      return NULL;
   }
   if (!llm_turn_blocks_calls_match(blocks, NULL)) {
      OLOG_WARNING("turn blocks: a reply's blocks record tool calls; saved without them");
      return NULL;
   }
   return llm_turn_blocks_to_stored(blocks);
}
