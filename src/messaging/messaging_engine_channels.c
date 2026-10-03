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
 * Messaging engine — channel resolution, binding, management, and outbound.
 *
 * Channel lookup by (provider, address) and (user, name); forever-conversation
 * binding (resolve/clear conversation_id); the SMS active-conversation window;
 * channel management (reset / unlink / rename / re-enable, by-name and by-id);
 * the outbound send + per-channel rate limit; the JSON/text channel listings
 * and link-attempt audit table; and the per-provider address_json builder.
 * Split out of messaging_engine.c; see messaging_engine_internal.h and
 * docs/MESSAGING_ENGINE_SPLIT_PLAN.md.
 */
#define AUTH_DB_INTERNAL_ALLOWED
#define MESSAGING_ENGINE_INTERNAL_ALLOWED

#include <ctype.h>
#include <json-c/json.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "core/rate_limiter.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "logging.h"
#include "messaging/messaging_engine.h"
#include "messaging/messaging_engine_internal.h"
#include "messaging/messaging_sender_gate.h"

/* Shared chats whose owner was never recorded, already warned about (a busy
 * group would otherwise log every message).  Small and best-effort: an entry
 * lost to wrap-around just warns again. */
#define RELINK_WARNED_MAX 16
static int64_t s_relink_warned[RELINK_WARNED_MAX];
static size_t s_relink_warned_next;
static pthread_mutex_t s_relink_warned_mutex = PTHREAD_MUTEX_INITIALIZER;

static void warn_relink_once(int64_t channel_id, const char *provider, const char *address) {
   pthread_mutex_lock(&s_relink_warned_mutex);
   for (size_t i = 0; i < RELINK_WARNED_MAX; i++) {
      if (s_relink_warned[i] == channel_id) {
         pthread_mutex_unlock(&s_relink_warned_mutex);
         return;
      }
   }
   s_relink_warned[s_relink_warned_next] = channel_id;
   s_relink_warned_next = (s_relink_warned_next + 1) % RELINK_WARNED_MAX;
   pthread_mutex_unlock(&s_relink_warned_mutex);
   OLOG_WARNING("messaging: channel %lld (%s:%s) is a group chat linked before senders were "
                "recorded; it answers no one until it is re-linked (send /link CODE again)",
                (long long)channel_id, provider, address);
}

channel_resolve_t resolve_inbound_channel(const char *provider,
                                          const char *provider_address,
                                          const char *sender_id,
                                          messaging_chat_kind_t kind,
                                          channel_ref_t *ref) {
   if (!provider || !provider_address || !ref) {
      return CHANNEL_NONE;
   }
   memset(ref, 0, sizeof(*ref));
   const messaging_driver_t *drv = find_driver(provider);
   if (!drv) {
      return CHANNEL_NONE;
   }
   ref->authenticates_sender = drv->authenticates_sender;
   /* A provider that doesn't vouch for senders has no sender to match. */
   const char *sender = (drv->authenticates_sender && sender_id && sender_id[0]) ? sender_id : NULL;

   AUTH_DB_LOCK_OR_RETURN(CHANNEL_NONE);
   /* This sender's own row first; else the chat's unowned row (at most one:
    * the owner index allows a single live row per owner). */
   const char *sql = "SELECT id, user_id, COALESCE(display_name,''), owner_sender, "
                     "COALESCE(last_used_at,0) FROM messaging_channels "
                     "WHERE provider = ? AND provider_address = ? AND " MESSAGING_LIVE_SQL " AND "
                     "(owner_sender = ?3 OR owner_sender IS NULL) "
                     "ORDER BY owner_sender IS NULL LIMIT 1";
   sqlite3_stmt *stmt = NULL;
   bool found = false;
   char owner[64] = { 0 };
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, provider, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 2, provider_address, -1, SQLITE_STATIC);
      if (sender) {
         sqlite3_bind_text(stmt, 3, sender, -1, SQLITE_STATIC);
      } else {
         sqlite3_bind_null(stmt, 3);
      }
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         found = true;
         ref->channel_id = sqlite3_column_int64(stmt, 0);
         ref->user_id = sqlite3_column_int(stmt, 1);
         const unsigned char *dn = sqlite3_column_text(stmt, 2);
         snprintf(ref->display_name, sizeof(ref->display_name), "%s", dn ? (const char *)dn : "");
         const unsigned char *ow = sqlite3_column_text(stmt, 3);
         snprintf(owner, sizeof(owner), "%s", ow ? (const char *)ow : "");
         ref->last_used_at = sqlite3_column_int64(stmt, 4);
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
      stmt = NULL;
   }
   if (!found) {
      AUTH_DB_UNLOCK();
      return CHANNEL_NONE;
   }

   messaging_sender_gate_result_t gate = messaging_sender_gate(owner, sender, kind,
                                                               drv->authenticates_sender);
   if (gate == MESSAGING_SENDER_BIND) {
      /* A one-to-one chat names its owner with its first message. */
      bool bound = false;
      if (sqlite3_prepare_v2(s_db.db,
                             "UPDATE messaging_channels SET owner_sender = ? "
                             "WHERE id = ? AND owner_sender IS NULL",
                             -1, &stmt, NULL) == SQLITE_OK) {
         sqlite3_bind_text(stmt, 1, sender, -1, SQLITE_STATIC);
         sqlite3_bind_int64(stmt, 2, ref->channel_id);
         bound = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(s_db.db) > 0);
         sqlite3_finalize(stmt);
      }
      AUTH_DB_UNLOCK();
      if (!bound) {
         OLOG_WARNING("messaging: couldn't record the sender of channel %lld (%s:%s); dropping",
                      (long long)ref->channel_id, provider, provider_address);
         return CHANNEL_NONE;
      }
      OLOG_INFO("messaging: channel %lld (%s:%s) now answers only its sender",
                (long long)ref->channel_id, provider, provider_address);
      return CHANNEL_RESOLVED;
   }
   AUTH_DB_UNLOCK();

   if (gate == MESSAGING_SENDER_RELINK) {
      warn_relink_once(ref->channel_id, provider, provider_address);
      return CHANNEL_NONE;
   }
   return (gate == MESSAGING_SENDER_ALLOW) ? CHANNEL_RESOLVED : CHANNEL_NONE;
}

/* Look up channel by (user_id, display_name) — case-insensitive name
 * match.  Returns the address_json (caller frees) and the provider via
 * out-param.  Returns NULL if not found. */
