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
 * Messaging engine — link-code flow + async outbound.
 *
 * One-time link-code issuance/lookup/atomic-claim (the /link proof-of-control
 * handshake), the link-attempt audit log, and the detached async-send helper
 * used to confirm /link and /new from the mosquitto callback thread without
 * self-deadlocking.  Split out of messaging_engine.c; see
 * messaging_engine_internal.h and docs/MESSAGING_ENGINE_SPLIT_PLAN.md.
 */
#define AUTH_DB_INTERNAL_ALLOWED
#define MESSAGING_ENGINE_INTERNAL_ALLOWED

#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <sodium.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "core/reply_code.h"
#include "dawn_error.h"
#include "logging.h"
#include "messaging/messaging_engine.h"
#include "messaging/messaging_engine_internal.h"
#include "messaging/messaging_format.h"

/* =============================================================================
 * Module-local constants and types
 * ============================================================================= */

/* Crockford base32 alphabet — excludes I/L/O/U to avoid typing
 * confusion. */
static const char CROCKFORD_ALPHABET[32] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/* Async outbound send item.  Used when we need to call drv->send_text()
 * from a thread that cannot block waiting for the response —
 * specifically, the mosquitto network callback thread that delivers
 * inbound SMS events via echo/events.  That thread is also the one that
 * delivers the echo/response message carrying ECHO's send_sms ack; if we
 * block it inside send_text waiting for that ack, we self-deadlock and
 * time out after 60s.  The normal LLM-reply path doesn't hit this because
 * the engine worker thread is the one that calls send_text, leaving
 * mosquitto's callback thread free.  The /link confirmation, however,
 * fires inline from handle_link_command which runs on the mosquitto
 * thread — hence the need for an async dispatch.
 *
 * Spawned threads are detached (no join) with a small stack (64 KB); they are
 * counted, and engine shutdown waits for them (engine_wait_async_sends). */
typedef struct {
   const messaging_driver_t *drv;
   int user_id;
   char provider_address[128];
   char address_json[MESSAGING_ADDRESS_JSON_BUF_SIZE];
   /* Heap-owned, already rendered into drv->out_format by engine_send_async.
    * Heap (not a fixed buffer) so HTML escaping/expansion can't truncate a
    * tag mid-stream and trip Telegram's HTTP-400 reject.  Freed by the
    * async-send thread. */
   char *text;
   /* Non-NULL: the text must not be kept (a verification code); a driver
    * that records what it sends records this instead, and the text is
    * wiped after sending. */
   char *log_text;
   /* A verification code's channel and the digest of that code: if the send
    * fails, the code (never delivered) is voided and its send given back
    * (0 = not a code). */
   int64_t refund_channel_id;
   char refund_hash[crypto_generichash_BYTES * 2 + 1];
} async_send_item_t;

static void verify_send_failed(int64_t channel_id, const char *hash);

/* Live link codes a user may hold at once (each one can make DAWN text a
 * number). */
#define MESSAGING_LINK_CODES_LIVE_MAX 5

static int live_link_codes_locked(int user_id, time_t now);

static int generate_random_code(char *out, size_t out_buf_size) {
   if (out_buf_size < MESSAGING_LINK_CODE_LEN + 1) {
      return MESSAGING_FAILURE;
   }
   /* Read MESSAGING_LINK_CODE_LEN bytes from /dev/urandom; each byte
    * indexes into the 32-char Crockford alphabet via mod-32. */
   int fd = open("/dev/urandom", O_RDONLY);
   if (fd < 0) {
      return MESSAGING_FAILURE;
   }
   unsigned char buf[MESSAGING_LINK_CODE_LEN];
   ssize_t n = read(fd, buf, sizeof(buf));
   close(fd);
   if (n != (ssize_t)sizeof(buf)) {
      return MESSAGING_FAILURE;
   }
   for (size_t i = 0; i < MESSAGING_LINK_CODE_LEN; i++) {
      out[i] = CROCKFORD_ALPHABET[buf[i] & 0x1F];
   }
   out[MESSAGING_LINK_CODE_LEN] = '\0';
   return MESSAGING_SUCCESS;
}

static void purge_expired_link_codes(void) {
   /* Gate behind a 60-second timestamp so clustered link-issuance
    * (e.g., operator stress-testing) doesn't run a DELETE per call. */
   static time_t s_last_purge = 0;
   time_t now = time(NULL);
   if (now - s_last_purge < 60) {
      return;
   }
   s_last_purge = now;

   AUTH_DB_LOCK_OR_RETURN_VOID();
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "DELETE FROM messaging_link_codes WHERE ROWID IN "
                          "(SELECT ROWID FROM messaging_link_codes "
                          "WHERE expires_at < ? LIMIT 100)",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, (int64_t)now);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
   }
   /* SMS links never verified and never used, a day after their code ran
    * out (or after they were made, if no code was ever sent).  They hold
    * nothing (no conversation is made before verification); a new /link
    * starts over.  A row that was once in use keeps its name, so a scheduled
    * delivery naming it still resolves once it's proven again. */
   int purged_users[8];
   size_t n_purged = 0;
   if (sqlite3_prepare_v2(s_db.db,
                          "DELETE FROM messaging_channels WHERE verified_at IS NULL AND "
                          "conversation_id IS NULL AND last_used_at IS NULL AND "
                          "COALESCE(verify_expires_at, created_at) < ? RETURNING user_id",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, (int64_t)now - 24 * 60 * 60);
      while (sqlite3_step(stmt) == SQLITE_ROW) {
         int uid = sqlite3_column_int(stmt, 0);
         bool seen = false;
         for (size_t i = 0; i < n_purged && !seen; i++) {
            seen = purged_users[i] == uid;
         }
         if (!seen && n_purged < sizeof(purged_users) / sizeof(purged_users[0])) {
            purged_users[n_purged++] = uid;
         }
      }
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();
   /* An open panel drops the removed rows (beyond the first few users, the
    * next refresh does). */
   for (size_t i = 0; i < n_purged; i++) {
      webui_broadcast_messaging_channels_changed(purged_users[i], 0, "removed", NULL);
   }
}

