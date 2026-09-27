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
 * Session conversation history: appends, id stamping, clear/replace, which DB
 * conversation the history holds, and the running turn's own (pinned) history.
 */

#include <json-c/json.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include "core/session_manager.h"
#include "dawn_error.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "utils/string_utils.h"

/* Caller holds history_mutex.  The array a turn-owned append/stamp/rebuild
 * works on: the pinned turn history, or the live history when none is pinned. */
static struct json_object *turn_target_locked(session_t *session) {
   return session->turn_history ? session->turn_history : session->conversation_history;
}

/* Caller holds history_mutex.  The live history, recreated if it is missing or
 * corrupt.  NULL only on allocation failure. */
static struct json_object *live_history_locked(session_t *session) {
   if (!session->conversation_history ||
       !json_object_is_type(session->conversation_history, json_type_array)) {
      OLOG_ERROR("Session %u: conversation_history is %s (expected array), recreating",
                 session->session_id,
                 session->conversation_history
                     ? json_type_to_name(json_object_get_type(session->conversation_history))
                     : "NULL");
      if (session->conversation_history) {
         json_object_put(session->conversation_history);
      }
      session->conversation_history = json_object_new_array();
   }
   return session->conversation_history;
}

/* Caller holds history_mutex.  Whether the caller is the running turn's own
 * code: its thread, or a tool thread carrying its token. */
static bool turn_is_caller_locked(const session_t *session) {
   const uint64_t token = session_turn_token();
   return session->turn_active && token != 0 && token == session->turn_owner_token;
}

/* Caller holds history_mutex.  Move the waiting facts @p token saved (0 = those
 * of ended turns) into @p out, from @p n already there; returns the new count. */
static int take_facts_locked(session_t *session,
                             uint64_t token,
                             session_fact_source_t out[SESSION_PENDING_FACT_SOURCES_MAX],
                             int n) {
   int keep = 0;
   for (int i = 0; i < session->pending_fact_source_count; i++) {
      const session_fact_source_t f = session->pending_fact_sources[i];
      if (f.turn_token == token && n < SESSION_PENDING_FACT_SOURCES_MAX) {
         out[n++] = f;
      } else {
         session->pending_fact_sources[keep++] = f;
      }
   }
   session->pending_fact_source_count = keep;
   return n;
}

/* Caller holds history_mutex.  The live history's context is being discarded
 * (cleared, or replaced by another conversation's): the facts ended turns left
 * waiting for the conversation it would have become will not get one.  A
 * running turn's facts stay with the turn. */
static void drop_fact_sources_locked(session_t *session) {
   session_fact_source_t dropped[SESSION_PENDING_FACT_SOURCES_MAX];
   (void)take_facts_locked(session, 0, dropped, 0);
}

/* Caller holds history_mutex.  Let go of a history message ref.  While a turn
 * runs, only its own code may (it reads its messages without the lock, and
 * json-c reference counts aren't atomic): another thread's is parked for the
 * turn to release at its end. */
static void release_msg_locked(session_t *session, struct json_object *msg) {
   if (!msg) {
      return;
   }
   if (!session->turn_active || turn_is_caller_locked(session)) {
      json_object_put(msg);
   } else if (session->parked_msg_count < SESSION_PARKED_MSGS_MAX) {
      session->parked_msgs[session->parked_msg_count++] = msg;
   } else {
      /* Kept rather than released under a reading thread: a leak, never a
       * use-after-free. */
      OLOG_WARNING("Session %u: leaked a message reference (no room to park it)",
                   session->session_id);
   }
}

/* Caller holds history_mutex.  Forget an ended turn's unsaved exchange: it
 * belonged to the context being replaced. */
static void drop_unclaimed_locked(session_t *session) {
   free(session->unclaimed_user);
   free(session->unclaimed_reply);
   session->unclaimed_user = NULL;
   session->unclaimed_reply = NULL;
   release_msg_locked(session, session->unclaimed_user_msg);
   release_msg_locked(session, session->unclaimed_reply_msg);
   session->unclaimed_user_msg = NULL;
   session->unclaimed_reply_msg = NULL;
}


/* Caller holds history_mutex.  Where a turn's message goes, and whether an
 * assistant reply should also be mirrored into the live history: when the
 * session reloaded the turn's own conversation mid-turn (reconnect restore), the
 * reply belongs in that fresh copy too, or the next turn would lack it. */
static struct json_object *turn_append_target_locked(session_t *session,
                                                     const char *role,
                                                     struct json_object **mirror_out) {
   *mirror_out = NULL;
   struct json_object *live = live_history_locked(session);
   if (!session->turn_history || session->turn_history == live) {
      return live;
   }
   if (strcmp(role, "assistant") == 0 && session->turn_history_conv > 0 &&
       atomic_load(&session->history_conversation_id) == session->turn_history_conv) {
      *mirror_out = live;
   }
   return session->turn_history;
}

bool session_turn_defers_writes_locked(const session_t *session) {
   return session->turn_active && session->turn_history &&
          session->turn_history == session->conversation_history &&
          session_turn_token() != session->turn_owner_token;
}

/* Caller holds history_mutex.  Append @p message (ownership taken) to @p target,
 * and a copy of it to @p mirror if any: no message object is ever in two
 * arrays, since json-c reference counts aren't atomic and each array's messages
 * are touched by a different thread.  A turn's reply is remembered (both
 * copies) so its row id is stamped wherever it sits. */
static void append_message_locked(session_t *session,
                                  struct json_object *target,
                                  struct json_object *mirror,
                                  struct json_object *message,
                                  const char *role,
                                  bool turn) {
   struct json_object *copy = NULL;
   if (mirror && json_object_deep_copy(message, &copy, NULL) == 0 && copy) {
      json_object_array_add(mirror, copy);
   }
   json_object_array_add(target, message);
   if (!turn) {
      return;
   }
   session->turn_appends++;
   if (session->turn_active && strcmp(role, "user") == 0) {
      if (session->turn_user_msg) {
         json_object_put(session->turn_user_msg);
      }
      session->turn_user_msg = json_object_get(message);
   }
   if (session->turn_active && strcmp(role, "assistant") == 0) {
      if (session->turn_reply) {
         json_object_put(session->turn_reply);
      }
      if (session->turn_reply_mirror) {
         json_object_put(session->turn_reply_mirror);
      }
      session->turn_reply = json_object_get(message);
      session->turn_reply_mirror = copy ? json_object_get(copy) : NULL;
   }
}

static bool add_message_impl(session_t *session, const char *role, const char *content, bool turn) {
   if (!session || !role || !content) {
      return false;
   }

   pthread_mutex_lock(&session->history_mutex);

   if (turn && session->turn_active && !turn_is_caller_locked(session)) {
      /* A turn's message from a thread that isn't the running turn's (one
       * replaced by a later turn): the history is that turn's now. */
      pthread_mutex_unlock(&session->history_mutex);
      OLOG_ERROR("Session %u: dropped a %s message from a turn that is no longer running",
                 session->session_id, role);
      return false;
   }

   struct json_object *mirror = NULL;
   struct json_object *target = turn ? turn_append_target_locked(session, role, &mirror)
                                     : live_history_locked(session);
   if (!target) {
      pthread_mutex_unlock(&session->history_mutex);
      OLOG_ERROR("Session %u: Failed to recreate conversation history", session->session_id);
      return false;
   }
   bool into_live = (target == session->conversation_history);

   struct json_object *message = json_object_new_object();
   if (!message) {
      pthread_mutex_unlock(&session->history_mutex);
      OLOG_ERROR("Failed to create message object");
      return false;
   }

   json_object_object_add(message, "role", json_object_new_string(role));
   json_object_object_add(message, "content", json_object_new_string(content));

   if (!turn && session_turn_defers_writes_locked(session)) {
      /* Only the turn writes the history it is serializing; another thread has
       * no business appending to a conversation mid-turn (device events go
       * through session_post_notice).  A caller reaching here is a bug. */
      pthread_mutex_unlock(&session->history_mutex);
      json_object_put(message);
      OLOG_ERROR("Session %u: dropped a %s message written from outside the running turn",
                 session->session_id, role);
      return false;
   }

   append_message_locked(session, target, mirror, message, role, turn);

   int count = json_object_array_length(target);
   OLOG_INFO("Session %u: Added %s message to %shistory (now %d messages)", session->session_id,
             role, into_live ? "" : "turn ", count);

   pthread_mutex_unlock(&session->history_mutex);
   return true;
}

