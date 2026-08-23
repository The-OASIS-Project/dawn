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
 * Messaging engine — session-slot map.
 *
 * The in-memory (provider, provider_address) -> session_t* slot table: LRU
 * eviction, get-or-create with post-restart history restore + per-turn name
 * refresh, the cross-channel staleness reload, and the self-reset deferral
 * probe.  Split out of messaging_engine.c; see messaging_engine_internal.h
 * and docs/MESSAGING_ENGINE_SPLIT_PLAN.md.
 */
#define AUTH_DB_INTERNAL_ALLOWED
#define MESSAGING_ENGINE_INTERNAL_ALLOWED

#include <json-c/json.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "llm/llm_interface.h"
#include "logging.h"
#include "memory/memory_history_loader.h"
#include "messaging/messaging_engine.h"
#include "messaging/messaging_engine_internal.h"

/* messaging_conv_get_max_msg_id and history_array_max_msg_id are file-local
 * helpers (used only by the staleness reload + history restore below). */

/* Find the in-memory session slot for (provider, provider_address) and
 * evict it.  Eviction triggers memory extraction on the closing
 * conversation via session_destroy's existing extraction hook.  No-op
 * when no slot matches (channel never received an inbound this
 * daemon-uptime).  Drops s_session_slots_mutex before calling
 * session_destroy per the same lock-order rule the LRU path follows
 * (per-module → global is forbidden). */
void evict_session_slot(const char *provider, const char *provider_address) {
   if (!provider || !provider_address) {
      return;
   }

   pthread_mutex_lock(&s_session_slots_mutex);
   session_t *evictee = NULL;
   uint32_t evictee_session_id = 0;
   for (size_t i = 0; i < MESSAGING_MAX_SESSIONS; i++) {
      if (s_session_slots[i].session && strcmp(s_session_slots[i].provider, provider) == 0 &&
          strcmp(s_session_slots[i].provider_address, provider_address) == 0) {
         evictee = s_session_slots[i].session;
         evictee_session_id = evictee->session_id;
         memset(&s_session_slots[i], 0, sizeof(session_slot_t));
         break;
      }
   }
   pthread_mutex_unlock(&s_session_slots_mutex);

   if (evictee) {
      OLOG_INFO("messaging: evicting session slot for %s:%s (session_id=%u)", provider,
                provider_address, evictee_session_id);
      /* Drop the engine's retain so session_destroy's ref-count wait
       * converges immediately.  session_destroy fires memory extraction
       * for the closing conversation. */
      session_release(evictee);
      session_destroy(evictee_session_id);
   }
}

/* Query the highest msg_id present for a conversation.  Returns 0 on
 * lock failure or an empty conversation.  Used by the staleness check
 * in process_inbound to detect external writers (WebUI conversation
 * panel, voice session, MCP) appending to the same conv between
 * messaging-channel turns. */
static int64_t messaging_conv_get_max_msg_id(int64_t conv_id) {
   if (conv_id <= 0) {
      return 0;
   }
   AUTH_DB_LOCK_OR_RETURN(0);
   sqlite3_stmt *stmt = NULL;
   int64_t max_id = 0;
   const char *sql = "SELECT COALESCE(MAX(id), 0) FROM messages WHERE conversation_id = ?";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, conv_id);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         max_id = sqlite3_column_int64(stmt, 0);
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();
   return max_id;
}

/* Walk a loaded history array and return the highest msg_id stamped
 * on any entry.  memory_history_load_from_db adds `id` to every
 * loaded entry — returns 0 if no entries carry one (defensive). */
static int64_t history_array_max_msg_id(struct json_object *history) {
   if (!history) {
      return 0;
   }
   int64_t max_id = 0;
   size_t n = (size_t)json_object_array_length(history);
   for (size_t i = 0; i < n; i++) {
      struct json_object *entry = json_object_array_get_idx(history, i);
      struct json_object *id_obj = NULL;
      if (entry && json_object_object_get_ex(entry, "id", &id_obj)) {
         int64_t id = json_object_get_int64(id_obj);
         if (id > max_id) {
            max_id = id;
         }
      }
   }
   return max_id;
}

