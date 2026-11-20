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
 * A session's live history follows its conversation: several sessions can
 * hold one conversation (two browsers, another client, a messaging channel,
 * a job), and each turn on it takes its lease first.  Holding it, the turn
 * brings its live history up to date with what the others wrote since it was
 * built: rows appended as a reload renders them, or the history built again
 * when anything a reload reads changed.  The point a history was built at is
 * recorded where it is loaded and where a turn holding the conversation ends.
 */

#include <json-c/json.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_conv_prefix.h"
#include "auth/auth_db_messages.h"
#include "core/conv_sync_point.h"
#include "core/session_manager.h"
#include "llm/llm_history_kind.h"
#include "logging.h"

static _Atomic(session_history_tail_loader_fn) s_tail_loader;

void session_set_history_tail_loader(session_history_tail_loader_fn loader) {
   atomic_store(&s_tail_loader, loader);
}

void session_replace_history_at(session_t *session,
                                struct json_object *history,
                                int64_t conv_id,
                                int user_id,
                                const conv_sync_point_t *at) {
   if (!session || !history) {
      json_object_put(history);
      return;
   }
   /* Read again after the load: the same point means the history is that
    * state (nothing it reads changed while it was built). */
   conv_sync_point_t after;
   const bool same = at && conv_id > 0 &&
                     conv_db_sync_point_get(conv_id, user_id, &after) == AUTH_DB_SUCCESS &&
                     conv_sync_point_equal(at, &after);
   pthread_mutex_lock(&session->history_mutex);
   session_install_history_locked(session, history, conv_id);
   if (same) {
      session->history_sync = *at;
      session->history_sync_conv = conv_id;
   }
   pthread_mutex_unlock(&session->history_mutex);
}

/* What the catch-up saw of the live history, and must find unchanged before
 * it changes it. */
typedef struct {
   struct json_object *live; /* a reference */
   int len;
   bool pinned; /* the turn runs on it */
   bool known;  /* was is the point it was built at */
   conv_sync_point_t was;
   int64_t newest; /* its newest row's id */
   bool question;  /* it holds a question (a summary is put in front of the first) */
   bool has_turns; /* it holds more than system messages and request context */
} catchup_view_t;

static void scan_history(struct json_object *hist, catchup_view_t *v) {
   for (int i = 0; i < v->len; i++) {
      struct json_object *msg = json_object_array_get_idx(hist, i);
      struct json_object *id = NULL;
      if (json_object_object_get_ex(msg, "id", &id) && json_object_get_int64(id) > v->newest) {
         v->newest = json_object_get_int64(id);
      }
      const bool context = llm_history_is_context(msg);
      v->question = v->question || (!context && llm_history_is_question(msg));
      v->has_turns = v->has_turns || (!context && !llm_history_role_is(msg, "system"));
   }
}

/* Caller holds history_mutex.  The running turn's own code, holding @p conv's
 * lease, with the live history holding @p conv and either pinned by the turn
 * with nothing of the turn's in it yet, or not pinned at all (a turn on its own
 * copy loaded it holding the lease). */
static bool turn_may_catch_up_locked(session_t *session, int64_t conv) {
   if (!session->turn_active || session_turn_token() != session->turn_owner_token ||
       session->turn_lease_conv != conv || !session->conversation_history ||
       atomic_load(&session->history_conversation_id) != conv) {
      return false;
   }
   if (session->turn_history == session->conversation_history) {
      return session->turn_appends == 0;
   }
   return session->turn_history == NULL;
}

static bool view_locked(session_t *session, int64_t conv, catchup_view_t *v) {
   memset(v, 0, sizeof(*v));
   if (!turn_may_catch_up_locked(session, conv)) {
      return false;
   }
   v->live = json_object_get(session->conversation_history);
   v->len = (int)json_object_array_length(v->live);
   v->pinned = session->turn_history == v->live;
   v->known = session->history_sync_conv == conv;
   v->was = session->history_sync;
   return true;
}

/* Caller holds history_mutex.  Nothing changed since view_locked(). */
static bool view_holds_locked(session_t *session, int64_t conv, const catchup_view_t *v) {
   return turn_may_catch_up_locked(session, conv) && session->conversation_history == v->live &&
          (int)json_object_array_length(v->live) == v->len &&
          (session->turn_history == v->live) == v->pinned &&
          (session->history_sync_conv == conv) == v->known &&
          (!v->known || conv_sync_point_equal(&session->history_sync, &v->was));
}

