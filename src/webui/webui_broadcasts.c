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
 * WebUI broadcasts — server-to-client fan-out helpers.
 *
 * Every function in this file iterates the active-connection registry
 * (s_active_connections + s_conn_registry_mutex, declared in
 * webui_internal.h) to push a message to one user's authenticated
 * sessions or to all sessions.  Includes:
 *   - deliver_missed_notifications  (per-connection replay on reconnect)
 *   - scheduler_broadcast_notification / _briefing_notification / route_tts_to_user
 *   - webui_broadcast_conversation_renamed
 *   - webui_broadcast_silent_observation  (silent-observe listener
 *     callback registered with llm_silent_observe in webui_server_init)
 *   - webui_broadcast_context_injection  (Phase 1g-i per-turn focus block)
 *   - webui_broadcast_memory_notice / _memory_proposals_changed
 *
 * Split out of webui_server.c so that file can stay under the size limits
 * in CLAUDE.md.  All cross-module entry points are declared in
 * webui_internal.h; the connection-registry state stays defined in
 * webui_server.c and is reached via the externs in that header.
 */

#include <json-c/json.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "core/attention/attention.h"
#include "core/conv_event.h"
#include "core/focus/focus_candidate_helpers.h"
#include "core/focus/focus_source.h"
#include "core/job_manager.h"
#include "core/job_reinvoke.h"
#include "core/missed_notifications_db.h"
#include "core/scheduler.h"
#include "logging.h"
#include "memory/memory_db_aliases.h"
#include "utils/string_utils.h"
#include "webui/webui_internal.h"
#include "webui/webui_server.h"

/* =============================================================================
 * Scheduler Notification Broadcast
 * ============================================================================= */

/**
 * @brief Deliver queued missed notifications to a newly-authenticated connection.
 *
 * Called after cookie-based authentication completes at WebSocket open. Reads
 * up to 32 queued notifications for the user and emits them as regular
 * scheduler_notification messages with a "missed" flag. Entries remain in the
 * DB until the user explicitly dismisses them (dismiss_missed action), so
 * reloading the page or opening another tab shows the same set.
 */
void deliver_missed_notifications(ws_connection_t *conn) {
   if (!conn || !conn->session || !conn->authenticated || conn->auth_user_id <= 0)
      return;

   missed_notif_t missed[MISSED_NOTIF_DELIVERY_BATCH];
   int count = 0;
   if (missed_notif_get_for_user(conn->auth_user_id, MISSED_NOTIF_DELIVERY_BATCH, missed, &count) !=
           AUTH_DB_SUCCESS ||
       count <= 0)
      return;

   for (int i = 0; i < count; i++) {
      /* Defensively scrub stored free-text before it goes into a WebSocket text
       * frame. Rows persisted before the boundary-safe truncation fix (or any
       * future surprise) may hold invalid UTF-8; an invalid frame fails the
       * connection (RFC 6455 §5.6) and, because missed notifications replay on
       * every reconnect, would wedge the web client in an endless loop. */
      sanitize_utf8_for_json(missed[i].name);
      sanitize_utf8_for_json(missed[i].message);

      json_object *root = json_object_new_object();
      json_object_object_add(root, "type", json_object_new_string("scheduler_notification"));

      json_object *payload = json_object_new_object();
      json_object_object_add(payload, "event_id", json_object_new_int64(missed[i].event_id));
      json_object_object_add(payload, "event_type", json_object_new_string(missed[i].event_type));
      json_object_object_add(payload, "status", json_object_new_string(missed[i].status));
      json_object_object_add(payload, "name", json_object_new_string(missed[i].name));
      json_object_object_add(payload, "message", json_object_new_string(missed[i].message));
      json_object_object_add(payload, "fire_at", json_object_new_int64((int64_t)missed[i].fire_at));
      json_object_object_add(payload, "missed", json_object_new_boolean(true));
      json_object_object_add(payload, "missed_notif_id", json_object_new_int64(missed[i].id));
      if (missed[i].conversation_id > 0) {
         json_object_object_add(payload, "conversation_id",
                                json_object_new_int64(missed[i].conversation_id));
      }

      json_object_object_add(root, "payload", payload);
      const char *json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
      char *json_copy = json_str ? strdup(json_str) : NULL;
      json_object_put(root);

      if (!json_copy)
         continue;

      ws_response_t resp = { .session = conn->session,
                             .type = WS_RESP_SCHEDULER_NOTIFICATION,
                             .scheduler_json = { .json = json_copy } };
      queue_response(&resp);
   }

   OLOG_INFO("WebUI: Delivered %d missed notification(s) to user %d", count, conn->auth_user_id);
}

/**
 * @brief Broadcast scheduler notification to all authenticated WebUI connections
 *
 * Strong symbol that overrides the weak stub in scheduler.c.
 * Queues a response per connected browser client. Satellites are notified
 * separately via satellite_send_response in the scheduler fire logic.
 */
