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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Calendar service — multi-account routing, background sync, business logic.
 * Coordinates between caldav_client (network) and calendar_db (storage).
 */

#include "tools/calendar_service.h"

#include <ctype.h>
#include <errno.h>
#include <json-c/json.h>
#include <pthread.h>
#include <sodium.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "config/config_parser.h"
#include "config/dawn_config.h"
#include "core/crypto_store.h"
#include "core/iso8601.h"
#include "core/path_utils.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/caldav_client.h"
#include "tools/calendar_ical.h"
#include "tools/calendar_pick.h"
#include "tools/calendar_query_window.h"
#include "tools/calendar_rrule.h"
#include "tools/email_db.h"
#include "tools/oauth_client.h"
#include "utils/string_utils.h"

/* Weak no-op fallback for the WebUI "calendar changed, refetch" broadcast,
 * compiled ONLY when WebUI is OFF (calendar_service.c is gated on the calendar
 * tool, not on ENABLE_WEBUI, so it builds in the local/ci presets). When WebUI
 * is on, the strong override in src/webui/webui_broadcasts.c is linked instead.
 * sync_account calls these unconditionally, so a definition must exist in every
 * build. Mirrors scheduler_broadcast_events_changed (src/core/scheduler.c). */
#ifndef ENABLE_WEBUI
void calendar_broadcast_events_changed(int user_id) __attribute__((weak));
void calendar_broadcast_events_changed(int user_id) {
   (void)user_id;
}
void calendar_broadcast_sync_done(int user_id, int64_t account_id, bool ok) __attribute__((weak));
void calendar_broadcast_sync_done(int user_id, int64_t account_id, bool ok) {
   (void)user_id;
   (void)account_id;
   (void)ok;
}
#endif

/* =============================================================================
 * State
 * ============================================================================= */

/* Accounts a sync was asked for (by a user, or a new account), waiting for
 * the sync thread: every sync runs there, one at a time. */
#define CALENDAR_MAX_SYNC_REQUESTS 16

/* Seconds before the sync thread asks again for accounts it couldn't read. */
#define CALENDAR_DB_RETRY_SEC 5

static struct {
   bool initialized;
   calendar_config_t config;
   pthread_t sync_thread;
   atomic_bool sync_running;
   pthread_mutex_t mutex;
   pthread_cond_t wake; /* a sync was asked for, or shutdown */
   int64_t requested[CALENDAR_MAX_SYNC_REQUESTS];
   int requested_count;
} s_cal = {
   .initialized = false,
   .sync_running = ATOMIC_VAR_INIT(false),
   .mutex = PTHREAD_MUTEX_INITIALIZER,
   .wake = PTHREAD_COND_INITIALIZER,
};

/* =============================================================================
 * Forward declarations
 * ============================================================================= */

static void *sync_thread_func(void *arg);
static int sync_account(int64_t account_id);

/* =============================================================================
 * Password encryption — delegates to shared crypto_store module
 * ============================================================================= */

/**
 * Encrypt a plaintext password into acct->encrypted_password.
 * Format: [nonce (24 bytes)][ciphertext (len + MAC)]
 * Returns 0 on success.
 */
int calendar_encrypt_password(const char *plaintext, calendar_account_t *acct) {
   if (!plaintext || !acct)
      return 1;

   size_t pt_len = strlen(plaintext);
   size_t out_written = 0;
   if (crypto_store_encrypt(plaintext, pt_len, acct->encrypted_password,
                            sizeof(acct->encrypted_password), &out_written) != 0) {
      OLOG_ERROR("calendar: failed to encrypt password");
      return 1;
   }

   acct->encrypted_password_len = (int)out_written;
   return 0;
}

/**
 * Decrypt acct->encrypted_password into a caller-provided buffer.
 * Returns 0 on success, password is NUL-terminated.
 */
int calendar_decrypt_password(const calendar_account_t *acct, char *out, size_t out_len) {
   if (!acct || !out || out_len == 0)
      return 1;
   if (acct->encrypted_password_len <= 0)
      return 1;

   /* Decrypt into buffer, leaving room for NUL */
   size_t dec_written = 0;
   if (crypto_store_decrypt(acct->encrypted_password, (size_t)acct->encrypted_password_len, out,
                            out_len - 1, &dec_written) != 0) {
      OLOG_ERROR("calendar: decryption failed (wrong key or corrupt data)");
      return 1;
   }

   out[dec_written] = '\0';
   return 0;
}

/** Convenience: decrypt password from an account into a stack buffer */
static int get_account_password(const calendar_account_t *acct, char *buf, size_t buf_len) {
   return calendar_decrypt_password(acct, buf, buf_len);
}

/* =============================================================================
 * OAuth Auth Helper
 * ============================================================================= */

/**
 * Build a caldav_auth_t for an account, handling both basic auth and OAuth.
 *
 * @param acct     Account to authenticate
 * @param auth     Output auth struct (pointers reference pw_buf or tok_buf)
 * @param pw_buf   Buffer for decrypted password (basic auth)
 * @param pw_len   Size of pw_buf
 * @param tok_buf  Buffer for OAuth access token
 * @param tok_len  Size of tok_buf
 * @return 0 on success, 1 on failure
 */
static int build_auth_for_account(const calendar_account_t *acct,
                                  caldav_auth_t *auth,
                                  char *pw_buf,
                                  size_t pw_len,
                                  char *tok_buf,
                                  size_t tok_len) {
   memset(auth, 0, sizeof(*auth));

   if (strcmp(acct->auth_type, "oauth") == 0) {
      /* OAuth: get access token (auto-refreshes if needed) */
      oauth_provider_config_t google;
      if (oauth_build_google_provider(GOOGLE_CALENDAR_SCOPE, &google) != 0)
         return 1;

      if (oauth_get_access_token(&google, acct->user_id, acct->oauth_account_key, tok_buf,
                                 tok_len) != 0) {
         OLOG_ERROR("calendar: failed to get OAuth token for '%s'", acct->name);
         return 1;
      }
      auth->bearer_token = tok_buf;
   } else {
      /* Basic auth: decrypt password */
      if (get_account_password(acct, pw_buf, pw_len) != 0) {
         OLOG_ERROR("calendar: failed to decrypt password for '%s'", acct->name);
         return 1;
      }
      auth->username = acct->username;
      auth->password = pw_buf;
   }
   return 0;
}

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

int calendar_service_init(const calendar_config_t *config) {
   if (!config || !config->enabled) {
      OLOG_INFO("calendar: service disabled");
      return 0;
   }

   pthread_mutex_lock(&s_cal.mutex);
   if (s_cal.initialized) {
      pthread_mutex_unlock(&s_cal.mutex);
      return 0;
   }

   s_cal.config = *config;
   s_cal.initialized = true;

   /* Wait on the monotonic clock: a wall-clock step back mustn't stall syncs. */
   pthread_condattr_t attr;
   pthread_condattr_init(&attr);
   pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
   pthread_cond_destroy(&s_cal.wake);
   pthread_cond_init(&s_cal.wake, &attr);
   pthread_condattr_destroy(&attr);

   /* Start background sync thread */
   atomic_store(&s_cal.sync_running, true);
   if (pthread_create(&s_cal.sync_thread, NULL, sync_thread_func, NULL) != 0) {
      OLOG_ERROR("calendar: failed to create sync thread");
      atomic_store(&s_cal.sync_running, false);
      s_cal.initialized = false;
      pthread_mutex_unlock(&s_cal.mutex);
      return 1;
   }

   pthread_mutex_unlock(&s_cal.mutex);
   OLOG_INFO("calendar: service initialized (sync every %ds)", config->sync_interval_sec);
   return 0;
}

void calendar_service_shutdown(void) {
   pthread_mutex_lock(&s_cal.mutex);
   if (!s_cal.initialized) {
      pthread_mutex_unlock(&s_cal.mutex);
      return;
   }
   atomic_store(&s_cal.sync_running, false);
   s_cal.initialized = false;
   s_cal.requested_count = 0;
   pthread_cond_broadcast(&s_cal.wake);
   pthread_t thread = s_cal.sync_thread;
   pthread_mutex_unlock(&s_cal.mutex);

   /* Wait for the sync thread to exit (after any sync it is in) */
   pthread_join(thread, NULL);

   OLOG_INFO("calendar: service shut down");
}

bool calendar_service_available(void) {
   return s_cal.initialized;
}

/* =============================================================================
 * Account Management
 * ============================================================================= */

