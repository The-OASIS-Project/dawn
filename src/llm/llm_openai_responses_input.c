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
 * Pure request-input shaping for the OpenAI Responses API (see the header and
 * the prompt-cache layout note below).
 */

#include "llm/llm_openai_responses_input.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm_claude_parts.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_tool_images_render.h"
#include "llm/llm_turn_blocks.h"
#include "logging.h"

/*
 * Prompt-cache layout.
 *
 * A conversation's request is append-only (src/core/session_prefix.c): its system
 * prompt and tool set are frozen at its first turn, in one leading system message,
 * and later changes (directives, instructions, tool-set changes, each turn's context)
 * are appended where they happen. That leading message goes in `instructions`, so
 * [instructions][tools][history] is byte-stable across turns and OpenAI caches it with
 * its implicit end-of-messages breakpoint; a turn's context is part of its question,
 * not a separate block. Conversation-scoped system messages (a directive, instruction
 * or tool-set change) are never part of the leading run: they are emitted inline at
 * their position (a tool-set change goes in the request's tools instead). A later
 * `role:"system"` message from a history an older build saved is emitted inline too.
 *
 * A history without a frozen prefix (a research run, whose system messages are its
 * prompt) can carry a second leading system message: a per-turn volatile block. It is
 * repositioned to a user-role input item just before the current question
 * (llm_responses_build_input), and on GPT-5.6+ an explicit `prompt_cache_breakpoint`
 * is stamped on the last stable input_text before it, paired with request-root
 * `prompt_cache_options:{mode:implicit}` (set in llm_openai_responses.c), so the
 * history before the volatile block still caches across rounds. Pre-5.6 Responses
 * models reject the field, so it is gated on the model version.
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
      /* A directive, instruction or tool-set change is the conversation's, not
       * the prompt's. */
      if (message_kind_conversation_scoped(llm_history_kind_of(msg)))
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

/*
 * Stamp an explicit prompt-cache breakpoint on the last stable input_text block.
 *
 * GPT-5.6+ prompt caching (OpenAI "Prompt caching" guide): implicit caching writes ONE
 * breakpoint at the end of the latest message, so the stable [instructions][tools][history]
 * prefix in front of a per-turn volatile block (only a history without a frozen prefix
 * has one: a research run) is never independently reusable — the documented "a shared
 * prefix is not always a cached prefix" gotcha (live-measured: within-turn cached ~full,
 * each NEW turn fell back to header-only ~21K).
 * An explicit breakpoint after the stable content makes that prefix cache cross-turn. It
 * is honored ALONGSIDE the implicit end breakpoint (mode stays "implicit"), so within-turn
 * tool-loop caching is preserved. OpenAI accepts breakpoints only on input-side text
 * blocks (input_text in user/developer/tool messages) — NOT assistant output_text and NOT
 * the top-level `instructions` field — so the walk-back skips assistant turns. Returns true
 * once placed. Adapted from the OpenAI multi-turn-agent caching example.
 */
static bool responses_mark_cache_breakpoint(struct json_object *item) {
   if (item == NULL)
      return false;
   struct json_object *content;
   if (!json_object_object_get_ex(item, "content", &content) ||
       json_object_get_type(content) != json_type_array)
      return false; /* function_call/function_call_output carry no content array here — skip */
   int cn = json_object_array_length(content);
   if (cn == 0)
      return false;
   struct json_object *last_part = json_object_array_get_idx(content, cn - 1);
   if (last_part == NULL || json_object_get_type(last_part) != json_type_object)
      return false;
   struct json_object *pt;
   if (!json_object_object_get_ex(last_part, "type", &pt) || json_object_get_string(pt) == NULL ||
       strcmp(json_object_get_string(pt), "input_text") != 0)
      return false; /* only input_text blocks accept a breakpoint */
   struct json_object *bp = json_object_new_object();
   if (bp == NULL)
      return false;
   json_object_object_add(bp, "mode", json_object_new_string("explicit"));
   json_object_object_add(last_part, "prompt_cache_breakpoint", bp);
   return true;
}

/* A Claude tool_result part as a function_call_output item. */
static void responses_add_claude_tool_result(struct json_object *part, struct json_object *input) {
   struct json_object *id = NULL;
   const char *call_id = json_object_object_get_ex(part, "tool_use_id", &id)
                             ? json_object_get_string(id)
                             : NULL;
   if (!call_id) {
      return; /* unpairable: sending it would be an API error */
   }
   struct json_object *content = NULL;
   json_object_object_get_ex(part, "content", &content);
   struct json_object *output = llm_tool_images_responses_output(content);
   struct json_object *item = output ? json_object_new_object() : NULL;
   if (item) {
      json_object_object_add(item, "type", json_object_new_string("function_call_output"));
      json_object_object_add(item, "call_id", json_object_new_string(call_id));
      json_object_object_add(item, "output", output);
      json_object_array_add(input, item);
   } else {
      json_object_put(output);
   }
}