int messaging_engine_generate_link_code(int user_id,
                                        const char *provider_hint,
                                        char *code_out,
                                        size_t code_buf_size) {
   if (user_id <= 0 || !code_out || code_buf_size < MESSAGING_LINK_CODE_BUF_SIZE) {
      return MESSAGING_FAILURE;
   }
   if (!atomic_load(&s_initialized)) {
      return MESSAGING_FAILURE;
   }

   purge_expired_link_codes();

   /* Try up to 5 times to find an unused code.  At 32^8 keyspace this
    * is essentially always one-shot, but defensively retry. */
   for (int attempt = 0; attempt < 5; attempt++) {
      if (generate_random_code(code_out, code_buf_size) != MESSAGING_SUCCESS) {
         return MESSAGING_FAILURE;
      }

      AUTH_DB_LOCK_OR_FAIL();
      sqlite3_stmt *stmt = NULL;
      int rc = SQLITE_ERROR;
      time_t now = time(NULL);
      /* Each live code can make DAWN text a number (an SMS link), so a user
       * holds only a few at a time; counted under the same lock as the
       * insert. */
      if (live_link_codes_locked(user_id, now) >= MESSAGING_LINK_CODES_LIVE_MAX) {
         AUTH_DB_UNLOCK();
         OLOG_WARNING("messaging_engine: user %d already holds %d live link codes", user_id,
                      MESSAGING_LINK_CODES_LIVE_MAX);
         return MESSAGING_RATE_LIMITED;
      }
      if (sqlite3_prepare_v2(s_db.db,
                             "INSERT INTO messaging_link_codes "
                             "(code, user_id, provider_hint, created_at, expires_at) "
                             "VALUES (?, ?, ?, ?, ?)",
                             -1, &stmt, NULL) == SQLITE_OK) {
         const char *hint = (provider_hint && provider_hint[0]) ? provider_hint : NULL;
         sqlite3_bind_text(stmt, 1, code_out, -1, SQLITE_STATIC);
         sqlite3_bind_int(stmt, 2, user_id);
         if (hint) {
            sqlite3_bind_text(stmt, 3, hint, -1, SQLITE_STATIC);
         } else {
            sqlite3_bind_null(stmt, 3);
         }
         sqlite3_bind_int64(stmt, 4, (int64_t)now);
         sqlite3_bind_int64(stmt, 5, (int64_t)(now + MESSAGING_LINK_TTL_SECONDS));
         rc = sqlite3_step(stmt);
         sqlite3_finalize(stmt);
      }
      AUTH_DB_UNLOCK();

      if (rc == SQLITE_DONE) {
         OLOG_INFO("messaging_engine: issued link code for user %d (hint='%s')", user_id,
                   provider_hint ? provider_hint : "");
         return MESSAGING_SUCCESS;
      }
      /* SQLITE_CONSTRAINT means the (unlikely) collision case — retry. */
   }
   return MESSAGING_FAILURE;
}

messaging_link_state_t messaging_engine_link_status(const char *code) {
   if (!code || code[0] == '\0') {
      return MESSAGING_LINK_STATE_NOT_FOUND;
   }
   messaging_link_state_t state = MESSAGING_LINK_STATE_NOT_FOUND;
   AUTH_DB_LOCK_OR_RETURN(MESSAGING_LINK_STATE_NOT_FOUND);
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT claimed_at, expires_at FROM messaging_link_codes "
                          "WHERE code = ?",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, code, -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         int64_t claimed = sqlite3_column_int64(stmt, 0);
         int64_t expires = sqlite3_column_int64(stmt, 1);
         if (claimed > 0) {
            state = MESSAGING_LINK_STATE_CLAIMED;
         } else if (expires < (int64_t)time(NULL)) {
            state = MESSAGING_LINK_STATE_EXPIRED;
         } else {
            state = MESSAGING_LINK_STATE_PENDING;
         }
      }
   }
   if (stmt) {
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();
   return state;
}

/* Async sends still running. */
static atomic_int s_async_inflight;

int engine_wait_async_sends(int timeout_ms) {
   const struct timespec step = { 0, 10 * 1000 * 1000 }; /* 10 ms */
   for (int waited_ms = 0; atomic_load(&s_async_inflight) > 0; waited_ms += 10) {
      if (waited_ms >= timeout_ms) {
         return FAILURE;
      }
      nanosleep(&step, NULL);
   }
   return SUCCESS;
}

static void *async_send_thread(void *arg) {
   async_send_item_t *item = (async_send_item_t *)arg;
   if (!item) {
      return NULL;
   }
   int rc = FAILURE;
   if (item->drv && item->text) {
      if (item->log_text && item->drv->send_text_unlogged) {
         rc = item->drv->send_text_unlogged(item->user_id, item->provider_address,
                                            item->address_json, item->text, item->log_text);
      } else if (item->drv->send_text) {
         /* A driver without the unlogged hook keeps no copy of what it sends. */
         rc = item->drv->send_text(item->user_id, item->provider_address, item->address_json,
                                   item->text);
      }
   }
   if (rc != SUCCESS && item->refund_channel_id > 0) {
      verify_send_failed(item->refund_channel_id, item->refund_hash);
   }
   if (item->text && item->log_text) {
      sodium_memzero(item->text, strlen(item->text));
   }
   free(item->text);
   free(item->log_text);
   free(item);
   atomic_fetch_sub(&s_async_inflight, 1);
   return NULL;
}

/* Render an engine-authored system string (link / new confirmation) into the
 * driver's native format.  These are short and never split, so we render with
 * a generous single-message cap and take the first part.  Returns a heap
 * string (caller owns), or a raw strdup fallback if formatting fails. */
static char *format_system_text(const messaging_driver_t *drv, const char *text) {
   char **parts = NULL;
   size_t nparts = 0;
   char err[256] = { 0 };
   char *out = NULL;
   if (messaging_format_render_split(text, drv->out_format, 1u << 20, &parts, &nparts, err,
                                     sizeof(err)) == SUCCESS &&
       nparts >= 1) {
      out = strdup(parts[0]);
   }
   messaging_format_free_parts(parts, nparts);
   if (!out) {
      out = strdup(text);
   }
   return out;
}