int session_rollback_turn(session_t *session) {
   if (!session) {
      return 0;
   }
   pthread_mutex_lock(&session->history_mutex);
   if (!turn_is_caller_locked(session)) {
      pthread_mutex_unlock(&session->history_mutex);
      OLOG_WARNING("Session %u: only the running turn can take its messages back",
                   session->session_id);
      return 0;
   }
   struct json_object *hist = turn_target_locked(session);
   const int len = hist ? (int)json_object_array_length(hist) : 0;
   /* From the turn's own question to the end: only this turn appends to its
    * history.  Found by identity, not by counting back, since a compaction
    * mid-turn may have rewritten what came before. */
   int from = -1;
   for (int i = len - 1; session->turn_user_msg && i >= 0; i--) {
      if (json_object_array_get_idx(hist, i) == session->turn_user_msg) {
         from = i;
         break;
      }
   }
   int removed = 0;
   if (from >= 0) {
      removed = len - from;
      json_object_array_del_idx(hist, from, removed);
   } else if (session->turn_user_msg) {
      OLOG_WARNING("Session %u: the turn's question is no longer in its history; nothing "
                   "taken back",
                   session->session_id);
   }
   session->turn_appends = 0;
   pthread_mutex_unlock(&session->history_mutex);
   return removed;
}

bool session_stop_turn(session_t *session, const char *note) {
   if (!session || !note) {
      return false;
   }
   pthread_mutex_lock(&session->history_mutex);
   if (!turn_is_caller_locked(session)) {
      pthread_mutex_unlock(&session->history_mutex);
      OLOG_WARNING("Session %u: only the running turn can stop itself", session->session_id);
      return false;
   }
   struct json_object *hist = turn_target_locked(session);
   const int len = hist ? (int)json_object_array_length(hist) : 0;
   /* The turn's question, found by identity (see session_rollback_turn); what
    * followed it (tool calls, results) goes, the question stays. */
   int at = -1;
   for (int i = len - 1; session->turn_user_msg && i >= 0; i--) {
      if (json_object_array_get_idx(hist, i) == session->turn_user_msg) {
         at = i;
         break;
      }
   }
   if (at < 0) {
      pthread_mutex_unlock(&session->history_mutex);
      return false; /* no question to keep (compacted away): the caller rolls back */
   }
   if (len - at - 1 > 0) {
      json_object_array_del_idx(hist, at + 1, len - at - 1);
   }
   session->turn_appends = 1; /* the question */
   pthread_mutex_unlock(&session->history_mutex);
   return session_add_turn_message(session, "assistant", note);
}

void session_add_message(session_t *session, const char *role, const char *content) {
   (void)add_message_impl(session, role, content, false);
}

bool session_add_turn_message(session_t *session, const char *role, const char *content) {
   return add_message_impl(session, role, content, true);
}


static pthread_once_t s_notices_header_once = PTHREAD_ONCE_INIT;
static char s_notices_header[80];

static void init_notices_header(void) {
   uint32_t tag = 0;
   if (getrandom(&tag, sizeof(tag), 0) != (ssize_t)sizeof(tag)) {
      tag = (uint32_t)time(NULL) ^ ((uint32_t)getpid() << 16);
   }
   snprintf(s_notices_header, sizeof(s_notices_header),
            "--- Recent device events [%08x] (oldest first) ---", tag);
}

const char *session_notices_header(void) {
   pthread_once(&s_notices_header_once, init_notices_header);
   return s_notices_header;
}

void session_post_notice_for(session_t *session, const char *text, int user_id) {
   if (!session || !text || text[0] == '\0') {
      return;
   }
   pthread_mutex_lock(&session->history_mutex);
   if (session->notice_count == SESSION_NOTICES_MAX) {
      /* Full: make room by dropping the oldest, preferring one of another
       * user's (a previous owner's, hidden from this surface now) over the
       * notices it still shows. */
      int drop = 0;
      if (user_id > 0) {
         for (int i = 0; i < session->notice_count; i++) {
            const int owner = session->notices[i].user_id;
            if (owner > 0 && owner != user_id) {
               drop = i;
               break;
            }
         }
      }
      memmove(&session->notices[drop], &session->notices[drop + 1],
              (size_t)(SESSION_NOTICES_MAX - 1 - drop) * sizeof(session->notices[0]));
      session->notice_count--;
   }
   session_notice_t *n = &session->notices[session->notice_count++];
   safe_strncpy(n->text, text, sizeof(n->text));
   utf8_trim_incomplete(n->text); /* a cut can split a character */
   /* One event, one line: its text can't start lines that read as more events. */
   for (char *c = n->text; *c; c++) {
      if (*c == '\n' || *c == '\r' || *c == '\t') {
         *c = ' ';
      }
   }
   n->at = time(NULL);
   n->user_id = user_id > 0 ? user_id : 0;
   pthread_mutex_unlock(&session->history_mutex);
}

void session_post_notice(session_t *session, const char *text) {
   session_post_notice_for(session, text, 0);
}

char *session_render_notices_locked(session_t *session, int viewer_user_id) {
   const time_t now = time(NULL);
   int keep = 0;
   for (int i = 0; i < session->notice_count; i++) {
      if (now - session->notices[i].at < SESSION_NOTICE_TTL_SEC) {
         if (keep != i) {
            session->notices[keep] = session->notices[i];
         }
         keep++;
      }
   }
   session->notice_count = keep;

   /* Only the household's events and this surface's user's.  Another user's are
    * kept, not dropped: the viewer was read before the lock, so a surface that
    * just changed hands may render once with its previous owner, and dropping
    * would lose the new owner's notice for good. */
   const char *header = session_notices_header();
   size_t cap = strlen(header) + 1;
   int shown = 0;
   for (int i = 0; i < keep; i++) {
      const int owner = session->notices[i].user_id;
      if (owner == 0 || owner == viewer_user_id) {
         cap += strlen(session->notices[i].text) + 32; /* "\n- (NNN min ago) " */
         shown++;
      }
   }
   if (shown == 0) {
      return strdup("");
   }
   char *out = malloc(cap);
   if (!out) {
      return NULL;
   }
   size_t len = (size_t)snprintf(out, cap, "%s", header);
   for (int i = 0; i < keep && len < cap; i++) {
      const int owner = session->notices[i].user_id;
      if (owner != 0 && owner != viewer_user_id) {
         continue;
      }
      const long mins = (long)((now - session->notices[i].at) / 60);
      if (mins < 1) {
         len += (size_t)snprintf(out + len, cap - len, "\n- (just now) %s",
                                 session->notices[i].text);
      } else {
         len += (size_t)snprintf(out + len, cap - len, "\n- (%ld min ago) %s", mins,
                                 session->notices[i].text);
      }
   }
   return out;
}

void session_stamp_last_message_id(session_t *session, const char *role, int64_t msg_id) {
   if (!session || !role || msg_id <= 0)
      return;

   pthread_mutex_lock(&session->history_mutex);

   /* The row belongs to the running turn: stamp its history (the pinned one if
    * the user opened another conversation mid-turn). */
   struct json_object *hist = turn_target_locked(session);
   if (!hist)
      goto unlock;

   /* The turn's own reply is stamped where it is, whatever was appended after
    * it.  Without one, an assistant row must be the final entry: when the reply never reached the
    * history (e.g. finalized after a cancel), scanning back would stamp an OLDER reply with this
    * id. */
   if (strcmp(role, "assistant") == 0 && session->turn_active && session->turn_reply) {
      if (!json_object_object_get_ex(session->turn_reply, "id", NULL)) {
         json_object_object_add(session->turn_reply, "id", json_object_new_int64(msg_id));
      }
      if (session->turn_reply_mirror &&
          !json_object_object_get_ex(session->turn_reply_mirror, "id", NULL)) {
         json_object_object_add(session->turn_reply_mirror, "id", json_object_new_int64(msg_id));
      }
      goto unlock;
   }
   int len = (int)json_object_array_length(hist);
   const int lowest = (strcmp(role, "assistant") == 0) ? len - 1 : 0;
   for (int i = len - 1; i >= lowest && i >= 0; i--) {
      struct json_object *entry = json_object_array_get_idx(hist, i);
      if (!entry)
         continue;
      struct json_object *role_obj;
      if (!json_object_object_get_ex(entry, "role", &role_obj))
         continue;
      if (strcmp(json_object_get_string(role_obj), role) != 0)
         continue;
      /* Skip entries that already have an ID stamped */
      if (json_object_object_get_ex(entry, "id", NULL))
         continue;
      json_object_object_add(entry, "id", json_object_new_int64(msg_id));
      break;
   }

   /* Mirror the user-msg id into a session field so the
    * WebUI prompt builder can read it without parsing JSON history.
    * Other roles (assistant / tool / system) don't drive turn_id —
    * focus injection rebuilds on USER turns only. */
   if (strcmp(role, "user") == 0)
      session->last_user_msg_id = msg_id;

unlock:
   pthread_mutex_unlock(&session->history_mutex);
}