static char *lookup_channel_address(int user_id,
                                    const char *channel_name,
                                    char *provider_out,
                                    size_t provider_buf_size,
                                    char *provider_address_out,
                                    size_t provider_address_buf_size,
                                    int64_t *channel_id_out) {
   if (user_id <= 0 || !channel_name) {
      return NULL;
   }

   AUTH_DB_LOCK_OR_RETURN(NULL);
   char *address_json = NULL;
   sqlite3_stmt *stmt = NULL;
   /* Return provider, address_json, AND provider_address so callers
    * can pass the typed primary key straight to drv->send_text and
    * skip the address_json JSON parse on the hot path.  Drivers that
    * need extras still receive address_json as the source of truth. */
   const char *sql = "SELECT provider, address_json, provider_address, id FROM messaging_channels "
                     "WHERE user_id = ? AND " MESSAGING_LIVE_SQL " AND "
                     "LOWER(COALESCE(display_name,'')) = LOWER(?) LIMIT 1";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, user_id);
      sqlite3_bind_text(stmt, 2, channel_name, -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         const unsigned char *p = sqlite3_column_text(stmt, 0);
         const unsigned char *a = sqlite3_column_text(stmt, 1);
         const unsigned char *pa = sqlite3_column_text(stmt, 2);
         if (p && provider_out && provider_buf_size > 0) {
            snprintf(provider_out, provider_buf_size, "%s", (const char *)p);
         }
         if (a) {
            address_json = strdup((const char *)a);
         }
         if (pa && provider_address_out && provider_address_buf_size > 0) {
            snprintf(provider_address_out, provider_address_buf_size, "%s", (const char *)pa);
         }
         if (channel_id_out) {
            *channel_id_out = sqlite3_column_int64(stmt, 3);
         }
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();
   return address_json;
}

/* Resolve the forever-binding conversation_id for a (provider,
 * provider_address) channel.  If the channel already has a non-NULL
 * conversation_id, return it.  Otherwise create a fresh conversations
 * row via conv_db_create_with_origin (origin="messaging:<provider>"),
 * stamp it onto the channel mapping, and return the new id.
 *
 * Returns 0 on any failure (lookup miss, conv-create failure, lock
 * failure).  Callers MUST handle the 0 case as "no DB persistence this
 * turn" — the conversation still works in memory, but messages won't
 * survive daemon restart and the recovery worker won't extract from
 * them.  See docs/MESSAGING_CHANNELS_DESIGN.md §13 Phase 2.5. */