static void send_async_impl(const messaging_driver_t *drv,
                            int user_id,
                            const char *provider_address,
                            const char *address_json,
                            const char *text,
                            const char *log_text,
                            int64_t refund_channel_id,
                            const char *refund_hash) {
   if (!drv || !drv->send_text || !text ||
       ((!provider_address || provider_address[0] == '\0') &&
        (!address_json || address_json[0] == '\0'))) {
      if (refund_channel_id > 0) {
         verify_send_failed(refund_channel_id, refund_hash);
      }
      return;
   }
   async_send_item_t *item = calloc(1, sizeof(*item));
   if (!item) {
      OLOG_WARNING("messaging: async-send alloc failed; dropping send to %s",
                   provider_address ? provider_address : "(no addr)");
      if (refund_channel_id > 0) {
         verify_send_failed(refund_channel_id, refund_hash);
      }
      return;
   }
   item->drv = drv;
   item->user_id = user_id;
   item->refund_channel_id = refund_channel_id;
   if (refund_hash) {
      snprintf(item->refund_hash, sizeof(item->refund_hash), "%s", refund_hash);
   }
   if (provider_address) {
      snprintf(item->provider_address, sizeof(item->provider_address), "%s", provider_address);
   }
   if (address_json) {
      snprintf(item->address_json, sizeof(item->address_json), "%s", address_json);
   }
   /* Render into the driver's native format here (not at the call sites) so
    * every async confirmation — /link, Telegram /new, SMS /new — is escaped
    * and converted exactly once.  Critical for Telegram: a stray & or < in a
    * confirmation string would otherwise reach the parse_mode=HTML send and
    * get the whole message rejected. */
   item->text = format_system_text(drv, text);
   if (log_text) {
      item->log_text = strdup(log_text);
   }
   if (!item->text || (log_text && !item->log_text)) {
      OLOG_WARNING("messaging: async-send format failed; dropping send to %s",
                   provider_address ? provider_address : "(no addr)");
      free(item->text);
      free(item->log_text);
      free(item);
      if (refund_channel_id > 0) {
         verify_send_failed(refund_channel_id, refund_hash);
      }
      return;
   }

   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   pthread_attr_setstacksize(&attr, 64 * 1024);

   pthread_t tid;
   atomic_fetch_add(&s_async_inflight, 1);
   int rc = pthread_create(&tid, &attr, async_send_thread, item);
   pthread_attr_destroy(&attr);
   if (rc != 0) {
      atomic_fetch_sub(&s_async_inflight, 1);
      OLOG_WARNING("messaging: async-send pthread_create failed (rc=%d); dropping send", rc);
      if (item->log_text) {
         sodium_memzero(item->text, strlen(item->text));
      }
      if (item->refund_channel_id > 0) {
         verify_send_failed(item->refund_channel_id, item->refund_hash);
      }
      free(item->text);
      free(item->log_text);
      free(item);
   }
}

void engine_send_async(const messaging_driver_t *drv,
                       int user_id,
                       const char *provider_address,
                       const char *address_json,
                       const char *text) {
   send_async_impl(drv, user_id, provider_address, address_json, text, NULL, 0, NULL);
}

/* As engine_send_async, for a verification code: its content is kept out of
 * DAWN's SMS log (readable through the phone tool; `log_text` is recorded in
 * its place), and if it can't be sent the code is voided and its send given
 * back (see verify_send_failed). */
static void engine_send_async_code(const messaging_driver_t *drv,
                                   int user_id,
                                   const char *provider_address,
                                   const char *address_json,
                                   const char *text,
                                   const char *log_text,
                                   int64_t channel_id,
                                   const char *hash) {
   send_async_impl(drv, user_id, provider_address, address_json, text, log_text, channel_id, hash);
}

void link_attempt_log(const char *provider,
                      const char *sender_address,
                      const char *code_tried,
                      const char *result) {
   AUTH_DB_LOCK_OR_RETURN_VOID();
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "INSERT INTO messaging_link_attempts "
                          "(provider, sender_address, code_tried, result, created_at) "
                          "VALUES (?, ?, ?, ?, ?)",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, provider, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 2, sender_address, -1, SQLITE_STATIC);
      /* Enough of the code to tell attempts apart, not enough to use it: a
       * refused code can still be live. */
      char shown[8];
      if (code_tried) {
         snprintf(shown, sizeof(shown), "%.3s*", code_tried);
         sqlite3_bind_text(stmt, 3, shown, -1, SQLITE_TRANSIENT);
      } else {
         sqlite3_bind_null(stmt, 3);
      }
      sqlite3_bind_text(stmt, 4, result, -1, SQLITE_STATIC);
      sqlite3_bind_int64(stmt, 5, (int64_t)time(NULL));
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();
}

/* Proving an SMS number: a 6-digit code texted to it, valid
 * MESSAGING_VERIFY_TTL_SECONDS for MESSAGING_VERIFY_MAX_ATTEMPTS tries.
 * Codes sent are budgeted per number and per user over a day, so neither a
 * forged /link nor resends can buy unlimited guesses or text a number (or
 * spend the daemon's SMS budget) at will. */
#define MESSAGING_VERIFY_CODE_LEN 6
_Static_assert(MESSAGING_VERIFY_CODE_LEN == REPLY_CODE_DIGITS, "a link code is a reply code");
#define MESSAGING_VERIFY_TTL_SECONDS (10 * 60)
/* Tries per channel per day, across every code sent in that day: resending
 * doesn't buy more guesses. */
#define MESSAGING_VERIFY_MAX_ATTEMPTS 10
#define MESSAGING_VERIFY_WINDOW_SECONDS (24 * 60 * 60)
#define MESSAGING_VERIFY_SENDS_PER_NUMBER 3 /* across every account linking it */
#define MESSAGING_VERIFY_SENDS_PER_USER 5
#define MESSAGING_VERIFY_HASH_HEX REPLY_CODE_DIGEST_HEX /* keyed: core/reply_code.h */

/* Read the single link-code token after "/link " (whitespace-trimmed,
 * uppercased).  @return its length. */
static bool link_space(char c) {
   return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static size_t parse_link_code(const char *code_part, char code[MESSAGING_LINK_CODE_BUF_SIZE]) {
   while (link_space(*code_part)) {
      code_part++;
   }
   size_t k = 0;
   for (size_t i = 0;
        code_part[i] != '\0' && !link_space(code_part[i]) && k < MESSAGING_LINK_CODE_BUF_SIZE - 1;
        i++) {
      code[k++] = (char)toupper((unsigned char)code_part[i]);
   }
   code[k] = '\0';
   return k;
}

/* Use up a link code that was refused where it can't stay secret: it expires
 * now (so its status reads expired, not linked).  Caller holds the auth_db
 * lock. */
static void link_code_burn_locked(const char *code) {
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE messaging_link_codes SET expires_at = 0 "
                          "WHERE code = ? AND claimed_at IS NULL",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, code, -1, SQLITE_STATIC);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
   }
}