int calendar_service_add_account(int user_id,
                                 const char *name,
                                 const char *caldav_url,
                                 const char *username,
                                 const char *password,
                                 bool read_only,
                                 const char *auth_type,
                                 const char *oauth_account_key) {
   bool is_oauth = auth_type && strcmp(auth_type, "oauth") == 0;

   if (!is_oauth && (!password || !password[0])) {
      OLOG_ERROR("calendar: password is required for basic auth account creation");
      return CALENDAR_RC_FAILURE;
   }

   /* Duplicate check: same user + (OAuth key or username+URL) */
   calendar_account_t existing[16];
   int count = 0;
   calendar_db_account_list(user_id, existing, 16, &count);
   for (int i = 0; i < count; i++) {
      if (is_oauth && oauth_account_key && oauth_account_key[0] &&
          strcmp(existing[i].oauth_account_key, oauth_account_key) == 0) {
         OLOG_INFO("calendar: account with OAuth key '%s' already exists, skipping",
                   oauth_account_key);
         return CALENDAR_RC_DUPLICATE;
      }
      if (!is_oauth && username && caldav_url && strcmp(existing[i].username, username) == 0 &&
          strcmp(existing[i].caldav_url, caldav_url) == 0) {
         OLOG_INFO("calendar: account '%s@%s' already exists, skipping", username, caldav_url);
         return CALENDAR_RC_DUPLICATE;
      }
   }

   calendar_account_t acct = { 0 };
   acct.user_id = user_id;
   acct.enabled = true;
   acct.read_only = read_only;
   acct.sync_interval_sec = s_cal.config.sync_interval_sec;
   snprintf(acct.name, sizeof(acct.name), "%s", name ? name : "Calendar");
   snprintf(acct.caldav_url, sizeof(acct.caldav_url), "%s", caldav_url ? caldav_url : "");
   snprintf(acct.username, sizeof(acct.username), "%s", username ? username : "");

   if (is_oauth) {
      snprintf(acct.auth_type, sizeof(acct.auth_type), "oauth");
      snprintf(acct.oauth_account_key, sizeof(acct.oauth_account_key), "%s",
               oauth_account_key ? oauth_account_key : "");
   } else {
      snprintf(acct.auth_type, sizeof(acct.auth_type), "basic");
      if (calendar_encrypt_password(password, &acct) != 0) {
         OLOG_ERROR("calendar: failed to encrypt password for account '%s'", name);
         return CALENDAR_RC_FAILURE;
      }
   }

   int64_t id = 0;
   if (calendar_db_account_create(&acct, &id) != 0) {
      OLOG_ERROR("calendar: failed to create account '%s'", name);
      return CALENDAR_RC_FAILURE;
   }

   OLOG_INFO("calendar: added %s account '%s' (id=%lld)", is_oauth ? "OAuth" : "basic", name,
             (long long)id);

   /* Discover calendars and ask for the initial sync */
   if (calendar_service_test_connection(id) == 0) {
      calendar_service_request_sync(id);
   } else {
      OLOG_WARNING("calendar: initial discovery failed for '%s', calendars will be discovered on "
                   "next test",
                   name);
   }

   return CALENDAR_RC_OK;
}

int calendar_service_remove_account(int64_t account_id) {
   /* Revoke and delete OAuth tokens if this is an OAuth account */
   calendar_account_t acct;
   if (calendar_db_account_get(account_id, &acct) == 0 && strcmp(acct.auth_type, "oauth") == 0 &&
       acct.oauth_account_key[0]) {
      /* Check if email service still uses this shared OAuth account before
       * revoking the token.  Only meaningful when the email tool is built in —
       * without it there are no email accounts and nothing else holds this grant,
       * so don't link-depend on email_db (calendar can be enabled with email off). */
      bool email_uses_account = false;
#ifdef DAWN_ENABLE_EMAIL_TOOL
      email_account_t email_accts[EMAIL_MAX_ACCOUNTS];
      int email_count = 0;
      email_db_account_list(acct.user_id, email_accts, EMAIL_MAX_ACCOUNTS, &email_count);
      for (int i = 0; i < email_count; i++) {
         if (strcmp(email_accts[i].auth_type, "oauth") == 0 &&
             strcmp(email_accts[i].oauth_account_key, acct.oauth_account_key) == 0) {
            email_uses_account = true;
            break;
         }
      }
      sodium_memzero(email_accts, sizeof(email_accts));
#endif

      if (email_uses_account) {
         OLOG_INFO("calendar: keeping OAuth token for '%s' (still used by email)",
                   acct.oauth_account_key);
      } else {
         oauth_provider_config_t google;
         if (oauth_build_google_provider(GOOGLE_CALENDAR_SCOPE, &google) == 0) {
            int revoke_rc = oauth_revoke_and_delete(&google, acct.user_id, acct.oauth_account_key);
            if (revoke_rc != 0) {
               OLOG_WARNING(
                   "calendar: OAuth token cleanup failed for account %lld (continuing removal)",
                   (long long)account_id);
            }
            sodium_memzero(&google, sizeof(google));
         } else {
            /* Config missing — just delete tokens locally without revoking */
            oauth_delete_tokens(acct.user_id, "google", acct.oauth_account_key);
         }
      }
   }

   int rc = calendar_db_account_delete(account_id);
   if (rc != 0) {
      OLOG_ERROR("calendar: failed to remove account %lld", (long long)account_id);
      return 1;
   }
   OLOG_INFO("calendar: removed account %lld", (long long)account_id);
   return 0;
}

/**
 * Google CalDAV uses known URL patterns for the calendar-home-set:
 *   https://apidata.googleusercontent.com/caldav/v2/{email}/
 *
 * We skip principal discovery (steps 1-2) and go straight to collection
 * enumeration on the known calendar-home URL, which discovers all calendars
 * the user has access to (primary, secondary, shared, etc.).
 */
static int google_caldav_test(const calendar_account_t *acct,
                              const caldav_auth_t *auth,
                              caldav_discovery_result_t *disc) {
   memset(disc, 0, sizeof(*disc));

   /* Validate oauth_account_key looks like an email (reject path traversal, injection) */
   const char *key = acct->oauth_account_key;
   if (!key || !key[0] || strlen(key) > 254) {
      OLOG_ERROR("calendar: invalid OAuth account key");
      return 1;
   }
   for (const char *p = key; *p; p++) {
      /* Allow alphanumeric, @, ., -, _, + (valid email chars) */
      if (!isalnum((unsigned char)*p) && *p != '@' && *p != '.' && *p != '-' && *p != '_' &&
          *p != '+') {
         OLOG_ERROR("calendar: OAuth account key contains invalid character: 0x%02x", *p);
         return 1;
      }
   }

   /* Use known Google calendar-home URL and run standard collection discovery */
   char home_url[512];
   snprintf(home_url, sizeof(home_url), "https://apidata.googleusercontent.com/caldav/v2/%s/", key);

   caldav_error_t err = caldav_discover(home_url, auth, disc);
   if (err != CALDAV_OK) {
      OLOG_ERROR("calendar: Google CalDAV discovery failed for '%s': %s", key,
                 caldav_strerror(err));
      return 1;
   }

   /* Fill in principal if discovery didn't set it */
   if (!disc->principal_url[0]) {
      snprintf(disc->principal_url, sizeof(disc->principal_url),
               "https://apidata.googleusercontent.com/caldav/v2/%s/user", key);
   }
   if (!disc->calendar_home_url[0]) {
      snprintf(disc->calendar_home_url, sizeof(disc->calendar_home_url), "%s", home_url);
   }

   OLOG_INFO("calendar: Google CalDAV connected for '%s', found %d calendars", key,
             disc->calendar_count);
   return 0;
}