int64_t resolve_channel_conversation_id(channel_ref_t *ref,
                                        const char *provider,
                                        const char *provider_address,
                                        bool *gone) {
   if (gone) {
      *gone = false;
   }
   if (!ref || ref->channel_id <= 0 || ref->user_id <= 0 || !provider || !provider_address) {
      return 0;
   }
   const int64_t channel_id = ref->channel_id;
   const int user_id = ref->user_id;

   /* Phase 1: read existing conversation_id under lock. */
   AUTH_DB_LOCK_OR_RETURN(0);
   int64_t existing = 0;
   char display_name[MESSAGING_DISPLAY_NAME_MAX + 1] = { 0 };
   sqlite3_stmt *stmt = NULL;
   const char *sel_sql = "SELECT COALESCE(conversation_id, 0), COALESCE(display_name,'') "
                         "FROM messaging_channels "
                         "WHERE id = ? AND user_id = ? AND " MESSAGING_LIVE_SQL;
   bool found = false;
   if (sqlite3_prepare_v2(s_db.db, sel_sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, channel_id);
      sqlite3_bind_int(stmt, 2, user_id);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         existing = sqlite3_column_int64(stmt, 0);
         const unsigned char *dn = sqlite3_column_text(stmt, 1);
         if (dn) {
            snprintf(display_name, sizeof(display_name), "%s", (const char *)dn);
         }
         found = true;
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();

   if (!found) {
      OLOG_INFO("messaging: channel %lld (%s:%s) was unlinked or isn't verified any more",
                (long long)channel_id, provider, provider_address);
      if (gone) {
         *gone = true;
      }
      return 0;
   }
   /* The current name, so a rename shows in this turn's prompt. */
   snprintf(ref->display_name, sizeof(ref->display_name), "%s", display_name);

   if (existing > 0) {
      return existing;
   }

   /* Phase 2: no existing conversation — create one.  Title uses the
    * channel's display_name when set, falling back to the address.
    * conv_db_create_with_origin takes the auth_db lock itself, so we
    * MUST release ours first. */
   char title[256];
   if (display_name[0] != '\0') {
      snprintf(title, sizeof(title), "%s (%s)", display_name, provider);
   } else {
      snprintf(title, sizeof(title), "%s: %s", provider, provider_address);
   }
   char origin[64];
   snprintf(origin, sizeof(origin), "messaging:%s", provider);

   int64_t new_conv_id = 0;
   int rc = conv_db_create_with_origin(user_id, title, origin, &new_conv_id);
   if (rc != AUTH_DB_SUCCESS || new_conv_id <= 0) {
      OLOG_ERROR("messaging: resolve_conv: conv_db_create_with_origin failed for %s:%s (rc=%d)",
                 provider, provider_address, rc);
      return 0;
   }

   /* Phase 3: stamp the new conv_id onto the channel row.  If this
    * UPDATE fails the conv row is orphaned (no link back to the
    * channel) — recoverable but ugly.  Logging surfaces the case for
    * post-hoc cleanup. */
   AUTH_DB_LOCK_OR_RETURN(new_conv_id); /* fall through on lock fail; conv exists, just unlinked */
   sqlite3_stmt *upd = NULL;
   const char *upd_sql = "UPDATE messaging_channels SET conversation_id = ? "
                         "WHERE id = ? AND is_enabled = 1 AND conversation_id IS NULL";
   bool stamped = false;
   if (sqlite3_prepare_v2(s_db.db, upd_sql, -1, &upd, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(upd, 1, new_conv_id);
      sqlite3_bind_int64(upd, 2, channel_id);
      if (sqlite3_step(upd) == SQLITE_DONE) {
         stamped = (sqlite3_changes(s_db.db) > 0);
      }
   }
   if (upd) {
      sqlite3_finalize(upd);
   }
   AUTH_DB_UNLOCK();

   if (!stamped) {
      OLOG_WARNING("messaging: resolve_conv: created conv %lld but UPDATE matched 0 rows for %s:%s "
                   "(concurrent claim?) — conv is orphaned, will be invisible to /new",
                   (long long)new_conv_id, provider, provider_address);
   } else {
      OLOG_INFO("messaging: bound channel %s:%s to conv %lld (origin=%s)", provider,
                provider_address, (long long)new_conv_id, origin);
   }

   /* Seed thinking ON for the fresh messaging conversation so the small global
    * model doesn't bluff tool calls over chat (reply "done" with no tool call).
    * NULL type/provider/model = inherit the global LLM; only thinking_mode +
    * reasoning_effort are set.  Stored per-conversation and user-overridable
    * (WebUI Messaging Channels panel, or the switch_llm tool in chat).
    * conv_db_lock_llm_settings takes the auth_db lock itself, so call it after
    * the UNLOCK above; it only writes while message_count == 0 (true here). */
   if (conv_db_lock_llm_settings(new_conv_id, user_id, NULL, NULL, NULL, NULL, "enabled", "low") !=
       AUTH_DB_SUCCESS) {
      /* Non-fatal: the conversation just falls back to the global LLM default
       * (no thinking-on seed).  Log so the missing seed is diagnosable. */
      OLOG_WARNING("messaging: failed to seed thinking-on for new conv %lld (uses global default)",
                   (long long)new_conv_id);
   }

   return new_conv_id;
}

/* Clear messaging_channels.conversation_id for a channel.  Returns
 * MESSAGING_SUCCESS on a 1-row update, MESSAGING_UNKNOWN_CHANNEL if no
 * row matched, MESSAGING_FAILURE on lock / prepare failure. */
int clear_channel_conversation_id(int64_t channel_id) {
   if (channel_id <= 0) {
      return MESSAGING_FAILURE;
   }
   AUTH_DB_LOCK_OR_RETURN(MESSAGING_FAILURE);
   sqlite3_stmt *stmt = NULL;
   int rc = MESSAGING_FAILURE;
   const char *sql = "UPDATE messaging_channels SET conversation_id = NULL "
                     "WHERE id = ? AND is_enabled = 1";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, channel_id);
      if (sqlite3_step(stmt) == SQLITE_DONE) {
         rc = (sqlite3_changes(s_db.db) > 0) ? MESSAGING_SUCCESS : MESSAGING_UNKNOWN_CHANNEL;
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();
   return rc;
}

/* Active-conversation window for SMS.  Returns true when an inbound
 * SMS from this channel should bypass the wake-word gate because the
 * channel had an LLM-bound exchange within g_config.messaging
 * .sms_active_window_sec.  False when the window is disabled (0 or
 * negative), or no exchange has happened yet, or the last one is older
 * than the window.  The channel's last exchange time came with its
 * resolution, so no second lookup. */
bool sms_within_active_window(const channel_ref_t *ref) {
   if (!ref || ref->last_used_at <= 0) {
      return false;
   }
   int window_sec = g_config.messaging.sms_active_window_sec;
   if (window_sec <= 0) {
      return false;
   }
   return ((int64_t)time(NULL) - ref->last_used_at) <= (int64_t)window_sec;
}

/* Touch a channel's last_used_at to NOW.  Called after a successful
 * LLM-bound exchange so the active-conversation window slides forward
 * with each turn.  Best-effort — failure is logged at DEBUG and the
 * turn still completes. */
void touch_channel_last_used(int64_t channel_id) {
   if (channel_id <= 0) {
      return;
   }
   AUTH_DB_LOCK_OR_RETURN_VOID();
   sqlite3_stmt *stmt = NULL;
   const char *sql =
       "UPDATE messaging_channels SET last_used_at = ? WHERE id = ? AND is_enabled = 1";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, (int64_t)time(NULL));
      sqlite3_bind_int64(stmt, 2, channel_id);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();
}

int messaging_engine_reset_channel(int user_id, int64_t channel_id) {
   if (user_id <= 0 || channel_id <= 0) {
      return MESSAGING_FAILURE;
   }
   if (!atomic_load(&s_initialized)) {
      return MESSAGING_FAILURE;
   }

   /* Validate the channel exists first.  reset is a no-op + error if
    * the channel isn't linked (matches messaging_engine_send's contract). */
   char provider[16] = { 0 };
   char provider_address[128] = { 0 };
   bool found = false;
   AUTH_DB_LOCK_OR_RETURN(MESSAGING_FAILURE);
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT provider, provider_address FROM messaging_channels "
                          "WHERE id = ? AND user_id = ? AND is_enabled = 1",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, channel_id);
      sqlite3_bind_int(stmt, 2, user_id);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         const unsigned char *p = sqlite3_column_text(stmt, 0);
         const unsigned char *a = sqlite3_column_text(stmt, 1);
         snprintf(provider, sizeof(provider), "%s", p ? (const char *)p : "");
         snprintf(provider_address, sizeof(provider_address), "%s", a ? (const char *)a : "");
         found = true;
      }
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();
   if (!found) {
      return MESSAGING_UNKNOWN_CHANNEL;
   }

   /* CRITICAL: detect self-reset and defer.  When the LLM emits
    * messaging.reset_conversation targeting its own channel, the
    * calling thread is the messaging worker mid-dispatch holding a
    * retain on the very session_t we'd destroy.  Synchronous
    * session_destroy times out after 3s and frees the session
    * anyway → UAF on resume.  Defer via pending_reset on the slot;
    * process_inbound checks at end-of-dispatch and performs the
    * actual eviction + clear there. */
   if (mark_pending_reset_if_self(channel_id)) {
      return MESSAGING_SUCCESS;
   }

   /* Order matters: evict FIRST (triggers extraction on the closing
    * conv via session_destroy → memory_trigger_extraction with the
    * session's in-memory history), then clear the DB binding.  If we
    * cleared first, the extraction trigger inside session_destroy
    * would still fire but on a session whose conv_id was already
    * un-stamped — extraction works either way (it copies history) but
    * the audit log makes more sense in eviction-then-clear order. */
   evict_session_slot(channel_id);

   int rc = clear_channel_conversation_id(channel_id);
   if (rc == MESSAGING_SUCCESS) {
      OLOG_INFO("messaging: /new reset channel %lld (%s:%s) — next inbound will start a fresh "
                "conv",
                (long long)channel_id, provider, provider_address);
   } else if (rc == MESSAGING_UNKNOWN_CHANNEL) {
      /* Channel exists (lookup succeeded above) but conversation_id was
       * already NULL — that's not an error, just nothing to clear. */
      OLOG_DEBUG("messaging: /new on channel %lld — conversation_id already NULL, no-op",
                 (long long)channel_id);
      rc = MESSAGING_SUCCESS;
   }
   return rc;
}

static int64_t resolve_enabled_channel_id_by_name(int user_id, const char *display_name);

int messaging_engine_reset_by_name(int user_id, const char *channel_name) {
   if (user_id <= 0 || !channel_name) {
      return MESSAGING_FAILURE;
   }
   if (!atomic_load(&s_initialized)) {
      return MESSAGING_FAILURE;
   }
   /* Resolve channel_name → row id under the authenticated user.  Mirrors
    * the ownership boundary that messaging_engine_send enforces. */
   int64_t channel_id = resolve_enabled_channel_id_by_name(user_id, channel_name);
   if (channel_id <= 0) {
      return MESSAGING_UNKNOWN_CHANNEL;
   }
   return messaging_engine_reset_channel(user_id, channel_id);
}

