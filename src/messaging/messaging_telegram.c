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
 * Telegram Bot API driver — long-poll inbound + REST outbound.  v1
 * text-only.  See docs/MESSAGING_CHANNELS_DESIGN.md §8 (Telegram).
 *
 * Connection reuse: single persistent CURL handle with TCP keep-alive
 * and HTTP/2 enabled.  Listener thread runs on a 64 KB pthread stack.
 *
 * Token redaction: the bot token lives in the URL path; the driver
 * builds a "safe" version of the URL for log output that replaces the
 * token with "<REDACTED>".
 */
#include "messaging/messaging_telegram.h"

#include <ctype.h>
#include <curl/curl.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <pthread.h>
#include <sodium.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/curl_buffer.h"
#include "dawn_error.h"
#include "logging.h"
#include "messaging/messaging_driver.h"
#include "messaging/messaging_engine.h"
#include "messaging/messaging_telegram_parse.h"

/* =============================================================================
 * Driver state
 * ============================================================================= */

/* Match CONFIG_API_KEY_MAX from include/config/dawn_config.h.  Today's
 * Telegram tokens are ~70 chars but raising the cap defends against
 * silent mid-token truncation if @BotFather ever issues longer ones. */
#define TG_BOT_TOKEN_MAX 256
#define TG_BASE_URL_MAX 320 /* "https://api.telegram.org/bot" + TG_BOT_TOKEN_MAX + null */
#define TG_USER_AGENT "DAWN-Telegram/0.1 (libcurl)"
#define TG_LONGPOLL_TIMEOUT 30 /* seconds */
#define TG_HTTP_TIMEOUT 45     /* seconds — must exceed long-poll timeout */
#define TG_LISTENER_STACK (64 * 1024)
#define TG_RECONNECT_BACKOFF 5 /* seconds between retries on transient errors */

static char s_bot_token[TG_BOT_TOKEN_MAX];
static char s_base_url[TG_BASE_URL_MAX]; /* "https://api.telegram.org/bot<TOKEN>" */
static atomic_bool s_running = ATOMIC_VAR_INIT(false);
static atomic_bool s_connected = ATOMIC_VAR_INIT(false);

static pthread_t s_listener_thread;
static bool s_listener_started = false;

static messaging_inbound_fn s_inbound_cb = NULL;
static pthread_mutex_t s_inbound_cb_mutex = PTHREAD_MUTEX_INITIALIZER;

static int64_t s_next_update_id = 0;

/* Per-thread CURL handles guard global state with their own mutex
 * because send_text() can be called from arbitrary worker threads. */
static CURL *s_send_curl = NULL;
static pthread_mutex_t s_send_curl_mutex = PTHREAD_MUTEX_INITIALIZER;

/* =============================================================================
 * Internal helpers
 * ============================================================================= */

/* Progress callback for both the listener long-poll and the send
 * handle.  Curl invokes this periodically (~1s default cadence) during
 * any transfer.  Returning non-zero aborts the transfer with
 * CURLE_ABORTED_BY_CALLBACK — the only way to break out of the 30-45
 * second long-poll cycle promptly when shutdown is signaled.  Without
 * this, pthread_join on the listener can wait the full HTTP timeout. */
static int tg_progress_callback(void *clientp,
                                curl_off_t dltotal,
                                curl_off_t dlnow,
                                curl_off_t ultotal,
                                curl_off_t ulnow) {
   (void)clientp;
   (void)dltotal;
   (void)dlnow;
   (void)ultotal;
   (void)ulnow;
   return atomic_load(&s_running) ? 0 : 1;
}

/* Build a logger-safe URL representation by replacing the token in the
 * path with "<REDACTED>".  Caller-supplied buffer. */