void scheduler_broadcast_notification(const sched_event_t *event, const char *text) {
   if (!event || !text)
      return;

   /* Build JSON notification */
   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("scheduler_notification"));

   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "event_id", json_object_new_int64(event->id));
   json_object_object_add(payload, "event_type",
                          json_object_new_string(sched_event_type_to_str(event->event_type)));
   json_object_object_add(payload, "status",
                          json_object_new_string(sched_status_to_str(event->status)));
   /* Scrub free-text (event name + message) before it enters a WS text frame:
    * a name byte-cut mid-glyph at storage, or any stray invalid byte, would fail
    * the frame (RFC 6455 §5.6) and wedge the client on every (re)broadcast. The
    * replay path already does this; the two live broadcasters must match. */
   char name_safe[SCHED_NAME_MAX];
   snprintf(name_safe, sizeof(name_safe), "%s", event->name);
   sanitize_utf8_for_json(name_safe);
   char *text_safe = strdup(text);
   if (text_safe) {
      sanitize_utf8_for_json(text_safe);
   }
   json_object_object_add(payload, "name", json_object_new_string(name_safe));
   json_object_object_add(payload, "message", json_object_new_string(text_safe ? text_safe : text));
   free(text_safe);
   json_object_object_add(payload, "fire_at", json_object_new_int64((int64_t)event->fire_at));

   /* Tell WebUI clients whether TTS audio is being routed to them (skip chime if so) */
   bool tts_routed = (event->source_client_type == SCHED_SOURCE_WEBUI);
   json_object_object_add(payload, "tts_routed", json_object_new_boolean(tts_routed));

   json_object_object_add(root, "payload", payload);
   const char *json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);

   /* Queue a response for matching clients.
    * If event has a user_id > 0, only notify that user's connections.
    * If user_id == 0 (system event), broadcast to all authenticated connections. */
   int sent = 0;
   int target_user = event->user_id;
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (!conn || !conn->session)
         continue;

      /* Skip unauthenticated non-satellite connections.
       * Note: unassigned satellites (user_id=0) receive system-wide events (target_user=0)
       * intentionally, so all devices get broadcast alerts like alarms. */
      if (!conn->authenticated && !conn->is_satellite)
         continue;

      /* Filter by user if event is user-specific */
      if (target_user > 0 && conn->auth_user_id != target_user)
         continue;

      char *json_copy = strdup(json_str);
      if (!json_copy)
         continue;

      ws_response_t resp = { .session = conn->session,
                             .type = WS_RESP_SCHEDULER_NOTIFICATION,
                             .scheduler_json = { .json = json_copy } };
      queue_response(&resp);
      sent++;
   }

   /* Queue as missed notification when no clients were available. Decision is
    * made and the DB insert runs while the conn mutex is still held so a
    * concurrent reconnect cannot slip between "nobody online" and the insert.
    * Only user-specific ringing events are queued; system events (user_id == 0)
    * and transient rebroadcasts (dismiss/timeout) are dropped. */
   bool queued_missed = false;
   if (sent == 0 && target_user > 0 && event->status == SCHED_STATUS_RINGING) {
      missed_notif_insert(target_user, event->id, sched_event_type_to_str(event->event_type),
                          sched_status_to_str(event->status), event->name, text, event->fire_at, 0);
      queued_missed = true;
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);

   if (sent > 0) {
      OLOG_INFO("Scheduler: Broadcast notification to %d client(s): %s", sent, text);
   } else if (queued_missed) {
      OLOG_INFO("Scheduler: No clients for user %d, queued missed: %s", target_user, text);
   } else {
      OLOG_INFO("Scheduler: no recipients for '%s' (target_user=%d, status=%s) — dropped", text,
                target_user, sched_status_to_str(event->status));
   }

   json_object_put(root);
}

void scheduler_broadcast_briefing_notification(const sched_event_t *event,
                                               const char *text,
                                               int64_t conversation_id) {
   if (!event || !text)
      return;

   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("scheduler_notification"));

   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "event_id", json_object_new_int64(event->id));
   json_object_object_add(payload, "event_type",
                          json_object_new_string(sched_event_type_to_str(event->event_type)));
   json_object_object_add(payload, "status",
                          json_object_new_string(sched_status_to_str(event->status)));
   char name_safe[SCHED_NAME_MAX];
   snprintf(name_safe, sizeof(name_safe), "%s", event->name);
   sanitize_utf8_for_json(name_safe);
   json_object_object_add(payload, "name", json_object_new_string(name_safe));

   /* Truncate preview to ~80 chars, backing up to a UTF-8 character boundary.
    * A blind byte-cut can split a multibyte sequence, leaving an invalid-UTF-8
    * string that fails the WebSocket text frame (RFC 6455 §5.6) when broadcast
    * or replayed — the whole web client then loops on reconnect.  Scrub any
    * interior invalid bytes too (the boundary cap only fixes the tail). */
   char preview[84];
   size_t text_len = strlen(text);
   size_t copy_len = focus_utf8_safe_cap(text, 80);
   memcpy(preview, text, copy_len);
   if (copy_len < text_len) {
      memcpy(preview + copy_len, "...", 4);
   } else {
      preview[copy_len] = '\0';
   }
   sanitize_utf8_for_json(preview);
   json_object_object_add(payload, "message", json_object_new_string(preview));
   json_object_object_add(payload, "fire_at", json_object_new_int64((int64_t)event->fire_at));

   if (conversation_id > 0) {
      json_object_object_add(payload, "conversation_id", json_object_new_int64(conversation_id));
   }

   json_object_object_add(root, "payload", payload);
   const char *json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);

   int sent = 0;
   int target_user = event->user_id;
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (!conn || !conn->session)
         continue;
      if (!conn->authenticated && !conn->is_satellite)
         continue;
      if (target_user > 0 && conn->auth_user_id != target_user)
         continue;

      char *json_copy = strdup(json_str);
      if (!json_copy)
         continue;

      ws_response_t resp = { .session = conn->session,
                             .type = WS_RESP_SCHEDULER_NOTIFICATION,
                             .scheduler_json = { .json = json_copy } };
      queue_response(&resp);
      sent++;
   }

   bool queued_missed = false;
   if (sent == 0 && target_user > 0 && event->status == SCHED_STATUS_RINGING) {
      missed_notif_insert(target_user, event->id, sched_event_type_to_str(event->event_type),
                          sched_status_to_str(event->status), event->name, preview, event->fire_at,
                          conversation_id);
      queued_missed = true;
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);

   if (sent > 0) {
      OLOG_INFO("Scheduler: Broadcast briefing notification to %d client(s)", sent);
   } else if (queued_missed) {
      OLOG_INFO("Scheduler: No clients for user %d, queued missed briefing", target_user);
   } else {
      OLOG_INFO("Scheduler: no recipients for briefing (target_user=%d, status=%s) — dropped",
                target_user, sched_status_to_str(event->status));
   }

   json_object_put(root);
}