int calendar_service_test_connection(int64_t account_id) {
   calendar_account_t acct;
   if (calendar_db_account_get(account_id, &acct) != 0) {
      OLOG_ERROR("calendar: account %lld not found", (long long)account_id);
      return 1;
   }

   char password[384];
   char token[OAUTH_TOKEN_BUF_SIZE];
   caldav_auth_t auth;
   if (build_auth_for_account(&acct, &auth, password, sizeof(password), token, sizeof(token)) !=
       0) {
      return 1;
   }

   caldav_discovery_result_t disc = { 0 };
   int rc;

   if (strcmp(acct.auth_type, "oauth") == 0 && acct.oauth_account_key[0]) {
      /* Google CalDAV — skip RFC 4791 discovery, use known URL patterns */
      rc = google_caldav_test(&acct, &auth, &disc);
   } else {
      /* Standard CalDAV discovery (iCloud, Fastmail, Radicale, etc.) */
      caldav_error_t err = caldav_discover(acct.caldav_url, &auth, &disc);
      rc = (err != CALDAV_OK) ? 1 : 0;
      if (rc != 0)
         OLOG_ERROR("calendar: discovery failed for '%s': %s", acct.name, caldav_strerror(err));
   }

   sodium_memzero(password, sizeof(password));
   sodium_memzero(token, sizeof(token));
   if (rc != 0) {
      caldav_discovery_free(&disc);
      return 1;
   }

   /* Cache discovery results */
   calendar_db_account_update_discovery(account_id, disc.principal_url, disc.calendar_home_url);

   /* Upsert discovered calendars (skip existing by caldav_path) */
   calendar_calendar_t existing_cals[32];
   int existing_count = 0;
   calendar_db_calendar_list(account_id, existing_cals, 32, &existing_count);
   time_t now = time(NULL);

   for (int i = 0; i < disc.calendar_count; i++) {
      caldav_calendar_info_t *ci = &disc.calendars[i];

      /* Check if this path already exists for the account */
      bool found = false;
      for (int j = 0; j < existing_count; j++) {
         if (strcmp(existing_cals[j].caldav_path, ci->path) == 0) {
            found = true;
            break;
         }
      }
      if (found)
         continue;

      /* Don't store ctag at discovery time — leave it empty so the first
       * sync_account() sees a mismatch and actually fetches events. */
      calendar_calendar_t cal = { 0 };
      cal.account_id = account_id;
      snprintf(cal.caldav_path, sizeof(cal.caldav_path), "%s", ci->path);
      snprintf(cal.display_name, sizeof(cal.display_name), "%s",
               ci->display_name[0] ? ci->display_name : "Calendar");
      snprintf(cal.color, sizeof(cal.color), "%s", ci->color);
      cal.is_active = !ci->read_only;
      cal.created_at = now;

      int64_t cal_id = 0;
      if (calendar_db_calendar_create(&cal, &cal_id) != 0)
         OLOG_WARNING("calendar: failed to create calendar '%s'", ci->display_name);
      else
         OLOG_INFO("calendar: discovered calendar '%s' (id=%lld)", ci->display_name,
                   (long long)cal_id);
   }

   int found_count = disc.calendar_count;
   caldav_discovery_free(&disc);
   OLOG_INFO("calendar: connection test OK for '%s', found %d calendars", acct.name, found_count);
   return 0;
}

/* =============================================================================
 * Storing an event and its occurrences
 * ============================================================================= */

/* An event's own start as its one occurrence. */
static void base_occurrence(int64_t event_id,
                            const caldav_event_t *ce,
                            calendar_occurrence_t *occ) {
   memset(occ, 0, sizeof(*occ));
   occ->event_id = event_id;
   occ->dtstart = ce->dtstart;
   occ->dtend = ce->dtend;
   occ->all_day = ce->all_day;
   snprintf(occ->dtstart_date, sizeof(occ->dtstart_date), "%s", ce->dtstart_date);
   snprintf(occ->dtend_date, sizeof(occ->dtend_date), "%s", ce->dtend_date);
   snprintf(occ->summary, sizeof(occ->summary), "%s", ce->summary);
   snprintf(occ->location, sizeof(occ->location), "%s", ce->location);
}

typedef struct {
   int64_t event_id;
   const caldav_event_t *ce;
   int duration;
   calendar_occurrence_t *occ; /* cap entries */
   int cap;
   int n;
} expand_ctx_t;

/* Whether @p start (an instance of @p ce's rule) is cancelled or moved: then
 * it isn't stored as a plain instance.  An all-day series compares dates. */
static bool instance_excepted(const caldav_event_t *ce, time_t start) {
   const time_t key = ce->all_day ? start - (start % 86400) : start;
   for (int i = 0; i < ce->exdate_count; i++) {
      if (ce->exdates[i] == key)
         return true;
   }
   for (int i = 0; i < ce->override_count; i++) {
      if (ce->overrides[i].original == key)
         return true;
   }
   return false;
}

static void format_occurrence_dates(calendar_occurrence_t *occ) {
   struct tm occ_tm;
   gmtime_r(&occ->dtstart, &occ_tm);
   iso8601_format_date(occ_tm.tm_year + 1900, occ_tm.tm_mon + 1, occ_tm.tm_mday, occ->dtstart_date,
                       sizeof(occ->dtstart_date));
   struct tm end_tm;
   gmtime_r(&occ->dtend, &end_tm);
   iso8601_format_date(end_tm.tm_year + 1900, end_tm.tm_mon + 1, end_tm.tm_mday, occ->dtend_date,
                       sizeof(occ->dtend_date));
}

/* One occurrence of a recurring event (calendar_rrule_expand's emit). */
static void collect_occurrence(time_t start, void *ctx) {
   expand_ctx_t *x = ctx;
   if (x->n >= x->cap || instance_excepted(x->ce, start))
      return;
   calendar_occurrence_t *occ = &x->occ[x->n++];
   base_occurrence(x->event_id, x->ce, occ);
   occ->dtstart = start;
   occ->dtend = start + x->duration;
   if (x->ce->all_day)
      format_occurrence_dates(occ);
   calendar_ical_instance_key(start, x->ce->all_day, occ->recurrence_id,
                              sizeof(occ->recurrence_id));
}

/* The series' moved instances that fall in the window, at their new times
 * (whether or not their original time is in it). */
static void collect_overrides(expand_ctx_t *x, time_t range_start, time_t range_end) {
   for (int i = 0; i < x->ce->override_count && x->n < x->cap; i++) {
      const caldav_override_t *o = &x->ce->overrides[i];
      if (o->cancelled || o->dtend < range_start || o->dtstart > range_end)
         continue;
      calendar_occurrence_t *occ = &x->occ[x->n++];
      memset(occ, 0, sizeof(*occ));
      occ->dtstart = o->dtstart;
      occ->dtend = o->dtend;
      occ->all_day = o->all_day;
      snprintf(occ->dtstart_date, sizeof(occ->dtstart_date), "%s", o->dtstart_date);
      snprintf(occ->dtend_date, sizeof(occ->dtend_date), "%s", o->dtend_date);
      snprintf(occ->summary, sizeof(occ->summary), "%s", o->summary);
      snprintf(occ->location, sizeof(occ->location), "%s", o->location);
      occ->is_override = true;
      calendar_ical_instance_key(o->original, x->ce->all_day, occ->recurrence_id,
                                 sizeof(occ->recurrence_id));
   }
}