static void redact_url(const char *url, char *out, size_t out_size) {
   if (!url || !out || out_size == 0) {
      return;
   }
   const char *bot_prefix = strstr(url, "/bot");
   if (!bot_prefix) {
      snprintf(out, out_size, "%s", url);
      return;
   }
   /* Find the next '/' after "/bot<TOKEN>". */
   const char *after_token = strchr(bot_prefix + 4, '/');
   if (!after_token) {
      after_token = bot_prefix + strlen(bot_prefix);
   }
   /* Length of the URL up to /bot. */
   size_t prefix_len = (size_t)(bot_prefix - url) + 4; /* include "/bot" */
   if (prefix_len >= out_size) {
      snprintf(out, out_size, "%s", url);
      return;
   }
   memcpy(out, url, prefix_len);
   const char *redacted = "<REDACTED>";
   size_t r_len = strlen(redacted);
   size_t tail_len = strlen(after_token);
   if (prefix_len + r_len + tail_len + 1 > out_size) {
      snprintf(out + prefix_len, out_size - prefix_len, "<REDACTED>");
      return;
   }
   memcpy(out + prefix_len, redacted, r_len);
   memcpy(out + prefix_len + r_len, after_token, tail_len);
   out[prefix_len + r_len + tail_len] = '\0';
}

/* =============================================================================
 * Outbound — sendMessage
 * ============================================================================= */

static int tg_extract_chat_id(const char *address_json, char *chat_id_out, size_t out_size) {
   if (!address_json || !chat_id_out || out_size == 0) {
      return FAILURE;
   }
   struct json_object *obj = json_tokener_parse(address_json);
   if (!obj) {
      return FAILURE;
   }
   struct json_object *cid_obj = NULL;
   int rc = FAILURE;
   if (json_object_object_get_ex(obj, "chat_id", &cid_obj) && cid_obj) {
      const char *s = json_object_get_string(cid_obj);
      if (s && s[0]) {
         snprintf(chat_id_out, out_size, "%s", s);
         rc = SUCCESS;
      }
   }
   json_object_put(obj);
   return rc;
}