int64_t session_get_last_user_msg_id(session_t *session) {
   if (!session)
      return 0;
   pthread_mutex_lock(&session->history_mutex);
   int64_t id = session->last_user_msg_id;
   pthread_mutex_unlock(&session->history_mutex);
   return id;
}

static bool add_message_with_images_impl(session_t *session,
                                         const char *role,
                                         const char *text,
                                         const char *const *vision_images,
                                         int vision_image_count,
                                         bool turn) {
   if (!session || !role || !text) {
      return false;
   }

   /* No images? Fall back to simple text message */
   if (!vision_images || vision_image_count <= 0) {
      return add_message_impl(session, role, text, turn);
   }

   pthread_mutex_lock(&session->history_mutex);

   if (turn && session->turn_active && !turn_is_caller_locked(session)) {
      /* A turn's message from a thread that isn't the running turn's (one
       * replaced by a later turn): the history is that turn's now. */
      pthread_mutex_unlock(&session->history_mutex);
      OLOG_ERROR("Session %u: dropped a %s message from a turn that is no longer running",
                 session->session_id, role);
      return false;
   }

   struct json_object *mirror = NULL;
   struct json_object *target = turn ? turn_append_target_locked(session, role, &mirror)
                                     : live_history_locked(session);
   if (!target) {
      pthread_mutex_unlock(&session->history_mutex);
      return false;
   }
   bool into_live = (target == session->conversation_history);

   struct json_object *message = json_object_new_object();
   if (!message) {
      pthread_mutex_unlock(&session->history_mutex);
      OLOG_ERROR("Failed to create message object");
      return false;
   }

   json_object_object_add(message, "role", json_object_new_string(role));

   /* Build multi-part content array in OpenAI format */
   struct json_object *content = json_object_new_array();
   if (!content) {
      json_object_put(message);
      pthread_mutex_unlock(&session->history_mutex);
      OLOG_ERROR("Failed to create content array");
      return false;
   }

   /* Add text part first.  INVARIANT: this text part must always precede any
    * image part, so a user-attached image message never collapses to a
    * lone-image content array — llm_tools.c's is_capture_image_message()
    * relies on that shape (single-element array, image-only) to distinguish
    * ambient tool captures (eviction-eligible) from images the user
    * deliberately attached (never evicted). Keep this ordering if this
    * function is ever changed to allow an empty/absent text argument. */
   struct json_object *text_part = json_object_new_object();
   json_object_object_add(text_part, "type", json_object_new_string("text"));
   json_object_object_add(text_part, "text", json_object_new_string(text));
   json_object_array_add(content, text_part);

   /* Add image parts */
   for (int i = 0; i < vision_image_count; i++) {
      if (!vision_images[i] || vision_images[i][0] == '\0') {
         continue;
      }

      struct json_object *image_part = json_object_new_object();
      json_object_object_add(image_part, "type", json_object_new_string("image_url"));

      struct json_object *image_url = json_object_new_object();

      /* Build data URI: data:image/jpeg;base64,<data> */
      size_t data_len = strlen(vision_images[i]);
      size_t uri_len = 23 + data_len + 1; /* "data:image/jpeg;base64," + data + null */
      char *data_uri = malloc(uri_len);
      if (data_uri) {
         snprintf(data_uri, uri_len, "data:image/jpeg;base64,%s", vision_images[i]);
         json_object_object_add(image_url, "url", json_object_new_string(data_uri));
         free(data_uri);
      }

      json_object_object_add(image_part, "image_url", image_url);
      json_object_array_add(content, image_part);
   }

   json_object_object_add(message, "content", content);
   append_message_locked(session, target, mirror, message, role, turn);

   OLOG_INFO("Session %u: Added message with %d images to %shistory", session->session_id,
             vision_image_count, into_live ? "" : "turn ");

   pthread_mutex_unlock(&session->history_mutex);
   return true;
}

void session_add_message_with_images(session_t *session,
                                     const char *role,
                                     const char *text,
                                     const char *const *vision_images,
                                     int vision_image_count) {
   (void)add_message_with_images_impl(session, role, text, vision_images, vision_image_count,
                                      false);
}

bool session_add_turn_message_with_images(session_t *session,
                                          const char *role,
                                          const char *text,
                                          const char *const *vision_images,
                                          int vision_image_count) {
   return add_message_with_images_impl(session, role, text, vision_images, vision_image_count,
                                       true);
}

struct json_object *session_get_history(session_t *session) {
   if (!session) {
      return NULL;
   }

   pthread_mutex_lock(&session->history_mutex);
   struct json_object *history = session->conversation_history;

   // Increment reference count so caller can use it
   json_object_get(history);

   pthread_mutex_unlock(&session->history_mutex);

   return history;
}

struct json_object *session_snapshot_history(session_t *session,
                                             int64_t *conv_out,
                                             int *count_out) {
   if (conv_out) {
      *conv_out = 0;
   }
   if (count_out) {
      *count_out = 0;
   }
   if (!session) {
      return NULL;
   }
   /* Deep-copied under the lock: a running turn appends to (and stamps ids into)
    * this same array, and json-c containers are not safe to read while resized. */
   pthread_mutex_lock(&session->history_mutex);
   struct json_object *copy = NULL;
   if (session->conversation_history) {
      copy = llm_history_strip_provider_state(session->conversation_history);
   }
   if (conv_out) {
      *conv_out = atomic_load(&session->history_conversation_id);
   }
   if (count_out && copy) {
      *count_out = (int)json_object_array_length(copy);
   }
   pthread_mutex_unlock(&session->history_mutex);
   return copy;
}

void session_new_context_locked(session_t *session, const char *system_prompt) {
   if (session->turn_active && session->turn_history &&
       session->turn_history == session->conversation_history) {
      /* The running turn keeps the history it is on, but that is no longer the
       * context the user is in: it isn't adopted back when it ends, nor given
       * the conversation created for the new one. */
      session->turn_pin_conv = 0;
      session->turn_awaits_conversation = false;
      session->turn_context_reset = true;
   }
   if (session->conversation_history) {
      json_object_put(session->conversation_history);
   }
   session->conversation_history = json_object_new_array();
   if (!session->conversation_history) {
      OLOG_ERROR("Failed to create new conversation history array");
   } else if (system_prompt) {
      struct json_object *system_message = json_object_new_object();
      if (system_message) {
         json_object_object_add(system_message, "role", json_object_new_string("system"));
         json_object_object_add(system_message, "content", json_object_new_string(system_prompt));
         json_object_array_add(session->conversation_history, system_message);
      }
   }
   atomic_store(&session->history_conversation_id, 0);
   /* The new context has none of the old one's visual guidelines or focus
    * items, nor its unsaved exchange or waiting facts. */
   session->visual_modules_loaded[0] = '\0';
   memset(&session->injected_set, 0, sizeof(session->injected_set));
   if (!session->turn_active) {
      session->turn_appends = 0;
   }
   drop_unclaimed_locked(session);
   drop_fact_sources_locked(session);
}

void session_clear_history(session_t *session) {
   if (!session) {
      return;
   }
   pthread_mutex_lock(&session->history_mutex);
   session_new_context_locked(session, NULL);
   pthread_mutex_unlock(&session->history_mutex);
}

void session_replace_history(session_t *session, struct json_object *history, int64_t conv_id) {
   if (!session || !history) {
      return;
   }
   pthread_mutex_lock(&session->history_mutex);
   if (session->conversation_history) {
      json_object_put(session->conversation_history);
   }
   session->conversation_history = history;
   drop_unclaimed_locked(session); /* a new context: that exchange isn't this one's */
   drop_fact_sources_locked(session);
   /* The new context has none of the visual guidelines the old one loaded, nor
    * the focus items injected into it. */
   session->visual_modules_loaded[0] = '\0';
   memset(&session->injected_set, 0, sizeof(session->injected_set));
   atomic_store(&session->history_conversation_id, conv_id > 0 ? conv_id : 0);
   pthread_mutex_unlock(&session->history_mutex);
}