const char *messaging_link_command_args(const char *body) {
   if (!body) {
      return NULL;
   }
   while (link_space(*body)) {
      body++;
   }
   const bool slashed = body[0] == '/';
   const char *p = slashed ? body + 1 : body;
   if (strncasecmp(p, "link", 4) != 0) {
      return NULL;
   }
   p += 4;
   /* Telegram's group form names the bot: "/link@SomeBot CODE". */
   if (slashed && *p == '@') {
      while (*p && !link_space(*p)) {
         p++;
      }
   }
   if (!link_space(*p)) {
      return NULL;
   }
   while (link_space(*p)) {
      p++;
   }
   /* Without the slash ("link CODE", for Slack), only a whole message that is
    * a code makes it a command, so "link received" stays a message. */
   if (!slashed) {
      if (!link_code_well_formed(p)) {
         return NULL;
      }
      const char *end = p;
      while (*end && !link_space(*end)) {
         end++;
      }
      while (link_space(*end)) {
         end++;
      }
      if (*end != '\0') {
         return NULL;
      }
   }
   return p;
}

/* A code's shape: MESSAGING_LINK_CODE_LEN characters of the Crockford
 * alphabet codes are made from (any case). */
bool link_code_well_formed(const char *code_part) {
   char code[MESSAGING_LINK_CODE_BUF_SIZE] = { 0 };
   if (!code_part || parse_link_code(code_part, code) != MESSAGING_LINK_CODE_LEN) {
      return false;
   }
   for (size_t i = 0; i < MESSAGING_LINK_CODE_LEN; i++) {
      if (!memchr(CROCKFORD_ALPHABET, code[i], sizeof(CROCKFORD_ALPHABET))) {
         return false;
      }
   }
   return true;
}

void link_code_burn(const char *code_part) {
   if (!code_part) {
      return;
   }
   char code[MESSAGING_LINK_CODE_BUF_SIZE] = { 0 };
   if (parse_link_code(code_part, code) != MESSAGING_LINK_CODE_LEN) {
      return;
   }
   AUTH_DB_LOCK_OR_RETURN_VOID();
   link_code_burn_locked(code);
   AUTH_DB_UNLOCK();
}

/* Another DAWN user's live row for the same person in the same chat (the
 * owner index allows one).  In a one-to-one chat an unowned row is that
 * same person too (it binds its owner on its next message).  Caller holds
 * the auth_db lock. */
static bool linked_for_another_user_locked(const char *provider,
                                           const char *address,
                                           const char *sender_id,
                                           messaging_chat_kind_t kind,
                                           int user_id) {
   sqlite3_stmt *stmt = NULL;
   bool taken = false;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT 1 FROM messaging_channels WHERE provider = ? AND "
                          "provider_address = ? AND " MESSAGING_LIVE_SQL " AND user_id != ? AND "
                          "(COALESCE(owner_sender,'') = COALESCE(?4,'') OR "
                          "(?5 AND owner_sender IS NULL)) LIMIT 1",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, provider, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 2, address, -1, SQLITE_STATIC);
      sqlite3_bind_int(stmt, 3, user_id);
      if (sender_id) {
         sqlite3_bind_text(stmt, 4, sender_id, -1, SQLITE_STATIC);
      } else {
         sqlite3_bind_null(stmt, 4);
      }
      sqlite3_bind_int(stmt, 5, kind == MESSAGING_CHAT_ONE_TO_ONE ? 1 : 0);
      taken = (sqlite3_step(stmt) == SQLITE_ROW);
      sqlite3_finalize(stmt);
   }
   return taken;
}

/* Live link codes this user holds (each can make DAWN text a number).
 * Caller holds the auth_db lock. */
static int live_link_codes_locked(int user_id, time_t now) {
   sqlite3_stmt *stmt = NULL;
   int n = 0;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT COUNT(*) FROM messaging_link_codes WHERE user_id = ? AND "
                          "claimed_at IS NULL AND expires_at > ?",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, user_id);
      sqlite3_bind_int64(stmt, 2, (int64_t)now);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         n = sqlite3_column_int(stmt, 0);
      }
      sqlite3_finalize(stmt);
   }
   return n;
}

/* Insert or refresh the channel row for a claimed link code: enabled, owned
 * by `sender_id`; usable now when the provider vouches for senders, else
 * (SMS) kept as it was proven: a row this user verified before stays
 * verified, a new or unproven one waits for a code.  Caller holds the
 * auth_db lock.  On success `*channel_id` and `*verified` describe the row.
 * @return MESSAGING_SUCCESS, MESSAGING_ALREADY_LINKED or MESSAGING_FAILURE. */
static int upsert_link_row_locked(int user_id,
                                  const char *provider,
                                  const char *address,
                                  const char *address_json,
                                  const char *sender_id,
                                  bool authenticates_sender,
                                  time_t now,
                                  int64_t *channel_id,
                                  bool *verified) {
   /* Default display_name is "<provider>_<addr_short>" — renamed in the
    * WebUI.  A re-link keeps the row's name and conversation and records
    * the new owner.  A verified row drops any code it was waiting for. */
   char default_name[64];
   snprintf(default_name, sizeof(default_name), "%s_%.16s", provider, address);
   sqlite3_stmt *stmt = NULL;
   int rc = MESSAGING_FAILURE;
   if (sqlite3_prepare_v2(
           s_db.db,
           "INSERT INTO messaging_channels "
           "(user_id, provider, provider_address, address_json, display_name, created_at, "
           "owner_sender, verified_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
           "ON CONFLICT(user_id, provider, provider_address) DO UPDATE SET "
           "is_enabled = 1, owner_sender = excluded.owner_sender, "
           "verified_at = COALESCE(messaging_channels.verified_at, excluded.verified_at), "
           "verify_code_hash = CASE WHEN COALESCE(messaging_channels.verified_at, "
           "excluded.verified_at) IS NOT NULL THEN NULL ELSE "
           "messaging_channels.verify_code_hash END "
           "RETURNING id, verified_at IS NOT NULL",
           -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, user_id);
      sqlite3_bind_text(stmt, 2, provider, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 3, address, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 4, address_json, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 5, default_name, -1, SQLITE_STATIC);
      sqlite3_bind_int64(stmt, 6, (int64_t)now);
      if (sender_id) {
         sqlite3_bind_text(stmt, 7, sender_id, -1, SQLITE_STATIC);
      } else {
         sqlite3_bind_null(stmt, 7);
      }
      if (authenticates_sender) {
         sqlite3_bind_int64(stmt, 8, (int64_t)now);
      } else {
         sqlite3_bind_null(stmt, 8);
      }
      int step = sqlite3_step(stmt);
      if (step == SQLITE_ROW) {
         *channel_id = sqlite3_column_int64(stmt, 0);
         *verified = sqlite3_column_int(stmt, 1) != 0;
         rc = MESSAGING_SUCCESS;
      } else if (sqlite3_extended_errcode(s_db.db) == SQLITE_CONSTRAINT_UNIQUE) {
         rc = MESSAGING_ALREADY_LINKED;
      } else {
         OLOG_ERROR("messaging: link row write failed: %s", sqlite3_errmsg(s_db.db));
      }
      sqlite3_finalize(stmt);
   }
   return rc;
}