static int tg_send_text(int user_id,
                        const char *provider_address,
                        const char *address_json,
                        const char *text) {
   /* Telegram bot token is bot-wide; user_id is irrelevant to the
    * send mechanics.  Accepted for contract uniformity. */
   (void)user_id;
   if (!text) {
      return FAILURE;
   }
   if (s_base_url[0] == '\0') {
      return FAILURE;
   }
   /* Prefer the typed provider_address (chat_id as decimal string).
    * Fall back to parsing address_json for callers that haven't yet
    * been migrated to the new contract.  The fallback covers the
    * narrow window between contract change and engine update; once
    * every caller passes provider_address explicitly the address_json
    * branch becomes dead code but stays as a safety net. */
   char chat_id[64];
   chat_id[0] = '\0';
   if (provider_address && provider_address[0] != '\0') {
      snprintf(chat_id, sizeof(chat_id), "%s", provider_address);
   } else if (address_json) {
      if (tg_extract_chat_id(address_json, chat_id, sizeof(chat_id)) != SUCCESS) {
         OLOG_WARNING("telegram: send failed — no chat_id in provider_address or address_json");
         return FAILURE;
      }
   } else {
      OLOG_WARNING("telegram: send failed — no provider_address or address_json");
      return FAILURE;
   }

   /* Build JSON request body. */
   struct json_object *body = json_object_new_object();
   if (!body) {
      return FAILURE;
   }
   json_object_object_add(body, "chat_id", json_object_new_string(chat_id));
   json_object_object_add(body, "text", json_object_new_string(text));
   /* PRECONDITION: `text` MUST already be valid, balanced Telegram HTML — the
    * engine renders every outbound message into the driver's out_format
    * (MSG_FMT_TELEGRAM_HTML) via messaging_deliver / engine_send_async before
    * calling send_text.  Telegram returns HTTP 400 and rejects the WHOLE
    * message on an unclosed/overlapping tag or an unescaped <, > or &, so this
    * must never be called with raw markdown or unescaped user text. */
   json_object_object_add(body, "parse_mode", json_object_new_string("HTML"));
   const char *body_str = json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN);

   char url[TG_BASE_URL_MAX + 32];
   snprintf(url, sizeof(url), "%s/sendMessage", s_base_url);

   curl_buffer_t resp;
   curl_buffer_init(&resp);
   int rc = FAILURE;

   pthread_mutex_lock(&s_send_curl_mutex);
   if (!s_send_curl) {
      s_send_curl = curl_easy_init();
   }
   if (s_send_curl) {
      curl_easy_reset(s_send_curl);
      curl_easy_setopt(s_send_curl, CURLOPT_URL, url);
      curl_easy_setopt(s_send_curl, CURLOPT_POST, 1L);
      curl_easy_setopt(s_send_curl, CURLOPT_POSTFIELDS, body_str);
      curl_apply_dawn_defaults(s_send_curl, TG_USER_AGENT, MESSAGING_SEND_TIMEOUT_SEC, &resp);
      struct curl_slist *hdrs = NULL;
      hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
      curl_easy_setopt(s_send_curl, CURLOPT_HTTPHEADER, hdrs);

      CURLcode cc = curl_easy_perform(s_send_curl);
      long http_status = 0;
      curl_easy_getinfo(s_send_curl, CURLINFO_RESPONSE_CODE, &http_status);
      curl_slist_free_all(hdrs);

      if (cc == CURLE_OK && http_status >= 200 && http_status < 300) {
         /* Check ok=true */
         struct json_object *r = json_tokener_parse(resp.data ? resp.data : "");
         if (r) {
            struct json_object *ok_obj = NULL;
            if (json_object_object_get_ex(r, "ok", &ok_obj) && json_object_get_boolean(ok_obj)) {
               rc = SUCCESS;
            }
            json_object_put(r);
         }
      } else {
         char safe[TG_BASE_URL_MAX + 32];
         redact_url(url, safe, sizeof(safe));
         OLOG_WARNING("telegram: sendMessage failed (curl=%d http=%ld) on %s", cc, http_status,
                      safe);
      }
   }
   pthread_mutex_unlock(&s_send_curl_mutex);

   curl_buffer_free(&resp);
   json_object_put(body);
   return rc;
}

/* Typing-indicator POST.  Fire-and-forget; errors logged at DEBUG and
 * swallowed — typing must never block real delivery.  Reuses the same
 * send_curl handle + mutex as tg_send_text (only one of
 * typing-or-message is in flight at a time per driver). */
static void tg_send_typing(int user_id, const char *provider_address, const char *address_json) {
   (void)user_id;
   if (s_base_url[0] == '\0') {
      return;
   }
   char chat_id[64];
   chat_id[0] = '\0';
   if (provider_address && provider_address[0] != '\0') {
      snprintf(chat_id, sizeof(chat_id), "%s", provider_address);
   } else if (address_json) {
      if (tg_extract_chat_id(address_json, chat_id, sizeof(chat_id)) != SUCCESS) {
         return;
      }
   } else {
      return;
   }

   struct json_object *body = json_object_new_object();
   if (!body) {
      return;
   }
   json_object_object_add(body, "chat_id", json_object_new_string(chat_id));
   json_object_object_add(body, "action", json_object_new_string("typing"));
   const char *body_str = json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN);

   char url[TG_BASE_URL_MAX + 32];
   snprintf(url, sizeof(url), "%s/sendChatAction", s_base_url);

   curl_buffer_t resp;
   curl_buffer_init(&resp);

   pthread_mutex_lock(&s_send_curl_mutex);
   if (!s_send_curl) {
      s_send_curl = curl_easy_init();
   }
   if (s_send_curl) {
      curl_easy_reset(s_send_curl);
      curl_easy_setopt(s_send_curl, CURLOPT_URL, url);
      curl_easy_setopt(s_send_curl, CURLOPT_POST, 1L);
      curl_easy_setopt(s_send_curl, CURLOPT_POSTFIELDS, body_str);
      /* Tighter timeout than send_text: typing is best-effort + repeats
       * every ~4s, so a slow link shouldn't pin the keepalive thread. */
      curl_apply_dawn_defaults(s_send_curl, TG_USER_AGENT, 5L, &resp);
      struct curl_slist *hdrs = NULL;
      hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
      curl_easy_setopt(s_send_curl, CURLOPT_HTTPHEADER, hdrs);

      CURLcode cc = curl_easy_perform(s_send_curl);
      long http_status = 0;
      curl_easy_getinfo(s_send_curl, CURLINFO_RESPONSE_CODE, &http_status);
      curl_slist_free_all(hdrs);

      if (cc != CURLE_OK || http_status < 200 || http_status >= 300) {
         OLOG_DEBUG("telegram: sendChatAction failed (curl=%d http=%ld)", cc, http_status);
      }
   }
   pthread_mutex_unlock(&s_send_curl_mutex);

   curl_buffer_free(&resp);
   json_object_put(body);
}