/* Record @p now for a history that already is that state. */
static void record_point(session_t *session,
                         int64_t conv,
                         const catchup_view_t *v,
                         const conv_sync_point_t *now) {
   pthread_mutex_lock(&session->history_mutex);
   if (view_holds_locked(session, conv, v)) {
      session->history_sync = *now;
      session->history_sync_conv = conv;
   }
   pthread_mutex_unlock(&session->history_mutex);
}

typedef enum {
   TAIL_DONE,
   TAIL_FAILED,
   TAIL_REBUILD
} tail_result_t;

/* What is in force now, when it changed: the record stored under @p hash
 * (*@p out NULL when the hash is empty: none is stored).  False when it can't
 * be read or doesn't parse. */
static bool stored_in_force(const char *hash, struct json_object **out) {
   *out = NULL;
   if (!hash[0]) {
      return true;
   }
   char *bytes = NULL;
   if (conv_db_prompt_blob_get(hash, &bytes) != AUTH_DB_SUCCESS) {
      return false;
   }
   *out = json_tokener_parse(bytes);
   free(bytes);
   const bool ok = json_object_is_type(*out, json_type_object);
   if (!ok) {
      json_object_put(*out);
      *out = NULL;
   }
   return ok;
}

/* The running turn goes on with a history its catch-up couldn't bring up to
 * date: its point is unknown at the turn's end, so the next turn builds it again. */
static void mark_stale(session_t *session) {
   pthread_mutex_lock(&session->history_mutex);
   if (session->turn_active && session_turn_token() == session->turn_owner_token) {
      session->turn_stale_history = true;
   }
   pthread_mutex_unlock(&session->history_mutex);
}

/* Append the rows after the recorded point, as a reload renders them, and
 * take on what is in force. */
static tail_result_t append_tail(session_t *session,
                                 int64_t conv,
                                 int user,
                                 const catchup_view_t *v,
                                 const conv_sync_point_t *now) {
   session_history_tail_loader_fn tail_loader = atomic_load(&s_tail_loader);
   bool rebuild = false;
   struct json_object *tail = tail_loader(conv, user, v->was.max_id, &rebuild);
   if (!tail) {
      return rebuild ? TAIL_REBUILD : TAIL_FAILED;
   }
   const bool in_force_moved = strcmp(v->was.in_force_hash, now->in_force_hash) != 0;
   struct json_object *in_force = NULL;
   if (in_force_moved && !stored_in_force(now->in_force_hash, &in_force)) {
      json_object_put(tail);
      return TAIL_REBUILD;
   }
   /* The rows read are the point's only if nothing changed while reading. */
   conv_sync_point_t after;
   if (conv_db_sync_point_get(conv, user, &after) != AUTH_DB_SUCCESS ||
       !conv_sync_point_equal(now, &after)) {
      json_object_put(in_force);
      json_object_put(tail);
      return TAIL_REBUILD;
   }
   const int n = (int)json_object_array_length(tail);
   tail_result_t result = TAIL_DONE;
   pthread_mutex_lock(&session->history_mutex);
   struct json_object *prefix = llm_history_prefix(v->live);
   if (!view_holds_locked(session, conv, v)) {
      /* Replaced meanwhile: its loader recorded its own point. */
   } else if (in_force_moved && in_force && !prefix) {
      result = TAIL_REBUILD; /* no frozen prompt to carry it: built again */
   } else {
      for (int i = 0; i < n; i++) {
         json_object_array_add(v->live, json_object_get(json_object_array_get_idx(tail, i)));
      }
      if (in_force_moved && prefix) {
         if (in_force) {
            json_object_object_add(prefix, LLM_HISTORY_IN_FORCE_KEY, in_force);
            in_force = NULL;
         } else {
            json_object_object_del(prefix, LLM_HISTORY_IN_FORCE_KEY);
         }
      }
      session->history_sync = *now;
      session->history_sync_conv = conv;
      OLOG_INFO("Session %u: conversation %lld: %d message(s) from elsewhere appended",
                session->session_id, (long long)conv, n);
   }
   pthread_mutex_unlock(&session->history_mutex);
   json_object_put(in_force);
   json_object_put(tail);
   return result;
}

/* Build the live history again as a restore builds it, with the point read
 * before the load recorded when it still stands after. */