/* A verification code that couldn't be sent: void it (it was never
 * delivered, so no one may try it) and give its send back.  Keyed on the
 * code's digest, so a newer code issued meanwhile (or a new day's window) is
 * left alone.  Takes the auth_db lock. */
static void verify_send_failed(int64_t channel_id, const char *hash) {
   if (!hash || !hash[0]) {
      return;
   }
   AUTH_DB_LOCK_OR_RETURN_VOID();
   sqlite3_stmt *stmt = NULL;
   int user_id = 0;
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE messaging_channels SET verify_code_hash = NULL, "
                          "verify_expires_at = 0, verify_sends = MAX(verify_sends - 1, 0) "
                          "WHERE id = ? AND verify_code_hash = ? RETURNING user_id",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, channel_id);
      sqlite3_bind_text(stmt, 2, hash, -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         user_id = sqlite3_column_int(stmt, 0);
      }
      sqlite3_finalize(stmt);
   }
   AUTH_DB_UNLOCK();
   if (user_id > 0) {
      OLOG_WARNING("messaging: verification code for channel %lld wasn't sent; voided, not "
                   "counted",
                   (long long)channel_id);
      webui_broadcast_messaging_channels_changed(user_id, channel_id, "code_voided", NULL);
   }
}

/* Give an unverified channel a fresh code, within the send budgets.  Caller
 * holds the auth_db lock.  @return MESSAGING_SUCCESS (code_out filled),
 * MESSAGING_RATE_LIMITED (budget spent; the current code, if any, stands),
 * MESSAGING_UNKNOWN_CHANNEL or MESSAGING_FAILURE. */
static int issue_verify_code_locked(int user_id,
                                    int64_t channel_id,
                                    time_t now,
                                    char code_out[MESSAGING_VERIFY_CODE_LEN + 1],
                                    char hash_out[MESSAGING_VERIFY_HASH_HEX]) {
   code_out[0] = '\0';
   hash_out[0] = '\0';
   const int64_t window_floor = (int64_t)now - MESSAGING_VERIFY_WINDOW_SECONDS;
   sqlite3_stmt *stmt = NULL;
   bool found = false;
   int number_sends = 0;
   int user_sends = 0;
   if (sqlite3_prepare_v2(
           s_db.db,
           "SELECT (SELECT COALESCE(SUM(verify_sends),0) FROM messaging_channels n WHERE "
           "  n.provider = c.provider AND n.provider_address = c.provider_address AND "
           "  COALESCE(n.verify_window_start,0) > ?3), "
           "(SELECT COALESCE(SUM(verify_sends),0) FROM messaging_channels u WHERE "
           "  u.user_id = ?1 AND COALESCE(u.verify_window_start,0) > ?3) "
           "FROM messaging_channels c WHERE c.id = ?2 AND c.user_id = ?1 AND c.is_enabled = 1 "
           "AND c.verified_at IS NULL",
           -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, user_id);
      sqlite3_bind_int64(stmt, 2, channel_id);
      sqlite3_bind_int64(stmt, 3, window_floor);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         found = true;
         number_sends = sqlite3_column_int(stmt, 0);
         user_sends = sqlite3_column_int(stmt, 1);
      }
      sqlite3_finalize(stmt);
      stmt = NULL;
   }
   if (!found) {
      return MESSAGING_UNKNOWN_CHANNEL;
   }
   if (number_sends >= MESSAGING_VERIFY_SENDS_PER_NUMBER ||
       user_sends >= MESSAGING_VERIFY_SENDS_PER_USER) {
      return MESSAGING_RATE_LIMITED;
   }

   char code[MESSAGING_VERIFY_CODE_LEN + 1];
   reply_code_new(code);
   char hash_hex[MESSAGING_VERIFY_HASH_HEX];
   reply_code_digest(code, hash_hex);

   int rc = MESSAGING_FAILURE;
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE messaging_channels SET verify_code_hash = ?1, "
                          "verify_expires_at = ?2, "
                          "verify_attempts = CASE WHEN COALESCE(verify_window_start,0) > ?3 "
                          "THEN verify_attempts ELSE 0 END, "
                          "verify_sends = CASE WHEN COALESCE(verify_window_start,0) > ?3 "
                          "THEN verify_sends + 1 ELSE 1 END, "
                          "verify_window_start = CASE WHEN COALESCE(verify_window_start,0) > ?3 "
                          "THEN verify_window_start ELSE ?4 END "
                          "WHERE id = ?5",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, hash_hex, -1, SQLITE_STATIC);
      sqlite3_bind_int64(stmt, 2, (int64_t)now + MESSAGING_VERIFY_TTL_SECONDS);
      sqlite3_bind_int64(stmt, 3, window_floor);
      sqlite3_bind_int64(stmt, 4, (int64_t)now);
      sqlite3_bind_int64(stmt, 5, channel_id);
      if (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(s_db.db) == 1) {
         rc = MESSAGING_SUCCESS;
         snprintf(code_out, MESSAGING_VERIFY_CODE_LEN + 1, "%s", code);
         snprintf(hash_out, MESSAGING_VERIFY_HASH_HEX, "%s", hash_hex);
      }
      sqlite3_finalize(stmt);
   }
   sodium_memzero(code, sizeof(code));
   return rc;
}

/* Text a verification code to its number, keeping it out of the SMS log. */
static void send_verify_code(const messaging_driver_t *drv,
                             int user_id,
                             int64_t channel_id,
                             const char *address,
                             const char *address_json,
                             const char *code,
                             const char *hash) {
   if (!drv) {
      verify_send_failed(channel_id, hash);
      return;
   }
   char text[192];
   snprintf(text, sizeof(text),
            "DAWN code: %s. Enter it in the DAWN web app (Settings, Messaging Channels) to link "
            "this number. If you didn't ask for this, ignore this text.",
            code);
   engine_send_async_code(drv, user_id, address, address_json, text,
                          "DAWN verification code (not kept)", channel_id, hash);
   sodium_memzero(text, sizeof(text));
}