/* =============================================================================
 * Inbound — getUpdates long-poll loop
 * ============================================================================= */

static void tg_handle_update(struct json_object *update) {
   struct json_object *upd_id_obj = NULL;
   struct json_object *msg_obj = NULL;
   if (!json_object_object_get_ex(update, "update_id", &upd_id_obj) || !upd_id_obj) {
      return;
   }
   int64_t upd_id = json_object_get_int64(upd_id_obj);
   if (upd_id >= s_next_update_id) {
      s_next_update_id = upd_id + 1;
   }

   if (!json_object_object_get_ex(update, "message", &msg_obj) || !msg_obj) {
      return; /* edits, channel posts, button presses: only new messages are answered */
   }

   /* A group upgraded to a supergroup gets a new chat id, so a link to the
    * old one stops answering.  Telegram says so in a service message; name
    * both ids so the user knows to re-link. */
   struct json_object *migrate_obj = NULL;
   struct json_object *chat_obj = NULL;
   struct json_object *old_id_obj = NULL;
   if (json_object_object_get_ex(msg_obj, "migrate_to_chat_id", &migrate_obj) && migrate_obj &&
       json_object_object_get_ex(msg_obj, "chat", &chat_obj) && chat_obj &&
       json_object_object_get_ex(chat_obj, "id", &old_id_obj) && old_id_obj) {
      OLOG_WARNING("telegram: group %" PRId64 " became supergroup %" PRId64
                   "; a channel linked to the group must be re-linked (send /link CODE there)",
                   json_object_get_int64(old_id_obj), json_object_get_int64(migrate_obj));
      return;
   }

   tg_message_t m;
   const char *why = NULL;
   if (!tg_parse_message(msg_obj, &m, &why)) {
      if (why) {
         OLOG_DEBUG("telegram: dropped update %" PRId64 " (%s)", upd_id, why);
      }
      return;
   }
   if (why) {
      OLOG_DEBUG("telegram: update %" PRId64 " has no single sender (%s)", upd_id, why);
   }

   /* Dispatch to engine. */
   messaging_inbound_fn cb = NULL;
   pthread_mutex_lock(&s_inbound_cb_mutex);
   cb = s_inbound_cb;
   pthread_mutex_unlock(&s_inbound_cb_mutex);
   if (cb) {
      messaging_inbound_t in = {
         .provider = "telegram",
         .provider_address = m.chat_id,
         .sender_id = m.sender_id[0] ? m.sender_id : NULL,
         .sender_display = m.sender_display,
         .body = m.body,
         .timestamp = m.timestamp,
         .chat_kind = m.chat_kind,
      };
      cb(&in);
   }
}