static void rebuild_history(session_t *session,
                            int64_t conv,
                            int user,
                            const catchup_view_t *v,
                            const conv_sync_point_t *now) {
   session_history_loader_fn loader = session_history_loader_get();
   /* The session keeps its own LLM settings (the loaded ones are unused): it was
    * already on this conversation, under the settings its user chose. */
   session_llm_config_t cfg;
   struct json_object *hist = loader(user, conv, &cfg);
   if (!hist) {
      OLOG_WARNING("Session %u: conversation %lld changed elsewhere and could not be reloaded; "
                   "the turn runs on the history it has",
                   session->session_id, (long long)conv);
      mark_stale(session);
      return;
   }
   conv_sync_point_t after;
   const bool same = conv_db_sync_point_get(conv, user, &after) == AUTH_DB_SUCCESS &&
                     conv_sync_point_equal(now, &after);
   const int n = (int)json_object_array_length(hist);
   pthread_mutex_lock(&session->history_mutex);
   if (!view_holds_locked(session, conv, v)) {
      pthread_mutex_unlock(&session->history_mutex);
      json_object_put(hist); /* replaced meanwhile: its loader recorded its own point */
      return;
   }
   if (v->pinned) {
      /* The turn moves onto the new history (it has written nothing yet). */
      json_object_put(session->turn_history);
      session->turn_history = NULL;
   }
   session_install_history_locked(session, hist, conv);
   if (v->pinned) {
      session->turn_history = json_object_get(session->conversation_history);
      session->turn_pin_conv = conv;
      session->turn_appends = 0;
   }
   if (same) {
      session->history_sync = *now;
      session->history_sync_conv = conv;
   }
   pthread_mutex_unlock(&session->history_mutex);
   OLOG_INFO("Session %u: conversation %lld: history rebuilt (%s; %d messages)",
             session->session_id, (long long)conv,
             v->known ? "changed elsewhere" : "its state unknown", n);
}

void session_catch_up(session_t *session, int64_t conv_id, int user_id) {
   if (!session || conv_id <= 0 || user_id <= 0 || !session_history_loader_get() ||
       !atomic_load(&s_tail_loader)) {
      return;
   }
   catchup_view_t v;
   pthread_mutex_lock(&session->history_mutex);
   const bool may = view_locked(session, conv_id, &v);
   pthread_mutex_unlock(&session->history_mutex);
   if (!may) {
      return;
   }
   conv_sync_point_t now;
   if (conv_db_sync_point_get(conv_id, user_id, &now) != AUTH_DB_SUCCESS) {
      OLOG_WARNING("Session %u: conversation %lld's state could not be read; the turn runs on "
                   "the history it has",
                   session->session_id, (long long)conv_id);
      mark_stale(session);
      session_put_history(session, v.live);
      return;
   }
   conv_sync_change_t change = v.known ? conv_sync_point_compare(&v.was, &now) : CONV_SYNC_REBUILD;
   if (change == CONV_SYNC_SAME) {
      session_put_history(session, v.live); /* the common turn: nothing to look at */
      return;
   }
   /* What the history holds, read only now that it may change. */
   pthread_mutex_lock(&session->history_mutex);
   const bool holds = view_holds_locked(session, conv_id, &v);
   if (holds) {
      scan_history(v.live, &v);
   }
   pthread_mutex_unlock(&session->history_mutex);
   if (!holds) {
      session_put_history(session, v.live); /* replaced meanwhile: its loader decided */
      return;
   }
   if (!v.known && !v.has_turns && now.max_id == 0) {
      change = CONV_SYNC_SAME; /* nothing written to it yet: this history is its state */
      record_point(session, conv_id, &v, &now);
   }
   /* A tail goes after what the history has: never when it already holds a
    * newer row than its point (built from a later state), nor when a summary
    * has no question to go in front of yet (a reload puts it on the tail's). */
   if (change == CONV_SYNC_TAIL &&
       (v.newest > v.was.max_id || (now.watermark > 0 && !v.question))) {
      change = CONV_SYNC_REBUILD;
   }
   if (change == CONV_SYNC_TAIL) {
      const tail_result_t r = append_tail(session, conv_id, user_id, &v, &now);
      if (r == TAIL_FAILED) {
         OLOG_WARNING("Session %u: conversation %lld's new rows could not be read; the turn "
                      "runs on the history it has",
                      session->session_id, (long long)conv_id);
         mark_stale(session);
      }
      change = r == TAIL_REBUILD ? CONV_SYNC_REBUILD : CONV_SYNC_SAME;
   }
   if (change == CONV_SYNC_REBUILD) {
      rebuild_history(session, conv_id, user_id, &v, &now);
   }
   session_put_history(session, v.live);
}