int handle_link_command(const char *provider,
                        const char *sender_address,
                        const char *sender_id,
                        messaging_chat_kind_t kind,
                        const char *code_part) {
   char code[MESSAGING_LINK_CODE_BUF_SIZE] = { 0 };
   size_t k = parse_link_code(code_part, code);
   if (k != MESSAGING_LINK_CODE_LEN) {
      link_attempt_log(provider, sender_address, code, "invalid");
      OLOG_WARNING("messaging: bad /link code shape from %s:%s (len=%zu)", provider, sender_address,
                   k);
      return MESSAGING_FAILURE;
   }

   const messaging_driver_t *drv = find_driver(provider);
   if (!drv) {
      return MESSAGING_FAILURE;
   }
   if (sender_id && sender_id[0] == '\0') {
      sender_id = NULL;
   }
   /* A code refused where others can read it is used up, so no one else can
    * claim it: in a group everyone sees it, and an SMS passes through the
    * phone service and its MQTT broker in the clear. */
   const bool burn_on_refusal = kind == MESSAGING_CHAT_SHARED || !drv->authenticates_sender;
   const bool shared = kind == MESSAGING_CHAT_SHARED;

   if (!drv->authenticates_sender) {
      sender_id = NULL; /* a provider that doesn't vouch for senders has none to record */
   } else if (!sender_id) {
      /* A chat link belongs to the person who sent the code; with no
       * provider-known sender (an anonymous admin, a forward) there is no one
       * to link it to. */
      link_attempt_log(provider, sender_address, code, "invalid");
      if (burn_on_refusal) {
         link_code_burn(code);
      }
      OLOG_WARNING("messaging: /link from %s:%s with no known sender — refused", provider,
                   sender_address);
      return MESSAGING_FAILURE;
   }

   /* Build the per-provider address_json blob and validate it before any
    * state changes.  This is the single source of truth for what shape each
    * provider's row carries. */
   char address_json[MESSAGING_ADDRESS_JSON_BUF_SIZE];
   build_address_json_for(provider, sender_address, address_json, sizeof(address_json));
   if (drv->validate_address && drv->validate_address(address_json) != SUCCESS) {
      link_attempt_log(provider, sender_address, code, "invalid");
      if (burn_on_refusal) {
         link_code_burn(code);
      }
      return MESSAGING_FAILURE;
   }

   AUTH_DB_LOCK_OR_FAIL();
   int user_id = 0;

   /* Read row first to capture user_id, validity and the app it was made for. */
   sqlite3_stmt *stmt = NULL;
   bool found = false;
   int64_t expires_at = 0;
   int64_t claimed_at = 0;
   char hint[16] = { 0 };
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT user_id, expires_at, COALESCE(claimed_at,0), "
                          "COALESCE(provider_hint,'') FROM messaging_link_codes WHERE code = ?",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, code, -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         found = true;
         user_id = sqlite3_column_int(stmt, 0);
         expires_at = sqlite3_column_int64(stmt, 1);
         claimed_at = sqlite3_column_int64(stmt, 2);
         const unsigned char *h = sqlite3_column_text(stmt, 3);
         snprintf(hint, sizeof(hint), "%s", h ? (const char *)h : "");
      }
      sqlite3_finalize(stmt);
      stmt = NULL;
   }

   if (!found) {
      AUTH_DB_UNLOCK();
      link_attempt_log(provider, sender_address, code, "unknown");
      OLOG_WARNING("messaging: unknown /link code from %s:%s", provider, sender_address);
      return MESSAGING_FAILURE;
   }

   time_t now = time(NULL);
   if (claimed_at > 0 || expires_at < (int64_t)now) {
      AUTH_DB_UNLOCK();
      link_attempt_log(provider, sender_address, code, claimed_at > 0 ? "claimed" : "expired");
      OLOG_WARNING("messaging: %s /link code from %s:%s",
                   claimed_at > 0 ? "already-claimed" : "expired", provider, sender_address);
      return MESSAGING_FAILURE;
   }

   /* A code made for one app links only that app.  In a private chat app
    * conversation it isn't used up, so the user can still send it where it
    * was meant. */
   if (hint[0] && strcmp(hint, provider) != 0) {
      if (burn_on_refusal) {
         link_code_burn_locked(code);
      }
      AUTH_DB_UNLOCK();
      link_attempt_log(provider, sender_address, code, "wrong_provider");
      OLOG_WARNING("messaging: /link code for %s used from %s:%s — refused", hint, provider,
                   sender_address);
      return MESSAGING_FAILURE;
   }

   /* One DAWN account per person per chat.  Checked before the code is
    * consumed (outside a group) so the user can still use it once the other
    * link is gone. */
   if (linked_for_another_user_locked(provider, sender_address, sender_id, kind, user_id)) {
      if (burn_on_refusal) {
         link_code_burn_locked(code);
      }
      AUTH_DB_UNLOCK();
      link_attempt_log(provider, sender_address, code, "already_linked");
      OLOG_WARNING("messaging: /link from %s:%s — already linked for this sender in another "
                   "account",
                   provider, sender_address);
      if (drv->authenticates_sender) {
         engine_send_async(drv, user_id, sender_address, address_json,
                           "This chat is already linked to another DAWN account for you. Unlink it "
                           "there first.");
      }
      return MESSAGING_ALREADY_LINKED;
   }

   /* The claim and the channel row go together: if the row can't be
    * written, the code stays unused. */
   if (sqlite3_exec(s_db.db, "SAVEPOINT link_claim", NULL, NULL, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MESSAGING_FAILURE;
   }

   /* Atomic claim: UPDATE...WHERE code=? AND claimed_at IS NULL AND
    * expires_at>?.  changes() tells us whether we won. */
   int claimed_rows = 0;
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE messaging_link_codes SET claimed_at = ? "
                          "WHERE code = ? AND claimed_at IS NULL AND expires_at > ?",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, (int64_t)now);
      sqlite3_bind_text(stmt, 2, code, -1, SQLITE_STATIC);
      sqlite3_bind_int64(stmt, 3, (int64_t)now);
      if (sqlite3_step(stmt) == SQLITE_DONE) {
         claimed_rows = sqlite3_changes(s_db.db);
      }
      sqlite3_finalize(stmt);
   }

   if (claimed_rows == 0) {
      /* Lost the atomic claim race — someone else claimed it between our
       * read and UPDATE.  Same operator-facing meaning as already-claimed. */
      sqlite3_exec(s_db.db, "ROLLBACK TO link_claim; RELEASE link_claim", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      link_attempt_log(provider, sender_address, code, "claimed");
      return MESSAGING_FAILURE;
   }

   int64_t channel_id = 0;
   bool verified = false;
   int rc = upsert_link_row_locked(user_id, provider, sender_address, address_json, sender_id,
                                   drv->authenticates_sender, now, &channel_id, &verified);
   if (rc != MESSAGING_SUCCESS) {
      sqlite3_exec(s_db.db, "ROLLBACK TO link_claim", NULL, NULL, NULL);
   }
   if (sqlite3_exec(s_db.db, "RELEASE link_claim", NULL, NULL, NULL) != SQLITE_OK) {
      /* Leave no transaction open on the shared connection. */
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      rc = MESSAGING_FAILURE;
   }
   if (rc != MESSAGING_SUCCESS && burn_on_refusal) {
      link_code_burn_locked(code);
   }
   char verify_code[MESSAGING_VERIFY_CODE_LEN + 1] = { 0 };
   char verify_hash[MESSAGING_VERIFY_HASH_HEX] = { 0 };
   int issue_rc = MESSAGING_SUCCESS;
   if (rc == MESSAGING_SUCCESS && !verified) {
      issue_rc = issue_verify_code_locked(user_id, channel_id, now, verify_code, verify_hash);
   }
   AUTH_DB_UNLOCK();

   if (rc != MESSAGING_SUCCESS) {
      link_attempt_log(provider, sender_address, code,
                       rc == MESSAGING_ALREADY_LINKED ? "already_linked" : "error");
      return rc;
   }

   /* The panel shows the new or re-linked channel (a pending SMS one with its
    * code box) without waiting for a refresh. */
   webui_broadcast_messaging_channels_changed(user_id, channel_id, verified ? "linked" : "pending",
                                              code);

   /* Reply through the driver.  ASYNC — this code path runs on the mosquitto
    * network callback thread for SMS (echo/events delivery), and a
    * synchronous send_text would self-deadlock waiting for echo/response
    * while the mosquitto thread is busy executing us.  Reuses the same
    * address_json shape the row carries; sender_address is the typed
    * primary key so the driver can skip the JSON parse. */
   if (!verified) {
      if (issue_rc == MESSAGING_SUCCESS) {
         link_attempt_log(provider, sender_address, code, "verify_sent");
         OLOG_INFO("messaging: channel %lld (%s:%s) waits for its verification code (user %d)",
                   (long long)channel_id, provider, sender_address, user_id);
         send_verify_code(drv, user_id, channel_id, sender_address, address_json, verify_code,
                          verify_hash);
         sodium_memzero(verify_code, sizeof(verify_code));
      } else {
         link_attempt_log(provider, sender_address, code, "verify_limit");
         OLOG_WARNING("messaging: channel %lld (%s:%s) — no verification code sent (daily "
                      "limit or error, rc=%d)",
                      (long long)channel_id, provider, sender_address, issue_rc);
      }
      return MESSAGING_SUCCESS;
   }

   link_attempt_log(provider, sender_address, code, "success");
   OLOG_INFO("messaging: linked %s:%s to user %d (channel %lld)", provider, sender_address, user_id,
             (long long)channel_id);
   engine_send_async(
       drv, user_id, sender_address, address_json,
       shared ? "This chat has been linked. The assistant answers only the person who linked it."
              : "Your channel has been linked. Messages here will now reach the assistant.");
   return MESSAGING_SUCCESS;
}