/* satellite_send_response: weak/strong sibling to scheduler_*; no public
 * header (kept file-extern by codebase convention for weak-override
 * symbols). scheduler_send_tts_to_session lives in core/scheduler.h. */
extern void satellite_send_response(session_t *session, const char *text);

int scheduler_route_tts_to_user(int user_id,
                                const char *text,
                                const char *skip_uuid,
                                int skip_user_id) {
   if (!text || !text[0])
      return 0;

   int delivered = 0;
   session_t *best_webui = NULL;
   time_t best_activity = 0;

   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (!conn || !conn->session)
         continue;
      if (!conn->authenticated && !conn->is_satellite)
         continue;
      if (user_id > 0 && conn->auth_user_id != user_id)
         continue;
      /* Skip originating user's sessions (for announce_all dedup) */
      if (skip_user_id > 0 && conn->auth_user_id == skip_user_id)
         continue;

      session_t *s = conn->session;
      if (s->disconnected)
         continue;

      if (conn->is_satellite) {
         /* Satellite: send TTS text for local synthesis (Tier 1 only) */
         if (s->type == SESSION_TYPE_DAP2 && s->tier == DAP2_TIER_1) {
            if (skip_uuid && skip_uuid[0] && strcmp(s->identity.uuid, skip_uuid) == 0)
               continue;
            satellite_send_response(s, text);
            delivered++;
            OLOG_INFO("scheduler: TTS routed to satellite %s for user %d", s->identity.uuid,
                      user_id);
         }
      } else {
         /* WebUI: pick the most recently active session to avoid multi-tab echo */
         if (!best_webui || s->last_activity > best_activity) {
            best_webui = s;
            best_activity = s->last_activity;
         }
      }
   }

   /* Retain best WebUI session before releasing mutex (prevents use-after-free
    * if the session disconnects between mutex release and TTS synthesis) */
   if (best_webui)
      session_retain(best_webui);

   pthread_mutex_unlock(&s_conn_registry_mutex);

   /* Send TTS to the best WebUI session (outside the mutex — TTS synthesis blocks) */
   if (best_webui) {
      scheduler_send_tts_to_session(best_webui, text);
      delivered++;
      OLOG_INFO("scheduler: TTS routed to WebUI session %u for user %d", best_webui->session_id,
                user_id);
      session_release(best_webui);
   }

   return delivered;
}

/* =============================================================================
 * Shared per-user JSON fan-out
 *
 * The many "build a {type,payload} object and push it to a user's browser
 * sessions" broadcasters share one loop body.  This helper owns it: it takes
 * ownership of @root, serializes it into a stable string, drops @root BEFORE the
 * registry walk (so the object tree isn't held alive across it), then fans out
 * strdup copies to matching connections.  user_id > 0 delivers to that user's
 * authenticated sessions; user_id <= 0 fans out to every authenticated session.
 * Returns the number of sessions queued.  Thread-safe (registry mutex + response
 * queue).  NOT for the scheduler notification / silent-observation /
 * context-injection paths — those carry per-function variation (missed-queue,
 * satellite filter, WebUI-session gate) that doesn't belong here.
 *
 * @p browsers_only skips satellite connections.  It exists because the return
 * value is now a DECISION for some callers, not just a log line: the job monitor
 * records a completion as delivered when this returns > 0, and a satellite
 * cannot render a browser toast — so counting one would fire the row and show
 * the user nothing.  Callers that only log the count pass false and keep the
 * historical fan-out.
 * ============================================================================= */
static int broadcast_json_to_user_ex(int user_id, json_object *root, bool browsers_only) {
   if (!root)
      return 0;

   const char *json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
   char *json_cached = json_str ? strdup(json_str) : NULL;
   json_object_put(root); /* drop the tree before the walk */
   if (!json_cached)
      return 0;

   int sent = 0;
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (!conn || !conn->session || !conn->authenticated)
         continue;
      if (user_id > 0 && conn->auth_user_id != user_id)
         continue;
      if (browsers_only && conn->is_satellite)
         continue;

      char *json_copy = strdup(json_cached);
      if (!json_copy)
         continue;

      ws_response_t resp = { .session = conn->session,
                             .type = WS_RESP_JSON,
                             .generic_json = { .json = json_copy } };
      queue_response(&resp);
      sent++;
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);

   free(json_cached);
   return sent;
}

/* =============================================================================
 * Conversation Title Broadcast (called from memory extraction thread)
 * ============================================================================= */

void webui_broadcast_conversation_renamed(int user_id, int64_t conv_id, const char *title) {
   if (user_id <= 0 || !title)
      return;

   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("conversation_renamed"));

   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "conversation_id", json_object_new_int64(conv_id));
   json_object_object_add(payload, "title", json_object_new_string(title));
   json_object_object_add(root, "payload", payload);

   int sent = broadcast_json_to_user_ex(user_id, root, false);
   if (sent > 0) {
      OLOG_INFO("WebUI: Broadcast conversation rename to %d client(s): %s", sent, title);
   }
}

