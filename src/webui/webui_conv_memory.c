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
 * WebUI handlers for a conversation's privacy and the memories learned from it:
 * set_private, and the forget offered after a conversation goes private.
 *
 * Counting and forgetting wait for the user's in-flight memory extraction (so
 * they see every row it writes) and touch many rows, so they run on a detached
 * worker and answer through the session's response queue rather than blocking
 * the WebSocket service thread.
 */

#include <json-c/json.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_withdraw.h"
#include "core/session_manager.h"
#include "core/session_prefix.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_extraction.h"
#include "memory/memory_forget.h"
#include "webui/webui_internal.h"
#include "webui/webui_server.h"

typedef enum {
   CONV_MEMORY_COUNT,  /* report what was learned (after going private) */
   CONV_MEMORY_FORGET, /* delete it */
} conv_memory_op_t;

typedef struct {
   uint32_t session_id; /* looked up again to reply: no reference held while waiting */
   int user_id;
   int64_t conv_id;
   conv_memory_op_t op;
} conv_memory_job_t;

/* Users with a count or forget in flight.  One at a time per user: each can wait
 * up to MEMORY_FORGET_WAIT_SEC, so an unbounded number would pin one thread
 * apiece. */
#define CONV_MEMORY_MAX_INFLIGHT 32
/* Each in-flight forget holds its user's extraction: there must be room for all. */
_Static_assert(CONV_MEMORY_MAX_INFLIGHT <= MEMORY_EXTRACTION_MAX_HOLDS,
               "every in-flight forget needs an extraction hold");
static pthread_mutex_t s_inflight_mutex = PTHREAD_MUTEX_INITIALIZER;
static int s_inflight_users[CONV_MEMORY_MAX_INFLIGHT];

/* Claim the user's in-flight slot.  false if one is already running (or full). */
static bool inflight_claim(int user_id) {
   bool ok = false;
   int free_slot = -1;
   pthread_mutex_lock(&s_inflight_mutex);
   for (int i = 0; i < CONV_MEMORY_MAX_INFLIGHT; i++) {
      if (s_inflight_users[i] == user_id) {
         free_slot = -1;
         break;
      }
      if (s_inflight_users[i] == 0 && free_slot < 0) {
         free_slot = i;
      }
   }
   if (free_slot >= 0) {
      s_inflight_users[free_slot] = user_id;
      ok = true;
   }
   pthread_mutex_unlock(&s_inflight_mutex);
   return ok;
}

static void inflight_release(int user_id) {
   pthread_mutex_lock(&s_inflight_mutex);
   for (int i = 0; i < CONV_MEMORY_MAX_INFLIGHT; i++) {
      if (s_inflight_users[i] == user_id) {
         s_inflight_users[i] = 0;
         break;
      }
   }
   pthread_mutex_unlock(&s_inflight_mutex);
}

/* {facts, outdated, summaries, preferences, relations, memories} for the client.
 * "memories" is what the Memory panel lists; "outdated" are superseded facts it
 * doesn't; relations are graph links. */
static void add_counts(json_object *payload, const memory_conv_learned_t *c) {
   json_object_object_add(payload, "memories",
                          json_object_new_int(c->facts + c->summaries + c->preferences));
   json_object_object_add(payload, "facts", json_object_new_int(c->facts));
   json_object_object_add(payload, "outdated", json_object_new_int(c->outdated));
   json_object_object_add(payload, "summaries", json_object_new_int(c->summaries));
   json_object_object_add(payload, "preferences", json_object_new_int(c->preferences));
   json_object_object_add(payload, "relations", json_object_new_int(c->relations));
}

/* Reply to the session if it is still connected (takes @p payload). */
static void send_frame(uint32_t session_id, const char *type, json_object *payload) {
   json_object *frame = json_object_new_object();
   if (!frame) {
      json_object_put(payload);
      return;
   }
   json_object_object_add(frame, "type", json_object_new_string(type));
   json_object_object_add(frame, "payload", payload);
   session_t *session = session_get(session_id); /* NULL once disconnected */
   if (session) {
      webui_send_session_json(session,
                              json_object_to_json_string_ext(frame, JSON_C_TO_STRING_PLAIN));
      session_release(session);
   }
   json_object_put(frame);
}