int messaging_engine_resend_verify_code(int user_id, int64_t channel_id) {
   if (user_id <= 0 || channel_id <= 0) {
      return MESSAGING_FAILURE;
   }
   if (!atomic_load(&s_initialized)) {
      return MESSAGING_FAILURE;
   }
   char provider[16] = { 0 };
   char address[128] = { 0 };
   char address_json[MESSAGING_ADDRESS_JSON_BUF_SIZE] = { 0 };
   char code[MESSAGING_VERIFY_CODE_LEN + 1] = { 0 };
   char hash[MESSAGING_VERIFY_HASH_HEX] = { 0 };

   AUTH_DB_LOCK_OR_RETURN(MESSAGING_FAILURE);
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT provider, provider_address, address_json FROM messaging_channels "
                          "WHERE id = ? AND user_id = ? AND is_enabled = 1 AND verified_at IS NULL",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, channel_id);
      sqlite3_bind_int(stmt, 2, user_id);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         const unsigned char *p = sqlite3_column_text(stmt, 0);
         const unsigned char *a = sqlite3_column_text(stmt, 1);
         const unsigned char *j = sqlite3_column_text(stmt, 2);
         snprintf(provider, sizeof(provider), "%s", p ? (const char *)p : "");
         snprintf(address, sizeof(address), "%s", a ? (const char *)a : "");
         snprintf(address_json, sizeof(address_json), "%s", j ? (const char *)j : "");
      }
      sqlite3_finalize(stmt);
   }
   /* Nothing can be texted without the provider's driver (an SMS link
    * needs the phone service): don't spend a send, or the current code. */
   const messaging_driver_t *drv = provider[0] ? find_driver(provider) : NULL;
   int rc = MESSAGING_UNKNOWN_CHANNEL;
   if (provider[0]) {
      rc = drv ? issue_verify_code_locked(user_id, channel_id, time(NULL), code, hash)
               : MESSAGING_DRIVER_NOT_REGISTERED;
   }
   AUTH_DB_UNLOCK();

   if (rc == MESSAGING_SUCCESS) {
      send_verify_code(drv, user_id, channel_id, address, address_json, code, hash);
      sodium_memzero(code, sizeof(code));
      link_attempt_log(provider, address, NULL, "verify_resent");
      webui_broadcast_messaging_channels_changed(user_id, channel_id, "code_sent", NULL);
   }
   return rc;
}