/* Strong override of the conv_event (Layer 2) weak seam: push one durable
 * observe-side step to the owner's attached clients (background-jobs Phase 2).
 *
 * Carries the DB-assigned `seq` so a client that receives this live while its
 * attach replay is still in flight can dedup against the replayed batch — the
 * one ordering hazard in the attach contract (§6).
 *
 * Fans out to ALL of the user's connections rather than the one viewing the
 * conversation: a jobs panel watches conversations the user is NOT currently
 * looking at, which is the entire point of a background job.  The client routes
 * by conversation_id, exactly as it already does for conversation-scoped
 * streaming frames. */
/* Defined below; used by the two broadcasters above its definition. */
static bool any_session_matches(int user_id, int64_t conv_id_or_zero);

void webui_broadcast_conversation_event(int user_id,
                                        int64_t conv_id,
                                        int64_t seq,
                                        const char *kind,
                                        const char *payload) {
   if (user_id <= 0 || conv_id <= 0 || kind == NULL) {
      return;
   }
   /* Every job turn is observable, so an unwatched "spawn and walk away" job
    * still emits status/tool_call/tool_result/complete here.  broadcast_json_to_
    * user_ex serializes + strdups the (up-to-16 KB) payload BEFORE it walks for
    * recipients, so with no session of this user connected that is pure waste
    * (tens of KB per event, ~1 MB over a research job).  Pre-flight it — user-
    * scoped (conv 0) to match the user-scoped broadcast, not conv-scoped. */
   if (!any_session_matches(user_id, 0)) {
      return;
   }

   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("conversation_event"));

   json_object *p = json_object_new_object();
   json_object_object_add(p, "conversation_id", json_object_new_int64(conv_id));
   json_object_object_add(p, "seq", json_object_new_int64(seq));
   json_object_object_add(p, "kind", json_object_new_string(kind));
   /* payload is pre-redacted + pre-capped JSON (event_payload.c).  Forwarded as
    * an opaque STRING, not re-parsed: the renderer treats it as untrusted text
    * (§8.7), so parsing it here would only invite a consumer to trust it. */
   if (payload) {
      json_object_object_add(p, "payload", json_object_new_string(payload));
   }
   json_object_object_add(root, "payload", p);

   /* NO json_object_put(root) here: broadcast_json_to_user TAKES OWNERSHIP and
    * drops the tree itself before the registry walk (see its contract above).
    * Every sibling broadcaster in this file relies on that; adding a second put
    * is a double free. */
   broadcast_json_to_user_ex(user_id, root, false);
}

/* Strong override of the conv_event (Layer 2) weak seam: deliver a persisted
 * assistant message WITH its body, so a client that only consumes the event
 * stream still receives the answer (§6.3, U-Crit).
 *
 * The existing conversation_messages_appended broadcast below is signal-only and
 * cannot serve this: it says "go refetch", which a line-printer or TUI has no
 * way to do. */
void webui_broadcast_message_appended(int user_id,
                                      int64_t conv_id,
                                      int64_t msg_id,
                                      const char *role,
                                      const char *text) {
   if (user_id <= 0 || conv_id <= 0 || text == NULL) {
      return;
   }
   /* Same waste as the event broadcaster above: an unwatched job's answer body
    * would be serialized + strdup'd for nobody.  User-scoped pre-flight. */
   if (!any_session_matches(user_id, 0)) {
      return;
   }

   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("message_appended"));

   json_object *p = json_object_new_object();
   json_object_object_add(p, "conversation_id", json_object_new_int64(conv_id));
   json_object_object_add(p, "message_id", json_object_new_int64(msg_id));
   json_object_object_add(p, "role", json_object_new_string(role ? role : "assistant"));
   json_object_object_add(p, "text", json_object_new_string(text));
   json_object_object_add(root, "payload", p);

   /* broadcast_json_to_user TAKES OWNERSHIP — no json_object_put here. */
   broadcast_json_to_user_ex(user_id, root, false);
}

/* Strong override of the job_reinvoke (Layer 2) weak seam: when a reinvoke_parent
 * re-engagement persists its reply to the parent conversation, refresh any open
 * tab viewing it by reusing the conversation_messages_appended broadcast. */
void job_reinvoke_notify_conv_appended(int user_id, int64_t conversation_id) {
   webui_broadcast_conversation_messages_appended(user_id, conversation_id);
}

void webui_broadcast_conversation_messages_appended(int user_id, int64_t conv_id) {
   if (user_id <= 0 || conv_id <= 0)
      return;

   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("conversation_messages_appended"));

   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "conversation_id", json_object_new_int64(conv_id));
   json_object_object_add(root, "payload", payload);

   int sent = broadcast_json_to_user_ex(user_id, root, false);
   if (sent > 0) {
      OLOG_INFO("WebUI: Broadcast conversation_messages_appended (conv=%lld) to %d client(s)",
                (long long)conv_id, sent);
   }
}