static long ms_since(const struct timespec *t0) {
   struct timespec t1;
   clock_gettime(CLOCK_MONOTONIC, &t1);
   return (long)(t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000;
}

/* Store an event with its occurrences within the cache window, in one
 * transaction: a series' instances (less the cancelled and moved ones) and
 * its moved instances at their new times, or, with no rule or one refused,
 * its own start once.  @p ms_out gets the ms libical took.  Returns SUCCESS,
 * or FAILURE when they couldn't be written (the cache is then as it was). */
static int store_event(const calendar_event_t *evt,
                       const caldav_event_t *ce,
                       time_t range_start,
                       time_t range_end,
                       long *ms_out) {
   calendar_occurrence_t one;
   expand_ctx_t x = { .event_id = 0, .ce = ce, .occ = &one, .cap = 1, .n = 0 };
   long ms = 0;
   bool expanded = false;
   if (ce->rrule[0]) {
      x.cap = CALENDAR_RRULE_MAX_OCCURRENCES + ce->override_count;
      x.occ = calloc((size_t)x.cap, sizeof(*x.occ));
   }
   if (ce->rrule[0] && x.occ) {
      x.duration = ce->duration_sec > 0 ? ce->duration_sec : (int)(ce->dtend - ce->dtstart);
      if (x.duration <= 0)
         x.duration = ce->all_day ? 86400 : 3600;
      int count = 0;
      struct timespec t0;
      clock_gettime(CLOCK_MONOTONIC, &t0);
      const int rc = calendar_rrule_expand(ce->rrule, ce->dtstart, ce->tzid, ce->all_day,
                                           ce->dtstart_date, x.duration, range_start, range_end,
                                           collect_occurrence, &x, &count);
      ms = ms_since(&t0);
      if (rc == SUCCESS) {
         expanded = true;
         collect_overrides(&x, range_start, range_end);
      } else {
         OLOG_WARNING("calendar: RRULE '%s' of event '%s' unreadable or refused; shown once",
                      ce->rrule, ce->uid);
         x.n = 0;
      }
   } else if (ce->rrule[0]) {
      x.occ = &one; /* out of memory: shown once */
      x.cap = 1;
   }
   if (!expanded) {
      base_occurrence(0, ce, x.occ);
      x.n = 1;
   }
   const int rc = calendar_db_event_store(evt, x.occ, x.n, NULL);
   if (x.occ != &one)
      free(x.occ);
   *ms_out = ms;
   return rc;
}

/* The cache row for a fetched event. */
static void event_from_fetched(const caldav_event_t *ce,
                               int64_t calendar_id,
                               time_t now,
                               calendar_event_t *evt) {
   memset(evt, 0, sizeof(*evt));
   evt->calendar_id = calendar_id;
   snprintf(evt->uid, sizeof(evt->uid), "%s", ce->uid);
   snprintf(evt->etag, sizeof(evt->etag), "%s", ce->etag);
   snprintf(evt->href, sizeof(evt->href), "%s", ce->href);
   snprintf(evt->summary, sizeof(evt->summary), "%s", ce->summary);
   snprintf(evt->description, sizeof(evt->description), "%s", ce->description);
   snprintf(evt->location, sizeof(evt->location), "%s", ce->location);
   evt->dtstart = ce->dtstart;
   evt->dtend = ce->dtend;
   evt->duration_sec = ce->duration_sec;
   evt->all_day = ce->all_day;
   snprintf(evt->dtstart_date, sizeof(evt->dtstart_date), "%s", ce->dtstart_date);
   snprintf(evt->dtend_date, sizeof(evt->dtend_date), "%s", ce->dtend_date);
   snprintf(evt->rrule, sizeof(evt->rrule), "%s", ce->rrule);
   evt->raw_ical = ce->raw_ical;
   evt->last_synced = now;
}

/* The window events are cached for. */
static void cache_window(time_t now, time_t *start, time_t *end) {
   *start = now - (s_cal.config.cache_past_days * 86400);
   *end = now + (s_cal.config.cache_future_days * 86400);
}

/**
 * RFC 6578 sync-collection reconcile for one calendar.
 *
 * Three jobs. (1) On first sight (empty stored token) it BASELINES: the server
 * enumerates the whole collection and mints a sync-token, and — since that member set
 * is complete — a whole-collection retroactive prune (calendar_db_event_prune_not_in_hrefs)
 * clears local rows whose href is not in the set, i.e. out-of-window upstream deletions
 * the in-window sentinel cannot see. (2) On subsequent runs (real token) it applies the
 * server's explicit incremental removals (members reported with a 404 status), which are
 * window-independent. (3) It advances the token. So the in-window sentinel and this path
 * divide the work: the sentinel owns the visible window on every server; this owns
 * out-of-window (retroactively at baseline, incrementally after). Best-effort: any
 * failure leaves the token untouched so the next pass retries.
 */
/* Placeholder stored in sync_token after a reconcile that produced no real RFC 6578
 * token (server lacks sync-collection, or a baseline came back incomplete). Non-empty,
 * so the ctag gate treats the calendar as reconciled and stops forcing a full fetch
 * every cycle; treated as "no token" below so a later pass re-attempts the baseline. A
 * real sync-token is a URI, so a leading control byte can never collide with one. */
#define CALENDAR_SYNC_NONE_MARKER "\x01"

static void sync_collection_reconcile(const calendar_calendar_t *cal, const caldav_auth_t *auth) {
   /* Treat the placeholder as "no token" for the request. */
   const bool have_real_token = (cal->sync_token[0] &&
                                 strcmp(cal->sync_token, CALENDAR_SYNC_NONE_MARKER) != 0);
   const char *token_in = have_real_token ? cal->sync_token : "";

   caldav_sync_result_t sr = { 0 };
   caldav_error_t err = caldav_sync_collection(cal->caldav_path, auth, token_in, &sr);

   if (err == CALDAV_ERR_SYNC_TOKEN_INVALID) {
      /* Stale/expired token: clear it so the next pass re-baselines from empty. */
      OLOG_INFO("calendar: sync-token stale for '%s' — re-baselining next pass", cal->display_name);
      calendar_db_calendar_update_sync_token(cal->id, "");
      caldav_sync_result_free(&sr);
      return;
   }
   if (err != CALDAV_OK) {
      /* Network, 5xx, or a server without sync-collection. Warn only if we had a real
       * token (a failed initial baseline on an unsupported server is expected). If we've
       * never obtained a token, record the placeholder so the ctag gate stops forcing a
       * full fetch every cycle — a future ctag change re-enters and retries the baseline.
       * Correctness is unaffected: the in-window sentinel already ran this pass. */
      if (have_real_token)
         OLOG_WARNING("calendar: sync-collection failed for '%s': %s", cal->display_name,
                      caldav_strerror(err));
      else if (!cal->sync_token[0])
         calendar_db_calendar_update_sync_token(cal->id, CALENDAR_SYNC_NONE_MARKER);
      caldav_sync_result_free(&sr);
      return;
   }

   /* Apply explicit removals — a 404 is definitive, so this is safe even when the
    * enumeration was incomplete (paging capped). */
   int removed = 0;
   for (int i = 0; i < sr.count; i++) {
      if (!sr.changes[i].gone)
         continue;
      int d = 0;
      if (calendar_db_event_delete_by_href(cal->id, sr.changes[i].href, &d) == 0)
         removed += d;
   }
   if (removed > 0)
      OLOG_INFO("calendar: sync-collection removed %d deleted event(s) from '%s'", removed,
                cal->display_name);

   /* Whole-collection retroactive prune (baseline only). On a COMPLETE baseline
    * enumeration (empty token → server returns the full member set as non-gone
    * entries), diff local non-empty-href rows against that set to clear out-of-window
    * upstream deletions the in-window sentinel can't see. Guarded on:
    *   - !have_real_token: this was a baseline, so sr.changes IS the full set (an
    *     incremental returns only deltas — never set-diff against a delta);
    *   - sr.complete: fully paged (plan-review requirement — never prune on partial);
    *   - added > 0: a non-empty member set (never blind-delete on a bogus empty set).
    * A legitimately-emptied upstream calendar (0 members) is therefore NOT reconciled by
    * this path — indistinguishable from a spurious empty response, so we keep the rows;
    * in-window remnants still clear via the sentinel. The DB helper adds a second guard
    * (refuse if no local href matches the set — href-format drift protection). */
   if (!have_real_token && sr.complete) {
      json_object *arr = json_object_new_array();
      if (arr) {
         int added = 0;
         for (int i = 0; i < sr.count; i++) {
            if (!sr.changes[i].gone && sr.changes[i].href[0]) {
               json_object_array_add(arr, json_object_new_string(sr.changes[i].href));
               added++;
            }
         }
         if (added > 0) {
            int wc_pruned = 0;
            calendar_db_event_prune_not_in_hrefs(cal->id, json_object_to_json_string(arr),
                                                 &wc_pruned);
            if (wc_pruned > 0)
               OLOG_INFO("calendar: reconciled %d out-of-window deletion(s) from '%s'", wc_pruned,
                         cal->display_name);
         }
         json_object_put(arr);
      }
   }

   if (sr.complete && sr.sync_token[0]) {
      /* Real token obtained — persist it (replaces empty or the placeholder). */
      if (strcmp(sr.sync_token, cal->sync_token) != 0)
         calendar_db_calendar_update_sync_token(cal->id, sr.sync_token);
   } else if (!cal->sync_token[0]) {
      /* Succeeded but incomplete (no token minted): record the placeholder so we don't
       * full-fetch every cycle; a future ctag change retries the baseline. */
      calendar_db_calendar_update_sync_token(cal->id, CALENDAR_SYNC_NONE_MARKER);
   }

   caldav_sync_result_free(&sr);
}

int calendar_service_request_sync(int64_t account_id) {
   int rc = FAILURE;
   pthread_mutex_lock(&s_cal.mutex);
   if (s_cal.initialized) {
      rc = SUCCESS;
      bool queued = false;
      for (int i = 0; i < s_cal.requested_count; i++)
         queued = queued || s_cal.requested[i] == account_id;
      if (!queued) {
         if (s_cal.requested_count < CALENDAR_MAX_SYNC_REQUESTS)
            s_cal.requested[s_cal.requested_count++] = account_id;
         else
            rc = FAILURE;
      }
      pthread_cond_signal(&s_cal.wake);
   }
   pthread_mutex_unlock(&s_cal.mutex);
   return rc;
}

/* Sync one account's calendars (on the sync thread only).  0 when every
 * active calendar was checked and, if changed, fully stored. */
static int sync_account(int64_t account_id) {
   calendar_account_t acct;
   if (calendar_db_account_get(account_id, &acct) != 0)
      return 1;

   char password[384];
   char token[OAUTH_TOKEN_BUF_SIZE];
   caldav_auth_t auth;
   if (build_auth_for_account(&acct, &auth, password, sizeof(password), token, sizeof(token)) != 0)
      return 1;

   /* Get calendars for this account */
   calendar_calendar_t cals[32];
   int cal_count = 0;
   calendar_db_calendar_list(account_id, cals, 32, &cal_count);
   if (cal_count <= 0) {
      OLOG_WARNING("calendar: no calendars for account '%s'", acct.name);
      sodium_memzero(password, sizeof(password));
      sodium_memzero(token, sizeof(token));
      return 1;
   }

   time_t now = time(NULL);
   time_t range_start, range_end;
   cache_window(now, &range_start, &range_end);

   /* Date-string form of the window, for the sentinel's all-day occurrence overlap. */
   char range_start_date[16] = { 0 };
   char range_end_date[16] = { 0 };
   {
      struct tm rs_tm, re_tm;
      gmtime_r(&range_start, &rs_tm);
      gmtime_r(&range_end, &re_tm);
      iso8601_format_date(rs_tm.tm_year + 1900, rs_tm.tm_mon + 1, rs_tm.tm_mday, range_start_date,
                          sizeof(range_start_date));
      iso8601_format_date(re_tm.tm_year + 1900, re_tm.tm_mon + 1, re_tm.tm_mday, range_end_date,
                          sizeof(range_end_date));
   }

   int synced = 0;
   int failed = 0; /* active calendars left unchecked, unfetched or half-stored */
   for (int i = 0; i < cal_count; i++) {
      if (!cals[i].is_active)
         continue;

      /* Check ctag for changes */
      char new_ctag[128] = { 0 };
      caldav_error_t err = caldav_get_ctag(cals[i].caldav_path, &auth, new_ctag, sizeof(new_ctag));
      if (err != CALDAV_OK) {
         OLOG_WARNING("calendar: ctag check failed for '%s'", cals[i].display_name);
         failed++;
         continue;
      }

      /* Skip only when the ctag is unchanged AND this calendar has been reconciled at
       * least once (sync_token non-empty — a real RFC 6578 token, or the
       * CALENDAR_SYNC_NONE_MARKER placeholder for a server without sync-collection). An
       * empty sync_token means we've never run the deletion reconcile — force one full
       * pass so the sentinel clears any pre-existing upstream-deleted ghost and (where
       * supported) sync-collection mints its baseline token. Deletions GOING FORWARD
       * move the ctag, so the normal path still catches them; this only closes the
       * one-time gap for events deleted before this code existed. */
      if (new_ctag[0] && strcmp(new_ctag, cals[i].ctag) == 0 && cals[i].sync_token[0]) {
         continue; /* No changes, already reconciled */
      }

      /* Fetch events */
      caldav_event_list_t events = { 0 };
      err = caldav_fetch_events(cals[i].caldav_path, &auth, range_start, range_end, &events);
      if (err != CALDAV_OK) {
         OLOG_WARNING("calendar: fetch failed for '%s': %s", cals[i].display_name,
                      caldav_strerror(err));
         failed++;
         continue;
      }

      /* Upsert events and expand occurrences.  Expansion is bounded per
       * event; slow ones also count against a budget, past which the
       * calendar is left as cached (not pruned, ctag kept) for the next sync. */
      long slow_ms = 0;
      bool budget_spent = false;
      bool store_failed = false;
      for (int j = 0; j < events.count && !budget_spent && !store_failed; j++) {
         caldav_event_t *ce = &events.events[j];

         calendar_event_t evt;
         event_from_fetched(ce, cals[i].id, now, &evt);

         /* The event and its occurrences, together (libical's work timed).
          * One that can't be stored leaves the calendar to be fetched again. */
         long ms = 0;
         if (store_event(&evt, ce, range_start, range_end, &ms) != SUCCESS) {
            OLOG_WARNING("calendar: storing event '%s' failed", ce->uid);
            store_failed = true;
         }
         if (ms > CALDAV_SLOW_EVENT_MS) {
            OLOG_WARNING("calendar: expanding '%s' took %ld ms", ce->uid, ms);
            slow_ms += ms;
            budget_spent = slow_ms > CALDAV_SLOW_BUDGET_MS;
         }
      }

      caldav_event_list_free(&events);
      if (store_failed) {
         failed++; /* not reconciled, ctag kept: fetched again next sync */
         continue;
      }
      if (budget_spent) {
         OLOG_WARNING("calendar: stopped expanding '%s' after %ld ms of slow events; the rest "
                      "keeps its cached copy",
                      cals[i].display_name, slow_ms);
         failed++;
         continue;
      }

      /* In-window deletion reconcile, keyed on OCCURRENCE overlap. The fetch above
       * re-stamped last_synced=now on every event the server still returns in-window;
       * any event NOT re-stamped whose occurrences fall in [range_start, range_end) is
       * gone upstream. Because it keys on occurrence overlap (not master dtstart), it
       * covers a recurring series whose master predates the window but whose occurrences
       * are visible in it — matching exactly what the user sees. Runs only here — after a
       * SUCCESSFUL, complete fetch (a truncated/failed fetch `continue`d above;
       * caldav_fetch_events is all-or-error, never a silent partial) — so we never prune
       * on partial data. Keyed on last_synced, so it also clears pre-v82 empty-href
       * ghosts and never touches out-of-window or just-created rows.
       *
       * Assumes a conformant time-range REPORT (returns a recurring event whenever any
       * instance is in-window, so it gets re-stamped); true for Google/iCloud/Nextcloud/
       * Radicale. An event with zero in-window occurrences is not visible and is left to
       * the sync-collection paths below. */
      int pruned = 0;
      calendar_db_event_prune_window_stale(cals[i].id, now, range_start, range_end,
                                           range_start_date, range_end_date, &pruned);
      if (pruned > 0)
         OLOG_INFO("calendar: pruned %d upstream-deleted event(s) from '%s'", pruned,
                   cals[i].display_name);

      /* RFC 6578 sync-collection: mint/advance the token and apply the server's
       * explicit out-of-window removals (404 hrefs). Independent of the sentinel
       * above; degrades silently on servers that don't support sync-collection. */
      sync_collection_reconcile(&cals[i], &auth);

      /* Update ctag last, once the calendar's events are fully reconciled. */
      if (new_ctag[0])
         calendar_db_calendar_update_ctag(cals[i].id, new_ctag);

      synced++;
   }

   sodium_memzero(password, sizeof(password));
   sodium_memzero(token, sizeof(token));
   calendar_db_account_update_sync(account_id, now);
   OLOG_INFO("calendar: synced %d calendars for '%s'", synced, acct.name);

   /* Nudge the owner's WebUI sessions to refetch. Gated on synced > 0, i.e. at
    * least one calendar was (re)fetched: for a ctag-capable server that means the
    * ctag moved (real change); a ctag-less server can't prove no-change, so it
    * fetches — and thus broadcasts — every interval (benign: signal-only refetch,
    * no event data on the wire). Covers all three sync entry points (background
    * thread, manual WS sync, post-add initial sync). */
   if (synced > 0)
      calendar_broadcast_events_changed(acct.user_id);

   return failed > 0 ? 1 : 0;
}

/* =============================================================================
 * Query Operations
 * ============================================================================= */

/** Get user's active calendar IDs (helper).
 *  When calendar_name is non-NULL, only return IDs for calendars it names: a
 *  display name (every calendar of it) or a label (calendar_pick_label: one).
 *  When NULL, return all active calendars. */
static int get_user_calendar_ids(int user_id,
                                 const char *calendar_name,
                                 int64_t *ids,
                                 int max_count) {
   calendar_calendar_t cals[32];
   int count = 0;
   calendar_db_active_calendars_for_user(user_id, cals, max_count < 32 ? max_count : 32, &count);
   if (!calendar_name) {
      for (int i = 0; i < count; i++)
         ids[i] = cals[i].id;
      return count;
   }

   int matched = 0;
   for (int i = 0; i < count; i++) {
      if (calendar_pick_names(cals, count, i, calendar_name))
         ids[matched++] = cals[i].id;
   }
   return matched;
}

int calendar_service_today(int user_id,
                           const tz_t *zone,
                           const char *calendar_name,
                           calendar_occurrence_t *out,
                           int max_count) {
   struct tm day;
   tz_local(zone, time(NULL), &day);
   day.tm_hour = 0;
   day.tm_min = 0;
   day.tm_sec = 0;
   const time_t start = tz_mktime(zone, &day);
   day.tm_mday += 1;
   return calendar_service_range(user_id, zone, start, tz_mktime(zone, &day), calendar_name, out,
                                 max_count);
}

/* Sort merged timed + all-day occurrences by start time (soonest first), with
 * occurrence id as a stable tiebreak so equal-dtstart rows render deterministically. */
static int occ_cmp_dtstart(const void *a, const void *b) {
   const calendar_occurrence_t *oa = a;
   const calendar_occurrence_t *ob = b;
   if (oa->dtstart < ob->dtstart)
      return -1;
   if (oa->dtstart > ob->dtstart)
      return 1;
   if (oa->id < ob->id)
      return -1;
   if (oa->id > ob->id)
      return 1;
   return 0;
}

int calendar_service_range(int user_id,
                           const tz_t *zone,
                           time_t start,
                           time_t end,
                           const char *calendar_name,
                           calendar_occurrence_t *out,
                           int max_count) {
   int64_t cal_ids[32];
   int cal_count = get_user_calendar_ids(user_id, calendar_name, cal_ids, 32);
   if (cal_count <= 0)
      return 0;

   int count = 0;
   calendar_db_occurrences_in_range(cal_ids, cal_count, start, end, out, max_count, &count);

   /* All-day occurrences are a DISJOINT query (the timed query hardcodes
    * all_day = 0), so without merging them holidays/birthdays/PTO never appear
    * (this also fixed the same latent gap on the LLM range path).  Fetch them
    * into a temp buffer and merge, so the max_count cap applies to the COMBINED
    * start-ordered set — a window that fills the cap with timed events must
    * still surface the soonest all-day events instead of dropping them all.
    * The window is [start, end), like the timed query. */
   char start_date[CALENDAR_DATE_LEN];
   char end_date[CALENDAR_DATE_LEN];
   calendar_window_dates(zone, start, end, start_date, end_date);

   calendar_occurrence_t *allday = malloc((size_t)max_count * sizeof(*allday));
   int allday_count = 0;
   if (allday) {
      calendar_db_allday_occurrences_in_range(cal_ids, cal_count, start_date, end_date, allday,
                                              max_count, &allday_count);
   }

   if (allday_count > 0) {
      /* Merge timed (out[0..count]) + all-day into one buffer, sort by start, and
       * keep the soonest max_count.  Each input is already start-ordered by its
       * query, but the combined set needs the re-sort. */
      int total = count + allday_count;
      calendar_occurrence_t *merged = malloc((size_t)total * sizeof(*merged));
      if (merged) {
         memcpy(merged, out, (size_t)count * sizeof(*merged));
         memcpy(merged + count, allday, (size_t)allday_count * sizeof(*merged));
         qsort(merged, total, sizeof(*merged), occ_cmp_dtstart);
         count = total < max_count ? total : max_count;
         memcpy(out, merged, (size_t)count * sizeof(*merged));
         free(merged);
      }
      /* merged-alloc failure → out keeps the timed-only set (graceful degradation). */
   }
   free(allday);
   return count;
}

int calendar_service_next(int user_id, const char *calendar_name, calendar_occurrence_t *out) {
   int64_t cal_ids[32];
   int cal_count = get_user_calendar_ids(user_id, calendar_name, cal_ids, 32);
   if (cal_count <= 0)
      return 1;

   return calendar_db_next_occurrence(cal_ids, cal_count, time(NULL), out);
}

int calendar_service_search(int user_id,
                            const char *query,
                            const char *calendar_name,
                            calendar_occurrence_t *out,
                            int max_count) {
   int64_t cal_ids[32];
   int cal_count = get_user_calendar_ids(user_id, calendar_name, cal_ids, 32);
   if (cal_count <= 0)
      return 0;

   int count = 0;
   calendar_db_occurrences_search(cal_ids, cal_count, query, out, max_count, &count);
   return count;
}

/* =============================================================================
 * iCalendar Input Sanitization
 * ============================================================================= */

/* =============================================================================
 * Mutation Operations
 * ============================================================================= */

/* One write at a time per account: the model calls tools in parallel, and a
 * server throttling per user refuses a burst.  Held across the read and the
 * write (and their retry) only; accounts share a slot when their ids collide,
 * which only serializes them. */
#define CALENDAR_WRITE_LOCKS CALENDAR_MAX_ACCOUNTS
static pthread_mutex_t s_write_mutex[CALENDAR_WRITE_LOCKS] = { [0 ... CALENDAR_WRITE_LOCKS -
                                                                1] = PTHREAD_MUTEX_INITIALIZER };

static pthread_mutex_t *write_lock(int64_t account_id) {
   return &s_write_mutex[(uint64_t)account_id % CALENDAR_WRITE_LOCKS];
}

/* The addresses each account is invited by (RFC 6638), read once per account
 * and principal (an account edited to another principal is read again); an
 * account whose server doesn't answer is asked again on its next write. */
static struct {
   pthread_mutex_t mutex;
   struct {
      int64_t account_id;
      char principal[sizeof(((calendar_account_t *)0)->principal_url)];
      int count;
      char addrs[CALDAV_MAX_USER_ADDRESSES][CALDAV_USER_ADDRESS_LEN];
   } slots[CALENDAR_MAX_ACCOUNTS];
   int used;
} s_addresses = { .mutex = PTHREAD_MUTEX_INITIALIZER };

/* The addresses invitations may name @p acct by, into @p out: its login (an
 * email for OAuth and most accounts) and the server's address set, which
 * covers a username login ("alice" invited as alice@example.com) and aliases.
 * Returns how many. */
static int account_addresses(const calendar_account_t *acct,
                             const caldav_auth_t *auth,
                             char (*out)[CALDAV_USER_ADDRESS_LEN],
                             int max) {
   const char *login = acct->username[0]            ? acct->username
                       : acct->oauth_account_key[0] ? acct->oauth_account_key
                                                    : acct->name;
   int n = 0;
   snprintf(out[n++], CALDAV_USER_ADDRESS_LEN, "%s", login);

   pthread_mutex_lock(&s_addresses.mutex);
   int slot = -1;
   for (int i = 0; i < s_addresses.used && slot < 0; i++) {
      if (s_addresses.slots[i].account_id == acct->id &&
          strcmp(s_addresses.slots[i].principal, acct->principal_url) == 0)
         slot = i;
   }
   if (slot >= 0) {
      for (int i = 0; i < s_addresses.slots[slot].count && n < max; i++)
         memcpy(out[n++], s_addresses.slots[slot].addrs[i], CALDAV_USER_ADDRESS_LEN);
   }
   pthread_mutex_unlock(&s_addresses.mutex);
   if (slot >= 0 || !acct->principal_url[0])
      return n;

   char found[CALDAV_MAX_USER_ADDRESSES][CALDAV_USER_ADDRESS_LEN];
   int count = 0;
   if (caldav_get_user_addresses(acct->principal_url, auth, found, CALDAV_MAX_USER_ADDRESSES,
                                 &count) != CALDAV_OK)
      return n;
   pthread_mutex_lock(&s_addresses.mutex);
   /* The account's slot (another write may have filled it meanwhile, or it
    * holds an old principal's addresses), else a new one. */
   for (int i = 0; i < s_addresses.used && slot < 0; i++) {
      if (s_addresses.slots[i].account_id == acct->id)
         slot = i;
   }
   if (slot < 0 && s_addresses.used < CALENDAR_MAX_ACCOUNTS)
      slot = s_addresses.used++;
   if (slot >= 0) {
      s_addresses.slots[slot].account_id = acct->id;
      snprintf(s_addresses.slots[slot].principal, sizeof(s_addresses.slots[slot].principal), "%s",
               acct->principal_url);
      s_addresses.slots[slot].count = count;
      memcpy(s_addresses.slots[slot].addrs, found, sizeof(found));
   }
   pthread_mutex_unlock(&s_addresses.mutex);
   for (int i = 0; i < count && n < max; i++)
      memcpy(out[n++], found[i], CALDAV_USER_ADDRESS_LEN);
   return n;
}

static int rc_from_caldav(caldav_error_t err) {
   switch (err) {
      case CALDAV_OK:
         return CALENDAR_RC_OK;
      case CALDAV_ERR_NOT_FOUND:
         return CALENDAR_RC_NOT_FOUND;
      case CALDAV_ERR_RATE_LIMITED:
         return CALENDAR_RC_RATE_LIMITED;
      case CALDAV_ERR_CONFLICT:
         return CALENDAR_RC_CONFLICT;
      case CALDAV_ERR_FORBIDDEN:
      case CALDAV_ERR_REJECTED:
         return CALENDAR_RC_REJECTED;
      default:
         return CALENDAR_RC_FAILURE;
   }
}

static int rc_from_ical(calendar_ical_rc_t rc) {
   switch (rc) {
      case CALENDAR_ICAL_OK:
         return CALENDAR_RC_OK;
      case CALENDAR_ICAL_ERR_BAD_RULE:
         return CALENDAR_RC_BAD_RULE;
      case CALENDAR_ICAL_ERR_NOT_OCCURRENCE:
         return CALENDAR_RC_NOT_OCCURRENCE;
      case CALENDAR_ICAL_ERR_GUEST:
         return CALENDAR_RC_GUEST;
      case CALENDAR_ICAL_ERR_ZONE:
         return CALENDAR_RC_ZONE;
      case CALENDAR_ICAL_ERR_BAD_TIME:
         return CALENDAR_RC_BAD_TIME;
      case CALENDAR_ICAL_ERR_UNREADABLE:
         return CALENDAR_RC_UNREADABLE;
      default:
         return CALENDAR_RC_FAILURE;
   }
}

/* Cache the event DAWN just wrote, at @p href in calendar @p calendar_id, as
 * read back from the text it wrote; the next sync (the write moved the
 * calendar's ctag) replaces it with the server's own copy. */
static void cache_written(int64_t calendar_id, const char *href, const char *etag, char *ical) {
   caldav_event_t ce = { 0 };
   const size_t href_len = strlen(href);
   if (href_len >= sizeof(ce.href))
      return; /* the next sync brings it */
   memcpy(ce.href, href, href_len + 1);
   snprintf(ce.etag, sizeof(ce.etag), "%s", etag);
   if (calendar_ical_read(ical, &ce, NULL)) {
      const time_t now = time(NULL);
      time_t range_start, range_end;
      cache_window(now, &range_start, &range_end);
      calendar_event_t row;
      event_from_fetched(&ce, calendar_id, now, &row);
      row.raw_ical = ical;
      long ms = 0;
      if (store_event(&row, &ce, range_start, range_end, &ms) != SUCCESS)
         OLOG_WARNING("calendar: caching '%s' after the write failed", ce.uid);
   }
   caldav_event_free(&ce);
}

int calendar_service_add(int user_id,
                         const calendar_ical_new_t *event,
                         const char *calendar_name,
                         char *uid_out,
                         size_t uid_out_len,
                         char *calendar_out,
                         size_t calendar_out_len) {
   /* Find target calendar */
   calendar_calendar_t cals[32];
   int cal_count = 0;
   calendar_db_active_calendars_for_user(user_id, cals, 32, &cal_count);
   if (cal_count <= 0) {
      OLOG_ERROR("calendar: no active calendars for user %d", user_id);
      return CALENDAR_RC_FAILURE;
   }
   int target = -1;
   const calendar_pick_rc_t pick = calendar_pick_writable(cals, cal_count, calendar_name, &target);
   if (pick != CALENDAR_PICK_OK)
      return pick == CALENDAR_PICK_READONLY ? CALENDAR_RC_READONLY : CALENDAR_RC_NOT_FOUND;
   if (calendar_out && calendar_out_len > 0)
      calendar_pick_label(cals, cal_count, target, calendar_out, calendar_out_len);

   /* Get account for auth */
   calendar_account_t acct;
   if (calendar_db_account_get(cals[target].account_id, &acct) != 0)
      return CALENDAR_RC_FAILURE;

   /* Generate UID with random component for unpredictability */
   char uid[256];
   unsigned char uid_rand[8];
   randombytes_buf(uid_rand, sizeof(uid_rand));
   snprintf(uid, sizeof(uid), "dawn-%lx-%02x%02x%02x%02x%02x%02x%02x%02x-%d@oasis",
            (long)time(NULL), uid_rand[0], uid_rand[1], uid_rand[2], uid_rand[3], uid_rand[4],
            uid_rand[5], uid_rand[6], uid_rand[7], user_id);

   calendar_ical_new_t n = *event;
   n.uid = uid;
   if (!n.all_day && n.end == 0)
      n.end = n.start + (s_cal.config.default_event_duration_min * 60);
   char *ical = NULL;
   const calendar_ical_rc_t built = calendar_ical_build(&n, &ical);
   if (built != CALENDAR_ICAL_OK)
      return rc_from_ical(built);

   char password[384];
   char token[OAUTH_TOKEN_BUF_SIZE];
   caldav_auth_t auth;
   if (build_auth_for_account(&acct, &auth, password, sizeof(password), token, sizeof(token)) !=
       0) {
      free(ical);
      return CALENDAR_RC_FAILURE;
   }

   char etag[128] = "";
   char href[1024] = "";
   pthread_mutex_lock(write_lock(acct.id));
   caldav_error_t err = caldav_create_event(cals[target].caldav_path, &auth, uid, ical, etag,
                                            sizeof(etag), href, sizeof(href));
   if (err == CALDAV_OK)
      cache_written(cals[target].id, href, etag, ical);
   pthread_mutex_unlock(write_lock(acct.id));
   sodium_memzero(password, sizeof(password));
   sodium_memzero(token, sizeof(token));
   if (err != CALDAV_OK) {
      OLOG_ERROR("calendar: create event failed: %s", caldav_strerror(err));
      free(ical);
      /* A 412 here means the UID is taken, not a change in flight. */
      return err == CALDAV_ERR_CONFLICT ? CALENDAR_RC_REJECTED : rc_from_caldav(err);
   }
   free(ical);

   if (uid_out && uid_out_len > 0)
      snprintf(uid_out, uid_out_len, "%s", uid);
   OLOG_INFO("calendar: created event '%s' (uid=%s)", event->summary, uid);
   return CALENDAR_RC_OK;
}

/* Whether @p href is on the same server (scheme, host and port) as the
 * calendar at @p path: an href comes from the server, and the account's
 * credentials go wherever it points. */
static bool same_origin(const char *href, const char *path) {
   const char *h = strstr(href, "://");
   const char *p = strstr(path, "://");
   if (!h || !p)
      return false;
   const size_t hlen = (size_t)(h + 3 - href) + strcspn(h + 3, "/");
   const size_t plen = (size_t)(p + 3 - path) + strcspn(p + 3, "/");
   return hlen == plen && strncasecmp(href, path, hlen) == 0;
}

/* The event a write is for: its cached row (which calendar, which href) and
 * its account.  @p calendar_name narrows it to the calendars it names; without
 * one, a UID cached in two calendars (an invitation seen in both) is
 * ambiguous. */
static int find_target(int user_id,
                       const char *uid,
                       const char *calendar_name,
                       calendar_event_t *evt,
                       calendar_calendar_t *cal,
                       calendar_account_t *acct) {
   int matches = 0;
   if (calendar_name) {
      int64_t ids[32];
      const int n = get_user_calendar_ids(user_id, calendar_name, ids, 32);
      for (int i = 0; i < n; i++) {
         calendar_event_t e = { 0 };
         int m = 0;
         if (calendar_db_event_find(user_id, uid, ids[i], &e, &m) == 0) {
            if (matches++ == 0)
               *evt = e;
            else
               free(e.raw_ical);
         }
      }
   } else {
      calendar_db_event_find(user_id, uid, 0, evt, &matches);
   }
   if (matches == 0)
      return CALENDAR_RC_NOT_FOUND;
   free(evt->raw_ical); /* the server's copy is read fresh */
   evt->raw_ical = NULL;
   if (matches > 1)
      return CALENDAR_RC_AMBIGUOUS;
   if (calendar_db_calendar_get(evt->calendar_id, cal) != 0 ||
       calendar_db_account_get(cal->account_id, acct) != 0)
      return CALENDAR_RC_FAILURE;
   if (acct->read_only) {
      OLOG_WARNING("calendar: account '%s' is read-only, cannot modify event", acct->name);
      return CALENDAR_RC_READONLY;
   }
   if (!evt->href[0])
      return CALENDAR_RC_NOT_SYNCED;
   if (!same_origin(evt->href, cal->caldav_path)) {
      OLOG_WARNING("calendar: event '%s' is at another server than its calendar; not writing", uid);
      return CALENDAR_RC_REJECTED;
   }
   return CALENDAR_RC_OK;
}

/* Change (@p c) or delete (@p c NULL) an event on its server: read it fresh,
 * apply the change to that copy, and write it back only if nobody changed it
 * since (If-Match), once more if somebody had.  @p written receives the text
 * written on a change. */
static int read_and_write(const calendar_event_t *evt,
                          const char *cal_path,
                          const caldav_auth_t *auth,
                          const calendar_ical_change_t *c,
                          char **written,
                          char *etag_out,
                          size_t etag_len) {
   int rc = CALENDAR_RC_OK;
   caldav_error_t err = CALDAV_OK;
   for (int attempt = 0; attempt < 2; attempt++) {
      char *ical = NULL;
      char etag[128] = "";
      err = caldav_get_event(cal_path, evt->href, auth, &ical, etag, sizeof(etag));
      if (err != CALDAV_OK)
         break;
      if (!calendar_ical_has_uid(ical, evt->uid)) {
         /* The server answered with another event than the one asked for. */
         OLOG_WARNING("calendar: '%s' came back as another event; not writing", evt->href);
         free(ical);
         return CALENDAR_RC_REJECTED;
      }
      if (!etag[0]) {
         /* Without one the write can't be conditional: it would overwrite
          * whatever changed since the read. */
         OLOG_WARNING("calendar: '%s' came without an ETag; not writing", evt->href);
         free(ical);
         return CALENDAR_RC_REJECTED;
      }
      if (c) {
         char *edited = NULL;
         const calendar_ical_rc_t ir = calendar_ical_edit(ical, c, &edited);
         free(ical);
         if (ir != CALENDAR_ICAL_OK)
            return rc_from_ical(ir);
         err = caldav_update_event(evt->href, auth, etag, edited, etag_out, etag_len);
         free(*written);
         *written = edited;
      } else {
         free(ical);
         err = caldav_delete_event(evt->href, auth, etag);
      }
      if (err != CALDAV_ERR_CONFLICT)
         break; /* else changed elsewhere between the read and the write: again */
   }
   if (err != CALDAV_OK) {
      OLOG_ERROR("calendar: %s of '%s' failed: %s", c ? "update" : "delete", evt->uid,
                 caldav_strerror(err));
      rc = (!c && err == CALDAV_ERR_NOT_FOUND) ? CALENDAR_RC_OK /* already gone */
                                               : rc_from_caldav(err);
   }
   return rc;
}

static int write_event(int user_id,
                       const char *uid,
                       const char *calendar_name,
                       const calendar_ical_change_t *c) {
   calendar_event_t evt = { 0 };
   calendar_calendar_t cal = { 0 };
   calendar_account_t acct = { 0 };
   int rc = find_target(user_id, uid, calendar_name, &evt, &cal, &acct);
   if (rc != CALENDAR_RC_OK)
      return rc;
   char password[384];
   char token[OAUTH_TOKEN_BUF_SIZE];
   caldav_auth_t auth;
   if (build_auth_for_account(&acct, &auth, password, sizeof(password), token, sizeof(token)) != 0)
      return CALENDAR_RC_FAILURE;

   calendar_ical_change_t change = CALENDAR_ICAL_CHANGE_INIT;
   char addresses[CALDAV_MAX_USER_ADDRESSES + 1][CALDAV_USER_ADDRESS_LEN];
   const char *address_list[CALDAV_MAX_USER_ADDRESSES + 1];
   if (c) {
      change = *c;
      /* Only a change beyond reminders needs to know whose event it is. */
      const bool beyond_reminders = (c->summary && c->summary[0]) ||
                                    (c->location && c->location[0]) ||
                                    (c->description && c->description[0]) || c->set_start ||
                                    c->set_end || c->rrule || c->occurrence || c->cancel;
      if (beyond_reminders)
         change.account_count = account_addresses(&acct, &auth, addresses,
                                                  CALDAV_MAX_USER_ADDRESSES + 1);
      for (int i = 0; i < change.account_count; i++)
         address_list[i] = addresses[i];
      change.accounts = address_list;
   }
   char *written = NULL;
   char etag[128] = "";
   /* The cache is updated before the lock is let go, so a second write to the
    * same event can't have its result cached under this one's. */
   pthread_mutex_lock(write_lock(acct.id));
   rc = read_and_write(&evt, cal.caldav_path, &auth, c ? &change : NULL, &written, etag,
                       sizeof(etag));
   if (rc == CALENDAR_RC_OK && c) {
      cache_written(evt.calendar_id, evt.href, etag, written);
   } else if (rc == CALENDAR_RC_OK) {
      /* By href, not row id: a write just before this one replaced the row. */
      calendar_db_event_delete_by_href(evt.calendar_id, evt.href, NULL);
   }
   pthread_mutex_unlock(write_lock(acct.id));
   sodium_memzero(password, sizeof(password));
   sodium_memzero(token, sizeof(token));
   free(written);

   if (rc == CALENDAR_RC_OK) {
      OLOG_INFO("calendar: %s event '%s'", c ? "updated" : "deleted", uid);
      calendar_broadcast_events_changed(user_id);
   }
   return rc;
}

int calendar_service_update(int user_id,
                            const char *uid,
                            const char *calendar_name,
                            const calendar_ical_change_t *change) {
   return write_event(user_id, uid, calendar_name, change);
}

int calendar_service_delete(int user_id, const char *uid, const char *calendar_name) {
   return write_event(user_id, uid, calendar_name, NULL);
}

int calendar_service_retry_after(void) {
   return caldav_last_retry_after();
}
/* =============================================================================
 * Access Summary
 * ============================================================================= */

int calendar_service_get_access_summary(int user_id,
                                        char *writable,
                                        size_t w_len,
                                        char *read_only_out,
                                        size_t r_len) {
   calendar_calendar_t cals[32];
   int count = 0;
   calendar_db_active_calendars_for_user(user_id, cals, 32, &count);
   if (count <= 0)
      return 0;

   int w_pos = 0, r_pos = 0;
   bool has_read_only = false;
   writable[0] = '\0';
   read_only_out[0] = '\0';

   for (int i = 0; i < count; i++) {
      char *dst = cals[i].account_read_only ? read_only_out : writable;
      size_t dst_len = cals[i].account_read_only ? r_len : w_len;
      int *pos = cals[i].account_read_only ? &r_pos : &w_pos;
      has_read_only = has_read_only || cals[i].account_read_only;
      if ((size_t)*pos >= dst_len)
         continue; /* full: snprintf returns what it would have written */
      char label[CALENDAR_PICK_LABEL_MAX];
      calendar_pick_label(cals, count, i, label, sizeof(label));
      int w = snprintf(dst + *pos, dst_len - (size_t)*pos, "%s%s", *pos ? ", " : "", label);
      if (w > 0)
         *pos += w;
   }
   return has_read_only ? 1 : 0;
}

/* =============================================================================
 * Background Sync Thread
 * ============================================================================= */

/* Seconds on the monotonic clock. */
static time_t mono_now(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec;
}

/* Wait until a sync is asked for, @p deadline (mono_now() seconds) passes, or
 * shutdown; take the waiting requests into @p ids.  Returns how many. */
static int wait_for_work(time_t deadline, int64_t *ids) {
   pthread_mutex_lock(&s_cal.mutex);
   while (atomic_load(&s_cal.sync_running) && s_cal.requested_count == 0 && mono_now() < deadline) {
      struct timespec until = { .tv_sec = deadline, .tv_nsec = 0 };
      pthread_cond_timedwait(&s_cal.wake, &s_cal.mutex, &until);
   }
   const int n = s_cal.requested_count;
   memcpy(ids, s_cal.requested, (size_t)n * sizeof(ids[0]));
   s_cal.requested_count = 0;
   pthread_mutex_unlock(&s_cal.mutex);
   return n;
}

/* Run the syncs asked for, telling each account's owner how it went. */
static void sync_requested(const int64_t *ids, int n) {
   for (int i = 0; i < n && atomic_load(&s_cal.sync_running); i++) {
      calendar_account_t acct;
      if (calendar_db_account_get(ids[i], &acct) != 0)
         continue;
      const bool ok = sync_account(ids[i]) == 0;
      calendar_broadcast_sync_done(acct.user_id, ids[i], ok);
   }
}

static void *sync_thread_func(void *arg) {
   (void)arg;
   OLOG_INFO("calendar: sync thread started");

   time_t next_periodic = 0; /* sync at startup */
   while (atomic_load(&s_cal.sync_running)) {
      int64_t ids[CALENDAR_MAX_SYNC_REQUESTS];
      const int n = wait_for_work(next_periodic, ids);
      if (!atomic_load(&s_cal.sync_running))
         break;
      sync_requested(ids, n);
      if (mono_now() < next_periodic)
         continue;

      /* Sync all enabled accounts that are due */
      calendar_account_t accounts[CALENDAR_MAX_ACCOUNTS];
      int acct_count = 0;
      if (calendar_db_account_list_enabled(accounts, CALENDAR_MAX_ACCOUNTS, &acct_count) != 0) {
         /* The database couldn't be read: try again shortly, not a whole
          * interval later. */
         next_periodic = mono_now() + CALENDAR_DB_RETRY_SEC;
         continue;
      }

      time_t now = time(NULL);
      int synced = 0;
      for (int i = 0; i < acct_count && atomic_load(&s_cal.sync_running); i++) {
         int interval = accounts[i].sync_interval_sec > 0 ? accounts[i].sync_interval_sec
                                                          : s_cal.config.sync_interval_sec;
         if (now - accounts[i].last_sync < interval)
            continue;

         if (sync_account(accounts[i].id) == 0)
            synced++;
      }

      if (synced > 0)
         OLOG_INFO("calendar: background sync completed %d/%d accounts", synced, acct_count);
      next_periodic = mono_now() + s_cal.config.sync_interval_sec;
   }

   OLOG_INFO("calendar: sync thread exiting");
   return NULL;
}