void session_bind_history_conversation(session_t *session, int64_t conv_id) {
   if (!session) {
      return;
   }
   pthread_mutex_lock(&session->history_mutex);
   atomic_store(&session->history_conversation_id, conv_id > 0 ? conv_id : 0);
   pthread_mutex_unlock(&session->history_mutex);
}

static _Atomic(session_history_loader_fn) s_history_loader;

void session_set_history_loader(session_history_loader_fn loader) {
   atomic_store(&s_history_loader, loader);
}

/* True when @p hist holds nothing but system messages and @p own_appends
 * messages the running turn added itself: no conversation's turns yet, so any
 * conversation may adopt it without mixing. */
static bool history_is_fresh(struct json_object *hist, int own_appends) {
   if (!hist) {
      return true;
   }
   int others = 0;
   const int len = (int)json_object_array_length(hist);
   for (int i = 0; i < len; i++) {
      struct json_object *entry = json_object_array_get_idx(hist, i);
      struct json_object *role = NULL;
      if (entry && json_object_object_get_ex(entry, "role", &role) &&
          strcmp(json_object_get_string(role), "system") == 0) {
         continue;
      }
      if (++others > own_appends) {
         return false; /* a long history answers after its first few messages */
      }
   }
   return true;
}

/* Caller holds history_mutex.  Pin @p hist (a new reference is taken) as the
 * running turn's history, holding conversation @p holds (0 = none yet). */
static void pin_turn_history_locked(session_t *session, struct json_object *hist, int64_t holds) {
   if (session->turn_history) {
      json_object_put(session->turn_history); /* a previous turn that never ended */
   }
   session->turn_history = json_object_get(hist);
   session->turn_pin_conv = holds;
   session->turn_appends = 0;
}

/* Caller holds history_mutex.  Pin the live history for @p conv_id if it holds
 * that conversation, or holds no conversation's turns yet while @p conv_id has
 * none either (@p conv_fresh) — then it is bound to @p conv_id.  Returns false
 * when the turn must run on its own copy instead. */
static bool try_pin_live_locked(session_t *session, int64_t conv_id, bool conv_fresh) {
   struct json_object *live = live_history_locked(session);
   const int64_t bound = atomic_load(&session->history_conversation_id);
   if (!live) {
      return false;
   }
   if (bound != conv_id) {
      if (bound != 0 || !conv_fresh || !history_is_fresh(live, 0)) {
         return false;
      }
      atomic_store(&session->history_conversation_id, conv_id);
   }
   pin_turn_history_locked(session, live, conv_id);
   return true;
}

/* Turn tokens, unique across sessions (see session_turn_token()). */
static _Atomic uint64_t s_turn_tokens;

/* Set the running turn's LLM settings to @p cfg (NULL = the session's own),
 * unless a later turn began meanwhile.  Never called under history_mutex (the
 * two session locks are never held together). */
static void set_turn_llm_config(session_t *session, const session_llm_config_t *cfg) {
   pthread_mutex_lock(&session->llm_config_mutex);
   if (session->turn_llm_config_set && session->turn_gen == session_turn_token()) {
      session->turn_llm_config = cfg ? *cfg : session->llm_config;
   }
   pthread_mutex_unlock(&session->llm_config_mutex);
}

/* The conversation's own history, detached from the session (NULL on failure).
 * Also sets the turn's LLM settings for it: the conversation's stored ones over
 * the session's, or the session's when it stores none or can't be loaded. */
static struct json_object *load_conversation(session_t *session, int user_id, int64_t conv_id) {
   session_history_loader_fn loader = atomic_load(&s_history_loader);
   session_llm_config_t base;
   session_llm_config_t cfg;
   bool has_cfg = false;
   pthread_mutex_lock(&session->llm_config_mutex);
   base = session->llm_config; /* the session's, never a previous turn load's */
   pthread_mutex_unlock(&session->llm_config_mutex);
   struct json_object *loaded = (loader && user_id > 0)
                                    ? loader(user_id, conv_id, &base, &cfg, &has_cfg)
                                    : NULL;
   if (!loaded) {
      OLOG_WARNING("Session %u: could not load conversation %lld for this turn; running it "
                   "without prior context",
                   session->session_id, (long long)conv_id);
   }
   set_turn_llm_config(session, loaded && has_cfg ? &cfg : NULL);
   return loaded;
}

/* Caller holds history_mutex.  Pin @p own (ownership taken; NULL = an empty
 * array) as the turn's history for @p conv_id, carrying over the last
 * @p carry messages of @p from (the turn's own, written before it knew its
 * conversation). */
static void pin_own_copy_locked(session_t *session,
                                struct json_object *own,
                                int64_t conv_id,
                                struct json_object *from,
                                int carry) {
   if (!own) {
      own = json_object_new_array();
   }
   if (!own) {
      return; /* OOM: the turn keeps what it had */
   }
   const int from_len = from ? (int)json_object_array_length(from) : 0;
   for (int i = from_len - carry; carry > 0 && i < from_len; i++) {
      if (i >= 0) {
         /* A copy: the original stays in the array it was written into.  The
          * turn's question and reply are the copies from now on (rollback and
          * id stamping find them in the turn's history). */
         struct json_object *orig = json_object_array_get_idx(from, i);
         struct json_object *copy = NULL;
         if (json_object_deep_copy(orig, &copy, NULL) == 0 && copy) {
            json_object_array_add(own, copy);
            struct json_object **ref = orig == session->turn_user_msg ? &session->turn_user_msg
                                       : orig == session->turn_reply  ? &session->turn_reply
                                                                      : NULL;
            if (ref) {
               json_object_put(*ref);
               *ref = json_object_get(copy);
            }
         }
      }
   }
   pin_turn_history_locked(session, own, conv_id);
   session->turn_appends = carry;
   json_object_put(own); /* the pin holds it */
   OLOG_INFO("Session %u: turn for conversation %lld runs on its own history (session history "
             "holds %lld)",
             session->session_id, (long long)conv_id,
             (long long)atomic_load(&session->history_conversation_id));
}

/* Pin the history a turn for @p conv_id (> 0) runs on.  The live history when it
 * holds that conversation; otherwise the conversation loaded on its own, detached
 * from the session, so neither the turn nor the conversation the user is viewing
 * sees the other's messages.  Loads outside history_mutex, then re-decides under
 * it: the live history may have been replaced meanwhile. */
static void pin_turn_for_conversation(session_t *session, int64_t conv_id, int user_id) {
   pthread_mutex_lock(&session->history_mutex);
   const bool pinned = atomic_load(&session->history_conversation_id) == conv_id &&
                       try_pin_live_locked(session, conv_id, false);
   pthread_mutex_unlock(&session->history_mutex);
   if (pinned) {
      return;
   }

   struct json_object *loaded = load_conversation(session, user_id, conv_id);

   pthread_mutex_lock(&session->history_mutex);
   /* A failed load is not an empty conversation: never bind the live history on it. */
   const bool conv_fresh = loaded && history_is_fresh(loaded, 0);
   const bool live = try_pin_live_locked(session, conv_id, conv_fresh);
   if (live) {
      json_object_put(loaded);
   } else {
      pin_own_copy_locked(session, loaded, conv_id, NULL, 0);
   }
   pthread_mutex_unlock(&session->history_mutex);
   if (live) {
      /* On the live history after all: the session's settings apply. */
      set_turn_llm_config(session, NULL);
   }
}