/* =============================================================================
 * Pre-flight registry scan (Phase 1j optimization)
 *
 * Several broadcasters today build a JSON payload, then iterate the
 * connection registry under the registry mutex, then drop the payload
 * when no recipients matched.  When the matching pool is empty (common
 * for per-user / per-conversation broadcasts on a quiet daemon), the
 * payload-build cost is wasted.
 *
 * `any_session_matches` does a cheap pre-flight scan under the registry
 * mutex and lets the broadcaster bail before doing any JSON work.
 *
 * Semantics — must match the broadcaster's iteration filter exactly so
 * the optimization is pure efficiency (no behavior change):
 *
 *   user_id == 0 → broadcast-to-all WebUI authenticated sessions
 *                  (silent_observation's system-scoped observation form)
 *   user_id  > 0 → only that user's authenticated WebUI sessions
 *
 *   conv_id_or_zero == 0 → don't filter by conversation (silent_observation
 *                          + future per-user broadcasts)
 *   conv_id_or_zero  > 0 → only sessions whose currently-active conv id
 *                          matches (context_injection per-turn delivery)
 *
 * SESSION_TYPE_WEBUI gate is hardcoded on purpose:
 *   - context_injection's broadcaster body already enforces it
 *   - silent_observation is a WebUI-rail-icon UI primitive (Phase 0
 *     design): satellites don't render the peek popup, and tightening
 *     the broadcaster body to match here removes the latent
 *     accidental-delivery to authenticated Tier-1 satellites
 *
 * NOT used for scheduler_broadcast_notification or
 * scheduler_broadcast_briefing_notification — those insert a
 * missed_notif DB row inside the registry mutex when sent == 0, so a
 * pre-flight skip would silently drop missed-alarm rows.
 *
 * TOCTOU note: between pre-flight and the broadcaster's own iteration,
 * a session could disconnect.  Behavior is identical to today
 * (broadcaster iterates, finds nothing, returns) — pure efficiency,
 * no correctness regression.
 * ============================================================================= */
static bool any_session_matches(int user_id, int64_t conv_id_or_zero) {
   pthread_mutex_lock(&s_conn_registry_mutex);
   bool found = false;
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (!conn || !conn->session || !conn->authenticated || !conn->wsi)
         continue;
      if (conn->session->type != SESSION_TYPE_WEBUI)
         continue;
      /* user_id == 0: broadcast-to-all; any authenticated WebUI session
       * matches.  user_id > 0: per-user. */
      if (user_id > 0 && conn->auth_user_id != user_id)
         continue;
      if (conv_id_or_zero > 0 && webui_get_active_conversation_id(conn->session) != conv_id_or_zero)
         continue;
      found = true;
      break;
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);
   OLOG_DEBUG("any_session_matches: user=%d conv=%lld → %d", user_id, (long long)conv_id_or_zero,
              found ? 1 : 0);
   return found;
}

/* =============================================================================
 * Memory Extraction Notice Broadcast
 * ============================================================================= */

/**
 * @brief Broadcast a silent-observation event to active WebUI sessions.
 *
 * Wired up as the listener for llm_silent_observe() — see webui_server_init().
 * Called synchronously from the silent-observe call thread; copies into the
 * per-connection response queue so the WebSocket service thread does the actual
 * send on its own loop.
 *
 * Routing rules:
 *   - user_id > 0 → only that user's authenticated sessions
 *   - user_id == 0 → all authenticated sessions (system-scoped observation)
 *
 * @param category      Allowlisted category, or "filtered" for filter rejections.
 * @param note          Sanitized note text or filter-match placeholder.
 * @param user_id       Owning user (0 = system-wide).
 * @param filter_match  True when this event represents a filter rejection.
 */
void webui_broadcast_silent_observation(const char *category,
                                        const char *note,
                                        int user_id,
                                        bool filter_match) {
   if (!category || !note)
      return;

   /* Pre-flight registry scan — bail before the json_object_new_*
    * cluster when no WebUI session would match.  silent_observation
    * is a WebUI-only UI primitive (rail icon + peek popup), so the
    * pre-flight gate matches the broadcaster body's filter below
    * (with SESSION_TYPE_WEBUI added in the same change). */
   if (!any_session_matches(user_id, /*conv_id_or_zero*/ 0))
      return;

   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("silent_observation"));

   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "ts", json_object_new_int64((int64_t)time(NULL)));
   json_object_object_add(payload, "category", json_object_new_string(category));
   json_object_object_add(payload, "note", json_object_new_string(note));
   json_object_object_add(payload, "filter_match", json_object_new_boolean(filter_match));
   json_object_object_add(root, "payload", payload);

   /* Serialize ONCE into a heap buffer, drop the json_object, then strdup
    * per connection from the cached canonical form.  The previous shape ran
    * the json_object's internal serializer N times on identical data and
    * held `root` alive for the duration of registry iteration. */
   const char *json_view = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
   char *json_canonical = json_view ? strdup(json_view) : NULL;
   json_object_put(root);
   if (!json_canonical)
      return;

   int sent = 0;
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (!conn || !conn->session || !conn->authenticated || !conn->wsi)
         continue;
      /* WebUI-only: silent_observation is consumed by the rail-icon
       * peek popup; satellites don't render it.  Aligns with the
       * pre-flight gate above. */
      if (conn->session->type != SESSION_TYPE_WEBUI)
         continue;
      if (user_id > 0 && conn->auth_user_id != user_id)
         continue;

      char *json_copy = strdup(json_canonical);
      if (!json_copy)
         continue;

      ws_response_t resp = { .session = conn->session,
                             .type = WS_RESP_JSON,
                             .generic_json = { .json = json_copy } };
      queue_response(&resp);
      sent++;
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);

   free(json_canonical);

   if (sent > 0) {
      OLOG_INFO(
          "WebUI: Broadcast silent_observation (category=%s, filter_match=%d) to %d client(s)",
          category, filter_match ? 1 : 0, sent);
   }
}

/* =============================================================================
 * Phase 1g-i: context_injection broadcast.  See webui_server.h doxygen for
 * the full contract; the wire format is the design-doc rev 3 §"UI surface"
 * shape.
 * ============================================================================= */