int messaging_engine_verify_channel(int user_id, int64_t channel_id, const char *code) {
   if (user_id <= 0 || channel_id <= 0 || !code) {
      return MESSAGING_FAILURE;
   }
   if (!atomic_load(&s_initialized)) {
      return MESSAGING_FAILURE;
   }
   /* Digits only, exactly the code's length (spaces a user typed dropped). */
   char digits[MESSAGING_VERIFY_CODE_LEN + 1] = { 0 };
   size_t n = 0;
   for (const char *c = code; *c; c++) {
      if (*c == ' ' || *c == '-') {
         continue;
      }
      if (!isdigit((unsigned char)*c) || n >= MESSAGING_VERIFY_CODE_LEN) {
         return MESSAGING_BAD_CODE;
      }
      digits[n++] = *c;
   }
   if (n != MESSAGING_VERIFY_CODE_LEN) {
      return MESSAGING_BAD_CODE;
   }

   char want[MESSAGING_VERIFY_HASH_HEX] = { 0 };
   char got[MESSAGING_VERIFY_HASH_HEX];
   reply_code_digest(digits, got);
   sodium_memzero(digits, sizeof(digits));

   char provider[16] = { 0 };
   char address[128] = { 0 };
   char address_json[MESSAGING_ADDRESS_JSON_BUF_SIZE] = { 0 };
   int64_t expires = 0;
   int attempts = 0;
   bool found = false;
   int rc = MESSAGING_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MESSAGING_FAILURE);
   /* One transaction: the try is counted, and kept, whatever the outcome. */
   if (sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MESSAGING_FAILURE;
   }
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT COALESCE(verify_code_hash,''), COALESCE(verify_expires_at,0), "
                          "verify_attempts, provider, provider_address, address_json "
                          "FROM messaging_channels WHERE id = ? AND user_id = ? AND "
                          "is_enabled = 1 AND verified_at IS NULL",
                          -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, channel_id);
      sqlite3_bind_int(stmt, 2, user_id);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         found = true;
         const unsigned char *h = sqlite3_column_text(stmt, 0);
         snprintf(want, sizeof(want), "%s", h ? (const char *)h : "");
         expires = sqlite3_column_int64(stmt, 1);
         attempts = sqlite3_column_int(stmt, 2);
         const unsigned char *p = sqlite3_column_text(stmt, 3);
         const unsigned char *a = sqlite3_column_text(stmt, 4);
         const unsigned char *j = sqlite3_column_text(stmt, 5);
         snprintf(provider, sizeof(provider), "%s", p ? (const char *)p : "");
         snprintf(address, sizeof(address), "%s", a ? (const char *)a : "");
         snprintf(address_json, sizeof(address_json), "%s", j ? (const char *)j : "");
      }
      sqlite3_finalize(stmt);
      stmt = NULL;
   }

   bool counted = false;
   if (!found) {
      rc = MESSAGING_UNKNOWN_CHANNEL;
   } else if (want[0] == '\0' || expires < (int64_t)time(NULL) ||
              attempts >= MESSAGING_VERIFY_MAX_ATTEMPTS) {
      rc = MESSAGING_BAD_CODE;
   } else if (sqlite3_prepare_v2(s_db.db,
                                 "UPDATE messaging_channels SET verify_attempts = "
                                 "verify_attempts + 1 WHERE id = ?",
                                 -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, channel_id);
      counted = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(s_db.db) == 1;
      sqlite3_finalize(stmt);
      stmt = NULL;
   }

   if (counted) {
      if (!reply_code_digest_equal(want, got)) {
         rc = MESSAGING_BAD_CODE;
      } else if (sqlite3_prepare_v2(s_db.db,
                                    "UPDATE messaging_channels SET verified_at = ?1, "
                                    "last_used_at = ?1, verify_code_hash = NULL, "
                                    "verify_expires_at = NULL, verify_attempts = 0 "
                                    "WHERE id = ?2 AND verified_at IS NULL",
                                    -1, &stmt, NULL) == SQLITE_OK) {
         /* Verifying counts as an exchange: the number can text right away,
          * without the wake word, for the conversation window. */
         sqlite3_bind_int64(stmt, 1, (int64_t)time(NULL));
         sqlite3_bind_int64(stmt, 2, channel_id);
         if (sqlite3_step(stmt) == SQLITE_DONE) {
            rc = (sqlite3_changes(s_db.db) > 0) ? MESSAGING_SUCCESS : MESSAGING_UNKNOWN_CHANNEL;
         } else if (sqlite3_extended_errcode(s_db.db) == SQLITE_CONSTRAINT_UNIQUE) {
            rc = MESSAGING_ALREADY_LINKED; /* another account verified this number meanwhile */
         }
         sqlite3_finalize(stmt);
      }
   }
   /* The try counts even when the code was wrong: commit unless the attempt
    * itself couldn't be recorded. */
   if (sqlite3_exec(s_db.db,
                    (found && !counted && rc != MESSAGING_BAD_CODE) ? "ROLLBACK" : "COMMIT", NULL,
                    NULL, NULL) != SQLITE_OK) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      rc = MESSAGING_FAILURE;
   }
   AUTH_DB_UNLOCK();

   if (rc == MESSAGING_SUCCESS) {
      webui_broadcast_messaging_channels_changed(user_id, channel_id, "verified", NULL);
      link_attempt_log(provider, address, NULL, "verified");
      OLOG_INFO("messaging: verified channel %lld (%s:%s) for user %d", (long long)channel_id,
                provider, address, user_id);
      /* Say how texts reach the assistant: freely within the conversation
       * window (opened just now), then starting with a greeting and its name. */
      char linked[256];
      char name[CONFIG_NAME_MAX];
      snprintf(name, sizeof(name), "%s", g_config.general.ai_name);
      name[0] = (char)toupper((unsigned char)name[0]);
      const bool window = g_config.messaging.sms_active_window_sec > 0;
      if (name[0]) {
         snprintf(linked, sizeof(linked), "This number is now linked. %sstart with \"Hey %s\".",
                  window ? "Text away; after a quiet spell, " : "Texts that reach me ", name);
      } else {
         snprintf(linked, sizeof(linked), "This number is now linked.");
      }
      engine_send_async(find_driver(provider), user_id, address, address_json, linked);
   } else if (rc == MESSAGING_BAD_CODE && found) {
      link_attempt_log(provider, address, NULL, "verify_failed");
      OLOG_WARNING("messaging: verification code refused for channel %lld (%s; tries used %d of "
                   "%d)",
                   (long long)channel_id, counted ? "wrong" : "expired or spent",
                   attempts + (counted ? 1 : 0), MESSAGING_VERIFY_MAX_ATTEMPTS);
   }
   return rc;
}