void session_turn_begin(session_t *session, int64_t conv_id, int user_id) {
   if (!session) {
      return;
   }
   /* The turn's token, and its settings as they stand now: a sidebar load
    * mid-turn changes the session's, never the running turn's. */
   const uint64_t outer = session_turn_token(); /* restored at the end */
   const uint64_t token = atomic_fetch_add(&s_turn_tokens, 1) + 1;
   pthread_mutex_lock(&session->llm_config_mutex);
   session->turn_gen = token;
   session->turn_llm_config = session->llm_config;
   session->turn_llm_config_set = true;
   pthread_mutex_unlock(&session->llm_config_mutex);
   session_set_turn_token(token);

   pthread_mutex_lock(&session->history_mutex);
   if (session->turn_active) {
      /* Every surface serializes its turns (the turn queue, the local mic's
       * busy gate); reaching here is a bug.  The earlier turn's writes are
       * refused from now on, and its end is ignored. */
      OLOG_ERROR("Session %u: a turn began while another was still running", session->session_id);
   }
   session->turn_active = true;
   session->turn_owner_token = token;
   session->turn_outer_token = outer;
   session->turn_user_id = user_id;
   session->turn_history_conv = conv_id > 0 ? conv_id : 0;
   session->turn_awaits_conversation = false;
   session->turn_background = false;
   session->turn_context_reset = false;
   free(session->turn_pending_user);
   session->turn_pending_user = NULL;
   free(session->turn_pending_reply);
   session->turn_pending_reply = NULL;
   if (session->turn_reply) {
      json_object_put(session->turn_reply);
      session->turn_reply = NULL;
   }
   if (session->turn_reply_mirror) {
      json_object_put(session->turn_reply_mirror);
      session->turn_reply_mirror = NULL;
   }
   if (session->turn_user_msg) {
      json_object_put(session->turn_user_msg);
      session->turn_user_msg = NULL;
   }
   /* The one writer of the turn's stream tag (see turn_bind_conversation_locked). */
   atomic_store(&session->stream_conversation_id, session->turn_history_conv);
   if (conv_id <= 0) {
      /* Conversation not known yet: start on the live history, recording which
       * conversation it holds; session_turn_set_conversation() resolves it
       * before the turn relies on it. */
      struct json_object *live = live_history_locked(session);
      if (session->turn_history) {
         json_object_put(session->turn_history);
         session->turn_history = NULL;
      }
      if (live) {
         pin_turn_history_locked(session, live, atomic_load(&session->history_conversation_id));
      }
      pthread_mutex_unlock(&session->history_mutex);
      return;
   }
   pthread_mutex_unlock(&session->history_mutex);
   pin_turn_for_conversation(session, conv_id, user_id);
}

/* Caller holds history_mutex, turn active.  Record @p conv_id as the turn's
 * conversation (the one funnel for both the pin's identity and the stream tag),
 * and keep the pin if it can serve it: it holds that conversation, or nothing
 * but the turn's own messages.  Returns true when the turn must move onto its
 * own copy of the conversation (see session_turn_set_conversation()). */
static bool turn_bind_conversation_locked(session_t *session, int64_t conv_id) {
   session->turn_history_conv = conv_id;
   session->turn_awaits_conversation = false; /* it has one now */
   atomic_store(&session->stream_conversation_id, conv_id);
   struct json_object *pinned = session->turn_history;
   const int64_t holds = session->turn_pin_conv;
   if (holds == conv_id) {
      return false;
   }
   if (pinned && pinned == session->conversation_history &&
       atomic_load(&session->history_conversation_id) == conv_id) {
      /* The live history it pins was bound to this conversation meanwhile (an
       * ended turn's exchange claimed for it): one identity, not a move. */
      session->turn_pin_conv = conv_id;
      return false;
   }
   if (holds == 0 && history_is_fresh(pinned, session->turn_appends)) {
      session->turn_pin_conv = conv_id;
      if (pinned && pinned == session->conversation_history &&
          atomic_load(&session->history_conversation_id) == 0) {
         atomic_store(&session->history_conversation_id, conv_id);
      }
      return false;
   }
   return true;
}

/* Caller holds history_mutex.  The turn already wrote into a live history held
 * by another conversation: that history can no longer be attributed to one. */
static void mark_live_mixed_locked(session_t *session,
                                   struct json_object *pinned,
                                   int64_t conv_id) {
   const int64_t holds = session->turn_pin_conv;
   if (session->turn_appends > 0 && pinned && pinned == session->conversation_history &&
       holds != 0 && atomic_load(&session->history_conversation_id) == holds) {
      atomic_store(&session->history_conversation_id, SESSION_HISTORY_CONV_MIXED);
      OLOG_WARNING("Session %u: turn for conversation %lld had written into history held by "
                   "%lld; that history is marked mixed",
                   session->session_id, (long long)conv_id, (long long)holds);
   }
}

/* Caller holds history_mutex.  Whether the live history is still no
 * conversation (the one facts wait on: session_defer_fact_source()). */
static bool live_unbound_locked(const session_t *session) {
   return atomic_load(&session->history_conversation_id) == 0;
}

/* Caller holds history_mutex; the running turn was just bound to a
 * conversation.  Take the facts it saved before it knew it, and, when the live
 * history it wrote into just became that conversation (@p became), those ended
 * turns left waiting for it: all are recorded to it once the lock is dropped. */
static int take_bound_facts_locked(session_t *session,
                                   bool became,
                                   session_fact_source_t out[SESSION_PENDING_FACT_SOURCES_MAX]) {
   int n = take_facts_locked(session, session->turn_owner_token, out, 0);
   if (became) {
      n = take_facts_locked(session, 0, out, n);
   }
   return n;
}

/* session_turn_set_conversation(); the facts to record to @p conv_id go to
 * @p facts / @p count, their owner to @p owner. */
static void set_conversation_impl(session_t *session,
                                  int64_t conv_id,
                                  bool may_load,
                                  session_fact_source_t facts[SESSION_PENDING_FACT_SOURCES_MAX],
                                  int *count,
                                  int *owner) {
   *count = 0;
   pthread_mutex_lock(&session->history_mutex);
   if (!session->turn_active) {
      pthread_mutex_unlock(&session->history_mutex);
      return;
   }
   *owner = session->turn_user_id;
   const bool was_unbound = live_unbound_locked(session);
   const bool moves = turn_bind_conversation_locked(session, conv_id);
   const bool became = was_unbound && atomic_load(&session->history_conversation_id) == conv_id;
   /* In this critical section, so a context reset can't drop them first. */
   *count = take_bound_facts_locked(session, became, facts);
   if (!moves) {
      pthread_mutex_unlock(&session->history_mutex);
      return;
   }
   struct json_object *pinned = session->turn_history;
   const int appends = session->turn_appends;
   const int user_id = session->turn_user_id;
   if (!may_load) {
      /* Not the turn's own thread: the turn keeps its history; the stream tag
       * and persistence already follow the conversation. */
      mark_live_mixed_locked(session, pinned, conv_id);
      pthread_mutex_unlock(&session->history_mutex);
      return;
   }
   pthread_mutex_unlock(&session->history_mutex);

   /* Move the turn onto its own conversation, carrying the messages it already
    * wrote (they belong to it, not to the history they were written into). */
   struct json_object *loaded = load_conversation(session, user_id, conv_id);

   pthread_mutex_lock(&session->history_mutex);
   if (session->turn_history != pinned || session->turn_appends != appends ||
       !session->turn_active || session->turn_history_conv != conv_id) {
      /* Changed while loading; the other writer decided. */
      pthread_mutex_unlock(&session->history_mutex);
      json_object_put(loaded);
      return;
   }
   mark_live_mixed_locked(session, pinned, conv_id);
   pin_own_copy_locked(session, loaded, conv_id, pinned, appends);
   pthread_mutex_unlock(&session->history_mutex);
}

void session_turn_set_conversation(session_t *session, int64_t conv_id, bool may_load) {
   if (!session || conv_id <= 0) {
      return;
   }
   session_fact_source_t facts[SESSION_PENDING_FACT_SOURCES_MAX];
   int count = 0;
   int owner = 0;
   set_conversation_impl(session, conv_id, may_load, facts, &count, &owner);
   /* What the turn saved before it knew its conversation. */
   session_record_fact_sources(facts, count, conv_id, owner);
}

void session_history_append(struct json_object *history, struct json_object *msg) {
   if (!history || !msg) {
      json_object_put(msg);
      return;
   }
   session_t *session = session_get_command_context();
   if (session) {
      pthread_mutex_lock(&session->history_mutex);
      if (session->turn_active && !turn_is_caller_locked(session) &&
          (history == session->turn_history || history == session->conversation_history)) {
         /* From a turn another has replaced: the history is that turn's now. */
         pthread_mutex_unlock(&session->history_mutex);
         OLOG_ERROR("Session %u: dropped a message from a turn that is no longer running",
                    session->session_id);
         json_object_put(msg);
         return;
      }
   }
   json_object_array_add(history, msg);
   if (session && session->turn_active && history == session->turn_history) {
      session->turn_appends++; /* the turn's own messages (see history_is_fresh) */
   }
   if (session) {
      pthread_mutex_unlock(&session->history_mutex);
   }
}

