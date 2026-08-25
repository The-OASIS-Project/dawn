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
 * Anthropic prompt-cache breakpoint injection for OpenRouter chat-completions
 * requests.  See llm_openai_cache.h for the rationale and the copy-on-write
 * contract.
 */

#include "llm/llm_openai_cache.h"

#include <stdlib.h>
#include <string.h>

bool llm_openai_anthropic_cache_applies(const char *model_name, const char *base_url) {
   return base_url != NULL && model_name != NULL && strstr(base_url, "openrouter.ai") != NULL &&
          strncmp(model_name, "anthropic/", 10) == 0;
}

/* OpenRouter forwards Anthropic prompt-cache breakpoints to Claude only when they
 * are present in the request.  The OpenAI chat-completions format we use for the
 * gateway carries none by default, so a Claude model fronted by OpenRouter never
 * caches — every tool-loop iteration reprocesses the full prompt (observed: 0
 * cached_tokens across 6 iters, ~25K input + images re-sent each time).
 *
 * Mirror the native llm_claude_format.c breakpoints (system block + last tool — 2 of
 * Anthropic's 4 allowed) in OpenAI-format shape: cache_control on the last tool, and
 * the first system message converted to a single-part content array carrying
 * cache_control.  Anthropic caches everything up to and including a marked block, so
 * the stable system prompt and the tools array each become their own cache prefix.
 *
 * Gated to openrouter + `anthropic/` slugs — OpenRouter slugs are "vendor/model", and sending
 * cache_control to a non-Anthropic upstream is at best ignored, at worst rejected. */
void llm_openai_add_anthropic_cache(json_object *root,
                                    const char *model_name,
                                    const char *base_url) {
   if (!root || !llm_openai_anthropic_cache_applies(model_name, base_url)) {
      return;
   }

   /* Breakpoint 1: last tool — caches the entire tools array as one prefix. */
   json_object *tools = NULL;
   if (json_object_object_get_ex(root, "tools", &tools) &&
       json_object_is_type(tools, json_type_array)) {
      int tool_count = json_object_array_length(tools);
      if (tool_count > 0) {
         json_object *last_tool = json_object_array_get_idx(tools, tool_count - 1);
         if (last_tool != NULL && json_object_is_type(last_tool, json_type_object)) {
            json_object *cc = json_object_new_object();
            json_object_object_add(cc, "type", json_object_new_string("ephemeral"));
            json_object_object_add(last_tool, "cache_control", cc);
         }
      }
   }

   /* Breakpoint 2: first system message — present its plain-string content as a
    * single-element parts array carrying cache_control. */
   json_object *messages = NULL;
   if (json_object_object_get_ex(root, "messages", &messages) &&
       json_object_is_type(messages, json_type_array)) {
      int msg_count = json_object_array_length(messages);

      /* Locate the first system message that still has plain-string content. */
      int sys_idx = -1;
      json_object *sys_role = NULL;
      json_object *sys_content = NULL;
      for (int i = 0; i < msg_count; i++) {
         json_object *msg = json_object_array_get_idx(messages, i);
         json_object *role_obj = NULL;
         if (msg == NULL || !json_object_object_get_ex(msg, "role", &role_obj)) {
            continue;
         }
         const char *role_str = json_object_get_string(role_obj);
         if (role_str == NULL || strcmp(role_str, "system") != 0) {
            continue;
         }
         json_object *content_obj = NULL;
         if (json_object_object_get_ex(msg, "content", &content_obj) &&
             json_object_is_type(content_obj, json_type_string)) {
            sys_idx = i;
            sys_role = role_obj;
            sys_content = content_obj;
         }
         break; /* first system message only */
      }

      if (sys_idx >= 0) {
         /* Build the cache_control-bearing wrapper.  json_object_get() bumps the
          * refcounts on the shared role/content strings so they survive when the
          * private message takes ownership of them. */
         json_object *part = json_object_new_object();
         json_object_object_add(part, "type", json_object_new_string("text"));
         json_object_object_add(part, "text", json_object_get(sys_content));
         json_object *cc = json_object_new_object();
         json_object_object_add(cc, "type", json_object_new_string("ephemeral"));
         json_object_object_add(part, "cache_control", cc);
         json_object *content_array = json_object_new_array();
         json_object_array_add(content_array, part);

         json_object *wrapped = json_object_new_object();
         json_object_object_add(wrapped, "role", json_object_get(sys_role));
         json_object_object_add(wrapped, "content", content_array);

         /* root.messages may ALIAS the caller's conversation_history —
          * llm_openai_prepare_chat_history returns it by shared reference in the
          * common no-op path.  Mutating the message (or the array) in place would
          * corrupt the session's canonical history: the wrapped JSON would leak
          * into the system-prompt inspector and hand array-typed content to
          * readers that expect a plain string.  Instead, swap root.messages for a
          * request-private shallow clone that carries a private copy of just this
          * one system message; every other message stays shared (read-only). */
         json_object *private_msgs = json_object_new_array();
         if (private_msgs != NULL) {
            for (int j = 0; j < msg_count; j++) {
               json_object *src = json_object_array_get_idx(messages, j);
               json_object_array_add(private_msgs, j == sys_idx ? wrapped : json_object_get(src));
            }
            json_object_object_add(root, "messages", private_msgs); /* releases old array ref */
         } else {
            json_object_put(wrapped); /* OOM: drop the private copy, leave root.messages as-is */
         }
      }
   }
}