/* Per-source enum → wire-format string.  Mirrors the design-doc lowercase
 * variants ("internal" / "external" / "user-content").  Keeps the C-side
 * representation (typed enum) stable while the JSON boundary stays string-
 * keyed for forward-compat. */
static const char *focus_source_type_str(focus_source_type_t t) {
   switch (t) {
      case FOCUS_SOURCE_INTERNAL:
         return "internal";
      case FOCUS_SOURCE_EXTERNAL:
         return "external";
      case FOCUS_SOURCE_USER_CONTENT:
         return "user-content";
   }
   /* Defensive — unreachable under typed enum, but keeps the compiler
    * happy and gives non-noisy output if a future enum value sneaks in
    * without a switch update. */
   return "unknown";
}

/* Defensive size-cap on candidate text.  Adapters truncate to this cap
 * already (focus_candidate_helpers.h), but a misbehaving adapter could
 * still emit oversize text — JSON-encoding a 1MB string into a server-
 * to-client message is a DoS surface, so cap belt-and-suspenders.
 *
 * Returns either `text` (when within cap) OR a freshly allocated
 * truncated copy that the caller must free.  `*out_owned` is set true
 * iff the caller takes ownership. */
static const char *capped_text_view(const char *text, char **owned, bool *out_owned) {
   *owned = NULL;
   *out_owned = false;
   if (text == NULL)
      return "";
   const size_t n = strlen(text);
   if (n <= FOCUS_TEXT_MAX_BYTES)
      return text;
   /* UTF-8-safe cut: a raw truncation at FOCUS_TEXT_MAX_BYTES can split a
    * multi-byte character, making this payload's text frame invalid UTF-8
    * (the browser then drops the WebSocket). Back the cut up to a char boundary. */
   const size_t cap = focus_utf8_safe_cap(text, FOCUS_TEXT_MAX_BYTES);
   *owned = malloc(cap + 1);
   if (*owned == NULL) {
      /* OOM — drop text content rather than fail the broadcast.  Log
       * the failure (security audit LOW): under sustained memory
       * pressure with an adapter emitting oversize text every turn,
       * this path silently degrades the UX otherwise. */
      OLOG_WARNING("WebUI: context_injection capped_text_view OOM (text len=%zu); "
                   "emitting empty string",
                   n);
      return "";
   }
   memcpy(*owned, text, cap);
   (*owned)[cap] = '\0';
   *out_owned = true;
   return *owned;
}

void webui_broadcast_context_injection(int user_id,
                                       int64_t conv_id,
                                       int64_t turn_id,
                                       const focus_compose_result_t *result) {
   if (result == NULL || user_id <= 0 || conv_id <= 0) {
      OLOG_DEBUG("WebUI: context_injection broadcast skipped (user=%d conv=%lld result=%p)",
                 user_id, (long long)conv_id, (const void *)result);
      return;
   }

   /* Pre-flight registry scan — bail before per-candidate JSON object
    * construction (the inner loop at result->candidate_count creates an
    * item object with score_breakdown sub-object per candidate) when no
    * matching session is open.  Three-gate routing (SESSION_TYPE_WEBUI +
    * user + conv) is preserved by the helper. */
   if (!any_session_matches(user_id, conv_id))
      return;

   /* Build the JSON payload ONCE; strdup the canonical string into each
    * recipient's response queue.  Mirrors the silent-observation
    * pattern at line 6428 — same rationale (avoid running the json_object
    * serializer N times on identical data). */
   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("context_injection"));
   json_object_object_add(root, "user_id", json_object_new_int(user_id));
   json_object_object_add(root, "conversation_id", json_object_new_int64(conv_id));
   json_object_object_add(root, "turn_id", json_object_new_int64(turn_id));

   json_object *items = json_object_new_array();
   for (int i = 0; i < result->candidate_count; i++) {
      const focus_candidate_t *c = &result->candidates[i];
      const focus_score_breakdown_t *b = (result->score_breakdowns != NULL)
                                             ? &result->score_breakdowns[i]
                                             : NULL;

      json_object *item = json_object_new_object();
      json_object_object_add(item, "source_id",
                             json_object_new_string(c->source_id ? c->source_id : ""));
      json_object_object_add(item, "source_type",
                             json_object_new_string(focus_source_type_str(c->source_type)));

      char *owned_text = NULL;
      bool owned = false;
      const char *text_view = capped_text_view(c->text, &owned_text, &owned);
      json_object_object_add(item, "text", json_object_new_string(text_view));
      if (owned)
         free(owned_text);

      json_object_object_add(item, "score",
                             json_object_new_double(b != NULL ? b->final_score : 0.0));

      json_object *bd = json_object_new_object();
      json_object_object_add(bd, "semantic",
                             json_object_new_double(b != NULL ? b->semantic_contribution : 0.0));
      json_object_object_add(bd, "recency",
                             json_object_new_double(b != NULL ? b->recency_contribution : 0.0));
      json_object_object_add(bd, "importance",
                             json_object_new_double(b != NULL ? b->importance_contribution : 0.0));
      json_object_object_add(bd, "source",
                             json_object_new_double(b != NULL ? b->source_contribution : 0.0));
      json_object_object_add(item, "score_breakdown", bd);
      json_object_object_add(item, "applied_source_weight",
                             json_object_new_double(b != NULL ? b->applied_source_weight : 0.0));

      /* Provenance integrity binding (design rev 3 line 279): omit
       * entirely when conv_id == 0 rather than emitting a zero-stub.
       * The struct field is `conv_id` (memory_types.h); the wire-
       * format key is `conversation_id` to match the design-doc JSON
       * example.  Clients reading provenance treat absence as
       * "not available." */
      if (c->provenance.conv_id != 0) {
         json_object *prov = json_object_new_object();
         json_object_object_add(prov, "conversation_id",
                                json_object_new_int64(c->provenance.conv_id));
         json_object_object_add(prov, "msg_id_start",
                                json_object_new_int64(c->provenance.msg_id_start));
         json_object_object_add(prov, "msg_id_end",
                                json_object_new_int64(c->provenance.msg_id_end));
         json_object_object_add(item, "provenance", prov);
      }

      json_object_array_add(items, item);
   }
   json_object_object_add(root, "items", items);

   /* filter_rejections[] — capped at MAX_FOCUS_SOURCES by struct shape;
    * each entry is {source_id, count}.  Counts are >=1 by rejection_bump
    * construction; the >0 guard is defensive against any future change. */
   json_object *rejections = json_object_new_array();
   for (int i = 0; i < result->rejection_count; i++) {
      const focus_filter_rejection_t *r = &result->rejections[i];
      if (r->count <= 0 || r->source_id == NULL)
         continue;
      json_object *rej = json_object_new_object();
      json_object_object_add(rej, "source_id", json_object_new_string(r->source_id));
      json_object_object_add(rej, "count", json_object_new_int(r->count));
      json_object_array_add(rejections, rej);
   }
   json_object_object_add(root, "filter_rejections", rejections);

   const char *json_view = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
   char *json_canonical = json_view ? strdup(json_view) : NULL;
   json_object_put(root);
   if (json_canonical == NULL)
      return;

   int sent = 0;
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (!conn || !conn->session || !conn->authenticated || !conn->wsi)
         continue;
      /* SESSION_TYPE_WEBUI gate is mandatory.  dawn_build_prompt runs
       * for LOCAL / DAP / DAP2 / WEBUI; only WEBUI sessions have a
       * `client_data` cast-able to ws_connection_t* and a browser tab
       * to deliver the event to.  Other types reach this point with
       * a positive conv_id but never have a matching session in
       * s_active_connections (registered only on WS auth), so the
       * type check is technically redundant given the registry
       * shape — keep it explicit per architectural invariant. */
      if (conn->session->type != SESSION_TYPE_WEBUI)
         continue;
      if (conn->auth_user_id != user_id)
         continue;
      if (webui_get_active_conversation_id(conn->session) != conv_id)
         continue;

      char *json_copy = strdup(json_canonical);
      if (json_copy == NULL)
         continue;
      ws_response_t resp = { .session = conn->session,
                             .type = WS_RESP_JSON,
                             .generic_json = { .json = json_copy } };
      queue_response(&resp);
      sent++;
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);

   free(json_canonical);

   if (sent > 0) {
      OLOG_DEBUG("WebUI: Broadcast context_injection (user=%d conv=%lld turn=%lld items=%d) to %d "
                 "client(s)",
                 user_id, (long long)conv_id, (long long)turn_id, result->candidate_count, sent);
   }
}

