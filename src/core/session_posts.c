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
 * Telling a turn what DAWN posted into its conversation from elsewhere (a
 * job's report or notice, a channel post).  Those rows are shown to the user
 * but never replayed to the model (conv_db_get_messages_for_llm leaves them
 * out), and a live session holding the conversation never reloads it: the
 * next turn tells them in its context instead, which replays with its
 * question.  Nothing earlier in the request changes, so caching and every
 * provider's message shapes are as they were.
 */

#include "core/session_posts.h"

#include <json-c/json.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_messages.h"
#include "core/session_history.h"
#include "core/session_manager.h"
#include "core/session_prefix.h"
#include "core/strbuf.h"
#include "llm/llm_context_text.h"
#include "llm/llm_history_kind.h"
#include "prompts.h"
#include "utils/string_utils.h"

static int64_t id_of(struct json_object *msg) {
   struct json_object *id = NULL;
   return json_object_object_get_ex(msg, "id", &id) ? json_object_get_int64(id) : 0;
}

/* Caller holds history_mutex.  The row posts are read after: the previous
 * question's (else the newest row other than this turn's question).
 * posts_told_id may raise it (posts an earlier turn already told). */
static int64_t floor_locked(struct json_object *hist, struct json_object *question) {
   const int64_t qid = id_of(question);
   int64_t prev_question = 0;
   int64_t newest = 0;
   const int len = (int)json_object_array_length(hist);
   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(hist, i);
      const int64_t id = id_of(msg);
      if (msg == question || id <= 0 || (qid > 0 && id >= qid)) {
         continue;
      }
      newest = id > newest ? id : newest;
      if (llm_history_is_question(msg) && id > prev_question) {
         prev_question = id;
      }
   }
   return prev_question > 0 ? prev_question : newest;
}

static bool held_locked(struct json_object *hist, int64_t id) {
   const int len = (int)json_object_array_length(hist);
   for (int i = 0; i < len; i++) {
      if (id_of(json_object_array_get_idx(hist, i)) == id) {
         return true;
      }
   }
   return false;
}

char *session_posts_take(session_t *session, int user_id) {
   if (!session || user_id <= 0) {
      return NULL;
   }
   /* Where the turn is, and from which row (no lock held across the read). */
   pthread_mutex_lock(&session->history_mutex);
   int64_t conv = 0;
   struct json_object *question = NULL;
   struct json_object *hist = session_prefix_turn_history_locked(session, &conv, &question);
   int64_t after = conv > 0 ? floor_locked(hist, question) : 0;
   if (conv > 0 && session->posts_told_conv == conv && session->posts_told_id > after) {
      after = session->posts_told_id;
   }
   pthread_mutex_unlock(&session->history_mutex);
   if (conv <= 0) {
      return NULL; /* no conversation yet, or a mixed history */
   }

   conv_posted_row_t rows[SESSION_POSTS_MAX];
   int n = 0;
   bool more = false;
   if (conv_db_posted_after(conv, user_id, after, SESSION_POST_TEXT_MAX, rows, SESSION_POSTS_MAX,
                            &n, &more) != AUTH_DB_SUCCESS ||
       n == 0) {
      return NULL;
   }

   strbuf_t sb;
   strbuf_init(&sb, 1024);
   strbuf_append(&sb, SESSION_POSTS_HEADER);
   int64_t newest = 0;
   pthread_mutex_lock(&session->history_mutex);
   int64_t now_conv = 0;
   hist = session_prefix_turn_history_locked(session, &now_conv, &question);
   int shown = 0;
   bool failed = false;
   if (now_conv == conv) { /* not replaced since (a sidebar load) */
      if (more) {
         strbuf_append(&sb, SESSION_POSTS_EARLIER); /* the newest are told */
      }
      for (int i = 0; i < n && !failed; i++) {
         newest = rows[i].id > newest ? rows[i].id : newest;
         if (held_locked(hist, rows[i].id)) {
            continue; /* this history already has it */
         }
         char *text = rows[i].content;
         const bool cut = strlen(text) > SESSION_POST_TEXT_MAX;
         if (cut) {
            text[SESSION_POST_TEXT_MAX] = '\0';
            utf8_trim_incomplete(text);
         }
         /* One line each: nothing in a post can start a line that reads as
          * another part of DAWN's context. */
         char *defused = llm_context_neutralize_line(text);
         failed = defused == NULL;
         if (!failed) {
            strbuf_appendf(&sb, "\n- %s%s", defused, cut ? SESSION_POST_CUT : "");
            shown++;
         }
         free(defused);
      }
      failed = failed || strbuf_oom(&sb);
      if (!failed) {
         /* Told by this turn: taken back with it (session_posts_untell_locked). */
         session->posts_told_prev = session->posts_told_conv == conv ? session->posts_told_id : 0;
         session->posts_told_conv = conv;
         session->posts_told_id = newest;
         session->posts_told_by = session->turn_active ? session->turn_owner_token : 0;
      }
   }
   pthread_mutex_unlock(&session->history_mutex);
   conv_db_posted_free(rows, n);
   if (shown == 0 || failed) {
      strbuf_free(&sb);
      return NULL;
   }
   return strbuf_steal(&sb);
}

void session_posts_untell_locked(session_t *session) {
   if (session->posts_told_by != 0 && session->posts_told_by == session->turn_owner_token) {
      session->posts_told_id = session->posts_told_prev;
      session->posts_told_by = 0;
   }
}