/* Collapse the leading run of plain-string system messages into one.  See the
 * header for the full rationale (DAWN's two-segment system prompt vs strict local
 * Jinja templates) and the copy-on-write contract.
 *
 * CACHING NOTE: this front-loads the per-turn volatile block into one system block,
 * which busts native-OpenAI /v1/chat-completions automatic prefix caching cross-turn
 * (same latent gap the Responses path had). Not fixed here. If native-OpenAI CC
 * cross-turn caching is ever pursued, mirror the Responses fix — keep only the stable
 * segment up front and reposition the volatile block as a user message just before
 * the current question (see extract_stable_instructions / build_responses_input in
 * llm_openai_responses.c and docs/RESPONSES_CACHE_REORDER_PLAN.md). Anthropic (the
 * add_anthropic_cache path above) needs no such move — its cache_control breakpoints
 * make placement irrelevant. */
void llm_openai_merge_leading_system_messages(json_object *root) {
   if (!root) {
      return;
   }
   json_object *messages = NULL;
   if (!json_object_object_get_ex(root, "messages", &messages) ||
       !json_object_is_type(messages, json_type_array)) {
      return;
   }
   int msg_count = json_object_array_length(messages);

   /* Measure the leading run of system messages that carry plain-string content;
    * a non-string (e.g. already cache-wrapped) system message ends the run. */
   int run = 0;
   size_t total = 0;
   for (int i = 0; i < msg_count; i++) {
      json_object *msg = json_object_array_get_idx(messages, i);
      json_object *role_obj = NULL;
      json_object *content_obj = NULL;
      if (msg == NULL || !json_object_object_get_ex(msg, "role", &role_obj)) {
         break;
      }
      const char *role_str = json_object_get_string(role_obj);
      if (role_str == NULL || strcmp(role_str, "system") != 0) {
         break;
      }
      if (!json_object_object_get_ex(msg, "content", &content_obj) ||
          !json_object_is_type(content_obj, json_type_string)) {
         break;
      }
      total += strlen(json_object_get_string(content_obj));
      run++;
   }

   if (run < 2) {
      return; /* zero or one leading system message — nothing to merge */
   }

   /* Concatenate the run with "\n\n" separators into one system message. */
   total += (size_t)(run - 1) * 2 + 1; /* separators + NUL */
   char *merged = malloc(total);
   if (merged == NULL) {
      return; /* OOM: leave the request as-is (still valid, just un-merged) */
   }
   size_t off = 0;
   for (int i = 0; i < run; i++) {
      json_object *content_obj = NULL;
      json_object_object_get_ex(json_object_array_get_idx(messages, i), "content", &content_obj);
      const char *s = json_object_get_string(content_obj);
      if (i > 0) {
         merged[off++] = '\n';
         merged[off++] = '\n';
      }
      size_t len = strlen(s);
      memcpy(merged + off, s, len);
      off += len;
   }
   merged[off] = '\0';

   json_object *merged_msg = json_object_new_object();
   json_object_object_add(merged_msg, "role", json_object_new_string("system"));
   json_object_object_add(merged_msg, "content", json_object_new_string(merged));
   free(merged);

   /* COW: root.messages may ALIAS the session's canonical history (see
    * llm_openai_add_anthropic_cache).  Swap it for a request-private array whose
    * front is the merged system message and whose tail (indices run..end) stays
    * shared read-only. */
   json_object *private_msgs = json_object_new_array();
   if (private_msgs == NULL) {
      json_object_put(merged_msg);
      return;
   }
   json_object_array_add(private_msgs, merged_msg);
   for (int j = run; j < msg_count; j++) {
      json_object_array_add(private_msgs, json_object_get(json_object_array_get_idx(messages, j)));
   }
   json_object_object_add(root, "messages", private_msgs); /* releases old array ref */
}

void llm_openai_apply_system_prompt_caching(json_object *root,
                                            const char *model_name,
                                            const char *base_url) {
   if (llm_openai_anthropic_cache_applies(model_name, base_url)) {
      /* Keep the two-segment system split intact so the cache_control breakpoint
       * lands on the stable region only. */
      llm_openai_add_anthropic_cache(root, model_name, base_url);
   } else {
      /* Every other provider (native OpenAI, llama.cpp, Ollama): collapse the
       * split so strict chat templates don't reject the second system message. */
      llm_openai_merge_leading_system_messages(root);
   }
}