/* Reject display names with control characters or characters that would be
 * unsafe when the name is echoed into logs, the admin text table, or an HTML
 * attribute.  Defense in depth: the WebUI also attribute-escapes on render,
 * but normalizing at the write boundary keeps the stored value clean for all
 * consumers. */
static bool display_name_unsafe(const char *s) {
   for (; *s != '\0'; s++) {
      unsigned char c = (unsigned char)*s;
      if (c < 0x20 || c == 0x7f) {
         return true; /* control chars / DEL / newlines */
      }
      if (c == '"' || c == '<' || c == '>' || c == '\\') {
         return true;
      }
   }
   return false;
}

/* Cascade a channel rename into scheduled_events.deliver_to.  The scheduler
 * stores the channel NAME (a snapshot taken at schedule time, scheduler.c),
 * not the row id, so a standing event that targeted the old name would
 * silently stop delivering after a rename.  Repoint any such event to the
 * new name so delivery keeps resolving.  Same shared auth_db handle (data
 * coupling only — no call into scheduler code, no link cycle).  Caller MUST
 * hold the auth_db lock. */
static void cascade_deliver_to_rename_unlocked(int user_id,
                                               const char *old_name,
                                               const char *new_name) {
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE scheduled_events SET deliver_to = ? "
                          "WHERE user_id = ? AND LOWER(COALESCE(deliver_to,'')) = LOWER(?)",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_STATIC);
      sqlite3_bind_int(stmt, 2, user_id);
      sqlite3_bind_text(stmt, 3, old_name, -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_DONE) {
         int n = sqlite3_changes(s_db.db);
         if (n > 0) {
            OLOG_INFO("messaging: rename cascade repointed %d scheduled deliver_to '%s' → '%s' "
                      "(user %d)",
                      n, old_name, new_name, user_id);
         }
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
   }
}

/* Resolve (user_id, display_name) → row id for an ENABLED channel under
 * the ownership boundary.  Returns the id (>0) or 0 if no enabled channel
 * by that name exists for the user.  Acquires + releases the auth_db lock
 * itself — callers must NOT hold it.  The by-name unlink/rename entry
 * points resolve the id here and delegate to their *_by_id cores, so there
 * is one mutation path per op (mirrors messaging_engine_reset_by_name and
 * _reenable_channel). */
static int64_t resolve_enabled_channel_id_by_name(int user_id, const char *display_name) {
   AUTH_DB_LOCK_OR_RETURN(0);
   sqlite3_stmt *stmt = NULL;
   int64_t id = 0;
   const char *sql = "SELECT id FROM messaging_channels "
                     "WHERE user_id = ? AND is_enabled = 1 AND "
                     "LOWER(COALESCE(display_name,'')) = LOWER(?) LIMIT 1";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, user_id);
      sqlite3_bind_text(stmt, 2, display_name, -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         id = sqlite3_column_int64(stmt, 0);
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();
   return id;
}

int messaging_engine_unlink_channel(int user_id, const char *display_name) {
   if (user_id <= 0 || !display_name || display_name[0] == '\0') {
      return MESSAGING_FAILURE;
   }
   if (!atomic_load(&s_initialized)) {
      return MESSAGING_FAILURE;
   }
   /* Resolve the row id by name, then delegate to the by-id core (the
    * single evict + soft-delete path). */
   int64_t id = resolve_enabled_channel_id_by_name(user_id, display_name);
   if (id <= 0) {
      return MESSAGING_UNKNOWN_CHANNEL;
   }
   return messaging_engine_unlink_channel_by_id(user_id, id);
}

int messaging_engine_rename_channel(int user_id, const char *old_name, const char *new_name) {
   if (user_id <= 0 || !old_name || old_name[0] == '\0' || !new_name || new_name[0] == '\0') {
      return MESSAGING_FAILURE;
   }
   if (!atomic_load(&s_initialized)) {
      return MESSAGING_FAILURE;
   }
   /* Resolve the row id by old_name, then delegate to the by-id core, which
    * owns the name validation (length + control/quote rejection), the
    * collision check, the UPDATE, and the scheduled_events.deliver_to
    * cascade — the single mutation path. */
   int64_t id = resolve_enabled_channel_id_by_name(user_id, old_name);
   if (id <= 0) {
      return MESSAGING_UNKNOWN_CHANNEL;
   }
   return messaging_engine_rename_channel_by_id(user_id, id, new_name);
}

int messaging_engine_unlink_channel_by_id(int user_id, int64_t channel_id) {
   if (user_id <= 0 || channel_id <= 0) {
      return MESSAGING_FAILURE;
   }
   if (!atomic_load(&s_initialized)) {
      return MESSAGING_FAILURE;
   }

   /* Resolve id → (provider, provider_address) under the (id, user_id)
    * ownership boundary so eviction can target the right session slot. */
   char provider[16] = { 0 };
   char provider_address[128] = { 0 };
   bool found = false;
   AUTH_DB_LOCK_OR_RETURN(MESSAGING_FAILURE);
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT provider, provider_address FROM messaging_channels "
                          "WHERE id = ? AND user_id = ? AND is_enabled = 1",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, channel_id);
      sqlite3_bind_int(stmt, 2, user_id);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         const unsigned char *p = sqlite3_column_text(stmt, 0);
         const unsigned char *a = sqlite3_column_text(stmt, 1);
         if (p) {
            snprintf(provider, sizeof(provider), "%s", (const char *)p);
         }
         if (a) {
            snprintf(provider_address, sizeof(provider_address), "%s", (const char *)a);
         }
         found = (p != NULL && a != NULL);
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
      stmt = NULL;
   }
   AUTH_DB_UNLOCK();
   if (!found) {
      return MESSAGING_UNKNOWN_CHANNEL;
   }

   evict_session_slot(channel_id);

   /* An SMS number can change hands while unlinked (carriers recycle
    * numbers), so linking it again must prove it afresh: unlinking drops
    * its verification.  Chat-app links name their owner instead. */
   const messaging_driver_t *drv = find_driver(provider);
   /* With the driver not running, assume the link needs proving again: a
    * chat app's re-link restores it, an SMS one must not come back unproven. */
   const bool keep_verified = drv && drv->authenticates_sender;

   AUTH_DB_LOCK_OR_RETURN(MESSAGING_FAILURE);
   int rc = MESSAGING_FAILURE;
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE messaging_channels SET is_enabled = 0, "
                          "verified_at = CASE WHEN ?3 THEN verified_at ELSE NULL END "
                          "WHERE id = ?1 AND user_id = ?2",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, channel_id);
      sqlite3_bind_int(stmt, 2, user_id);
      sqlite3_bind_int(stmt, 3, keep_verified ? 1 : 0);
      if (sqlite3_step(stmt) == SQLITE_DONE) {
         rc = (sqlite3_changes(s_db.db) > 0) ? MESSAGING_SUCCESS : MESSAGING_UNKNOWN_CHANNEL;
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();
   if (rc == MESSAGING_SUCCESS) {
      OLOG_INFO("messaging: unlinked channel id %lld (%s:%s) for user %d", (long long)channel_id,
                provider, provider_address, user_id);
      webui_broadcast_messaging_channels_changed(user_id, channel_id, "unlinked", NULL);
   }
   return rc;
}