static void tg_poll_once(CURL *handle) {
   char url[TG_BASE_URL_MAX + 128];
   snprintf(url, sizeof(url), "%s/getUpdates?offset=%" PRId64 "&timeout=%d", s_base_url,
            s_next_update_id, TG_LONGPOLL_TIMEOUT);

   curl_buffer_t resp;
   curl_buffer_init(&resp);

   /* The static setopts (USERAGENT / KEEPALIVE / HTTP_VERSION /
    * WRITEFUNCTION / TIMEOUT) are applied ONCE in tg_listener_thread
    * before the poll loop — they don't change between cycles, and
    * curl_easy_reset would wipe the connection-reuse hints.  Only
    * URL and WRITEDATA need to be re-set per cycle. */
   curl_easy_setopt(handle, CURLOPT_URL, url);
   curl_easy_setopt(handle, CURLOPT_WRITEDATA, &resp);

   CURLcode cc = curl_easy_perform(handle);
   long http_status = 0;
   curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &http_status);

   if (cc != CURLE_OK || http_status < 200 || http_status >= 300) {
      /* CURLE_ABORTED_BY_CALLBACK is our own shutdown signal — exit
       * immediately rather than logging it as a failure or sleeping
       * the reconnect backoff.  The next loop iteration will see
       * s_running=false and the listener will return. */
      if (cc == CURLE_ABORTED_BY_CALLBACK) {
         atomic_store(&s_connected, false);
         curl_buffer_free(&resp);
         return;
      }
      char safe[TG_BASE_URL_MAX + 64];
      redact_url(url, safe, sizeof(safe));
      OLOG_WARNING("telegram: getUpdates failed (curl=%d http=%ld) on %s", cc, http_status, safe);
      atomic_store(&s_connected, false);
      curl_buffer_free(&resp);
      sleep(TG_RECONNECT_BACKOFF);
      return;
   }

   atomic_store(&s_connected, true);

   if (!resp.data) {
      curl_buffer_free(&resp);
      return;
   }

   struct json_object *r = json_tokener_parse(resp.data);
   if (!r) {
      OLOG_WARNING("telegram: failed to parse getUpdates response");
      curl_buffer_free(&resp);
      return;
   }

   struct json_object *ok_obj = NULL;
   if (json_object_object_get_ex(r, "ok", &ok_obj) && json_object_get_boolean(ok_obj)) {
      struct json_object *result_arr = NULL;
      if (json_object_object_get_ex(r, "result", &result_arr) &&
          json_object_is_type(result_arr, json_type_array)) {
         size_t n = json_object_array_length(result_arr);
         for (size_t i = 0; i < n; i++) {
            struct json_object *upd = json_object_array_get_idx(result_arr, i);
            if (upd) {
               tg_handle_update(upd);
            }
         }
      }
   } else {
      struct json_object *desc_obj = NULL;
      if (json_object_object_get_ex(r, "description", &desc_obj) && desc_obj) {
         OLOG_WARNING("telegram: getUpdates ok=false: %s", json_object_get_string(desc_obj));
      }
   }

   json_object_put(r);
   curl_buffer_free(&resp);
}

static void *tg_listener_thread(void *arg) {
   (void)arg;
   OLOG_INFO("telegram: listener thread started");

   CURL *handle = curl_easy_init();
   if (!handle) {
      OLOG_ERROR("telegram: curl_easy_init failed; listener exiting");
      return NULL;
   }

   /* One-time setopts that don't change per cycle.  Hoisting them
    * out of tg_poll_once saves the per-cycle curl_easy_reset that
    * was wiping connection-reuse hints.
    *
    * WRITEDATA is set per-cycle in tg_poll_once (resp buffer is a
    * stack-local in each poll), so pass NULL here — the helper still
    * applies WRITEFUNCTION + all other defenses.
    *
    * Progress callback lets shutdown abort the in-flight long-poll
    * promptly — without it, pthread_join on this thread can wait the
    * full TG_HTTP_TIMEOUT for the current cycle to finish. */
   curl_apply_dawn_defaults(handle, TG_USER_AGENT, (long)TG_HTTP_TIMEOUT, NULL);
   curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
   curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, tg_progress_callback);

   while (atomic_load(&s_running)) {
      tg_poll_once(handle);
   }

   curl_easy_cleanup(handle);
   atomic_store(&s_connected, false);
   OLOG_INFO("telegram: listener thread exiting");
   return NULL;
}