void session_history_replace_contents(struct json_object *history, struct json_object *from) {
   if (!history || !from) {
      return;
   }
   session_t *session = session_get_command_context();
   if (session) {
      pthread_mutex_lock(&session->history_mutex);
   }
   json_object_array_del_idx(history, 0, json_object_array_length(history));
   const int n = (int)json_object_array_length(from);
   for (int i = 0; i < n; i++) {
      json_object_array_add(history, json_object_get(json_object_array_get_idx(from, i)));
   }
   if (session) {
      pthread_mutex_unlock(&session->history_mutex);
   }
}

void session_turn_await_conversation(session_t *session) {
   if (!session) {
      return;
   }
   pthread_mutex_lock(&session->history_mutex);
   /* Not a turn whose context the user reset meanwhile: the conversation
    * created next is the new context's. */
   if (session->turn_active && session->turn_history_conv == 0 && !session->turn_context_reset) {
      session->turn_awaits_conversation = true;
   }
   pthread_mutex_unlock(&session->history_mutex);
}

void session_turn_mark_background(session_t *session) {
   if (!session) {
      return;
   }
   pthread_mutex_lock(&session->history_mutex);
   if (session->turn_active) {
      session->turn_background = true;
   }
   pthread_mutex_unlock(&session->history_mutex);
}


bool session_turn_is_caller(session_t *session) {
   if (!session) {
      return false;
   }
   pthread_mutex_lock(&session->history_mutex);
   const bool caller = turn_is_caller_locked(session);
   pthread_mutex_unlock(&session->history_mutex);
   return caller;
}

bool session_turn_active(session_t *session) {
   if (!session) {
      return false;
   }
   pthread_mutex_lock(&session->history_mutex);
   const bool active = session->turn_active;
   pthread_mutex_unlock(&session->history_mutex);
   return active;
}

bool session_turn_user_originated(session_t *session) {
   if (!session || session->type == SESSION_TYPE_JOB) {
      return false;
   }
   pthread_mutex_lock(&session->history_mutex);
   const bool user = turn_is_caller_locked(session) && !session->turn_background;
   pthread_mutex_unlock(&session->history_mutex);
   return user;
}

/* Caller holds history_mutex.  The running turn's pending slot for @p role. */
static char **pending_slot_locked(session_t *session, const char *role) {
   return strcmp(role, "assistant") == 0 ? &session->turn_pending_reply
                                         : &session->turn_pending_user;
}

void session_turn_set_pending(session_t *session, const char *role, const char *persist_text) {
   if (!session || !role || !persist_text) {
      return;
   }
   char *copy = strdup(persist_text);
   pthread_mutex_lock(&session->history_mutex);
   if (session->turn_active) {
      char **slot = pending_slot_locked(session, role);
      free(*slot);
      *slot = copy;
      copy = NULL;
   }
   pthread_mutex_unlock(&session->history_mutex);
   free(copy);
}

char *session_turn_take_pending(session_t *session, const char *role, int64_t *conv_out) {
   if (conv_out) {
      *conv_out = 0;
   }
   if (!session || !role) {
      return NULL;
   }
   char *text = NULL;
   pthread_mutex_lock(&session->history_mutex);
   char **slot = pending_slot_locked(session, role);
   if (session->turn_active && *slot && session->turn_history_conv > 0) {
      text = *slot;
      *slot = NULL;
      if (conv_out) {
         *conv_out = session->turn_history_conv;
      }
   }
   pthread_mutex_unlock(&session->history_mutex);
   return text;
}

static _Atomic(session_fact_source_fn) s_fact_source_hook;

void session_set_fact_source_hook(session_fact_source_fn fn) {
   atomic_store(&s_fact_source_hook, fn);
}

int session_defer_fact_source(session_t *session,
                              int64_t fact_id,
                              int user_id,
                              bool created,
                              int64_t *conv_out) {
   *conv_out = 0;
   if (!session || fact_id <= 0) {
      return FAILURE;
   }
   int result = FAILURE;
   pthread_mutex_lock(&session->history_mutex);
   if (!turn_is_caller_locked(session)) {
      /* Not a turn: no conversation to wait for. */
   } else if (session->turn_history_conv > 0) {
      *conv_out = session->turn_history_conv;
      result = SUCCESS;
   } else {
      /* Waits for the turn's conversation (once per fact). */
      const uint64_t token = session->turn_owner_token;
      for (int i = 0; i < session->pending_fact_source_count; i++) {
         session_fact_source_t *f = &session->pending_fact_sources[i];
         if (f->fact_id == fact_id && f->turn_token == token) {
            f->created = f->created || created;
            result = SESSION_FACT_SOURCE_QUEUED;
            break;
         }
      }
      if (result != SESSION_FACT_SOURCE_QUEUED) {
         if (session->pending_fact_source_count < SESSION_PENDING_FACT_SOURCES_MAX) {
            session->pending_fact_sources[session->pending_fact_source_count++] =
                (session_fact_source_t){ .fact_id = fact_id,
                                         .user_id = user_id,
                                         .created = created,
                                         .turn_token = token };
            result = SESSION_FACT_SOURCE_QUEUED;
         } else {
            OLOG_WARNING("Session %u: too many facts waiting for their conversation",
                         session->session_id);
            result = SESSION_FACT_SOURCE_DROPPED;
         }
      }
   }
   pthread_mutex_unlock(&session->history_mutex);
   return result;
}

int session_take_fact_sources_locked(session_t *session,
                                     session_fact_source_t out[SESSION_PENDING_FACT_SOURCES_MAX]) {
   return take_facts_locked(session, 0, out, 0);
}
void session_record_fact_sources(const session_fact_source_t *facts,
                                 int count,
                                 int64_t conv_id,
                                 int owner_user_id) {
   session_fact_source_fn hook = atomic_load(&s_fact_source_hook);
   if (!hook || conv_id <= 0 || count <= 0) {
      return;
   }
   /* Another user's (a satellite remapped between turns): not taught in this
    * user's conversation. */
   session_fact_source_t owned[SESSION_PENDING_FACT_SOURCES_MAX];
   int n = 0;
   for (int i = 0; i < count && n < SESSION_PENDING_FACT_SOURCES_MAX; i++) {
      if (owner_user_id <= 0 || facts[i].user_id == owner_user_id) {
         owned[n++] = facts[i];
      }
   }
   if (n > 0) {
      hook(owned, n, conv_id);
   }
}
void session_flush_fact_sources(session_t *session, int64_t conv_id, int owner_user_id) {
   if (!session || conv_id <= 0) {
      return;
   }
   session_fact_source_t facts[SESSION_PENDING_FACT_SOURCES_MAX];
   pthread_mutex_lock(&session->history_mutex);
   const int n = session_take_fact_sources_locked(session, facts);
   pthread_mutex_unlock(&session->history_mutex);
   session_record_fact_sources(facts, n, conv_id, owner_user_id);
}

/* Caller holds history_mutex.  Let go of the claimed messages (and any ids
 * still waiting to be stamped on them). */
static void drop_claimed_locked(session_t *session) {
   release_msg_locked(session, session->claimed_user_msg);
   release_msg_locked(session, session->claimed_reply_msg);
   session->claimed_user_msg = NULL;
   session->claimed_reply_msg = NULL;
   session->claimed_user_row = 0;
   session->claimed_reply_row = 0;
}