int messaging_engine_rename_channel_by_id(int user_id, int64_t channel_id, const char *new_name) {
   if (user_id <= 0 || channel_id <= 0 || !new_name || new_name[0] == '\0') {
      return MESSAGING_FAILURE;
   }
   if (strlen(new_name) >= MESSAGING_DISPLAY_NAME_MAX || display_name_unsafe(new_name)) {
      return MESSAGING_INVALID_NAME;
   }
   if (!atomic_load(&s_initialized)) {
      return MESSAGING_FAILURE;
   }

   AUTH_DB_LOCK_OR_RETURN(MESSAGING_FAILURE);
   sqlite3_stmt *stmt = NULL;
   int rc = MESSAGING_FAILURE;

   /* Verify the target exists + is owned, capturing the old name for the
    * deliver_to cascade below. */
   char old_name[MESSAGING_DISPLAY_NAME_MAX] = { 0 };
   bool exists = false;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT COALESCE(display_name,'') FROM messaging_channels "
                          "WHERE id = ? AND user_id = ? AND is_enabled = 1",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, channel_id);
      sqlite3_bind_int(stmt, 2, user_id);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         const unsigned char *n = sqlite3_column_text(stmt, 0);
         if (n) {
            snprintf(old_name, sizeof(old_name), "%s", (const char *)n);
         }
         exists = true;
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
      stmt = NULL;
   }
   if (!exists) {
      AUTH_DB_UNLOCK();
      return MESSAGING_UNKNOWN_CHANNEL;
   }

   /* Reject collision with a DIFFERENT enabled channel of this user. */
   bool collision = false;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT 1 FROM messaging_channels WHERE user_id = ? AND is_enabled = 1 "
                          "AND LOWER(COALESCE(display_name,'')) = LOWER(?) AND id != ? LIMIT 1",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, user_id);
      sqlite3_bind_text(stmt, 2, new_name, -1, SQLITE_STATIC);
      sqlite3_bind_int64(stmt, 3, channel_id);
      collision = (sqlite3_step(stmt) == SQLITE_ROW);
   }
   if (stmt) {
      sqlite3_finalize(stmt);
      stmt = NULL;
   }
   if (collision) {
      AUTH_DB_UNLOCK();
      return MESSAGING_NAME_TAKEN;
   }

   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE messaging_channels SET display_name = ? "
                          "WHERE id = ? AND user_id = ?",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_STATIC);
      sqlite3_bind_int64(stmt, 2, channel_id);
      sqlite3_bind_int(stmt, 3, user_id);
      if (sqlite3_step(stmt) == SQLITE_DONE) {
         rc = MESSAGING_SUCCESS;
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
   }
   if (rc == MESSAGING_SUCCESS && old_name[0] != '\0') {
      cascade_deliver_to_rename_unlocked(user_id, old_name, new_name);
   }
   AUTH_DB_UNLOCK();
   if (rc == MESSAGING_SUCCESS) {
      OLOG_INFO("messaging: renamed channel id %lld → '%s' for user %d", (long long)channel_id,
                new_name, user_id);
      webui_broadcast_messaging_channels_changed(user_id, channel_id, "renamed", NULL);
   }
   return rc;
}

int messaging_engine_reenable_channel_by_id(int user_id, int64_t channel_id) {
   if (user_id <= 0 || channel_id <= 0) {
      return MESSAGING_FAILURE;
   }
   if (!atomic_load(&s_initialized)) {
      return MESSAGING_FAILURE;
   }

   AUTH_DB_LOCK_OR_RETURN(MESSAGING_FAILURE);
   sqlite3_stmt *stmt = NULL;
   int rc = MESSAGING_FAILURE;

   /* Find the DISABLED target owned by the user; capture its name to
    * check for a collision with an enabled channel. */
   char name[MESSAGING_DISPLAY_NAME_MAX] = { 0 };
   bool found = false;
   bool verified = false;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT COALESCE(display_name,''), verified_at IS NOT NULL "
                          "FROM messaging_channels WHERE id = ? AND user_id = ? AND is_enabled = 0",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, channel_id);
      sqlite3_bind_int(stmt, 2, user_id);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         const unsigned char *n = sqlite3_column_text(stmt, 0);
         if (n) {
            snprintf(name, sizeof(name), "%s", (const char *)n);
         }
         verified = sqlite3_column_int(stmt, 1) != 0;
         found = true;
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
      stmt = NULL;
   }
   if (!found) {
      AUTH_DB_UNLOCK();
      return MESSAGING_UNKNOWN_CHANNEL;
   }
   /* An unverified row (an SMS link that never got its code, or one unlinked
    * since) comes back only through a new /link. */
   if (!verified) {
      AUTH_DB_UNLOCK();
      return MESSAGING_NOT_VERIFIED;
   }

   /* Reject if re-enabling would duplicate an enabled channel's name. */
   if (name[0] != '\0') {
      bool collision = false;
      if (sqlite3_prepare_v2(
              s_db.db,
              "SELECT 1 FROM messaging_channels WHERE user_id = ? AND is_enabled = 1 "
              "AND LOWER(COALESCE(display_name,'')) = LOWER(?) LIMIT 1",
              -1, &stmt, NULL) == SQLITE_OK) {
         sqlite3_bind_int(stmt, 1, user_id);
         sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
         collision = (sqlite3_step(stmt) == SQLITE_ROW);
      }
      if (stmt) {
         sqlite3_finalize(stmt);
         stmt = NULL;
      }
      if (collision) {
         AUTH_DB_UNLOCK();
         return MESSAGING_NAME_TAKEN;
      }
   }

   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE messaging_channels SET is_enabled = 1 "
                          "WHERE id = ? AND user_id = ?",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, channel_id);
      sqlite3_bind_int(stmt, 2, user_id);
      int step = sqlite3_step(stmt);
      if (step == SQLITE_DONE) {
         rc = (sqlite3_changes(s_db.db) > 0) ? MESSAGING_SUCCESS : MESSAGING_UNKNOWN_CHANNEL;
      } else if (sqlite3_extended_errcode(s_db.db) == SQLITE_CONSTRAINT_UNIQUE) {
         /* The same person's chat is linked to another DAWN account (the
          * owner index allows one live row). */
         rc = MESSAGING_ALREADY_LINKED;
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();
   if (rc == MESSAGING_SUCCESS) {
      OLOG_INFO("messaging: re-enabled channel id %lld ('%s') for user %d", (long long)channel_id,
                name, user_id);
      webui_broadcast_messaging_channels_changed(user_id, channel_id, "enabled", NULL);
   }
   return rc;
}

