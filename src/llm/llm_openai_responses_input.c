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
 * Pure request-input shaping for the OpenAI Responses API (see header +
 * docs/RESPONSES_CACHE_REORDER_PLAN.md).
 */

#include "llm/llm_openai_responses_input.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Prompt-cache layout (see docs/RESPONSES_CACHE_REORDER_PLAN.md).
 *
 * CROSS-MODULE INVARIANT: DAWN's two-segment system prompt is produced by
 * rebuild_history_with_two_system_messages_locked() (session_manager.c) as a
 * LEADING CONTIGUOUS RUN of system messages: index 0 = stable prefix (persona +
 * static rules, byte-identical across turns), index 1 = volatile block (per-turn
 * memory/docs/calendar + [system_time], the "--- TURN CONTEXT ---" DATA block).
 * A later `role:"system"` message is NOT part of that pair — it is a mid-history
 * broadcast (session_broadcast_system_message(), e.g. an incoming-call notice).
 *
 * For OpenAI Responses (automatic prefix caching, no cache_control breakpoints —
 * unlike the Anthropic path in llm_openai_cache.c), only the STABLE segment goes in
 * `instructions` (kept byte-stable so [instructions][tools][history] caches
 * cross-turn); the VOLATILE segment is repositioned to a user-role input item just
 * before the current question (llm_responses_build_input), and mid-history broadcasts
 * are emitted inline at their position. If the CC path (llm_openai_cache.c) is ever
 * taught to cache cross-turn, use this same "volatile as a user item before the
 * question" pattern.
 */

int llm_responses_count_leading_system_run(struct json_object *history) {
   int len = json_object_array_length(history);
   int run = 0;
   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *role_obj;
      if (!json_object_object_get_ex(msg, "role", &role_obj))
         break;
      if (strcmp(json_object_get_string(role_obj), "system") != 0)
         break;
      run++;
   }
   return run;
}

char *llm_responses_extract_stable_instructions(struct json_object *history) {
   if (llm_responses_count_leading_system_run(history) < 1)
      return NULL;
   struct json_object *msg = json_object_array_get_idx(history, 0);
   struct json_object *content_obj;
   if (!json_object_object_get_ex(msg, "content", &content_obj))
      return NULL;
   const char *txt = json_object_get_string(content_obj);
   return (txt && *txt) ? strdup(txt) : NULL;
}

char *llm_responses_extract_volatile_context(struct json_object *history) {
   int run = llm_responses_count_leading_system_run(history);
   if (run < 2)
      return NULL;

   size_t total = 0;
   for (int i = 1; i < run; i++) {
      struct json_object *content_obj;
      if (!json_object_object_get_ex(json_object_array_get_idx(history, i), "content",
                                     &content_obj))
         continue;
      const char *txt = json_object_get_string(content_obj);
      if (txt)
         total += strlen(txt) + 2;
   }
   if (total == 0)
      return NULL;

   char *out = malloc(total + 1);
   if (!out)
      return NULL;
   out[0] = '\0';
   size_t off = 0;
   bool first = true;
   for (int i = 1; i < run; i++) {
      struct json_object *content_obj;
      if (!json_object_object_get_ex(json_object_array_get_idx(history, i), "content",
                                     &content_obj))
         continue;
      const char *txt = json_object_get_string(content_obj);
      if (!txt || !*txt)
         continue;
      if (!first)
         off += snprintf(out + off, total + 1 - off, "\n\n");
      off += snprintf(out + off, total + 1 - off, "%s", txt);
      first = false;
   }
   return out;
}

/* Append vision images to a content_part array (Responses schema). */
void llm_responses_append_vision_parts(struct json_object *content_array,
                                       const char **vision_images,
                                       const size_t *vision_image_sizes,
                                       int vision_image_count) {
   for (int i = 0; i < vision_image_count; i++) {
      if (!vision_images[i])
         continue;
      if (vision_image_sizes && vision_image_sizes[i] == 0)
         continue;

      struct json_object *part = json_object_new_object();
      json_object_object_add(part, "type", json_object_new_string("input_image"));
      const char *prefix = "data:image/jpeg;base64,";
      size_t uri_len = strlen(prefix) + strlen(vision_images[i]) + 1;
      char *uri = malloc(uri_len);
      if (uri) {
         snprintf(uri, uri_len, "%s%s", prefix, vision_images[i]);
         json_object_object_add(part, "image_url", json_object_new_string(uri));
         free(uri);
      }
      json_object_array_add(content_array, part);
   }
}

