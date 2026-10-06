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
 * Which unbound images a live session still holds.  See session_image_hold.h.
 */

#include "core/session_image_hold.h"

#include <json-c/json.h>
#include <pthread.h>
#include <string.h>

#include "core/image_rehydrate.h"
#include "core/job_manager.h"
#include "core/session_manager.h"
#include "llm/llm_history_rows.h"

/* Mark @p id among @p ids; how many it newly marked. */
static int mark(const char *id, const char *const ids[], int n, bool held[]) {
   int added = 0;
   for (int i = 0; id && i < n; i++) {
      if (!held[i] && strcmp(ids[i], id) == 0) {
         held[i] = true;
         added++;
      }
   }
   return added;
}

int session_images_named(struct json_object *json, const char *const ids[], int n, bool held[]) {
   int marked = 0;
   if (json_object_is_type(json, json_type_array)) {
      const size_t len = json_object_array_length(json);
      for (size_t i = 0; i < len; i++) {
         marked += session_images_named(json_object_array_get_idx(json, i), ids, n, held);
      }
      return marked;
   }
   if (!json_object_is_type(json, json_type_object)) {
      return 0;
   }
   json_object_object_foreach(json, key, val) {
      const bool row_images = strcmp(key, LLM_HISTORY_ROW_IMAGES_KEY) == 0 &&
                              json_object_is_type(val, json_type_array);
      if (strcmp(key, IMAGE_PART_ID_KEY) == 0 && json_object_is_type(val, json_type_string)) {
         marked += mark(json_object_get_string(val), ids, n, held); /* an image part's */
      } else if (row_images) {
         const size_t len = json_object_array_length(val); /* a saved row's */
         for (size_t i = 0; i < len; i++) {
            marked += mark(json_object_get_string(json_object_array_get_idx(val, i)), ids, n, held);
         }
      } else if (json_object_is_type(val, json_type_array) ||
                 json_object_is_type(val, json_type_object)) {
         marked += session_images_named(val, ids, n, held); /* content holding parts */
      }
   }
   return marked;
}

typedef struct {
   const char *const *ids;
   const int *owners;
   int n;
   bool *held;
} hold_ctx_t;

/* The images @p session's unsaved history names, and what a compaction took
 * out of it for the voice save: only its own user's (an image is held by its
 * owner's sessions alone). */
static void session_hold(session_t *session, void *p) {
   hold_ctx_t *c = p;
   const int user = session_effective_user_id(session);
   const char *ids[SESSION_IMAGES_HELD_MAX];
   bool held[SESSION_IMAGES_HELD_MAX] = { false };
   int at[SESSION_IMAGES_HELD_MAX];
   int n = 0;
   for (int i = 0; i < c->n && n < SESSION_IMAGES_HELD_MAX; i++) {
      if (!c->held[i] && user > 0 && c->owners[i] == user) {
         ids[n] = c->ids[i];
         at[n++] = i;
      }
   }
   if (n == 0) {
      return;
   }
   pthread_mutex_lock(&session->history_mutex);
   (void)session_images_named(session->conversation_history, ids, n, held);
   (void)session_images_named(session->compaction.voice_removed, ids, n, held);
   pthread_mutex_unlock(&session->history_mutex);
   for (int i = 0; i < n; i++) {
      c->held[at[i]] = c->held[at[i]] || held[i];
   }
}

void session_images_held(const char *const ids[], const int owners[], int n, bool held[]) {
   if (!ids || !owners || !held || n <= 0) {
      return;
   }
   hold_ctx_t c = { .ids = ids,
                    .owners = owners,
                    .n = n < SESSION_IMAGES_HELD_MAX ? n : SESSION_IMAGES_HELD_MAX,
                    .held = held };
   /* Disconnected sessions too: a satellite that dropped before its idle
    * save still holds its unsaved captures. */
   session_manager_for_each_session_any(session_hold, &c);
   job_manager_for_each_session(session_hold, &c);
}