int messaging_engine_reenable_channel(int user_id, const char *display_name) {
   if (user_id <= 0 || !display_name || display_name[0] == '\0') {
      return MESSAGING_FAILURE;
   }
   if (!atomic_load(&s_initialized)) {
      return MESSAGING_FAILURE;
   }

   /* Resolve the disabled row's id by name, then delegate to the by-id
    * path (which owns the collision check + enable). */
   int64_t target_id = 0;
   AUTH_DB_LOCK_OR_RETURN(MESSAGING_FAILURE);
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT id FROM messaging_channels WHERE user_id = ? AND is_enabled = 0 "
                          "AND LOWER(COALESCE(display_name,'')) = LOWER(?) "
                          "ORDER BY verified_at IS NULL, id ASC LIMIT 1",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, user_id);
      sqlite3_bind_text(stmt, 2, display_name, -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         target_id = sqlite3_column_int64(stmt, 0);
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();

   if (target_id <= 0) {
      return MESSAGING_UNKNOWN_CHANNEL;
   }
   return messaging_engine_reenable_channel_by_id(user_id, target_id);
}

int messaging_engine_send(int user_id, const char *channel_name, const char *text) {
   if (user_id <= 0 || !channel_name || !text || text[0] == '\0') {
      return MESSAGING_FAILURE;
   }
   if (!atomic_load(&s_initialized)) {
      return MESSAGING_FAILURE;
   }

   char provider[16] = { 0 };
   char provider_address[128] = { 0 };
   int64_t channel_id = 0;
   char *address_json = lookup_channel_address(user_id, channel_name, provider, sizeof(provider),
                                               provider_address, sizeof(provider_address),
                                               &channel_id);
   if (!address_json) {
      return MESSAGING_UNKNOWN_CHANNEL;
   }

   /* Per-user-channel rate limit.  Lowercase the channel_name into the
    * key so two callers using different casings of the same channel
    * ("Family" vs "family") hit the same bucket — matches the
    * case-insensitive LOWER(...) lookup the channel resolver uses
    * upstream.  channel_lc sized to fit "uNN:<48-char channel>" in
    * rl_key with margin for an 11-digit user_id. */
   char rl_key[64];
   char channel_lc[48];
   size_t cn_len = 0;
   for (; channel_name[cn_len] != '\0' && cn_len < sizeof(channel_lc) - 1; cn_len++) {
      channel_lc[cn_len] = (char)tolower((unsigned char)channel_name[cn_len]);
   }
   channel_lc[cn_len] = '\0';
   snprintf(rl_key, sizeof(rl_key), "u%d:%s", user_id, channel_lc);
   if (rate_limiter_check(&s_outbound_per_user_limiter, rl_key)) {
      free(address_json);
      return MESSAGING_RATE_LIMITED;
   }

   const messaging_driver_t *drv = find_driver(provider);
   if (!drv) {
      free(address_json);
      return MESSAGING_DRIVER_NOT_REGISTERED;
   }

   /* Pass the typed provider_address (chat_id / channel_id / E.164)
    * so the driver can skip the JSON parse on the hot path.  Empty
    * string acts as NULL — driver falls back to parsing address_json.
    *
    * Routed through messaging_deliver (not drv->send_text directly) so
    * scheduler / tool fan-out gets the same per-channel markdown conversion
    * + cap-aware splitting as the inbound reply path.  `text` is canonical
    * markdown authored by the LLM or a scheduler payload. */
   const char *pa_arg = provider_address[0] ? provider_address : NULL;
   int rc = messaging_deliver(drv, user_id, pa_arg, address_json, text);
   free(address_json);

   if (rc == MESSAGING_SUCCESS) {
      /* Persist the delivered message to the channel's forever-conversation so
       * it appears in the WebUI log and is part of the channel's history on the
       * next turn — symmetric with the inbound reply path (process_inbound).
       * Covers both the messaging.send tool and scheduler fan-out.  The
       * canonical markdown is stored (the WebUI renders it); per-channel
       * formatting was a throwaway copy inside messaging_deliver.  Best-effort:
       * a persistence failure does not undo the successful send.  Done outside
       * the last_used lock below — resolve_channel_conversation_id and
       * conv_db_add_message_ex take the auth_db leaf lock themselves. */
      if (provider_address[0]) {
         channel_ref_t ref = { .channel_id = channel_id, .user_id = user_id };
         int64_t conv_id = resolve_channel_conversation_id(&ref, provider, provider_address, NULL);
         if (conv_id > 0) {
            int64_t msg_id = 0;
            /* no-blocks: a channel post DAWN sent, not a model turn. */
            if (conv_db_add_message_ex(conv_id, user_id, "assistant", text, &msg_id) ==
                    AUTH_DB_SUCCESS &&
                msg_id > 0) {
               webui_broadcast_conversation_messages_appended(user_id, conv_id);
            }
         }
      }

      /* Bump last_used_at. */
      AUTH_DB_LOCK_OR_RETURN(MESSAGING_SUCCESS); /* if lock fails just skip */
      sqlite3_stmt *stmt = NULL;
      if (sqlite3_prepare_v2(s_db.db, "UPDATE messaging_channels SET last_used_at = ? WHERE id = ?",
                             -1, &stmt, NULL) == SQLITE_OK) {
         sqlite3_bind_int64(stmt, 1, (int64_t)time(NULL));
         sqlite3_bind_int64(stmt, 2, channel_id);
         sqlite3_step(stmt);
         sqlite3_finalize(stmt);
      }
      AUTH_DB_UNLOCK();
      return MESSAGING_SUCCESS;
   }

   return MESSAGING_FAILURE;
}