/* Update the slot's last_known_msg_id to `max(current, msg_id)`.  Slot
 * lookup by session pointer is safe because the caller holds a retain
 * on the session — eviction (the only path that nulls the slot's
 * session pointer) can't happen concurrently. */
void slot_bump_last_known_msg_id(session_t *session, int64_t msg_id) {
   if (!session || msg_id <= 0) {
      return;
   }
   pthread_mutex_lock(&s_session_slots_mutex);
   for (size_t i = 0; i < MESSAGING_MAX_SESSIONS; i++) {
      if (s_session_slots[i].session == session) {
         if (msg_id > s_session_slots[i].last_known_msg_id) {
            s_session_slots[i].last_known_msg_id = msg_id;
         }
         break;
      }
   }
   pthread_mutex_unlock(&s_session_slots_mutex);
}

/* Cross-channel staleness check.  When an external writer (WebUI,
 * voice, MCP) appends to a conv between messaging turns, the cached
 * session_t holds frozen in-memory history and the LLM responds
 * without seeing those messages.  Detect via DB MAX(id) > slot's
 * last_known_msg_id; recover by reloading session->conversation_history
 * from DB.
 *
 * Universal across all messaging providers — every channel funnels
 * through process_inbound which calls this, so SMS / Telegram /
 * future Discord/Slack all get the fix for free.
 *
 * Called by process_inbound BEFORE dispatch, so the channel-hint
 * builder (which inspects the last assistant message for truncation
 * markers) and the LLM call both see the reloaded history. */
void reload_session_history_if_stale(session_t *session,
                                     const char *provider,
                                     const char *provider_address,
                                     int64_t conv_id,
                                     int user_id) {
   if (!session || !provider || !provider_address || conv_id <= 0 || user_id <= 0) {
      return;
   }

   /* Read the slot's last_known_msg_id under the slot mutex, release
    * before the DB query. */
   int64_t slot_last_known = 0;
   pthread_mutex_lock(&s_session_slots_mutex);
   for (size_t i = 0; i < MESSAGING_MAX_SESSIONS; i++) {
      if (s_session_slots[i].session == session) {
         slot_last_known = s_session_slots[i].last_known_msg_id;
         break;
      }
   }
   pthread_mutex_unlock(&s_session_slots_mutex);

   int64_t db_max = messaging_conv_get_max_msg_id(conv_id);
   if (db_max <= slot_last_known) {
      return; /* no drift — common case */
   }

   /* Drift detected — external writer added messages.  Reload. */
   OLOG_INFO("messaging: history drift on %s:%s (db_max=%lld > last_known=%lld); reloading",
             provider, provider_address, (long long)db_max, (long long)slot_last_known);

   size_t restored_chars = 0;
   struct json_object *loaded = memory_history_load_from_db(conv_id, user_id, &restored_chars);
   if (!loaded) {
      return;
   }
   size_t restored_count = (size_t)json_object_array_length(loaded);

   pthread_mutex_lock(&session->history_mutex);
   if (session->conversation_history) {
      json_object_put(session->conversation_history);
   }
   session->conversation_history = loaded;
   pthread_mutex_unlock(&session->history_mutex);

   int64_t new_high = history_array_max_msg_id(loaded);
   slot_bump_last_known_msg_id(session, new_high);

   OLOG_INFO("messaging: reloaded %zu messages (%zu chars) from conv %lld; last_known now %lld",
             restored_count, restored_chars, (long long)conv_id, (long long)new_high);
}

/* Try to detect that the caller is the LLM running on the same
 * session the reset would destroy.  When this returns true, the
 * caller MUST treat the reset as deferred (set pending_reset on the
 * slot, return MESSAGING_SUCCESS, let process_inbound handle it after
 * dispatch).  Synchronous reset on a self-targeting call leads to
 * UAF: session_destroy times out waiting for the worker's retain to
 * drop, frees the session_t anyway, worker resumes on freed memory.
 *
 * Detection: session_get_command_context() returns the session
 * currently executing tool callbacks on this thread.  We compare its
 * session_id against the slot's session for (provider, address).  A
 * match means the LLM tool is running on THIS slot's session — exact
 * self-reset case.  Non-match (or no command context, or no slot
 * yet) → immediate reset is safe. */