/* A Claude base64 image part as an input_image part. */
static void responses_add_claude_image(struct json_object *part, struct json_object *parts) {
   char *url = llm_claude_image_data_url(part);
   struct json_object *img = url ? json_object_new_object() : NULL;
   if (img) {
      json_object_object_add(img, "type", json_object_new_string("input_image"));
      json_object_object_add(img, "image_url", json_object_new_string(url));
      json_object_array_add(parts, img);
   }
   free(url);
}

/* The call_id of a function_call or function_call_output item, or NULL. */
static const char *pairing_id(struct json_object *item, bool *is_call) {
   struct json_object *t = NULL, *id = NULL;
   if (!json_object_object_get_ex(item, "type", &t) || !json_object_get_string(t)) {
      return NULL;
   }
   const char *type = json_object_get_string(t);
   *is_call = strcmp(type, "function_call") == 0;
   if (!*is_call && strcmp(type, "function_call_output") != 0) {
      return NULL;
   }
   return json_object_object_get_ex(item, "call_id", &id) ? json_object_get_string(id) : NULL;
}

/* @p input without any function call or output that lacks its partner: a
 * history compacted mid-exchange, or converted from another provider, can hold
 * one, and either alone fails the request.  Each output pairs with the
 * nearest unanswered call of its id before it (servers that number calls per
 * turn reuse ids).  Takes @p input; returns the result. */
static struct json_object *drop_unpaired_calls(struct json_object *input) {
   const size_t n = json_object_array_length(input);
   bool *keep = calloc(n ? n : 1, sizeof(*keep));
   struct json_object *out = keep ? json_object_new_array() : NULL;
   if (!out) {
      free(keep);
      return input;
   }
   for (size_t j = 0; j < n; j++) {
      bool is_call = false;
      const char *id = pairing_id(json_object_array_get_idx(input, j), &is_call);
      if (!id) {
         keep[j] = true;
         continue;
      }
      if (is_call) {
         continue; /* a call is kept by the output that answers it */
      }
      /* An output answers the nearest call of its id before it not yet answered. */
      for (size_t i = j; i-- > 0;) {
         bool other_is_call = false;
         const char *other = pairing_id(json_object_array_get_idx(input, i), &other_is_call);
         if (other && other_is_call && !keep[i] && strcmp(other, id) == 0) {
            keep[i] = keep[j] = true;
            break;
         }
      }
   }
   size_t dropped = 0;
   for (size_t i = 0; i < n; i++) {
      if (keep[i]) {
         json_object_array_add(out, json_object_get(json_object_array_get_idx(input, i)));
      } else {
         dropped++;
      }
   }
   if (dropped > 0) {
      OLOG_WARNING("Responses: dropped %zu function call item(s) without a partner", dropped);
   }
   free(keep);
   json_object_put(input);
   return out;
}