char *messaging_engine_list_channels_json(int user_id) {
   if (user_id <= 0) {
      return NULL;
   }

   /* Acquire the DB lock BEFORE allocating the JSON array — otherwise
    * a lock-acquisition failure would leak `arr`.  Pattern matches
    * lookup_channel_address above. */
   AUTH_DB_LOCK_OR_RETURN(strdup("[]"));

   struct json_object *arr = json_object_new_array();
   if (!arr) {
      AUTH_DB_UNLOCK();
      return NULL;
   }

   sqlite3_stmt *stmt = NULL;
   /* id + last_used_at added for the WebUI management panel (Phase 6):
    * `id` is the stable operation key for rename/unlink (display_name is
    * mutable + non-unique); `last_used_at` drives the "last active"
    * column.  The LLM tool consumer reads only name/provider/enabled, so
    * the extra fields are harmless there.
    *
    * LEFT JOIN conversations surfaces the forever-conversation's per-channel
    * LLM settings (conversation_id + llm_*) so the WebUI panel can show and
    * edit the model/reasoning used for this channel.  COALESCE keeps unbound
    * channels (no conversation yet) and never-customized fields as empties. */
   const char *sql = "SELECT mc.id, COALESCE(mc.display_name,''), mc.provider, mc.is_enabled, "
                     "COALESCE(mc.last_used_at,0), COALESCE(mc.conversation_id,0), "
                     "COALESCE(c.llm_type,''), COALESCE(c.cloud_provider,''), "
                     "COALESCE(c.model,''), COALESCE(c.thinking_mode,''), "
                     "COALESCE(c.reasoning_effort,''), (mc.verified_at IS NOT NULL), "
                     "COALESCE(mc.verify_expires_at,0) "
                     "FROM messaging_channels mc "
                     "LEFT JOIN conversations c ON c.id = mc.conversation_id "
                     "AND c.user_id = mc.user_id "
                     "WHERE mc.user_id = ? "
                     "ORDER BY mc.last_used_at DESC NULLS LAST, mc.id ASC";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, user_id);
      while (sqlite3_step(stmt) == SQLITE_ROW) {
         struct json_object *obj = json_object_new_object();
         int64_t id = sqlite3_column_int64(stmt, 0);
         const unsigned char *name = sqlite3_column_text(stmt, 1);
         const unsigned char *prov = sqlite3_column_text(stmt, 2);
         int enabled = sqlite3_column_int(stmt, 3);
         int64_t last_used = sqlite3_column_int64(stmt, 4);
         int64_t conv_id = sqlite3_column_int64(stmt, 5);
         const unsigned char *llm_type = sqlite3_column_text(stmt, 6);
         const unsigned char *cloud_provider = sqlite3_column_text(stmt, 7);
         const unsigned char *model = sqlite3_column_text(stmt, 8);
         const unsigned char *thinking_mode = sqlite3_column_text(stmt, 9);
         const unsigned char *reasoning_effort = sqlite3_column_text(stmt, 10);
         int verified = sqlite3_column_int(stmt, 11);
         int64_t verify_expires = sqlite3_column_int64(stmt, 12);
         json_object_object_add(obj, "id", json_object_new_int64(id));
         json_object_object_add(obj, "name",
                                json_object_new_string(name ? (const char *)name : ""));
         json_object_object_add(obj, "provider",
                                json_object_new_string(prov ? (const char *)prov : ""));
         json_object_object_add(obj, "enabled", json_object_new_boolean(enabled != 0));
         /* false = an SMS link waiting for the code texted to the number */
         json_object_object_add(obj, "verified", json_object_new_boolean(verified != 0));
         if (!verified) {
            /* Seconds the code texted to the number has left (0 = expired,
             * -1 = none sent); relative, so a browser's clock doesn't matter. */
            int64_t left = -1;
            if (verify_expires > 0) {
               left = verify_expires - (int64_t)time(NULL);
               left = left < 0 ? 0 : left;
            }
            json_object_object_add(obj, "verify_ttl_seconds", json_object_new_int64(left));
         }
         json_object_object_add(obj, "last_used_at", json_object_new_int64(last_used));
         json_object_object_add(obj, "conversation_id", json_object_new_int64(conv_id));
         json_object_object_add(obj, "llm_type",
                                json_object_new_string(llm_type ? (const char *)llm_type : ""));
         json_object_object_add(obj, "cloud_provider",
                                json_object_new_string(cloud_provider ? (const char *)cloud_provider
                                                                      : ""));
         json_object_object_add(obj, "model",
                                json_object_new_string(model ? (const char *)model : ""));
         json_object_object_add(obj, "thinking_mode",
                                json_object_new_string(thinking_mode ? (const char *)thinking_mode
                                                                     : ""));
         json_object_object_add(obj, "reasoning_effort",
                                json_object_new_string(
                                    reasoning_effort ? (const char *)reasoning_effort : ""));
         json_object_array_add(arr, obj);
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();

   /* Annotate each channel with whether its provider's driver is currently
    * loaded (find_driver != NULL — false when e.g. no bot token is configured,
    * so the driver never self-registered).  A channel can be "linked" in the DB
    * while its driver is down; the UI uses this to show "Not connected" instead
    * of "Active".  Done AFTER releasing the auth_db lock so the drivers mutex
    * (taken by find_driver) never nests under the auth_db leaf lock. */
   size_t n = json_object_array_length(arr);
   for (size_t i = 0; i < n; i++) {
      struct json_object *obj = json_object_array_get_idx(arr, i);
      struct json_object *pv = NULL;
      const char *prov = json_object_object_get_ex(obj, "provider", &pv)
                             ? json_object_get_string(pv)
                             : "";
      json_object_object_add(obj, "provider_available",
                             json_object_new_boolean(find_driver(prov) != NULL));
   }

   const char *json_str = json_object_to_json_string_ext(arr, JSON_C_TO_STRING_PLAIN);
   char *result = json_str ? strdup(json_str) : strdup("[]");
   json_object_put(arr);
   return result;
}