bool mark_pending_reset_if_self(const char *provider, const char *provider_address) {
   session_t *caller = session_get_command_context();
   if (!caller) {
      return false;
   }
   uint32_t caller_id = caller->session_id;

   pthread_mutex_lock(&s_session_slots_mutex);
   bool self_reset = false;
   for (size_t i = 0; i < MESSAGING_MAX_SESSIONS; i++) {
      if (s_session_slots[i].session && strcmp(s_session_slots[i].provider, provider) == 0 &&
          strcmp(s_session_slots[i].provider_address, provider_address) == 0) {
         if (s_session_slots[i].session->session_id == caller_id) {
            s_session_slots[i].pending_reset = true;
            self_reset = true;
            OLOG_INFO("messaging: /new on %s:%s deferred — caller IS the target session "
                      "(session_id=%u), processing after dispatch completes",
                      provider, provider_address, caller_id);
         }
         break;
      }
   }
   pthread_mutex_unlock(&s_session_slots_mutex);
   return self_reset;
}

/* NOTE: engine_shutdown uses session_destroy for each slot, so
 * in-flight forever-conversations get their facts extracted on
 * graceful shutdown via session_destroy's existing memory-extraction
 * hook.  See messaging_engine_shutdown above.
 *
 * Lock ordering: this function NEVER holds s_session_slots_mutex
 * across session_create or session_destroy.  Both global-lock-
 * acquiring helpers are called outside the per-module mutex.  See
 * the inline release/re-acquire dance in the eviction branch and the
 * create-new-session branch. */
