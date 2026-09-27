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
 * Satellite user mapping: whose speech a satellite session's history holds,
 * and moving it to another user without mixing two users' speech.
 */

#include "webui/satellite_remap.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "auth/auth_db.h"
#include "core/session_manager.h"
#include "core/turn_queue.h"
#include "llm/llm_command_parser.h"
#include "logging.h"
#include "utils/string_utils.h"

/* Whether mapping the session to @p new_user makes its speech someone else's
 * (unmapped = a guest's, 0). */
bool satellite_owner_changes(session_t *session, int new_user) {
   pthread_mutex_lock(&session->metrics_mutex);
   const int prev_user = session->metrics.user_id;
   pthread_mutex_unlock(&session->metrics_mutex);
   return (prev_user > 0 ? prev_user : 0) != (new_user > 0 ? new_user : 0);
}

/* Apply a mapping to the session: its user, and the prompt it starts with
 * (personalized for a mapped user) plus its room context. */
void satellite_apply_mapping(session_t *session,
                             int user_id,
                             const char *location,
                             const char *ha_area) {
#ifdef ENABLE_MULTI_CLIENT
   session_set_metrics_user(session, user_id);
#endif
   if (user_id > 0) {
      /* Phase 1f: SESSION_START builder boundary on satellite rebind — clear
       * dedup state so the next PER_TURN starts fresh against the new user's
       * persona/memory. */
      session_injected_set_clear(session);
      char *user_prompt = session_manager_build_system_prompt_string(user_id);
      if (user_prompt) {
         session_update_system_prompt(session, user_prompt);
         free(user_prompt);
      }
   }
   session_append_satellite_context(session, location, ha_area && ha_area[0] ? ha_area : NULL);
}

typedef struct {
   session_t *session;
   int user_id;
   char location[SATELLITE_LOCATION_MAX];
   char ha_area[SATELLITE_LOCATION_MAX];
} satellite_remap_t;

static void satellite_remap_free(void *arg) {
   satellite_remap_t *w = (satellite_remap_t *)arg;
   if (w) {
      session_release(w->session);
      free(w);
   }
}

/* The satellite now belongs to another user: save the previous user's
 * conversation (a new context starts), or start a new one when there is
 * nothing to save, so one history never holds two users' speech nor the
 * previous user's personalized prompt.  Then apply the new mapping.  Runs
 * behind any query in progress, off the WebSocket service thread. */
static void *satellite_remap_entry(void *arg) {
   satellite_remap_t *w = (satellite_remap_t *)arg;
   session_t *s = w->session;
   const uint32_t sid = s->session_id;
   if (!atomic_load(&s->being_destroyed)) {
      int64_t saved = 0;
      if (session_save_voice_conversation(s, &saved) != 0) {
         char *prompt = get_remote_command_prompt_dup();
         session_init_system_prompt(s, prompt ? prompt : "");
         free(prompt);
      }
      satellite_apply_mapping(s, w->user_id, w->location, w->ha_area);
      OLOG_INFO("Satellite: session %u now belongs to user %d%s", sid, w->user_id,
                saved > 0 ? " (previous user's conversation saved)" : "");
   }
   satellite_remap_free(w);
   turn_queue_turn_done(sid);
   return NULL;
}

static void satellite_remap_spawn(void *arg) {
   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   int ret = pthread_create(&thread, &attr, satellite_remap_entry, arg);
   pthread_attr_destroy(&attr);
   if (ret != 0) {
      satellite_remap_t *w = (satellite_remap_t *)arg;
      const uint32_t sid = w->session->session_id;
      OLOG_ERROR("Satellite: failed to start the user remap for session %u (%d)", sid, ret);
      satellite_remap_free(w);
      turn_queue_turn_done(sid);
   }
}

/* Queue the remap; when it can't be queued, drop the history instead of carrying
 * it over to the next user. */
void satellite_queue_remap(session_t *session,
                           int user_id,
                           const char *location,
                           const char *ha_area) {
   satellite_remap_t *w = calloc(1, sizeof(*w));
   if (w) {
      session_retain(session);
      w->session = session;
      w->user_id = user_id;
      safe_strscpy(w->location, location ? location : "");
      safe_strscpy(w->ha_area, ha_area ? ha_area : "");
      if (turn_queue_enqueue(session->session_id, TURN_SOURCE_BACKGROUND, w, satellite_remap_spawn,
                             satellite_remap_free) == TURN_QUEUE_OK) {
         return;
      }
      satellite_remap_free(w);
   }
   OLOG_WARNING("Satellite: could not queue the user remap for session %u; history discarded",
                session->session_id);
   char *prompt = get_remote_command_prompt_dup();
   session_init_system_prompt(session, prompt ? prompt : "");
   free(prompt);
   satellite_apply_mapping(session, user_id, location, ha_area);
}