struct json_object *llm_responses_build_input(struct json_object *history,
                                              const char *input_text,
                                              const char **vision_images,
                                              const size_t *vision_image_sizes,
                                              int vision_image_count,
                                              const char *volatile_block,
                                              int leading_system_run) {
   struct json_object *input = json_object_new_array();
   if (!input)
      return NULL;

   int len = json_object_array_length(history);
   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *role_obj;
      if (!json_object_object_get_ex(msg, "role", &role_obj))
         continue;
      const char *role = json_object_get_string(role_obj);

      /* Leading system run (stable + volatile) → instructions + repositioned
       * volatile below; skip here. A LATER system message is a mid-history
       * broadcast (session_broadcast_system_message, e.g. an incoming-call notice)
       * — emit it inline at its position so it is not lost and stays frozen in the
       * cacheable prefix. */
      if (strcmp(role, "system") == 0) {
         if (i < leading_system_run)
            continue;
         struct json_object *bc_content;
         if (!json_object_object_get_ex(msg, "content", &bc_content))
            continue;
         const char *bc_txt = json_object_get_string(bc_content);
         if (!bc_txt || !*bc_txt)
            continue;
         struct json_object *bc_item = json_object_new_object();
         json_object_object_add(bc_item, "type", json_object_new_string("message"));
         json_object_object_add(bc_item, "role", json_object_new_string("system"));
         struct json_object *bc_arr = json_object_new_array();
         struct json_object *bc_part = json_object_new_object();
         json_object_object_add(bc_part, "type", json_object_new_string("input_text"));
         json_object_object_add(bc_part, "text", json_object_new_string(bc_txt));
         json_object_array_add(bc_arr, bc_part);
         json_object_object_add(bc_item, "content", bc_arr);
         json_object_array_add(input, bc_item);
         continue;
      }

      /* Tool result message (chat-completions role:"tool") */
      if (strcmp(role, "tool") == 0) {
         struct json_object *call_id_obj, *content_obj;
         if (!json_object_object_get_ex(msg, "tool_call_id", &call_id_obj))
            continue;
         if (!json_object_object_get_ex(msg, "content", &content_obj))
            continue;
         struct json_object *item = json_object_new_object();
         json_object_object_add(item, "type", json_object_new_string("function_call_output"));
         json_object_object_add(item, "call_id",
                                json_object_new_string(json_object_get_string(call_id_obj)));
         json_object_object_add(item, "output",
                                json_object_new_string(json_object_get_string(content_obj)));
         json_object_array_add(input, item);
         continue;
      }

      /* Assistant message: emit reasoning items first, then text, then function_call items */
      if (strcmp(role, "assistant") == 0) {
         /* Echoed reasoning items (Mode B round-trip) */
         struct json_object *prov_state, *openai_resp, *r_items;
         if (json_object_object_get_ex(msg, "_provider_state", &prov_state) &&
             json_object_object_get_ex(prov_state, "openai_responses", &openai_resp) &&
             json_object_object_get_ex(openai_resp, "reasoning_items", &r_items) &&
             json_object_get_type(r_items) == json_type_array) {
            int n = json_object_array_length(r_items);
            for (int k = 0; k < n; k++) {
               struct json_object *item = json_object_array_get_idx(r_items, k);
               json_object_array_add(input, json_object_get(item));
            }
         }

         /* Pre-tool assistant text (if any) */
         struct json_object *content_obj;
         if (json_object_object_get_ex(msg, "content", &content_obj)) {
            const char *txt = json_object_get_string(content_obj);
            if (txt && *txt) {
               struct json_object *item = json_object_new_object();
               json_object_object_add(item, "type", json_object_new_string("message"));
               json_object_object_add(item, "role", json_object_new_string("assistant"));
               struct json_object *content_array = json_object_new_array();
               struct json_object *part = json_object_new_object();
               json_object_object_add(part, "type", json_object_new_string("output_text"));
               json_object_object_add(part, "text", json_object_new_string(txt));
               json_object_array_add(content_array, part);
               json_object_object_add(item, "content", content_array);
               json_object_array_add(input, item);
            }
         }

         /* Tool calls → function_call items */
         struct json_object *tool_calls;
         if (json_object_object_get_ex(msg, "tool_calls", &tool_calls) &&
             json_object_get_type(tool_calls) == json_type_array) {
            int n = json_object_array_length(tool_calls);
            for (int k = 0; k < n; k++) {
               struct json_object *tc = json_object_array_get_idx(tool_calls, k);
               struct json_object *id_obj, *fn_obj;
               if (!json_object_object_get_ex(tc, "id", &id_obj))
                  continue;
               if (!json_object_object_get_ex(tc, "function", &fn_obj))
                  continue;
               struct json_object *name_obj, *args_obj;
               if (!json_object_object_get_ex(fn_obj, "name", &name_obj))
                  continue;
               if (!json_object_object_get_ex(fn_obj, "arguments", &args_obj))
                  continue;
               struct json_object *item = json_object_new_object();
               json_object_object_add(item, "type", json_object_new_string("function_call"));
               json_object_object_add(item, "call_id",
                                      json_object_new_string(json_object_get_string(id_obj)));
               json_object_object_add(item, "name",
                                      json_object_new_string(json_object_get_string(name_obj)));
               json_object_object_add(item, "arguments",
                                      json_object_new_string(json_object_get_string(args_obj)));
               json_object_array_add(input, item);
            }
         }
         continue;
      }

      /* User message (string content; or array content for vision in legacy format) */
      if (strcmp(role, "user") == 0) {
         struct json_object *content_obj;
         if (!json_object_object_get_ex(msg, "content", &content_obj))
            continue;

         struct json_object *item = json_object_new_object();
         json_object_object_add(item, "type", json_object_new_string("message"));
         json_object_object_add(item, "role", json_object_new_string("user"));
         struct json_object *content_array = json_object_new_array();

         /* Chat-completions string content → single input_text part */
         if (json_object_get_type(content_obj) == json_type_string) {
            struct json_object *part = json_object_new_object();
            json_object_object_add(part, "type", json_object_new_string("input_text"));
            json_object_object_add(part, "text",
                                   json_object_new_string(json_object_get_string(content_obj)));
            json_object_array_add(content_array, part);
         } else if (json_object_get_type(content_obj) == json_type_array) {
            /* Chat-completions multimodal array → translate text/image_url parts */
            int n = json_object_array_length(content_obj);
            for (int k = 0; k < n; k++) {
               struct json_object *part_in = json_object_array_get_idx(content_obj, k);
               struct json_object *type_obj;
               if (!json_object_object_get_ex(part_in, "type", &type_obj))
                  continue;
               const char *t = json_object_get_string(type_obj);
               if (strcmp(t, "text") == 0) {
                  struct json_object *txt_obj;
                  if (json_object_object_get_ex(part_in, "text", &txt_obj)) {
                     struct json_object *part = json_object_new_object();
                     json_object_object_add(part, "type", json_object_new_string("input_text"));
                     json_object_object_add(
                         part, "text", json_object_new_string(json_object_get_string(txt_obj)));
                     json_object_array_add(content_array, part);
                  }
               } else if (strcmp(t, "image_url") == 0) {
                  struct json_object *url_wrapper, *url_obj;
                  if (json_object_object_get_ex(part_in, "image_url", &url_wrapper) &&
                      json_object_object_get_ex(url_wrapper, "url", &url_obj)) {
                     struct json_object *part = json_object_new_object();
                     json_object_object_add(part, "type", json_object_new_string("input_image"));
                     json_object_object_add(part, "image_url",
                                            json_object_new_string(
                                                json_object_get_string(url_obj)));
                     json_object_array_add(content_array, part);
                  }
               }
            }
         }

         json_object_object_add(item, "content", content_array);
         json_object_array_add(input, item);
      }
   }

   /* Append the new user input + vision (if not already part of history) */
   if (input_text && *input_text) {
      struct json_object *item = json_object_new_object();
      json_object_object_add(item, "type", json_object_new_string("message"));
      json_object_object_add(item, "role", json_object_new_string("user"));
      struct json_object *content_array = json_object_new_array();
      struct json_object *part = json_object_new_object();
      json_object_object_add(part, "type", json_object_new_string("input_text"));
      json_object_object_add(part, "text", json_object_new_string(input_text));
      json_object_array_add(content_array, part);
      llm_responses_append_vision_parts(content_array, vision_images, vision_image_sizes,
                                        vision_image_count);
      json_object_object_add(item, "content", content_array);
      json_object_array_add(input, item);
   } else if (vision_image_count > 0) {
      /* Vision attached to last user message in history is the chat-completions pattern;
       * replicate by appending images to that message if we just emitted it. */
      int n = json_object_array_length(input);
      if (n > 0) {
         struct json_object *last = json_object_array_get_idx(input, n - 1);
         struct json_object *role_obj, *content_obj;
         if (json_object_object_get_ex(last, "role", &role_obj) &&
             strcmp(json_object_get_string(role_obj), "user") == 0 &&
             json_object_object_get_ex(last, "content", &content_obj) &&
             json_object_get_type(content_obj) == json_type_array) {
            llm_responses_append_vision_parts(content_obj, vision_images, vision_image_sizes,
                                              vision_image_count);
         }
      }
   }

   /* Reposition the volatile TURN CONTEXT block as a user item IMMEDIATELY BEFORE the
    * current question (the last user-role item in the fully-assembled input, after
    * both vision branches above). This keeps `instructions` byte-stable so
    * [instructions][tools][history] caches cross-turn (see the cache-layout note at
    * the top of this file). Anchoring on "last user item" is robust to whether the
    * question arrived via history or input_text, and keeps the volatile pinned before
    * the (fixed) question across tool-loop iterations. Not persisted — the session
    * already rebuilds/replaces the two system messages each turn. */
   if (volatile_block && *volatile_block) {
      int n = json_object_array_length(input);
      int last_user = -1;
      for (int i = n - 1; i >= 0; i--) {
         struct json_object *it = json_object_array_get_idx(input, i);
         struct json_object *r;
         if (json_object_object_get_ex(it, "role", &r) && json_object_get_string(r) &&
             strcmp(json_object_get_string(r), "user") == 0) {
            last_user = i;
            break;
         }
      }

      struct json_object *vitem = json_object_new_object();
      json_object_object_add(vitem, "type", json_object_new_string("message"));
      json_object_object_add(vitem, "role", json_object_new_string("user"));
      struct json_object *vcontent = json_object_new_array();
      struct json_object *vpart = json_object_new_object();
      json_object_object_add(vpart, "type", json_object_new_string("input_text"));
      json_object_object_add(vpart, "text", json_object_new_string(volatile_block));
      json_object_array_add(vcontent, vpart);
      json_object_object_add(vitem, "content", vcontent);

      if (last_user < 0) {
         json_object_array_add(input, vitem); /* degenerate: no user item → append */
      } else {
         /* json-c has no array insert; splice vitem in before last_user via a fresh
          * array (carried items get an extra ref before the old array is released). */
         struct json_object *spliced = json_object_new_array();
         if (!spliced) {
            json_object_put(vitem);
         } else {
            for (int i = 0; i < n; i++) {
               if (i == last_user)
                  json_object_array_add(spliced, vitem);
               json_object_array_add(spliced, json_object_get(json_object_array_get_idx(input, i)));
            }
            json_object_put(input);
            input = spliced;
         }
      }
   }

   return input;
}