int messaging_engine_list_channels_text(int user_id, char *buf, size_t buflen) {
   if (!buf || buflen == 0) {
      return MESSAGING_FAILURE;
   }
   buf[0] = '\0';
   if (user_id <= 0) {
      return MESSAGING_FAILURE;
   }

   AUTH_DB_LOCK_OR_RETURN(MESSAGING_FAILURE);
   sqlite3_stmt *stmt = NULL;
   const char *sql = "SELECT id, COALESCE(display_name,''), provider, is_enabled, "
                     "COALESCE(last_used_at,0), (verified_at IS NOT NULL) "
                     "FROM messaging_channels WHERE user_id = ? "
                     "ORDER BY last_used_at DESC NULLS LAST, id ASC";

   size_t off = 0;
   int n = snprintf(buf + off, buflen - off, "%-5s  %-24s  %-8s  %-7s  %s\n", "ID", "NAME",
                    "PROVIDER", "ENABLED", "LAST USED");
   if (n > 0 && (size_t)n < buflen - off) {
      off += (size_t)n;
   }

   int rows = 0;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, user_id);
      while (sqlite3_step(stmt) == SQLITE_ROW) {
         int64_t id = sqlite3_column_int64(stmt, 0);
         const unsigned char *name = sqlite3_column_text(stmt, 1);
         const unsigned char *prov = sqlite3_column_text(stmt, 2);
         int enabled = sqlite3_column_int(stmt, 3);
         int64_t last_used = sqlite3_column_int64(stmt, 4);
         int verified = sqlite3_column_int(stmt, 5);

         char timebuf[32] = "never";
         if (last_used > 0) {
            time_t tt = (time_t)last_used;
            struct tm tmv;
            if (localtime_r(&tt, &tmv)) {
               strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tmv);
            }
         }
         n = snprintf(buf + off, buflen - off, "%-5lld  %-24.24s  %-8.8s  %-7s  %s\n",
                      (long long)id, name ? (const char *)name : "", prov ? (const char *)prov : "",
                      !enabled ? "no" : (!verified ? "pending" : "yes"), timebuf);
         if (n <= 0 || (size_t)n >= buflen - off) {
            snprintf(buf + off, buflen - off, "... (truncated)\n");
            break;
         }
         off += (size_t)n;
         rows++;
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();

   if (rows == 0 && off < buflen) {
      snprintf(buf + off, buflen - off, "(no channels linked)\n");
   }
   return MESSAGING_SUCCESS;
}

int messaging_engine_link_attempts_text(const char *provider_filter,
                                        int64_t since,
                                        int limit,
                                        char *buf,
                                        size_t buflen) {
   if (!buf || buflen == 0) {
      return MESSAGING_FAILURE;
   }
   buf[0] = '\0';
   if (limit <= 0 || limit > 200) {
      limit = 50;
   }
   if (since <= 0) {
      since = (int64_t)time(NULL) - (int64_t)(7 * 24 * 3600);
   }
   const bool has_provider = (provider_filter && provider_filter[0] != '\0');

   AUTH_DB_LOCK_OR_RETURN(MESSAGING_FAILURE);
   sqlite3_stmt *stmt = NULL;
   const char *sql = has_provider
                         ? "SELECT provider, sender_address, COALESCE(code_tried,''), result, "
                           "created_at FROM messaging_link_attempts WHERE created_at >= ? AND "
                           "provider = ? ORDER BY created_at DESC LIMIT ?"
                         : "SELECT provider, sender_address, COALESCE(code_tried,''), result, "
                           "created_at FROM messaging_link_attempts WHERE created_at >= ? "
                           "ORDER BY created_at DESC LIMIT ?";

   size_t off = 0;
   int n = snprintf(buf + off, buflen - off, "%-8s  %-22s  %-8s  %-10s  %s\n", "PROVIDER", "SENDER",
                    "CODE", "RESULT", "TIME");
   if (n > 0 && (size_t)n < buflen - off) {
      off += (size_t)n;
   }

   int rows = 0;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      int idx = 1;
      sqlite3_bind_int64(stmt, idx++, since);
      if (has_provider) {
         sqlite3_bind_text(stmt, idx++, provider_filter, -1, SQLITE_STATIC);
      }
      sqlite3_bind_int(stmt, idx++, limit);
      while (sqlite3_step(stmt) == SQLITE_ROW) {
         const unsigned char *prov = sqlite3_column_text(stmt, 0);
         const unsigned char *sender = sqlite3_column_text(stmt, 1);
         const unsigned char *codet = sqlite3_column_text(stmt, 2);
         const unsigned char *res = sqlite3_column_text(stmt, 3);
         int64_t ts = sqlite3_column_int64(stmt, 4);

         char timebuf[32] = "?";
         time_t tt = (time_t)ts;
         struct tm tmv;
         if (localtime_r(&tt, &tmv)) {
            strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tmv);
         }
         n = snprintf(buf + off, buflen - off, "%-8.8s  %-22.22s  %-8.8s  %-10.10s  %s\n",
                      prov ? (const char *)prov : "", sender ? (const char *)sender : "",
                      codet ? (const char *)codet : "", res ? (const char *)res : "", timebuf);
         if (n <= 0 || (size_t)n >= buflen - off) {
            snprintf(buf + off, buflen - off,
                     "... (truncated; narrow with --provider / --since / --limit)\n");
            break;
         }
         off += (size_t)n;
         rows++;
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();

   if (rows == 0 && off < buflen) {
      snprintf(buf + off, buflen - off, "(no link attempts in window)\n");
   }
   return MESSAGING_SUCCESS;
}

/* Per-provider address_json builder.  Delegates to the driver's
 * `build_address_json` hook when the driver is registered + provides
 * one — that's the single source of truth for each provider's JSON
 * shape and lets Phase 3 (Discord) and Phase 4 (Slack) add new drivers
 * without touching the engine.  Falls back to an inline shape table
 * when no driver is registered yet (e.g., during /link claim, before
 * the driver's init has wired its callbacks — though in practice the
 * driver always registers before claims).
 *
 * Telegram → {"chat_id":"<numeric_string>"}
 * SMS      → {"phone_e164":"<+1...>"}
 * Discord  → reserved for Phase 3 ({"channel_id":"...","user_id":"..."})
 * Slack    → reserved for Phase 4 ({"team_id":"T...","channel_id":"D..."})
 * Unknown  → "{}" (compatible with both UNIQUE constraint and
 *            future driver-supplied extras).
 */
void build_address_json_for(const char *provider,
                            const char *sender_address,
                            char *buf,
                            size_t buf_size) {
   if (!provider || !sender_address || !buf || buf_size == 0) {
      if (buf && buf_size > 0) {
         buf[0] = '\0';
      }
      return;
   }
   /* Driver-side dispatch (preferred). */
   const messaging_driver_t *drv = find_driver(provider);
   if (drv && drv->build_address_json) {
      drv->build_address_json(sender_address, buf, buf_size);
      return;
   }
   /* Fallback inline table.  Kept as a safety net for the narrow
    * window where /link claims a row before the driver finishes
    * registering its callbacks. */
   if (strcmp(provider, "telegram") == 0) {
      snprintf(buf, buf_size, "{\"chat_id\":\"%s\"}", sender_address);
   } else if (strcmp(provider, "sms") == 0) {
      snprintf(buf, buf_size, "{\"phone_e164\":\"%s\"}", sender_address);
   } else {
      snprintf(buf, buf_size, "{}");
   }
}