struct json_object *llm_responses_build_input(struct json_object *history,
                                              const char *input_text,
                                              const char *volatile_block,
                                              int leading_system_run,
                                              bool enable_cache_breakpoint,
                                              const char *carrier,
                                              const char *model) {
   struct json_object *input = json_object_new_array();
   if (!input)
      return NULL;

   /* The newest user item while it is still the conversation's last word (an
    * operator's direction after it doesn't change that). */
   struct json_object *question_item = NULL;

   int len = json_object_array_length(history);
   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *role_obj;
      if (!json_object_object_get_ex(msg, "role", &role_obj))
         continue;
      const char *role = json_object_get_string(role_obj);

      /* Leading system run (stable + volatile) → instructions + repositioned
       * volatile below; skip here. A LATER system message (from a history an
       * older build saved) is emitted inline at its position so it is not lost
       * and stays frozen in the cacheable prefix. */
      if (strcmp(role, "system") == 0) {
         if (i < leading_system_run)
            continue;
         /* A tool-set change goes in the request's tools (folded:
          * llm_tool_defs_for_request), not here. */
         if (llm_history_kind_of(msg) == MESSAGE_KIND_TOOL_CHANGE)
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
         if (llm_history_kind_of(msg) == MESSAGE_KIND_NONE) {
            question_item = NULL; /* an older build's system message ends the question */
         }
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
         /* A result that carried images: its text and images, in order
          * (input_text / input_image items: llm_tool_images_render.h). */
         struct json_object *output = llm_tool_images_responses_output(content_obj);
         json_object_object_add(item, "output", output ? output : json_object_new_string(""));
         json_object_array_add(input, item);
         question_item = NULL;
         continue;
      }

      /* Assistant message: rendered from its blocks, in the order produced (OpenAI's
       * own reasoning items included, other vendors' left out). */
      if (strcmp(role, "assistant") == 0) {
         struct json_object *blocks = llm_turn_message_blocks(msg);
         llm_turn_blocks_render_responses(blocks, input, carrier, model);
         json_object_put(blocks);
         question_item = NULL;
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
            /* A multimodal array (chat-completions or Claude parts) → input parts.
             * A Claude tool_result becomes its own function_call_output item, ahead
             * of the message (the call it answers came just before it). */
            int n = json_object_array_length(content_obj);
            for (int k = 0; k < n; k++) {
               struct json_object *part_in = json_object_array_get_idx(content_obj, k);
               struct json_object *type_obj;
               if (!json_object_object_get_ex(part_in, "type", &type_obj))
                  continue;
               const char *t = json_object_get_string(type_obj);
               if (!t) {
                  continue;
               }
               if (strcmp(t, "tool_result") == 0) {
                  responses_add_claude_tool_result(part_in, input);
               } else if (strcmp(t, "image") == 0) {
                  responses_add_claude_image(part_in, content_array);
               } else if (strcmp(t, "text") == 0) {
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

         if (json_object_array_length(content_array) > 0) {
            json_object_object_add(item, "content", content_array);
            json_object_array_add(input, item);
            question_item = item;
         } else {
            /* Only tool results: they were emitted as their own items. */
            json_object_put(content_array);
            json_object_put(item);
            question_item = NULL;
         }
      }
   }

   /* Append the new user input — but ONLY if the question isn't already the last item.
    * The caller may have already added the user turn to history (the "no-add" path in
    * session_manager_llm.c: the WebUI/voice layer appends it, then passes it here as
    * `input_text` too). Appending unconditionally then DUPLICATES the question — once
    * from history, once here — which the reorder made visible as [Q][volatile][Q] and
    * which also poisons cross-turn prompt caching. The chat-completions builder guards
    * this with the identical last-is-user check (llm_openai_history.c:638); mirror it so
    * the Responses request carries the question exactly once. When the last item already
    * IS the user turn (its context, its text and its own images: a turn's images live
    * in its question's history message), it is kept exactly as built above. */
   if (question_item == NULL && input_text && *input_text) {
      struct json_object *item = json_object_new_object();
      json_object_object_add(item, "type", json_object_new_string("message"));
      json_object_object_add(item, "role", json_object_new_string("user"));
      struct json_object *content_array = json_object_new_array();
      struct json_object *part = json_object_new_object();
      json_object_object_add(part, "type", json_object_new_string("input_text"));
      json_object_object_add(part, "text", json_object_new_string(input_text));
      json_object_array_add(content_array, part);
      json_object_object_add(item, "content", content_array);
      json_object_array_add(input, item);
   }

   /* After the question is placed: dropping an unpaired call first could make an
    * earlier question the tail, and the new one would replace it. */
   input = drop_unpaired_calls(input);

   /* Reposition the volatile TURN CONTEXT block as a user item IMMEDIATELY BEFORE the
    * current question (the last user-role item in the fully-assembled input, after
    * the question's placement above). This keeps `instructions` byte-stable so
    * [instructions][tools][history] caches cross-turn (see the cache-layout note at
    * the top of this file). Anchoring on "last user item" is robust to whether the
    * question arrived via history or input_text, and keeps the volatile pinned before
    * the (fixed) question across tool-loop iterations. Only a history without a frozen
    * prefix has one (a session whose system messages are its prompt: a research run);
    * a frozen conversation's turn context is part of its question. */
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

      /* Explicit cache breakpoint at the end of the stable prefix — the last input_text
       * block BEFORE the volatile lands (i.e. below last_user). Walk back past assistant
       * turns and content-less tool items to the nearest input_text-bearing message. This
       * is what makes [instructions][tools][history] cache cross-turn (see
       * responses_mark_cache_breakpoint). Stamped before the splice so the extra
       * ref-counted references carry it through. Gated on GPT-5.6+ by the caller — pre-5.6
       * Responses models (gpt-5.4/5.5) reject prompt_cache_breakpoint. */
      if (enable_cache_breakpoint) {
         for (int i = last_user - 1; i >= 0; i--) {
            if (responses_mark_cache_breakpoint(json_object_array_get_idx(input, i)))
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