bool session_bind_created_conversation(session_t *session,
                                       int64_t conv_id,
                                       char **user_out,
                                       char **reply_out,
                                       bool *adopted_out) {
   if (user_out) {
      *user_out = NULL;
   }
   if (reply_out) {
      *reply_out = NULL;
   }
   if (adopted_out) {
      *adopted_out = false;
   }
   if (!session || conv_id <= 0 || !user_out || !reply_out || !adopted_out) {
      return false;
   }
   pthread_mutex_lock(&session->history_mutex);
   /* One critical section: a turn ending between "claim" and "adopt" would
    * otherwise leave its exchange unclaimed with the conversation already
    * handed out, and lose it. */
   const bool fresh = (session->unclaimed_user || session->unclaimed_reply) &&
                      time(NULL) - session->unclaimed_at <= SESSION_UNCLAIMED_TURN_SEC;
   const bool was_unbound = live_unbound_locked(session);
   const bool adopt = session->turn_active && session->turn_awaits_conversation &&
                      session->turn_history_conv == 0;
   bool hand_out = false;
   if (fresh) {
      drop_claimed_locked(session);
      session->claimed_user_msg = session->unclaimed_user_msg;
      session->claimed_reply_msg = session->unclaimed_reply_msg;
      session->unclaimed_user_msg = NULL;
      session->unclaimed_reply_msg = NULL;
      if (adopt) {
         /* The adopting turn writes them, ahead of its own rows, on its own
          * thread (which also stamps them: it reads those messages). */
         free(session->turn_prior_user);
         free(session->turn_prior_reply);
         session->turn_prior_user = session->unclaimed_user;
         session->turn_prior_reply = session->unclaimed_reply;
      } else {
         *user_out = session->unclaimed_user;
         *reply_out = session->unclaimed_reply;
         hand_out = true;
      }
      session->unclaimed_user = NULL;
      session->unclaimed_reply = NULL;
      /* The live history holds that exchange and nothing else of any
       * conversation's: it is this conversation's now. */
      if (was_unbound) {
         atomic_store(&session->history_conversation_id, conv_id);
      }
   } else {
      drop_unclaimed_locked(session);
   }
   /* A turn still running that was dispatched before the row existed. */
   session_fact_source_t facts[SESSION_PENDING_FACT_SOURCES_MAX];
   int fact_count = 0;
   if (adopt) {
      session->turn_awaits_conversation = false;
      if (turn_bind_conversation_locked(session, conv_id)) {
         mark_live_mixed_locked(session, session->turn_history, conv_id);
      }
      *adopted_out = true;
   }
   const bool became = was_unbound && atomic_load(&session->history_conversation_id) == conv_id;
   /* What was saved before the conversation existed: by the adopted turn, and
    * by ended turns into the history that is this conversation now. */
   if (adopt) {
      fact_count = take_bound_facts_locked(session, became, facts);
   } else if (became) {
      fact_count = take_facts_locked(session, 0, facts, 0);
   }
   pthread_mutex_unlock(&session->history_mutex);
   session_record_fact_sources(facts, fact_count, conv_id, 0);
   return hand_out;
}

/* Caller holds history_mutex. */
static void stamp_object_locked(struct json_object *msg, int64_t row_id) {
   if (msg && row_id > 0 && !json_object_object_get_ex(msg, "id", NULL)) {
      json_object_object_add(msg, "id", json_object_new_int64(row_id));
   }
}

void session_stamp_claimed(session_t *session, int64_t user_row_id, int64_t reply_row_id) {
   if (!session) {
      return;
   }
   pthread_mutex_lock(&session->history_mutex);
   if (session->turn_active && !turn_is_caller_locked(session)) {
      /* A turn is reading these messages without the lock: it stamps them
       * when it ends. */
      session->claimed_user_row = user_row_id;
      session->claimed_reply_row = reply_row_id;
   } else {
      stamp_object_locked(session->claimed_user_msg, user_row_id);
      stamp_object_locked(session->claimed_reply_msg, reply_row_id);
      drop_claimed_locked(session);
   }
   pthread_mutex_unlock(&session->history_mutex);
}

bool session_turn_take_prior(session_t *session,
                             int64_t *conv_out,
                             char **user_out,
                             char **reply_out) {
   *conv_out = 0;
   *user_out = NULL;
   *reply_out = NULL;
   if (!session) {
      return false;
   }
   pthread_mutex_lock(&session->history_mutex);
   const bool have = turn_is_caller_locked(session) && session->turn_history_conv > 0 &&
                     (session->turn_prior_user || session->turn_prior_reply);
   if (have) {
      *conv_out = session->turn_history_conv;
      *user_out = session->turn_prior_user;
      *reply_out = session->turn_prior_reply;
      session->turn_prior_user = NULL;
      session->turn_prior_reply = NULL;
   }
   pthread_mutex_unlock(&session->history_mutex);
   return have;
}

/* The highest database row id among @p hist's messages (0 if none). */
static int64_t newest_row_id(struct json_object *hist) {
   int64_t newest = 0;
   const int len = (int)json_object_array_length(hist);
   for (int i = 0; i < len; i++) {
      struct json_object *id = NULL;
      if (json_object_object_get_ex(json_object_array_get_idx(hist, i), "id", &id) &&
          json_object_get_int64(id) > newest) {
         newest = json_object_get_int64(id);
      }
   }
   return newest;
}

/* Whether @p hist holds @p reply: the object itself, or the row it was saved as
 * (same id), as a reload from the database has it. */
static bool history_holds_reply(struct json_object *hist,
                                struct json_object *reply,
                                struct json_object *reply_mirror) {
   struct json_object *id_obj = NULL;
   const int64_t id = json_object_object_get_ex(reply, "id", &id_obj)
                          ? json_object_get_int64(id_obj)
                          : 0;
   for (int i = (int)json_object_array_length(hist) - 1; i >= 0; i--) {
      struct json_object *entry = json_object_array_get_idx(hist, i);
      if (entry == reply || (reply_mirror && entry == reply_mirror)) {
         return true;
      }
      struct json_object *eid = NULL;
      if (id > 0 && entry && json_object_object_get_ex(entry, "id", &eid) &&
          json_object_get_int64(eid) == id) {
         return true;
      }
   }
   return false;
}

/* Ends the turn, or hands out what it has yet to write; see
 * session_turn_finish().  @p out may be NULL (the caller writes nothing). */