/* =============================================================================
 * Driver hooks
 * ============================================================================= */

static int tg_init(const char *credentials_json) {
   /* credentials_json is `{"bot_token":"..."}`.  Driver also reads
    * directly from g_secrets at startup if credentials_json is NULL. */
   char token[TG_BOT_TOKEN_MAX] = { 0 };
   if (credentials_json && credentials_json[0]) {
      struct json_object *obj = json_tokener_parse(credentials_json);
      if (obj) {
         struct json_object *tk = NULL;
         if (json_object_object_get_ex(obj, "bot_token", &tk) && tk) {
            snprintf(token, sizeof(token), "%s", json_object_get_string(tk));
         }
         json_object_put(obj);
      }
   }
   if (token[0] == '\0') {
      OLOG_ERROR("telegram: no bot_token supplied");
      return FAILURE;
   }

   snprintf(s_bot_token, sizeof(s_bot_token), "%s", token);
   snprintf(s_base_url, sizeof(s_base_url), "https://api.telegram.org/bot%s", token);

   /* Spawn listener thread with 64 KB stack. */
   atomic_store(&s_running, true);
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setstacksize(&attr, TG_LISTENER_STACK);
   int prc = pthread_create(&s_listener_thread, &attr, tg_listener_thread, NULL);
   pthread_attr_destroy(&attr);
   if (prc != 0) {
      atomic_store(&s_running, false);
      OLOG_ERROR("telegram: failed to spawn listener thread (rc=%d)", prc);
      return FAILURE;
   }
   s_listener_started = true;
   OLOG_INFO("telegram: driver initialized (bot endpoint configured)");
   return SUCCESS;
}

static void tg_shutdown(void) {
   atomic_store(&s_running, false);
   if (s_listener_started) {
      pthread_join(s_listener_thread, NULL);
      s_listener_started = false;
   }
   pthread_mutex_lock(&s_send_curl_mutex);
   if (s_send_curl) {
      curl_easy_cleanup(s_send_curl);
      s_send_curl = NULL;
   }
   pthread_mutex_unlock(&s_send_curl_mutex);
   /* Wipe the token from process memory so a post-shutdown core dump
    * or /proc/$PID/maps inspection can't recover it.  reconnect() does
    * not need it (tg_init re-reads from credentials on next invoke). */
   sodium_memzero(s_bot_token, sizeof(s_bot_token));
   sodium_memzero(s_base_url, sizeof(s_base_url));
   OLOG_INFO("telegram: driver shut down");
}

static int tg_register_inbound_cb(messaging_inbound_fn cb) {
   pthread_mutex_lock(&s_inbound_cb_mutex);
   s_inbound_cb = cb;
   pthread_mutex_unlock(&s_inbound_cb_mutex);
   return SUCCESS;
}

/* Telegram chat IDs: all digits with optional leading '-' (private
 * chats positive, supergroups/channels negative).  Shared between
 * validate_address and build_address_json so the public driver surface
 * emits well-formed JSON even if a future caller skipped
 * validate_address.  Matches the Slack driver's defense-in-depth
 * pattern (sk_build_address_json calls is_valid_slack_channel). */
static bool is_valid_chat_id(const char *s) {
   if (!s || !s[0]) {
      return false;
   }
   size_t i = (s[0] == '-') ? 1 : 0;
   if (s[i] == '\0') {
      return false;
   }
   for (; s[i] != '\0'; i++) {
      if (!isdigit((unsigned char)s[i])) {
         return false;
      }
   }
   return true;
}

