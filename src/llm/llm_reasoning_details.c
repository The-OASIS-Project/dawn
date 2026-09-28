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
 * OpenRouter reasoning_details, gathered from a stream for replay.  See
 * llm_reasoning_details.h.
 */

#include "llm/llm_reasoning_details.h"

#include <json-c/json.h>
#include <string.h>

#include "logging.h"

static const char *str_of(struct json_object *obj, const char *key) {
   struct json_object *v = NULL;
   return (obj && json_object_object_get_ex(obj, key, &v)) ? json_object_get_string(v) : NULL;
}

void llm_reasoning_details_init(llm_reasoning_details_t *acc) {
   memset(acc, 0, sizeof(*acc));
   strbuf_init_with_max(&acc->text, 0, LLM_REASONING_DETAILS_BYTES_MAX + 1);
   strbuf_init_with_max(&acc->summary, 0, LLM_REASONING_DETAILS_BYTES_MAX + 1);
}

void llm_reasoning_details_free(llm_reasoning_details_t *acc) {
   json_object_put(acc->entries);
   json_object_put(acc->open);
   strbuf_free(&acc->text);
   strbuf_free(&acc->summary);
   acc->entries = NULL;
   acc->open = NULL;
}

/* Drop everything: a cap was passed. */
static void give_up(llm_reasoning_details_t *acc, const char *why) {
   OLOG_WARNING("LLM: reasoning_details %s; not kept for replay", why);
   llm_reasoning_details_free(acc);
   acc->over = true;
}

/* Move the open entry, its text and summary set, to the finished ones. */
static void close_open(llm_reasoning_details_t *acc) {
   if (!acc->open) {
      return;
   }
   if (acc->has_text) {
      json_object_object_add(acc->open, "text", json_object_new_string(strbuf_str(&acc->text)));
   }
   if (acc->has_summary) {
      json_object_object_add(acc->open, "summary",
                             json_object_new_string(strbuf_str(&acc->summary)));
   }
   if (!acc->entries) {
      acc->entries = json_object_new_array();
   }
   if (acc->entries) {
      json_object_array_add(acc->entries, acc->open);
   } else {
      json_object_put(acc->open);
   }
   acc->open = NULL;
   strbuf_free(&acc->text);
   strbuf_free(&acc->summary);
   strbuf_init_with_max(&acc->text, 0, LLM_REASONING_DETAILS_BYTES_MAX + 1);
   strbuf_init_with_max(&acc->summary, 0, LLM_REASONING_DETAILS_BYTES_MAX + 1);
   acc->has_text = acc->has_summary = false;
}

/* Whether @p piece continues the open entry: same type, same index (or none). */
static bool continues(llm_reasoning_details_t *acc, struct json_object *piece) {
   if (!acc->open) {
      return false;
   }
   const char *type = str_of(piece, "type");
   const char *open_type = str_of(acc->open, "type");
   if (!type || !open_type || strcmp(type, open_type) != 0) {
      return false;
   }
   struct json_object *index = NULL, *open_index = NULL;
   const bool has = json_object_object_get_ex(piece, "index", &index) && index;
   const bool open_has = json_object_object_get_ex(acc->open, "index", &open_index) && open_index;
   if (has != open_has) {
      return false;
   }
   return !has || json_object_get_int(index) == json_object_get_int(open_index);
}

void llm_reasoning_details_add(llm_reasoning_details_t *acc, struct json_object *piece) {
   if (acc->over || !json_object_is_type(piece, json_type_object)) {
      return;
   }
   if (!continues(acc, piece)) {
      close_open(acc);
      if (++acc->count > LLM_REASONING_DETAILS_ENTRIES_MAX) {
         give_up(acc, "has too many entries");
         return;
      }
      acc->open = json_object_new_object();
      if (!acc->open) {
         give_up(acc, "could not be kept (out of memory)");
         return;
      }
   }
   json_object_object_foreach(piece, key, val) {
      const bool is_text = strcmp(key, "text") == 0;
      const bool is_summary = strcmp(key, "summary") == 0;
      if ((is_text || is_summary) && json_object_is_type(val, json_type_string)) {
         const size_t len = (size_t)json_object_get_string_len(val);
         acc->bytes += len;
         strbuf_t *buf = is_text ? &acc->text : &acc->summary;
         (void)strbuf_append_n(buf, json_object_get_string(val), len);
         if (is_text) {
            acc->has_text = true;
         } else {
            acc->has_summary = true;
         }
      } else if (!json_object_is_type(val, json_type_null)) {
         /* Every piece repeats its type, format and index: count a field's
          * size once, and only its growth when a piece sets it again. */
         const char *s = json_object_to_json_string_ext(val, JSON_C_TO_STRING_PLAIN);
         const size_t now = s ? strlen(s) : 0;
         struct json_object *had = NULL;
         size_t before = 0;
         if (json_object_object_get_ex(acc->open, key, &had) && had) {
            const char *h = json_object_to_json_string_ext(had, JSON_C_TO_STRING_PLAIN);
            before = h ? strlen(h) : 0;
         }
         acc->bytes += now > before ? now - before : 0;
         struct json_object *copy = NULL;
         if (json_object_deep_copy(val, &copy, NULL) == 0) {
            json_object_object_add(acc->open, key, copy);
         }
      }
   }
   if (acc->bytes > LLM_REASONING_DETAILS_BYTES_MAX || strbuf_oom(&acc->text) ||
       strbuf_oom(&acc->summary)) {
      give_up(acc, "passed the size cap");
   }
}

bool llm_served_as_asked(const char *served, const char *asked) {
   if (!served || !*served || !asked || !*asked) {
      return true;
   }
   /* An OpenRouter variant ("vendor/model:free") is still the model. */
   const char *colon = strchr(asked, ':');
   const size_t a = colon ? (size_t)(colon - asked) : strlen(asked);
   if (strncmp(served, asked, a) != 0) {
      return false;
   }
   return served[a] == '\0' || served[a] == '-'; /* the same, or a dated version of it */
}

struct json_object *llm_reasoning_details_finish(llm_reasoning_details_t *acc) {
   if (acc->over) {
      return NULL;
   }
   close_open(acc);
   struct json_object *out = acc->entries;
   acc->entries = NULL;
   return out;
}