static int turn_end_impl(session_t *session, session_turn_unsaved_t *out) {
   if (out) {
      memset(out, 0, sizeof(*out));
   }
   if (!session) {
      return SESSION_TURN_ENDED;
   }
   pthread_mutex_lock(&session->history_mutex);
   if (session->turn_active && session_turn_token() != session->turn_owner_token) {
      /* Not this thread's turn: ending it would pull the history out from under
       * the thread still running it. */
      pthread_mutex_unlock(&session->history_mutex);
      OLOG_WARNING("Session %u: a thread that isn't running the turn tried to end it",
                   session->session_id);
      return SESSION_TURN_ENDED;
   }
   if (session->turn_active && out && session->turn_history_conv > 0 &&
       (session->turn_pending_user || session->turn_pending_reply || session->turn_prior_user ||
        session->turn_prior_reply)) {
      /* The conversation is known and messages wait for it: the caller writes
       * them while the turn is still open, so their row ids are stamped on the
       * turn's own history, then finishes again. */
      out->conv = session->turn_history_conv;
      out->prior_user = session->turn_prior_user;
      out->prior_reply = session->turn_prior_reply;
      out->user = session->turn_pending_user;
      out->reply = session->turn_pending_reply;
      session->turn_prior_user = NULL;
      session->turn_prior_reply = NULL;
      session->turn_pending_user = NULL;
      session->turn_pending_reply = NULL;
      pthread_mutex_unlock(&session->history_mutex);
      return SESSION_TURN_WRITE_UNSAVED;
   }
   const bool was_active = session->turn_active;
   const uint64_t my_token = session->turn_owner_token;
   const uint64_t outer_token = session->turn_outer_token;
   const int64_t turn_conv = session->turn_history_conv;
   /* The view is published before a sidebar load replaces the history (under
    * this lock), so it can be newer than the history here, never older: at
    * worst the turn's copy is adopted and then replaced by that load. */
   const int64_t viewed_conv = atomic_load(&session->viewed_conversation_id);
   struct json_object *own = session->turn_history;
   if (own && own != session->conversation_history && viewed_conv > 0 &&
       session->turn_pin_conv == viewed_conv) {
      /* The turn ran on its own copy of the conversation now being viewed while
       * the live history holds something else (another conversation after a
       * failed restore, nothing attributable, or a mix): the copy is exactly
       * what the user is looking at, so adopt it rather than reload it from
       * the database on every later turn. */
      const int64_t bound = atomic_load(&session->history_conversation_id);
      if (bound != viewed_conv) {
         /* The live context is replaced: what its ended turns left behind. */
         drop_fact_sources_locked(session);
         drop_unclaimed_locked(session);
         json_object_put(session->conversation_history);
         session->conversation_history = json_object_get(own);
         session->visual_modules_loaded[0] = '\0';
         memset(&session->injected_set, 0, sizeof(session->injected_set));
         atomic_store(&session->history_conversation_id, viewed_conv);
         OLOG_INFO("Session %u: adopted conversation %lld's history as the session history",
                   session->session_id, (long long)viewed_conv);
      }
   }
   /* The live history was reloaded for this turn's conversation while the turn
    * ran on its own copy (the user opened it, or reconnected).  The reload came
    * from the database, which may lack what the turn hadn't saved yet (its tool
    * calls, the reply, even the question).  With nothing newer than the copy,
    * the copy is the fuller context: adopt it.  Otherwise keep the reload and
    * make sure it has the reply. */
   struct json_object *reply = session->turn_reply;
   struct json_object *live = session->conversation_history;
   if (live && own && live != own && session->turn_history_conv > 0 &&
       session->turn_pin_conv == session->turn_history_conv &&
       atomic_load(&session->history_conversation_id) == session->turn_history_conv) {
      if (newest_row_id(live) <= newest_row_id(own)) {
         json_object_put(session->conversation_history);
         session->conversation_history = json_object_get(own);
         session->visual_modules_loaded[0] = '\0';
         memset(&session->injected_set, 0, sizeof(session->injected_set));
      } else if (reply && !history_holds_reply(live, reply, session->turn_reply_mirror)) {
         struct json_object *copy = NULL;
         if (json_object_deep_copy(reply, &copy, NULL) == 0 && copy) {
            json_object_array_add(live, copy);
         }
      }
   }
   const bool wrote_unbound_live = session->turn_history &&
                                   session->turn_history == session->conversation_history &&
                                   live_unbound_locked(session);
   if (session->turn_history) {
      json_object_put(session->turn_history);
      session->turn_history = NULL;
   }
   /* Messages this turn was reading that another thread let go of, and the
    * ids it was asked to stamp on them (this thread is the one that may). */
   if (session->claimed_user_row > 0 || session->claimed_reply_row > 0) {
      stamp_object_locked(session->claimed_user_msg, session->claimed_user_row);
      stamp_object_locked(session->claimed_reply_msg, session->claimed_reply_row);
      drop_claimed_locked(session);
   }
   for (int i = 0; i < session->parked_msg_count; i++) {
      json_object_put(session->parked_msgs[i]);
   }
   session->parked_msg_count = 0;
   if (session->turn_prior_user || session->turn_prior_reply) {
      OLOG_WARNING("Session %u: a turn ended without writing the exchange it adopted",
                   session->session_id);
      free(session->turn_prior_user);
      free(session->turn_prior_reply);
      session->turn_prior_user = NULL;
      session->turn_prior_reply = NULL;
   }
   /* Facts this turn saved and never got a conversation for: they wait for the
    * one the live history becomes when that is the history it wrote (a new
    * chat's first exchange, a voice session saved later); otherwise there is
    * none to record. */
   for (int i = 0; i < session->pending_fact_source_count;) {
      session_fact_source_t *f = &session->pending_fact_sources[i];
      if (f->turn_token != my_token) {
         i++;
      } else if (wrote_unbound_live) {
         f->turn_token = 0;
         i++;
      } else {
         *f = session->pending_fact_sources[--session->pending_fact_source_count];
      }
   }
   session->turn_active = false;
   session->turn_owner_token = 0;
   /* A prompt refresh another thread made while the turn held the live history. */
   if (session->deferred_system_prompt) {
      session_update_system_prompt_locked(session, session->deferred_system_prompt);
      free(session->deferred_system_prompt);
      session->deferred_system_prompt = NULL;
   }
   session->turn_history_conv = 0;
   session->turn_pin_conv = 0;
   session->turn_appends = 0;
   /* Messages the turn couldn't save yet.  Decided in this same critical
    * section as the end, so a conversation adopted at the last moment can't
    * slip between "nothing to write" and "discard". */
   if (session->turn_pending_user || session->turn_pending_reply) {
      if (turn_conv <= 0 && session->turn_context_reset) {
         /* The user cleared the context this exchange was in: it belongs to no
          * conversation created from now on. */
         free(session->turn_pending_user);
         free(session->turn_pending_reply);
      } else if (turn_conv <= 0) {
         /* A new chat's first exchange whose conversation doesn't exist yet
          * waits for it (session_bind_created_conversation), with the history messages
          * its row ids go on. */
         drop_unclaimed_locked(session);
         session->unclaimed_user = session->turn_pending_user;
         session->unclaimed_reply = session->turn_pending_reply;
         session->unclaimed_at = time(NULL);
         if (session->unclaimed_user) {
            session->unclaimed_user_msg = session->turn_user_msg;
            session->turn_user_msg = NULL;
         }
         if (session->unclaimed_reply) {
            session->unclaimed_reply_msg = reply ? json_object_get(reply) : NULL;
         }
      } else {
         OLOG_WARNING("Session %u: a turn ended with unsaved messages and no one to write them",
                      session->session_id);
         free(session->turn_pending_user);
         free(session->turn_pending_reply);
      }
      session->turn_pending_user = NULL;
      session->turn_pending_reply = NULL;
   }
   if (session->turn_reply) {
      json_object_put(session->turn_reply);
      session->turn_reply = NULL;
   }
   if (session->turn_reply_mirror) {
      json_object_put(session->turn_reply_mirror);
      session->turn_reply_mirror = NULL;
   }
   if (session->turn_user_msg) {
      json_object_put(session->turn_user_msg);
      session->turn_user_msg = NULL;
   }
   session->turn_awaits_conversation = false;
   session->turn_outer_token = 0;
   pthread_mutex_unlock(&session->history_mutex);

   if (!was_active) {
      return SESSION_TURN_ENDED;
   }
   pthread_mutex_lock(&session->llm_config_mutex);
   if (session->turn_gen == my_token) { /* not a turn begun since */
      session->turn_llm_config_set = false;
      session->turn_gen = 0;
   }
   pthread_mutex_unlock(&session->llm_config_mutex);
   session_set_turn_token(outer_token); /* the thread's token before this turn */
   return SESSION_TURN_ENDED;
}

void session_turn_end(session_t *session) {
   (void)turn_end_impl(session, NULL);
}

int session_turn_finish(session_t *session, session_turn_unsaved_t *out) {
   return turn_end_impl(session, out);
}

bool session_turn_on_own_history(session_t *session) {
   if (!session) {
      return false;
   }
   pthread_mutex_lock(&session->history_mutex);
   const bool own = session_turn_on_own_history_locked(session);
   pthread_mutex_unlock(&session->history_mutex);
   return own;
}

int64_t session_turn_conversation(session_t *session) {
   if (!session) {
      return 0;
   }
   pthread_mutex_lock(&session->history_mutex);
   const int64_t conv = session->turn_active ? session->turn_history_conv
                                             : atomic_load(&session->history_conversation_id);
   pthread_mutex_unlock(&session->history_mutex);
   return conv > 0 ? conv : 0;
}

int64_t session_history_conversation_of(session_t *session, struct json_object *history) {
   if (!session || !history) {
      return 0;
   }
   pthread_mutex_lock(&session->history_mutex);
   int64_t conv = 0;
   if (history == session->turn_history) {
      conv = session->turn_pin_conv;
   } else if (history == session->conversation_history) {
      conv = atomic_load(&session->history_conversation_id);
   }
   pthread_mutex_unlock(&session->history_mutex);
   return conv > 0 ? conv : 0;
}

struct json_object *session_get_turn_history(session_t *session) {
   if (!session) {
      return NULL;
   }
   pthread_mutex_lock(&session->history_mutex);
   struct json_object *h = session->turn_history ? session->turn_history
                                                 : session->conversation_history;
   if (h) {
      json_object_get(h);
   }
   pthread_mutex_unlock(&session->history_mutex);
   return h;
}

void session_put_history(session_t *session, struct json_object *history) {
   if (!history) {
      return;
   }
   /* json-c reference counts are plain read-modify-write: every change to the
    * count of an array the session holds happens under its lock. */
   if (session) {
      pthread_mutex_lock(&session->history_mutex);
   }
   json_object_put(history);
   if (session) {
      pthread_mutex_unlock(&session->history_mutex);
   }
}

bool session_has_messages(session_t *session) {
   if (!session) {
      return false;
   }

   pthread_mutex_lock(&session->history_mutex);
   int count = session->conversation_history
                   ? (int)json_object_array_length(session->conversation_history)
                   : 0;
   pthread_mutex_unlock(&session->history_mutex);

   /* Require at least 2 messages (system prompt + user message) */
   return count >= 2;
}
