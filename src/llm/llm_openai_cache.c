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
 * A chat-completions request's leading system messages, merged into one.  See
 * llm_openai_cache.h for the rationale and the copy-on-write contract.
 */

#include "llm/llm_openai_cache.h"

#include <stdlib.h>
#include <string.h>

/* Collapse the leading run of plain-string system messages into one.  See the
 * header for the rationale and the copy-on-write contract. */
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
    * a non-string (e.g. a content-part array) system message ends the run. */
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

   /* COW: root.messages may ALIAS the session's canonical history.  Swap it for a request-private
    * array whose front is the merged system message and whose tail (indices run..end) stays shared
    * read-only. */
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
