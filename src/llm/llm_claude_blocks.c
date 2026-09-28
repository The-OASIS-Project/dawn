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
 * Streamed Claude content blocks, captured in order.  See llm_claude_blocks.h.
 */

#include "llm/llm_claude_blocks.h"

#include <json-c/json.h>
#include <string.h>

#include "logging.h"

/* A block's streamed text can be long (a thinking block, a large tool input);
 * a signature is small.  Past these the block is too large to replay intact. */
#define CAPTURE_TEXT_MAX (8u * 1024u * 1024u)
#define CAPTURE_SIGNATURE_MAX (256u * 1024u)

static const char *type_of(struct json_object *block) {
   struct json_object *v = NULL;
   return json_object_object_get_ex(block, "type", &v) ? json_object_get_string(v) : "";
}

static void clear_buffers(llm_claude_capture_t *c) {
   if (c->text_init) {
      strbuf_free(&c->text);
      c->text_init = false;
   }
   if (c->signature_init) {
      strbuf_free(&c->signature);
      c->signature_init = false;
   }
}

static void append(strbuf_t *sb, bool *init, size_t max, const char *s) {
   if (!s || !*s) {
      return;
   }
   if (!*init) {
      strbuf_init_with_max(sb, 0, max);
      *init = true;
   }
   strbuf_append(sb, s);
}

void llm_claude_capture_start(llm_claude_capture_t *c, struct json_object *content_block) {
   if (!c || !content_block) {
      return;
   }
   if (c->current) {
      llm_claude_capture_stop(c); /* a start without a stop: close the open block */
   }
   /* The start event's block is the block itself (a thinking block starts with
    * empty text, a tool_use with an empty input); deltas fill it in. */
   struct json_object *copy = NULL;
   if (json_object_deep_copy(content_block, &copy, NULL) != 0) {
      return;
   }
   c->current = copy;
}

void llm_claude_capture_delta(llm_claude_capture_t *c, struct json_object *delta) {
   if (!c || !c->current || !delta) {
      return;
   }
   struct json_object *v = NULL;
   const char *type = type_of(delta);
   if (strcmp(type, "thinking_delta") == 0 && json_object_object_get_ex(delta, "thinking", &v)) {
      append(&c->text, &c->text_init, CAPTURE_TEXT_MAX, json_object_get_string(v));
   } else if (strcmp(type, "signature_delta") == 0 &&
              json_object_object_get_ex(delta, "signature", &v)) {
      append(&c->signature, &c->signature_init, CAPTURE_SIGNATURE_MAX, json_object_get_string(v));
   } else if (strcmp(type, "text_delta") == 0 && json_object_object_get_ex(delta, "text", &v)) {
      append(&c->text, &c->text_init, CAPTURE_TEXT_MAX, json_object_get_string(v));
   } else if (strcmp(type, "input_json_delta") == 0 &&
              json_object_object_get_ex(delta, "partial_json", &v)) {
      append(&c->text, &c->text_init, CAPTURE_TEXT_MAX, json_object_get_string(v));
   }
}

/* Set @p key to the streamed text, appended to what the start event held.
 * false if the result can't be held whole (the block is then not replayable). */
static bool finish_string(struct json_object *block,
                          const char *key,
                          const strbuf_t *sb,
                          bool init) {
   struct json_object *v = NULL;
   const char *start = json_object_object_get_ex(block, key, &v) ? json_object_get_string(v) : "";
   if (!start) {
      start = "";
   }
   if (!init || strbuf_len(sb) == 0) {
      json_object_object_add(block, key, json_object_new_string(start));
      return true;
   }
   if (!*start) {
      /* The usual case: the start event's value is empty. */
      json_object_object_add(block, key, json_object_new_string(strbuf_str(sb)));
      return true;
   }
   strbuf_t joined;
   strbuf_init_with_max(&joined, 0, CAPTURE_TEXT_MAX + strlen(start) + 1);
   strbuf_append(&joined, start);
   strbuf_append(&joined, strbuf_str(sb));
   const bool whole = !strbuf_oom(&joined);
   if (whole) {
      json_object_object_add(block, key, json_object_new_string(strbuf_str(&joined)));
   }
   strbuf_free(&joined);
   return whole;
}

void llm_claude_capture_stop(llm_claude_capture_t *c) {
   if (!c || !c->current) {
      return;
   }
   struct json_object *block = c->current;
   c->current = NULL;
   const bool overflow = (c->text_init && strbuf_oom(&c->text)) ||
                         (c->signature_init && strbuf_oom(&c->signature));
   const char *type = type_of(block);
   if (overflow) {
      /* Replaying a cut block would be a tampered signature (always a 400);
       * leaving it out is a drop the binding controls report and absorb. */
      OLOG_WARNING("Claude capture: a %s block exceeded the capture limit; it won't be replayed",
                   type);
      json_object_put(block);
      clear_buffers(c);
      return;
   }
   bool whole = true;
   if (strcmp(type, "thinking") == 0) {
      whole = finish_string(block, "thinking", &c->text, c->text_init) &&
              finish_string(block, "signature", &c->signature, c->signature_init);
   } else if (strcmp(type, "text") == 0) {
      whole = finish_string(block, "text", &c->text, c->text_init);
   } else if (strcmp(type, "tool_use") == 0 && c->text_init && strbuf_len(&c->text) > 0) {
      struct json_object *input = json_tokener_parse(strbuf_str(&c->text));
      if (input) {
         json_object_object_add(block, "input", input);
      } else {
         OLOG_WARNING("Claude capture: tool_use input didn't parse; keeping the start's input");
      }
   }
   if (!whole) {
      OLOG_WARNING("Claude capture: a %s block couldn't be held whole; it won't be replayed", type);
      json_object_put(block);
      clear_buffers(c);
      return;
   }
   if (!c->content) {
      c->content = json_object_new_array();
   }
   if (c->content) {
      json_object_array_add(c->content, block);
   } else {
      json_object_put(block);
   }
   clear_buffers(c);
}

struct json_object *llm_claude_capture_take(llm_claude_capture_t *c) {
   if (!c) {
      return NULL;
   }
   if (c->current) {
      /* A stream that ended mid-block: its signature is incomplete, and a
       * replay of it would be rejected as tampered.  It isn't part of the turn. */
      json_object_put(c->current);
      c->current = NULL;
      clear_buffers(c);
   }
   struct json_object *content = c->content;
   c->content = NULL;
   return content;
}

void llm_claude_capture_reset(llm_claude_capture_t *c) {
   if (!c) {
      return;
   }
   json_object_put(c->current);
   json_object_put(c->content);
   c->current = NULL;
   c->content = NULL;
   clear_buffers(c);
}