static void *conv_memory_worker(void *arg) {
   conv_memory_job_t *job = (conv_memory_job_t *)arg;
   json_object *payload = json_object_new_object();
   if (payload) {
      json_object_object_add(payload, "conversation_id", json_object_new_int64(job->conv_id));
      memory_conv_learned_t c;
      if (job->op == CONV_MEMORY_COUNT) {
         const bool ok = memory_conversation_learned(job->user_id, job->conv_id, &c) == SUCCESS;
         json_object_object_add(payload, "success", json_object_new_boolean(ok));
         if (ok) {
            add_counts(payload, &c);
         }
      } else {
         conv_db_withdraw_intent_begin(job->user_id); /* the user removing it */
         int rc = memory_forget_conversation(job->user_id, job->conv_id, &c);
         conv_db_withdraw_intent_end();
         json_object_object_add(payload, "success", json_object_new_boolean(rc == SUCCESS));
         if (rc == SUCCESS) {
            /* What's gone leaves the conversations it was sent into; the next
             * turn's memory is built without it. */
            (void)session_withdraw_forgotten(job->user_id, true);
            add_counts(payload, &c);
         } else {
            json_object_object_add(payload, "error",
                                   json_object_new_string(
                                       rc == MEMORY_FORGET_BUSY
                                           ? "Memory is still being saved from this "
                                             "conversation; try again in a minute"
                                       : rc == MEMORY_FORGET_TOO_LONG
                                           ? "This conversation has too many continuations to "
                                             "forget at once"
                                           : "Could not remove the memories; nothing was changed"));
         }
         OLOG_INFO("WebUI: user %d forget for conversation %lld: %s", job->user_id,
                   (long long)job->conv_id, rc == SUCCESS ? "done" : "not done");
      }
   }
   /* Free the user's slot before replying, so a client acting on the reply at
    * once isn't told it's still busy. */
   inflight_release(job->user_id);
   if (payload) {
      send_frame(job->session_id,
                 job->op == CONV_MEMORY_COUNT ? "conversation_learned"
                                              : "forget_conversation_memories_response",
                 payload);
   }
   free(job);
   return NULL;
}

typedef enum {
   CONV_MEMORY_STARTED,
   CONV_MEMORY_BUSY,   /* one is already running for this user */
   CONV_MEMORY_FAILED, /* could not start */
} conv_memory_start_t;

/* Run @p op for @p conv_id on a detached worker. */
static conv_memory_start_t start_conv_memory_job(ws_connection_t *conn,
                                                 int64_t conv_id,
                                                 conv_memory_op_t op) {
   if (!conn->session) {
      return CONV_MEMORY_FAILED;
   }
   if (!inflight_claim(conn->auth_user_id)) {
      return CONV_MEMORY_BUSY;
   }
   conv_memory_job_t *job = calloc(1, sizeof(*job));
   if (!job) {
      inflight_release(conn->auth_user_id);
      return CONV_MEMORY_FAILED;
   }
   job->session_id = conn->session->session_id;
   job->user_id = conn->auth_user_id;
   job->conv_id = conv_id;
   job->op = op;

   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   int rc = pthread_create(&thread, &attr, conv_memory_worker, job);
   pthread_attr_destroy(&attr);
   if (rc != 0) {
      OLOG_ERROR("WebUI: could not start conversation-memory worker (%d)", rc);
      inflight_release(job->user_id);
      free(job);
      return CONV_MEMORY_FAILED;
   }
   return CONV_MEMORY_STARTED;
}

static void send_error(ws_connection_t *conn, const char *type, int64_t conv_id, const char *msg) {
   json_object *response = json_object_new_object();
   json_object *payload = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string(type));
   json_object_object_add(payload, "success", json_object_new_boolean(0));
   if (conv_id > 0) {
      json_object_object_add(payload, "conversation_id", json_object_new_int64(conv_id));
   }
   json_object_object_add(payload, "error", json_object_new_string(msg));
   json_object_object_add(response, "payload", payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* Count what was learned from @p conv_id (conversation_learned).  When the
 * count can't start, say so, so the client can ask again. */
static void request_learned_count(ws_connection_t *conn, int64_t conv_id) {
   const conv_memory_start_t st = start_conv_memory_job(conn, conv_id, CONV_MEMORY_COUNT);
   if (st == CONV_MEMORY_STARTED) {
      return;
   }
   json_object *response = json_object_new_object();
   json_object *payload = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("conversation_learned"));
   json_object_object_add(payload, "success", json_object_new_boolean(0));
   json_object_object_add(payload, "conversation_id", json_object_new_int64(conv_id));
   json_object_object_add(payload, "busy", json_object_new_boolean(st == CONV_MEMORY_BUSY));
   json_object_object_add(response, "payload", payload);
   send_json_response(conn, response);
   json_object_put(response);
}