void webui_broadcast_memory_notice(int user_id, const char *level, const char *message) {
   if (user_id <= 0 || !level || !message)
      return;

   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("memory_extraction_notice"));

   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "level", json_object_new_string(level));
   json_object_object_add(payload, "message", json_object_new_string(message));
   json_object_object_add(root, "payload", payload);

   int sent = broadcast_json_to_user_ex(user_id, root, false);
   if (sent > 0) {
      OLOG_INFO("WebUI: Broadcast memory notice (%s) to %d client(s)", level, sent);
   }
}

/* SAGE proactive-attention banner — strong override of the weak stub in
 * attention_core.c.  Its own channel (not scheduler_notification) so it shows an
 * ATTENTION badge and never triggers the scheduler client chime. */
void webui_broadcast_attention_alert(int user_id, const char *summary, const char *level) {
   if (user_id <= 0 || !summary || !level)
      return;

   /* Scrub free-text before it enters a WS text frame (RFC 6455 §5.6): a stray
    * invalid byte would fail the frame and wedge the client.  Stack buffer (no
    * alloc, no OOM-fallback-sends-unsanitized gap). */
   char summary_safe[SAGE_SUMMARY_LEN];
   snprintf(summary_safe, sizeof(summary_safe), "%s", summary);
   sanitize_utf8_for_json(summary_safe);

   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("attention_alert"));
   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "summary", json_object_new_string(summary_safe));
   json_object_object_add(payload, "level", json_object_new_string(level));
   json_object_object_add(root, "payload", payload);

   int sent = broadcast_json_to_user_ex(user_id, root, false);
   if (sent > 0) {
      OLOG_INFO("WebUI: attention alert (%s) to %d client(s)", level, sent);
   }
}

/* Strong override of the weak job-notification symbol in job_manager.c: a
 * silent job-completion toast to the owner's browser sessions (no voice). */
int webui_broadcast_job_notification(int user_id,
                                     const char *text,
                                     int64_t conv_id,
                                     int running_count) {
   if (user_id <= 0 || !text) {
      return 0;
   }
   char text_safe[512];
   snprintf(text_safe, sizeof(text_safe), "%s", text);
   sanitize_utf8_for_json(text_safe);

   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("job_notification"));
   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "text", json_object_new_string(text_safe));
   json_object_object_add(payload, "conv_id", json_object_new_int64(conv_id));
   json_object_object_add(payload, "running", json_object_new_int(running_count));
   json_object_object_add(root, "payload", payload);

   /* Browsers only, and the count is load-bearing: the monitor records the
    * completion as delivered when this returns > 0, so counting a satellite —
    * which has no toast surface — would fire the row and show the user nothing. */
   int sent = broadcast_json_to_user_ex(user_id, root, /*browsers_only=*/true);
   if (sent > 0) {
      OLOG_INFO("WebUI: job notification to %d browser client(s)", sent);
   }
   return sent;
}