session_t *get_or_create_messaging_session(const char *provider,
                                           const char *provider_address,
                                           int user_id,
                                           int64_t conversation_id) {
   pthread_mutex_lock(&s_session_slots_mutex);

   /* Find existing slot. */
   for (size_t i = 0; i < MESSAGING_MAX_SESSIONS; i++) {
      if (s_session_slots[i].session && strcmp(s_session_slots[i].provider, provider) == 0 &&
          strcmp(s_session_slots[i].provider_address, provider_address) == 0) {
         s_session_slots[i].last_used = time(NULL);
         session_t *s = s_session_slots[i].session;
         /* Re-stamp user_id in case it changed (e.g., admin re-linked
          * the same address to a different DAWN user). */
         if (user_id > 0) {
            s->metrics.user_id = user_id;
         }
         session_retain(s);
         pthread_mutex_unlock(&s_session_slots_mutex);
         /* Refresh the cached display_name from the DB so a mid-session
          * rename is reflected in the next prompt build — the current-channel
          * injection reads messaging_identity.channel_name, and a stale name
          * would make the LLM emit the wrong scheduler `deliver_to`.  Done
          * AFTER releasing the slots mutex (lookup_channel_user takes the
          * auth_db leaf lock — must not nest under a per-module lock) and on
          * the worker thread that builds the prompt downstream, so the write
          * has a single thread and races with no reader.  Steady-state (no
          * rename) re-writes the identical name, so the cached stable prefix
          * stays byte-identical and the Anthropic cache holds; a real rename
          * changes it once and the drift detector logs the reset. */
         char cur_name[sizeof(s->messaging_identity.channel_name)] = { 0 };
         if (lookup_channel_user(provider, provider_address, cur_name, sizeof(cur_name)) > 0 &&
             cur_name[0] != '\0') {
            snprintf(s->messaging_identity.channel_name, sizeof(s->messaging_identity.channel_name),
                     "%s", cur_name);
         }
         return s;
      }
   }

   /* Find a free slot (or evict the LRU). */
   size_t target = MESSAGING_MAX_SESSIONS;
   time_t lru_time = 0;
   size_t lru_idx = 0;
   for (size_t i = 0; i < MESSAGING_MAX_SESSIONS; i++) {
      if (!s_session_slots[i].session) {
         target = i;
         break;
      }
      if (lru_time == 0 || s_session_slots[i].last_used < lru_time) {
         lru_time = s_session_slots[i].last_used;
         lru_idx = i;
      }
   }

   /* If we need to evict, capture the evictee's identity, NULL the
    * slot, drop the per-module mutex, then release+destroy outside
    * the lock.  session_destroy:
    *   - reclaims the global sessions[] slot (closes the slow-burn
    *     leak that plain session_release would create),
    *   - triggers memory extraction for the conversation (gated on
    *     user_id > 0, which we stamped when the session was created
    *     or last used), so Telegram/SMS conversations participate in
    *     the same extraction pipeline as WebUI/DAP2 sessions,
    *   - waits for ref_count to reach zero (3 s timeout); in normal
    *     operation no one else holds a retain on a messaging-backed
    *     session, so this returns essentially instantly.
    * Dropping the mutex before session_destroy fixes the lock-order
    * inversion on this path (per-module → global is forbidden; we
    * now release → call → re-acquire). */
   session_t *evictee = NULL;
   uint32_t evictee_session_id = 0;
   if (target == MESSAGING_MAX_SESSIONS) {
      OLOG_INFO("messaging: evicting LRU session slot %zu (%s:%s)", lru_idx,
                s_session_slots[lru_idx].provider, s_session_slots[lru_idx].provider_address);
      evictee = s_session_slots[lru_idx].session;
      evictee_session_id = evictee->session_id;
      memset(&s_session_slots[lru_idx], 0, sizeof(session_slot_t));
      target = lru_idx;
   }
   if (evictee) {
      pthread_mutex_unlock(&s_session_slots_mutex);
      /* Drop the engine's retain so session_destroy's ref-count wait
       * can converge to zero immediately. */
      session_release(evictee);
      session_destroy(evictee_session_id);
      pthread_mutex_lock(&s_session_slots_mutex);

      /* Defensive re-scan: today there's a single worker thread so
       * `target` is guaranteed still empty after we re-acquire, but if
       * a future refactor adds concurrent dispatchers, another thread
       * could have claimed this slot during the unlock window.  Pick
       * any free slot if `target` got taken. */
      if (s_session_slots[target].session) {
         target = MESSAGING_MAX_SESSIONS;
         for (size_t i = 0; i < MESSAGING_MAX_SESSIONS; i++) {
            if (!s_session_slots[i].session) {
               target = i;
               break;
            }
         }
         if (target == MESSAGING_MAX_SESSIONS) {
            /* Every slot taken again — would require multiple
             * concurrent inbound creates filling the freshly evicted
             * slot AND every other slot.  Drop this inbound. */
            OLOG_WARNING("messaging: lost slot race after eviction; dropping inbound for %s:%s",
                         provider, provider_address);
            pthread_mutex_unlock(&s_session_slots_mutex);
            return NULL;
         }
      }
   }

   /* Lock-order discipline: session_create acquires the GLOBAL
    * session_manager_rwlock.  Per ARCHITECTURE.md "Mutex Lock
    * Ordering", global locks must be acquired BEFORE per-module
    * locks.  Holding s_session_slots_mutex across session_create
    * would invert that order — no deadlock today (nothing else
    * acquires slots mutex while holding session_manager_rwlock), but
    * a real footgun if/when a future refactor adds concurrent
    * messaging dispatchers.
    *
    * Drop the slots mutex BEFORE session_create.  Set up the new
    * session (stamp user_id, restore history) while no engine locks
    * are held — the session_t isn't yet visible to any other thread
    * since we haven't installed it in the slot.  Re-acquire the
    * slots mutex to install; defensive re-scan handles the case
    * where another thread claimed `target` during the unlock window
    * (currently impossible with a single worker, but defensive for
    * future concurrent dispatchers). */
   pthread_mutex_unlock(&s_session_slots_mutex);

   /* Create new session with SESSION_TYPE_MESSAGING.  The type itself
    * exempts the session from session_cleanup_expired (see
    * session_manager.c) — the messaging engine owns this session's
    * lifetime via its own LRU eviction (which calls session_destroy
    * when needed).  client_data stays NULL; code that branches on
    * type=WEBUI for WebSocket delivery (webui_send_*, ws_connection_t)
    * correctly excludes messaging sessions. */
   session_t *s = session_create(SESSION_TYPE_MESSAGING, -1);
   if (!s) {
      return NULL;
   }

   /* Stamp user_id so per-turn focus injection runs against the right
    * user AND session-end memory extraction fires (gated on user_id > 0).
    * Without this, every messaging-backed session would default to
    * user 0 and silently bypass both.  Safe to set without locking:
    * the session_t isn't yet visible to any other thread. */
   if (user_id > 0) {
      s->metrics.user_id = user_id;
   }

   /* Stamp messaging identity so the prompt build path can surface
    * "you are responding through this channel" context to the LLM —
    * mirrors how dap2_identity_t.location surfaces Room.  Resolved via
    * the same lookup_channel_user used during inbound dispatch, so
    * display_name reflects the user's current display_name for this
    * channel.  Safe to set without locking: session_t isn't visible
    * to any other thread yet. */
   snprintf(s->messaging_identity.provider, sizeof(s->messaging_identity.provider), "%s",
            provider ? provider : "");
   char channel_name[sizeof(s->messaging_identity.channel_name)] = { 0 };
   (void)lookup_channel_user(provider, provider_address, channel_name, sizeof(channel_name));
   if (channel_name[0]) {
      snprintf(s->messaging_identity.channel_name, sizeof(s->messaging_identity.channel_name), "%s",
               channel_name);
   }

   /* Post-restart history restore.  Without this, daemon restart
    * silently breaks the forever-conversation contract: DB still has
    * the channel binding + conv_id, but the new in-memory session
    * starts empty, so the LLM has zero prior context while
    * conv_db_add_message_ex keeps appending to the same conv row.
    * Loads the conv's full history (image markers stripped — vision
    * blobs blow context for no benefit on a text-only channel).
    * Empty result (fresh conv just created by
    * resolve_channel_conversation_id) is a no-op since the array
    * starts empty.  conv_db_get_messages also enforces ownership
    * against user_id — defense in depth on the FK already in place.
    * Still safe to call without engine locks: session_t is private. */
   int64_t restored_max_msg_id = 0;
   if (conversation_id > 0 && user_id > 0) {
      size_t restored_chars = 0;
      struct json_object *loaded = memory_history_load_from_db(conversation_id, user_id,
                                                               &restored_chars);
      if (loaded) {
         size_t restored_count = (size_t)json_object_array_length(loaded);
         if (restored_count > 0) {
            /* Capture the highest msg_id BEFORE handing the array off
             * to the session — used below to seed the slot's
             * last_known_msg_id so the cross-channel staleness check
             * in process_inbound knows what we've seen. */
            restored_max_msg_id = history_array_max_msg_id(loaded);
            pthread_mutex_lock(&s->history_mutex);
            if (s->conversation_history) {
               json_object_put(s->conversation_history);
            }
            s->conversation_history = loaded;
            pthread_mutex_unlock(&s->history_mutex);
            OLOG_INFO("messaging: restored %zu messages (%zu chars) into session %u from conv %lld",
                      restored_count, restored_chars, s->session_id, (long long)conversation_id);
         } else {
            /* Fresh conv — nothing to restore.  Free the empty array
             * we just allocated. */
            json_object_put(loaded);
         }
      }
   }

   /* Apply this channel's per-conversation LLM settings to the session.
    *
    * The forever-conversation row owns these (the same conversations.llm_*
    * columns the WebUI per-conversation control uses).  New messaging
    * conversations are seeded thinking-ON at creation (resolve_channel_
    * conversation_id) so the small global model stops bluffing tool calls;
    * the user changes them per channel from the WebUI Messaging Channels
    * panel or in chat via the switch_llm tool.  This is the server-side apply
    * that messaging needs because, unlike the WebUI, there is no browser
    * client to round-trip the stored conv settings through.
    *
    * Stash conversation_id on the session so switch_llm can persist a later
    * in-chat change back to this conversation's row.
    *
    * Set ONCE at creation.  The channel-name fast-path refresh (~296) must NOT
    * mirror this — LLM config is per-creation/restart, not per-turn (per-turn
    * re-application would churn the Anthropic stable-prefix cache).  Safe
    * without engine locks: session_t is thread-private here, and
    * session_set_llm_config takes only the leaf llm_config_mutex. */
   s->messaging_identity.conversation_id = conversation_id;
   if (conversation_id > 0 && user_id > 0) {
      conversation_t conv;
      if (conv_db_get(conversation_id, user_id, &conv) == AUTH_DB_SUCCESS) {
         session_llm_config_t mcfg;
         session_get_llm_config(s, &mcfg); /* seed ALL fields from the global default */

         if (conv.llm_type[0]) {
            mcfg.type = (strcmp(conv.llm_type, "local") == 0) ? LLM_LOCAL : LLM_CLOUD;
         }
         if (conv.cloud_provider[0]) {
            /* type already set from conv.llm_type above; we only need the
             * provider enum here, so t is intentionally discarded. */
            llm_type_t t;
            cloud_provider_t cp;
            if (cloud_provider_from_string(conv.cloud_provider, &t, &cp) == SUCCESS) {
               (void)t;
               mcfg.cloud_provider = cp;
            }
         }
         if (conv.model[0]) {
            strncpy(mcfg.model, conv.model, sizeof(mcfg.model) - 1);
            mcfg.model[sizeof(mcfg.model) - 1] = '\0';
         }
         if (conv.thinking_mode[0]) {
            strncpy(mcfg.thinking_mode, conv.thinking_mode, sizeof(mcfg.thinking_mode) - 1);
            mcfg.thinking_mode[sizeof(mcfg.thinking_mode) - 1] = '\0';
         }
         if (conv.reasoning_effort[0]) {
            strncpy(mcfg.reasoning_effort, conv.reasoning_effort,
                    sizeof(mcfg.reasoning_effort) - 1);
            mcfg.reasoning_effort[sizeof(mcfg.reasoning_effort) - 1] = '\0';
         }

         if (session_set_llm_config(s, &mcfg) != SUCCESS) {
            OLOG_WARNING(
                "messaging: conv %lld LLM settings rejected; session %u uses global default",
                (long long)conversation_id, s->session_id);
         }
         conv_free(&conv);
      }
   }

   /* Re-acquire slots mutex and install.  Defensive re-scan against
    * concurrent dispatchers (single-worker today means `target` will
    * still be free, but future refactors can't trip on this). */
   pthread_mutex_lock(&s_session_slots_mutex);
   if (s_session_slots[target].session) {
      target = MESSAGING_MAX_SESSIONS;
      for (size_t i = 0; i < MESSAGING_MAX_SESSIONS; i++) {
         if (!s_session_slots[i].session) {
            target = i;
            break;
         }
      }
      if (target == MESSAGING_MAX_SESSIONS) {
         /* All slots filled during the unlock window.  Tear down the
          * just-created session and drop the inbound.  Same shape as
          * the post-eviction race-loss path above. */
         OLOG_WARNING("messaging: lost slot race during create for %s:%s — dropping inbound",
                      provider, provider_address);
         pthread_mutex_unlock(&s_session_slots_mutex);
         session_destroy(s->session_id);
         return NULL;
      }
   }

   snprintf(s_session_slots[target].provider, sizeof(s_session_slots[target].provider), "%s",
            provider);
   snprintf(s_session_slots[target].provider_address,
            sizeof(s_session_slots[target].provider_address), "%s", provider_address);
   s_session_slots[target].session = s;
   s_session_slots[target].last_used = time(NULL);
   s_session_slots[target].pending_reset = false;
   /* Seed last_known_msg_id from the freshly-restored history (or 0
    * when the conv was just created with no messages yet).  The
    * cross-channel staleness check in process_inbound uses this to
    * detect external writers between our turns. */
   s_session_slots[target].last_known_msg_id = restored_max_msg_id;

   /* Retain once for the map and once for the caller. */
   session_retain(s);
   pthread_mutex_unlock(&s_session_slots_mutex);
   return s;
}