void handle_set_private(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *id_obj, *private_obj;
   if (!json_object_object_get_ex(payload, "conversation_id", &id_obj) ||
       !json_object_object_get_ex(payload, "is_private", &private_obj)) {
      send_error(conn, "set_private_response", 0, "Missing conversation_id or is_private");
      return;
   }

   int64_t conv_id = json_object_get_int64(id_obj);
   bool is_private = json_object_get_boolean(private_obj);

   int result = conv_db_set_private(conv_id, conn->auth_user_id, is_private);
   if (result == AUTH_DB_NOT_FOUND) {
      send_error(conn, "set_private_response", conv_id, "Conversation not found");
      return;
   }
   if (result != AUTH_DB_SUCCESS) {
      send_error(conn, "set_private_response", conv_id, "Failed to update privacy");
      return;
   }

   /* Update active conversation tracking if this is the current conversation */
   if (conn->active_conversation_id == conv_id) {
      conn->active_conversation_private = is_private;
   }

   json_object *response = json_object_new_object();
   json_object *resp_payload = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("set_private_response"));
   json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   json_object_object_add(resp_payload, "conversation_id", json_object_new_int64(conv_id));
   json_object_object_add(resp_payload, "is_private", json_object_new_boolean(is_private));
   json_object_object_add(resp_payload, "message",
                          json_object_new_string(is_private ? "Conversation marked private"
                                                            : "Conversation marked public"));
   if (is_private) {
      /* Continuations went private with it (conv_db_set_private). */
      int64_t chain[CONV_CHAIN_MAX];
      int n = 0;
      if (conv_db_continuation_chain(conv_id, conn->auth_user_id, chain, CONV_CHAIN_MAX, &n) ==
              AUTH_DB_SUCCESS &&
          n > 1) {
         json_object *also = json_object_new_array();
         for (int i = 1; i < n; i++) {
            json_object_array_add(also, json_object_new_int64(chain[i]));
         }
         json_object_object_add(resp_payload, "also_private", also);
      }
   }
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);

   /* Going private stops future extraction (for its continuations too).  Report
    * what was already learned, once any extraction in flight has finished, so the
    * client can offer to forget it (conversation_learned). */
   if (is_private) {
      request_learned_count(conn, conv_id);
   }
}

void handle_conversation_learned_request(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }
   json_object *id_obj;
   int64_t conv_id = 0;
   if (payload && json_object_object_get_ex(payload, "conversation_id", &id_obj)) {
      conv_id = json_object_get_int64(id_obj);
   }
   /* Ownership, and only for a private conversation (what the offer is for). */
   bool is_private = false;
   if (conv_id <= 0 ||
       conv_db_is_private(conv_id, conn->auth_user_id, &is_private) != AUTH_DB_SUCCESS ||
       !is_private) {
      send_error(conn, "conversation_learned", conv_id, "Conversation not found");
      return;
   }
   request_learned_count(conn, conv_id);
}

void handle_forget_conversation_memories(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *id_obj;
   int64_t conv_id = 0;
   if (payload && json_object_object_get_ex(payload, "conversation_id", &id_obj)) {
      conv_id = json_object_get_int64(id_obj);
   }

   /* Ownership check before anything is queued.  Privacy isn't required: the
    * user asked to forget this conversation's memories, private or not. */
   bool is_private = false;
   if (conv_id <= 0 ||
       conv_db_is_private(conv_id, conn->auth_user_id, &is_private) != AUTH_DB_SUCCESS) {
      send_error(conn, "forget_conversation_memories_response", conv_id, "Conversation not found");
      return;
   }
   conv_memory_start_t st = start_conv_memory_job(conn, conv_id, CONV_MEMORY_FORGET);
   if (st != CONV_MEMORY_STARTED) {
      send_error(conn, "forget_conversation_memories_response", conv_id,
                 st == CONV_MEMORY_BUSY ? "Still working on a previous request; try again shortly"
                                        : "Could not start; try again");
   }
}