/* The job lifecycle frames (job_update / jobs_snapshot / list_jobs_response)
 * live in webui_jobs.c — see the lifetime-split rationale at the top of it. */

/* Strong override of the job_reinvoke weak seam: find a live, idle, connected
 * WebUI session the owner is currently viewing on @p conv_id, and atomically
 * claim a turn on it so a reinvoke result can stream in (warm cache, native
 * token stream).  Claim-under-registry-lock so a concurrent user turn either
 * hasn't started (turn_in_flight==0 gate) or supersedes us afterward via
 * request_generation.  Returns the retained+claimed session, or NULL. */
session_t *webui_find_reinvoke_viewer(int64_t conv_id, int user_id) {
   if (conv_id <= 0 || user_id <= 0) {
      return NULL;
   }
   session_t *found = NULL;
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (!conn || !conn->session || !conn->authenticated) {
         continue;
      }
      if (conn->auth_user_id != user_id || conn->session->type != SESSION_TYPE_WEBUI) {
         continue;
      }
      if (webui_get_active_conversation_id(conn->session) != conv_id) {
         continue;
      }
      session_t *s = conn->session;
      /* A connected viewer of the parent conv.  Do NOT skip a mid-turn session:
       * the turn queue serializes the reinvoke behind the active turn (that is the
       * whole point — no second gate).  Retain ONLY; the turn claim
       * (begin_turn_flags + request_generation + turn_in_flight + stream binding)
       * happens at DEQUEUE in the reinvoke turn closure, being_destroyed-safe. */
      /* Skip a disconnected OR tearing-down session: no live viewer to stream
       * into → the caller takes the detached path.  (being_destroyed is redundant
       * with disconnected today — teardown sets disconnected first — but checking
       * it explicitly makes the teardown-safety self-evident and independent of
       * session_destroy's phase ordering.) */
      if (atomic_load(&s->disconnected) || atomic_load(&s->being_destroyed)) {
         continue;
      }
      session_retain(s);
      found = s;
      break;
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);
   return found;
}

/* Strong override of the Layer-2 weak seam: the conversation the session's client
 * is currently viewing (used by the reinvoke closure's server-persist decision). */
int64_t webui_session_active_conversation(session_t *s) {
   return webui_get_active_conversation_id(s);
}

void webui_broadcast_memory_proposals_changed(int user_id) {
   if (user_id <= 0)
      return;

   /* Re-fetch the count via the memory-db helper so the broadcast is
    * race-free against concurrent inserts/resolves.  Pending = resolved_at
    * IS NULL. */
   int pending = 0;
   memory_db_proposal_count_pending(user_id, &pending);

   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("memory_proposals_changed"));
   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "count", json_object_new_int(pending));
   json_object_object_add(root, "payload", payload);

   int sent = broadcast_json_to_user_ex(user_id, root, false);
   if (sent > 0) {
      OLOG_INFO("WebUI: Broadcast memory_proposals_changed (count=%d) to %d client(s)", pending,
                sent);
   }
}

/*
 * Strong symbol that overrides the weak stub in scheduler.c.
 * Empty payload — clients refetch the queue on receipt.
 */
void scheduler_broadcast_events_changed(int user_id) {
   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("scheduler_events_changed"));
   json_object_object_add(root, "payload", json_object_new_object());

   /* user_id <= 0 ("system event", e.g. the startup missed-recovery sweep) fans
    * out to every authenticated session — handled by the helper's convention. */
   int sent = broadcast_json_to_user_ex(user_id, root, false);
   if (sent > 0) {
      OLOG_INFO("WebUI: Broadcast scheduler_events_changed (user=%d) to %d client(s)", user_id,
                sent);
   }
}

#ifdef DAWN_ENABLE_CODE_PROJECTS
#include "tools/code_project_service.h"

/* Strong override of the weak code_project_broadcast_status_changed stub in
 * code_project_service.c. Pushes a lightweight ping to every authenticated
 * browser so each re-fetches its own (access-scoped) project list; the opaque
 * project id carries nothing sensitive. Called from the import worker thread and
 * from import/delete, so it must use the thread-safe response queue. */
void code_project_broadcast_status_changed(int64_t project_id) {
   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("code_project_status_changed"));
   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "project_id", json_object_new_int64(project_id));
   json_object_object_add(root, "payload", payload);

   /* Ping every authenticated browser (user_id <= 0 = fan out to all). */
   broadcast_json_to_user_ex(0, root, false);
}

/* Strong override of the weak code_project_broadcast_import_failed stub. A
 * pending import was rejected before any row was created (repo not found /
 * unreachable, or a duplicate that raced the pre-check), so there is nothing to
 * appear in the list — instead toast only the requesting user. Called from the
 * import worker thread; uses the thread-safe response queue. */
void code_project_broadcast_import_failed(int64_t user_id, const char *name, const char *reason) {
   if (user_id <= 0) {
      return;
   }
   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("code_project_import_failed"));
   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "name", json_object_new_string(name != NULL ? name : ""));
   json_object_object_add(payload, "reason", json_object_new_string(reason != NULL ? reason : ""));
   json_object_object_add(root, "payload", payload);

   /* Toast only the requesting user. */
   broadcast_json_to_user_ex((int)user_id, root, false);
}
#endif /* DAWN_ENABLE_CODE_PROJECTS */