static int tg_validate_address(const char *address_json) {
   if (!address_json || address_json[0] == '\0') {
      return FAILURE;
   }
   struct json_object *obj = json_tokener_parse(address_json);
   if (!obj) {
      return FAILURE;
   }
   struct json_object *cid = NULL;
   int rc = FAILURE;
   if (json_object_object_get_ex(obj, "chat_id", &cid) && cid) {
      const char *s = json_object_get_string(cid);
      if (is_valid_chat_id(s)) {
         rc = SUCCESS;
      }
   }
   json_object_put(obj);
   return rc;
}

static int tg_is_connected(void) {
   return atomic_load(&s_connected) ? 1 : 0;
}

static void tg_build_address_json(const char *provider_address, char *buf, size_t buf_size) {
   if (!buf || buf_size == 0) {
      return;
   }
   /* Re-validate even though all current callers pre-validate.  Defense
    * in depth on the driver's public surface — a future caller bypassing
    * validate_address would otherwise emit malformed JSON via snprintf
    * below.  is_valid_chat_id rejects '"' and '\\'. */
   if (!provider_address || !is_valid_chat_id(provider_address)) {
      snprintf(buf, buf_size, "{}");
      return;
   }
   snprintf(buf, buf_size, "{\"chat_id\":\"%s\"}", provider_address);
}

static int tg_reconnect(void) {
   /* The listener naturally reconnects on transient failure
    * (curl_easy_perform fails → sleep → loop).  Explicit reconnect
    * not needed for v1. */
   return SUCCESS;
}

/* =============================================================================
 * Driver descriptor + registration entry point
 * ============================================================================= */

static const messaging_driver_t s_telegram_driver = {
   .name = "telegram",
   .authenticates_sender = true,
   .out_format = MSG_FMT_TELEGRAM_HTML,
   .init = tg_init,
   .shutdown = tg_shutdown,
   .send_text = tg_send_text,
   .build_address_json = tg_build_address_json,
   .register_inbound_cb = tg_register_inbound_cb,
   .validate_address = tg_validate_address,
   .is_connected = tg_is_connected,
   .reconnect = tg_reconnect,
   .send_typing = tg_send_typing,
};

int messaging_telegram_register(const char *bot_token) {
   if (!bot_token || bot_token[0] == '\0') {
      OLOG_INFO("telegram: no bot_token configured; driver not registered");
      return FAILURE;
   }
   /* Init BEFORE engine registration so that an init failure (bad token,
    * listener spawn fail) doesn't leave a dead driver in the engine's
    * registry.  tg_init rolls back its own state internally on failure.
    * The race window where the listener could fire an update before
    * register_driver wires up the engine's inbound_cb is theoretical —
    * the long-poll cycle takes ~30 s while register_driver completes in
    * microseconds. */
   /* Build the credentials envelope through json-c so a token containing
    * '"' or '\\' is escaped correctly.  Naive snprintf would substitute
    * one token for another on malformed input — same defense-in-depth
    * shape as Slack + Discord registration. */
   int rc = FAILURE;
   struct json_object *obj = json_object_new_object();
   if (!obj) {
      return FAILURE;
   }
   json_object_object_add(obj, "bot_token", json_object_new_string(bot_token));
   const char *creds_str = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
   if (creds_str && tg_init(creds_str) == SUCCESS) {
      if (messaging_engine_register_driver(&s_telegram_driver) == MESSAGING_SUCCESS) {
         rc = SUCCESS;
      } else {
         OLOG_ERROR("telegram: driver registration with engine failed");
         tg_shutdown(); /* roll back the listener we just spawned */
      }
   }
   json_object_put(obj);
   return rc;
}

void messaging_telegram_shutdown(void) {
   tg_shutdown();
}
