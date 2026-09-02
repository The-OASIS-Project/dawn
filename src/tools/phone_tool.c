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
 * Phone LLM tool — voice-controlled phone calls and SMS.
 * Actions: call, confirm_call, answer, hang_up, send_sms, confirm_sms,
 *          read_sms, call_log, sms_log, status
 */

#include "tools/phone_tool.h"

#include <json-c/json.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "logging.h"
#include "tools/phone_audio_config.h"
#include "tools/phone_contacts.h"
#include "tools/phone_db.h"
#include "tools/phone_service.h"
#include "tools/toml.h"
#include "tools/tool_registry.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define RESULT_BUF_SIZE 8192

/* =============================================================================
 * Config
 * ============================================================================= */

typedef struct {
   bool enabled;
   bool confirm_outbound;
   bool warn_on_multi_segment;
   int delete_rate_limit_per_hour; /* confirm_delete_* per hour */
} phone_tool_config_t;

static phone_tool_config_t s_config = {
   .enabled = true,
   .confirm_outbound = true,
   .warn_on_multi_segment = true,
   .delete_rate_limit_per_hour = 10,
};

/* Parsed [phone] call-audio config, forwarded to phone_service at init.  Owned as
 * one unit in phone_audio_config.c so parse<->write stay symmetric (anti-clobber). */
static phone_audio_config_t s_audio;
static bool s_audio_parsed = false;

/* Global mutex protecting all mutable phone_tool state.
 *
 * Tool callbacks can run concurrently across WebUI worker threads — adding
 * "phone" to SEQUENTIAL_TOOLS only serializes within a single tool-call
 * batch from one session, not across sessions. This lock guards:
 *   - s_pending_call_number / s_pending_sms_* / s_pending_delete_*
 *   - s_delete_buckets[]
 * Critical sections are short (string copies, timestamp records) so a
 * single coarse-grained mutex is appropriate. */
static pthread_mutex_t s_phone_tool_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Delete rate-limit sliding-window timestamps. Scoped per-user_id. */
#define DELETE_BUCKET_SIZE 16

typedef struct {
   int user_id;
   time_t timestamps[DELETE_BUCKET_SIZE];
   unsigned int count;
} delete_bucket_t;

/* We only expect one or two active users in flight; scan linearly. */
#define DELETE_BUCKETS 4
static delete_bucket_t s_delete_buckets[DELETE_BUCKETS];

/* Returns true if the delete is allowed, false if rate-limited. Records the
 * timestamp on success. Takes s_phone_tool_mutex for the duration. */
static bool check_delete_rate_limit(int user_id) {
   if (s_config.delete_rate_limit_per_hour <= 0)
      return true; /* disabled */

   pthread_mutex_lock(&s_phone_tool_mutex);

   time_t now = time(NULL);
   time_t cutoff = now - 3600;

   delete_bucket_t *bucket = NULL;
   delete_bucket_t *free_slot = NULL;
   for (int i = 0; i < DELETE_BUCKETS; i++) {
      if (s_delete_buckets[i].user_id == user_id) {
         bucket = &s_delete_buckets[i];
         break;
      }
      if (!free_slot && s_delete_buckets[i].user_id == 0)
         free_slot = &s_delete_buckets[i];
   }
   if (!bucket) {
      /* Reuse free slot; or evict the bucket whose most recent activity is
       * oldest (true LRU). Using cumulative count would evict new users first
       * and keep near-limit attackers' state alive. */
      if (!free_slot) {
         free_slot = &s_delete_buckets[0];
         time_t free_slot_last = 0;
         for (int j = 0; j < DELETE_BUCKET_SIZE; j++)
            if (s_delete_buckets[0].timestamps[j] > free_slot_last)
               free_slot_last = s_delete_buckets[0].timestamps[j];
         for (int i = 1; i < DELETE_BUCKETS; i++) {
            time_t last = 0;
            for (int j = 0; j < DELETE_BUCKET_SIZE; j++)
               if (s_delete_buckets[i].timestamps[j] > last)
                  last = s_delete_buckets[i].timestamps[j];
            if (last < free_slot_last) {
               free_slot = &s_delete_buckets[i];
               free_slot_last = last;
            }
         }
         memset(free_slot, 0, sizeof(*free_slot));
      }
      free_slot->user_id = user_id;
      bucket = free_slot;
   }

   int active = 0;
   unsigned int scan = (bucket->count < DELETE_BUCKET_SIZE) ? bucket->count : DELETE_BUCKET_SIZE;
   for (unsigned int i = 0; i < scan; i++) {
      if (bucket->timestamps[i] >= cutoff)
         active++;
   }
   if (active >= s_config.delete_rate_limit_per_hour) {
      pthread_mutex_unlock(&s_phone_tool_mutex);
      return false;
   }

   unsigned int idx = bucket->count % DELETE_BUCKET_SIZE;
   bucket->timestamps[idx] = now;
   bucket->count++;
   pthread_mutex_unlock(&s_phone_tool_mutex);
   return true;
}

/* Pending confirmation state (per-session would be ideal, but single user for now).
 * Timestamps bound the replay window — matches the delete pending-state TTL below. */
static char s_pending_call_number[24] = "";
static time_t s_pending_call_at = 0;
static char s_pending_sms_number[24] = "";
static char s_pending_sms_body[1024] = "";
static time_t s_pending_sms_at = 0;

/* Pending deletion state with TTL.
 *
 * Matches the email-tool pattern: preview arms pending state, next-turn
 * confirm_delete_* executes. The 120s TTL bounds replay windows if the user
 * walks away after the preview. Rate-limit caps are applied at confirm time.
 */
#define PHONE_TOOL_PENDING_TTL_SEC 120

static char s_pending_delete_kind[16] = "";   /* "sms" or "call" */
static int s_pending_delete_user_id = -1;     /* user that armed it */
static int64_t s_pending_delete_id = -1;      /* used if criteria=by-id */
static char s_pending_delete_number[24] = ""; /* used if criteria=by-number */
static time_t s_pending_delete_cutoff = 0;    /* used if criteria=older-than */
static int s_pending_delete_count = 0;        /* preview count shown to user */
static time_t s_pending_delete_at = 0;        /* arm time for TTL */

/* Caller must hold s_phone_tool_mutex. */
static void clear_pending_delete_locked(void) {
   s_pending_delete_kind[0] = '\0';
   s_pending_delete_user_id = -1;
   s_pending_delete_id = -1;
   s_pending_delete_number[0] = '\0';
   s_pending_delete_cutoff = 0;
   s_pending_delete_count = 0;
   s_pending_delete_at = 0;
}

static void clear_pending_delete(void) {
   pthread_mutex_lock(&s_phone_tool_mutex);
   clear_pending_delete_locked();
   pthread_mutex_unlock(&s_phone_tool_mutex);
}

/* Forward decl — used in the delete-preview handlers before the definition. */
static void arm_pending_delete(int user_id,
                               const char *kind,
                               int64_t id,
                               const char *number,
                               time_t cutoff,
                               int preview_count);

/* =============================================================================
 * JSON Helpers
 * ============================================================================= */

static const char *json_get_str(struct json_object *obj, const char *key) {
   struct json_object *val = NULL;
   if (!json_object_object_get_ex(obj, key, &val))
      return NULL;
   return json_object_get_string(val);
}

static int json_get_int(struct json_object *obj, const char *key, int def) {
   struct json_object *val = NULL;
   if (!json_object_object_get_ex(obj, key, &val))
      return def;
   return json_object_get_int(val);
}

/* =============================================================================
 * SMS segment estimator
 *
 * Rough heuristic that mirrors ECHO's UCS2-only PDU encoding. Any non-ASCII
 * byte in the body → UCS2 (67 chars/segment after UDH). Pure ASCII → 153
 * chars/segment (reserved for when ECHO adds GSM7). Used to warn the user in
 * the send-confirmation preview when a message will be split.
 *
 * Keep in sync with ECHO src/pdu.c:pdu_segment_count(). When GSM7 lands,
 * extract to a shared header.
 * ============================================================================= */

static int estimate_sms_segments(const char *body) {
   if (!body || !*body)
      return 1;

   bool is_ucs2 = false;
   size_t char_count = 0;
   const unsigned char *p = (const unsigned char *)body;
   const unsigned char *end = p + strlen(body);
   while (p < end) {
      if (*p < 0x80) {
         p++;
         char_count++;
      } else {
         is_ucs2 = true;
         /* UTF-8 multi-byte sequence — advance by length. UCS2 segment limits
          * are in UTF-16 code units, so a 4-byte UTF-8 (non-BMP, e.g. emoji)
          * costs 2 code units (surrogate pair). 2- and 3-byte sequences fit
          * in one UTF-16 code unit. Bounded so truncated/malformed UTF-8
          * can't read past end. */
         int n;
         size_t units;
         if ((*p & 0xE0) == 0xC0) {
            n = 2;
            units = 1;
         } else if ((*p & 0xF0) == 0xE0) {
            n = 3;
            units = 1;
         } else if ((*p & 0xF8) == 0xF0) {
            n = 4;
            units = 2;
         } else {
            n = 1;
            units = 1;
         }
         if (p + n > end) {
            /* Truncated sequence — advance one byte, treat as single unit. */
            p++;
            char_count++;
         } else {
            p += n;
            char_count += units;
         }
      }
   }

   int per_seg = is_ucs2 ? 67 : 153;
   /* A body that fits in a single segment without UDH is more generous
    * (70 UCS2 / 160 GSM7), so only report multi-seg when the UDH-reduced
    * budget is actually exceeded. */
   int single_cap = is_ucs2 ? 70 : 160;
   if ((int)char_count <= single_cap)
      return 1;

   return (int)((char_count + per_seg - 1) / per_seg);
}

/* =============================================================================
 * Action Handlers
 * ============================================================================= */

/* Wrap a phone_service_* result. The service writes a human-readable message into `result` and
 * returns 0 on success / 1 on error (phone_service.h). Prefix the WebUI failure mark from the
 * STATUS code — never parsed from the text — so a failed call/answer/hangup/SMS reds the pill.
 * NOTE: the multi-contact disambiguation path also returns 1 (a "which contact?" prompt), so it
 * reds too; accepted (distinguishing it would need a dedicated service return code). */
static char *phone_service_result_dup(int rc, const char *result) {
   if (rc == 0 || result == NULL) { /* 0 == success per phone_service.h */
      return strdup(result ? result : "");
   }
   size_t n = strlen(result);
   char *out = malloc(n + 2);
   if (out == NULL) {
      return strdup(result); /* degrade: unmarked rather than drop the message */
   }
   out[0] = TOOL_RESULT_ERROR_MARK[0];
   memcpy(out + 1, result, n + 1);
   return out;
}

static char *handle_call(struct json_object *details, int user_id) {
   const char *target = json_get_str(details, "target");
   if (!target || target[0] == '\0') {
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: 'target' is required (phone number or contact name)");
   }

   if (s_config.confirm_outbound) {
      /* Resolve the contact NOW (at preview) so the user confirms a real,
       * verified number — not a name we haven't checked.  Ambiguous or
       * fuzzy-only matches surface candidates instead of arming a call. */
      phone_resolve_t rez;
      phone_contacts_resolve(user_id, target, &rez);
      if (rez.kind != PHONE_RESOLVE_NUMBER && rez.kind != PHONE_RESOLVE_UNIQUE) {
         s_pending_call_number[0] = '\0'; /* nothing armed — model must disambiguate first */
         char buf[512];
         phone_contacts_format_disambiguation(target, &rez, buf, sizeof(buf));
         return strdup(buf);
      }

      /* Arm the resolved number so confirm dials exactly what was previewed. */
      snprintf(s_pending_call_number, sizeof(s_pending_call_number), "%s", rez.number);
      s_pending_call_at = time(NULL);
      char buf[256];
      if (rez.name[0]) {
         snprintf(buf, sizeof(buf), "About to call %s at %s. Say 'confirm' to proceed.", rez.name,
                  rez.number);
      } else {
         snprintf(buf, sizeof(buf), "About to call %s. Say 'confirm' to proceed.", rez.number);
      }
      return strdup(buf);
   }

   /* No confirmation — dial immediately (service layer applies the same guard) */
   char result[RESULT_BUF_SIZE];
   int rc = phone_service_call(user_id, target, result, sizeof(result));
   return phone_service_result_dup(rc, result);
}

static char *handle_confirm_call(int user_id) {
   if (s_pending_call_number[0] == '\0') {
      return strdup(TOOL_RESULT_ERROR_MARK "Error: no pending call to confirm");
   }
   if (time(NULL) - s_pending_call_at > PHONE_TOOL_PENDING_TTL_SEC) {
      s_pending_call_number[0] = '\0';
      s_pending_call_at = 0;
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: confirmation expired. Please retry the call request.");
   }

   char result[RESULT_BUF_SIZE];
   int rc = phone_service_call(user_id, s_pending_call_number, result, sizeof(result));

   s_pending_call_number[0] = '\0';
   s_pending_call_at = 0;
   return phone_service_result_dup(rc, result);
}

static char *handle_answer(int user_id) {
   char result[RESULT_BUF_SIZE];
   int rc = phone_service_answer(user_id, result, sizeof(result));
   return phone_service_result_dup(rc, result);
}

static char *handle_hang_up(int user_id) {
   char result[RESULT_BUF_SIZE];
   int rc = phone_service_hangup(user_id, result, sizeof(result));
   return phone_service_result_dup(rc, result);
}

static char *handle_send_sms(struct json_object *details, int user_id) {
   const char *target = json_get_str(details, "target");
   const char *body = json_get_str(details, "body");

   if (!target || target[0] == '\0') {
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: 'target' is required (phone number or contact name)");
   }
   if (!body || body[0] == '\0') {
      return strdup(TOOL_RESULT_ERROR_MARK "Error: 'body' is required (message text)");
   }

   if (s_config.confirm_outbound) {
      /* Resolve the recipient NOW (at preview) — same guard as calls, so the
       * user confirms a verified number rather than an unchecked name. */
      phone_resolve_t rez;
      phone_contacts_resolve(user_id, target, &rez);
      if (rez.kind != PHONE_RESOLVE_NUMBER && rez.kind != PHONE_RESOLVE_UNIQUE) {
         s_pending_sms_number[0] = '\0'; /* nothing armed — disambiguate first */
         s_pending_sms_body[0] = '\0';
         char buf[512];
         phone_contacts_format_disambiguation(target, &rez, buf, sizeof(buf));
         return strdup(buf);
      }

      /* Arm the resolved number so confirm sends exactly what was previewed. */
      snprintf(s_pending_sms_number, sizeof(s_pending_sms_number), "%s", rez.number);
      snprintf(s_pending_sms_body, sizeof(s_pending_sms_body), "%s", body);
      s_pending_sms_at = time(NULL);

      const char *recipient = rez.name[0] ? rez.name : rez.number;
      int segs = s_config.warn_on_multi_segment ? estimate_sms_segments(body) : 1;
      char buf[768];
      if (segs > 1) {
         snprintf(buf, sizeof(buf),
                  "About to send SMS to %s: \"%s\". This will send as %d text messages. "
                  "Say 'confirm' to send.",
                  recipient, body, segs);
      } else {
         snprintf(buf, sizeof(buf), "About to send SMS to %s: \"%s\". Say 'confirm' to send.",
                  recipient, body);
      }
      return strdup(buf);
   }

   char result[RESULT_BUF_SIZE];
   int rc = phone_service_send_sms(user_id, target, body, result, sizeof(result));
   return phone_service_result_dup(rc, result);
}

static char *handle_confirm_sms(int user_id) {
   if (s_pending_sms_number[0] == '\0') {
      return strdup(TOOL_RESULT_ERROR_MARK "Error: no pending SMS to confirm");
   }
   if (time(NULL) - s_pending_sms_at > PHONE_TOOL_PENDING_TTL_SEC) {
      s_pending_sms_number[0] = '\0';
      s_pending_sms_body[0] = '\0';
      s_pending_sms_at = 0;
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: confirmation expired. Please retry the send request.");
   }

   char result[RESULT_BUF_SIZE];
   int rc = phone_service_send_sms(user_id, s_pending_sms_number, s_pending_sms_body, result,
                                   sizeof(result));

   s_pending_sms_number[0] = '\0';
   s_pending_sms_body[0] = '\0';
   s_pending_sms_at = 0;
   return phone_service_result_dup(rc, result);
}

static char *handle_read_sms(int user_id) {
   /* Capped at 10 resident rows to keep stack frame bounded after body[2048] bump. */
   phone_sms_log_t entries[10];
   int count = 0;
   phone_db_sms_get_unread(user_id, entries, 10, &count);

   char *buf = malloc(RESULT_BUF_SIZE);
   if (!buf) {
      return strdup(TOOL_RESULT_ERROR_MARK "Error: memory allocation failed");
   }

   int pos = 0;
   if (count <= 0) {
      pos += snprintf(buf, RESULT_BUF_SIZE, "No unread text messages.");
   } else {
      pos += snprintf(buf, RESULT_BUF_SIZE, "Unread text messages (%d):\n", count);
      for (int i = 0; i < count && pos < RESULT_BUF_SIZE - 512; i++) {
         const char *dir = entries[i].direction == PHONE_DIR_INCOMING ? "From" : "To";
         /* Show "Name (+1...)" when a contact matches, bare number otherwise —
          * the LLM needs the number visible to route delete-by-number requests. */
         char display[96];
         if (entries[i].contact_name[0]) {
            snprintf(display, sizeof(display), "%s (%s)", entries[i].contact_name,
                     entries[i].number);
         } else {
            snprintf(display, sizeof(display), "%s", entries[i].number);
         }

         /* Include DB id so the LLM can target delete_sms {id: N}. */
         pos += snprintf(buf + pos, RESULT_BUF_SIZE - pos, "\n%d. [id=%lld] %s: %s\n   %s\n", i + 1,
                         (long long)entries[i].id, dir, display, entries[i].body);

         /* Mark as read */
         phone_db_sms_mark_read(entries[i].id);
      }
   }

   return buf;
}

/* =============================================================================
 * Delete Handlers — two-step confirmation with TTL + nonce.
 *
 * Flow:
 *   1. LLM calls delete_sms / delete_call with {id} OR {number} OR
 *      {older_than_days}. Exactly one criterion required.
 *   2. Handler looks up match count (0 → immediate error, don't arm pending).
 *   3. Preview is returned with a generated 4-digit confirmation code.
 *      Pending state is armed with criteria + timestamp + nonce.
 *   4. LLM calls confirm_delete_sms / confirm_delete_call with
 *      {confirm_code: "NNNN"}. Handler checks TTL (120s) and nonce match,
 *      executes DELETE, audit-logs, clears pending.
 * ============================================================================= */

/* Convert timestamp → compact yyyy-mm-dd string for previews. */
static void format_short_date(time_t t, char *out, size_t out_size) {
   struct tm tm_buf;
   localtime_r(&t, &tm_buf);
   strftime(out, out_size, "%Y-%m-%d", &tm_buf);
}

static char *handle_delete_sms(struct json_object *details, int user_id) {
   if (!details)
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: delete_sms requires one of {id, number, older_than_days}.");

   struct json_object *id_obj = NULL;
   struct json_object *num_obj = NULL;
   struct json_object *older_obj = NULL;
   json_object_object_get_ex(details, "id", &id_obj);
   json_object_object_get_ex(details, "number", &num_obj);
   json_object_object_get_ex(details, "older_than_days", &older_obj);

   int criteria_count = (id_obj ? 1 : 0) + (num_obj ? 1 : 0) + (older_obj ? 1 : 0);
   if (criteria_count != 1)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: delete_sms requires exactly one of "
                                           "{id, number, older_than_days}.");

   char buf[1024];

   if (id_obj) {
      int64_t id = (int64_t)json_object_get_int64(id_obj);

      /* Fetch the exact row by id — works for old rows too and saves ~21 KB stack. */
      phone_sms_log_t match;
      int rc = phone_db_sms_log_get_by_id(user_id, id, &match);
      if (rc == PHONE_DB_NOT_FOUND) {
         clear_pending_delete();
         snprintf(buf, sizeof(buf), "Error: SMS record #%lld not found.", (long long)id);
         return strdup(buf);
      }
      if (rc != PHONE_DB_SUCCESS) {
         clear_pending_delete();
         return strdup(TOOL_RESULT_ERROR_MARK "Error: database error looking up SMS record.");
      }

      /* Truncate body preview */
      char preview[128];
      size_t blen = strlen(match.body);
      if (blen > 100) {
         memcpy(preview, match.body, 100);
         preview[100] = '\0';
         strncat(preview, "...", sizeof(preview) - strlen(preview) - 1);
      } else {
         memcpy(preview, match.body, blen + 1);
      }
      char date[32];
      format_short_date(match.timestamp, date, sizeof(date));
      const char *dir = match.direction == PHONE_DIR_INCOMING ? "in" : "out";
      const char *name = match.contact_name[0] ? match.contact_name : match.number;

      arm_pending_delete(user_id, "sms", id, NULL, 0, 1);

      snprintf(buf, sizeof(buf),
               "About to delete SMS #%lld [%s %s, %s]: \"%s\". Say 'confirm' to delete.",
               (long long)id, dir, name, date, preview);
      return strdup(buf);
   }

   if (num_obj) {
      const char *number = json_object_get_string(num_obj);
      if (!number || !*number) {
         clear_pending_delete();
         return strdup(TOOL_RESULT_ERROR_MARK "Error: 'number' must be a non-empty phone number.");
      }
      int match_count = 0;
      if (phone_db_sms_log_count_by_number(user_id, number, &match_count) != PHONE_DB_SUCCESS) {
         clear_pending_delete();
         return strdup(TOOL_RESULT_ERROR_MARK "Error: database error counting SMS by number.");
      }
      if (match_count == 0) {
         clear_pending_delete();
         snprintf(buf, sizeof(buf), "No SMS records match number %s.", number);
         return strdup(buf);
      }

      arm_pending_delete(user_id, "sms", -1, number, 0, match_count);

      snprintf(buf, sizeof(buf),
               "About to delete %d SMS message(s) matching number %s. "
               "Say 'confirm' to delete all %d.",
               match_count, number, match_count);
      return strdup(buf);
   }

   /* older_than_days */
   int days = json_object_get_int(older_obj);
   if (days <= 0) {
      clear_pending_delete();
      return strdup(TOOL_RESULT_ERROR_MARK "Error: 'older_than_days' must be a positive integer.");
   }
   time_t cutoff = time(NULL) - (time_t)days * 86400;

   int match_count = 0;
   if (phone_db_sms_log_count_older_than(user_id, cutoff, &match_count) != PHONE_DB_SUCCESS) {
      clear_pending_delete();
      return strdup(TOOL_RESULT_ERROR_MARK "Error: database error counting older SMS.");
   }
   if (match_count == 0) {
      clear_pending_delete();
      snprintf(buf, sizeof(buf), "No SMS records older than %d days.", days);
      return strdup(buf);
   }

   arm_pending_delete(user_id, "sms", -1, NULL, cutoff, match_count);

   char cutoff_date[32];
   format_short_date(cutoff, cutoff_date, sizeof(cutoff_date));
   snprintf(buf, sizeof(buf),
            "About to delete %d SMS record(s) older than %d days (before %s). "
            "Say 'confirm' to delete.",
            match_count, days, cutoff_date);
   return strdup(buf);
}

/* Arm pending deletion for the given user. Takes the mutex for the assignment
 * so an in-flight confirm sees a consistent snapshot. */
static void arm_pending_delete(int user_id,
                               const char *kind,
                               int64_t id,
                               const char *number,
                               time_t cutoff,
                               int preview_count) {
   pthread_mutex_lock(&s_phone_tool_mutex);
   snprintf(s_pending_delete_kind, sizeof(s_pending_delete_kind), "%s", kind);
   s_pending_delete_user_id = user_id;
   s_pending_delete_id = id;
   if (number) {
      snprintf(s_pending_delete_number, sizeof(s_pending_delete_number), "%s", number);
   } else {
      s_pending_delete_number[0] = '\0';
   }
   s_pending_delete_cutoff = cutoff;
   s_pending_delete_count = preview_count;
   s_pending_delete_at = time(NULL);
   pthread_mutex_unlock(&s_phone_tool_mutex);
}

/* Shared confirm logic for sms / call. Returns non-NULL error message if any
 * precondition fails; caller proceeds to execute DELETE on NULL return.
 * Validates that the confirming user matches the user that armed pending —
 * defense against cross-session state corruption. */
static char *validate_pending_delete(const char *expected_kind, int user_id) {
   pthread_mutex_lock(&s_phone_tool_mutex);

   if (!s_pending_delete_kind[0]) {
      pthread_mutex_unlock(&s_phone_tool_mutex);
      return strdup(TOOL_RESULT_ERROR_MARK "Error: no pending deletion to confirm.");
   }
   if (s_pending_delete_user_id != user_id) {
      /* Don't clear — another user's pending; leave it alone. */
      pthread_mutex_unlock(&s_phone_tool_mutex);
      return strdup(TOOL_RESULT_ERROR_MARK "Error: no pending deletion to confirm.");
   }
   if (strcmp(s_pending_delete_kind, expected_kind) != 0) {
      clear_pending_delete_locked();
      pthread_mutex_unlock(&s_phone_tool_mutex);
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: pending deletion is for a different record type.");
   }
   if (time(NULL) - s_pending_delete_at > PHONE_TOOL_PENDING_TTL_SEC) {
      clear_pending_delete_locked();
      pthread_mutex_unlock(&s_phone_tool_mutex);
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: confirmation expired. Please retry the delete request.");
   }
   pthread_mutex_unlock(&s_phone_tool_mutex);
   return NULL;
}

static char *handle_confirm_delete_sms(int user_id) {
   char *err = validate_pending_delete("sms", user_id);
   if (err)
      return err;

   if (!check_delete_rate_limit(user_id)) {
      OLOG_WARNING("phone_tool: user=%d delete rate-limited (>%d/hour)", user_id,
                   s_config.delete_rate_limit_per_hour);
      clear_pending_delete();
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: too many deletions in the last hour. Try again later.");
   }

   /* Snapshot pending state under the mutex so a concurrent arm can't alter
    * the criteria mid-execute. */
   int64_t snap_id;
   char snap_number[24];
   time_t snap_cutoff;
   pthread_mutex_lock(&s_phone_tool_mutex);
   snap_id = s_pending_delete_id;
   snprintf(snap_number, sizeof(snap_number), "%s", s_pending_delete_number);
   snap_cutoff = s_pending_delete_cutoff;
   pthread_mutex_unlock(&s_phone_tool_mutex);

   char buf[256];
   int deleted = 0;
   int rc;

   if (snap_id >= 0) {
      rc = phone_db_sms_log_delete(user_id, snap_id);
      if (rc == PHONE_DB_SUCCESS) {
         deleted = 1;
         OLOG_INFO("phone_tool: user=%d deleted sms id=%lld", user_id, (long long)snap_id);
         snprintf(buf, sizeof(buf), "Deleted 1 SMS record.");
      } else if (rc == PHONE_DB_NOT_FOUND) {
         snprintf(buf, sizeof(buf), "SMS record #%lld no longer exists.", (long long)snap_id);
      } else {
         snprintf(buf, sizeof(buf), "Deletion failed: database error.");
      }
   } else if (snap_number[0]) {
      rc = phone_db_sms_log_delete_by_number(user_id, snap_number, &deleted);
      if (rc == PHONE_DB_SUCCESS) {
         char redacted[32];
         phone_number_redact(snap_number, redacted, sizeof(redacted));
         OLOG_INFO("phone_tool: user=%d deleted %d sms for number=%s", user_id, deleted, redacted);
         snprintf(buf, sizeof(buf), "Deleted %d SMS record(s).", deleted);
      } else {
         snprintf(buf, sizeof(buf), "Deletion failed: database error.");
      }
   } else if (snap_cutoff > 0) {
      rc = phone_db_sms_log_delete_older_than(user_id, snap_cutoff, &deleted);
      if (rc == PHONE_DB_SUCCESS) {
         OLOG_INFO("phone_tool: user=%d deleted %d sms older than %lld", user_id, deleted,
                   (long long)snap_cutoff);
         snprintf(buf, sizeof(buf), "Deleted %d SMS record(s).", deleted);
      } else {
         snprintf(buf, sizeof(buf), "Deletion failed: database error.");
      }
   } else {
      snprintf(buf, sizeof(buf), "Error: pending state is inconsistent.");
   }

   clear_pending_delete();
   return strdup(buf);
}

static char *handle_delete_call(struct json_object *details, int user_id) {
   if (!details)
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: delete_call requires one of {id, older_than_days}.");

   struct json_object *id_obj = NULL;
   struct json_object *older_obj = NULL;
   json_object_object_get_ex(details, "id", &id_obj);
   json_object_object_get_ex(details, "older_than_days", &older_obj);

   int criteria_count = (id_obj ? 1 : 0) + (older_obj ? 1 : 0);
   if (criteria_count != 1)
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: delete_call requires exactly one of {id, older_than_days}.");

   char buf[768];

   if (id_obj) {
      int64_t id = (int64_t)json_object_get_int64(id_obj);

      phone_call_log_t match;
      int rc = phone_db_call_log_get_by_id(user_id, id, &match);
      if (rc == PHONE_DB_NOT_FOUND) {
         clear_pending_delete();
         snprintf(buf, sizeof(buf), "Error: call record #%lld not found.", (long long)id);
         return strdup(buf);
      }
      if (rc != PHONE_DB_SUCCESS) {
         clear_pending_delete();
         return strdup(TOOL_RESULT_ERROR_MARK "Error: database error looking up call record.");
      }

      char date[32];
      format_short_date(match.timestamp, date, sizeof(date));
      const char *dir = match.direction == PHONE_DIR_INCOMING ? "in" : "out";
      const char *name = match.contact_name[0] ? match.contact_name : match.number;

      arm_pending_delete(user_id, "call", id, NULL, 0, 1);

      snprintf(buf, sizeof(buf),
               "About to delete call #%lld [%s %s, %s, %ds]. Say 'confirm' to delete.",
               (long long)id, dir, name, date, match.duration_sec);
      return strdup(buf);
   }

   /* older_than_days */
   int days = json_object_get_int(older_obj);
   if (days <= 0) {
      clear_pending_delete();
      return strdup(TOOL_RESULT_ERROR_MARK "Error: 'older_than_days' must be a positive integer.");
   }
   time_t cutoff = time(NULL) - (time_t)days * 86400;
   int match_count = 0;
   if (phone_db_call_log_count_older_than(user_id, cutoff, &match_count) != PHONE_DB_SUCCESS) {
      clear_pending_delete();
      return strdup(TOOL_RESULT_ERROR_MARK "Error: database error counting older calls.");
   }
   if (match_count == 0) {
      clear_pending_delete();
      snprintf(buf, sizeof(buf), "No call records older than %d days.", days);
      return strdup(buf);
   }

   arm_pending_delete(user_id, "call", -1, NULL, cutoff, match_count);

   char cutoff_date[32];
   format_short_date(cutoff, cutoff_date, sizeof(cutoff_date));
   snprintf(buf, sizeof(buf),
            "About to delete %d call record(s) older than %d days (before %s). "
            "Say 'confirm' to delete.",
            match_count, days, cutoff_date);
   return strdup(buf);
}

static char *handle_confirm_delete_call(int user_id) {
   char *err = validate_pending_delete("call", user_id);
   if (err)
      return err;

   if (!check_delete_rate_limit(user_id)) {
      OLOG_WARNING("phone_tool: user=%d delete rate-limited (>%d/hour)", user_id,
                   s_config.delete_rate_limit_per_hour);
      clear_pending_delete();
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: too many deletions in the last hour. Try again later.");
   }

   /* Snapshot pending state under the mutex. */
   int64_t snap_id;
   time_t snap_cutoff;
   pthread_mutex_lock(&s_phone_tool_mutex);
   snap_id = s_pending_delete_id;
   snap_cutoff = s_pending_delete_cutoff;
   pthread_mutex_unlock(&s_phone_tool_mutex);

   char buf[256];
   int deleted = 0;
   int rc;

   if (snap_id >= 0) {
      rc = phone_db_call_log_delete(user_id, snap_id);
      if (rc == PHONE_DB_SUCCESS) {
         deleted = 1;
         OLOG_INFO("phone_tool: user=%d deleted call id=%lld", user_id, (long long)snap_id);
         snprintf(buf, sizeof(buf), "Deleted 1 call record.");
      } else if (rc == PHONE_DB_NOT_FOUND) {
         snprintf(buf, sizeof(buf), "Call record #%lld no longer exists.", (long long)snap_id);
      } else {
         snprintf(buf, sizeof(buf), "Deletion failed: database error.");
      }
   } else if (snap_cutoff > 0) {
      rc = phone_db_call_log_delete_older_than(user_id, snap_cutoff, &deleted);
      if (rc == PHONE_DB_SUCCESS) {
         OLOG_INFO("phone_tool: user=%d deleted %d calls older than %lld", user_id, deleted,
                   (long long)snap_cutoff);
         snprintf(buf, sizeof(buf), "Deleted %d call record(s).", deleted);
      } else {
         snprintf(buf, sizeof(buf), "Deletion failed: database error.");
      }
   } else {
      snprintf(buf, sizeof(buf), "Error: pending state is inconsistent.");
   }

   clear_pending_delete();
   return strdup(buf);
}

static char *handle_call_log(struct json_object *details, int user_id) {
   int count = json_get_int(details, "count", 10);
   if (count < 1) {
      count = 10;
   }
   if (count > 20) {
      count = 20;
   }

   phone_call_log_t entries[20];
   int actual = 0;
   phone_db_call_log_recent(user_id, entries, count, &actual);

   char *buf = malloc(RESULT_BUF_SIZE);
   if (!buf) {
      return strdup(TOOL_RESULT_ERROR_MARK "Error: memory allocation failed");
   }

   int pos = 0;
   if (actual <= 0) {
      pos += snprintf(buf, RESULT_BUF_SIZE, "No call history.");
   } else {
      pos += snprintf(buf, RESULT_BUF_SIZE, "Recent calls (%d):\n", actual);
      for (int i = 0; i < actual && pos < RESULT_BUF_SIZE - 256; i++) {
         const char *dir = entries[i].direction == PHONE_DIR_INCOMING ? "Incoming" : "Outgoing";
         /* Show both name and number so the LLM can match either. */
         char display[96];
         if (entries[i].contact_name[0]) {
            snprintf(display, sizeof(display), "%s (%s)", entries[i].contact_name,
                     entries[i].number);
         } else {
            snprintf(display, sizeof(display), "%s", entries[i].number);
         }
         const char *status_str[] = { "answered", "missed", "rejected", "failed" };
         const char *status = (entries[i].status >= 0 && entries[i].status <= 3)
                                  ? status_str[entries[i].status]
                                  : "unknown";

         /* Include DB id so the LLM can target delete_call {id: N}. */
         pos += snprintf(buf + pos, RESULT_BUF_SIZE - pos, "\n%d. [id=%lld] %s %s — %s (%ds)\n",
                         i + 1, (long long)entries[i].id, dir, display, status,
                         entries[i].duration_sec);
      }
   }

   return buf;
}

static char *handle_sms_log(struct json_object *details, int user_id) {
   int count = json_get_int(details, "count", 10);
   if (count < 1) {
      count = 10;
   }
   /* Capped at 10 resident rows to keep stack frame bounded after body[2048] bump. */
   if (count > 10) {
      count = 10;
   }

   phone_sms_log_t entries[10];
   int actual = 0;
   phone_db_sms_log_recent(user_id, entries, count, &actual);

   char *buf = malloc(RESULT_BUF_SIZE);
   if (!buf) {
      return strdup(TOOL_RESULT_ERROR_MARK "Error: memory allocation failed");
   }

   int pos = 0;
   if (actual <= 0) {
      pos += snprintf(buf, RESULT_BUF_SIZE, "No text message history.");
   } else {
      pos += snprintf(buf, RESULT_BUF_SIZE, "Recent text messages (%d):\n", actual);
      for (int i = 0; i < actual && pos < RESULT_BUF_SIZE - 512; i++) {
         const char *dir = entries[i].direction == PHONE_DIR_INCOMING ? "From" : "To";
         /* Show both name and number so the LLM can match either. */
         char display[96];
         if (entries[i].contact_name[0]) {
            snprintf(display, sizeof(display), "%s (%s)", entries[i].contact_name,
                     entries[i].number);
         } else {
            snprintf(display, sizeof(display), "%s", entries[i].number);
         }

         /* Truncate body for log display */
         char preview[200];
         size_t blen = strlen(entries[i].body);
         if (blen > 150) {
            memcpy(preview, entries[i].body, 150);
            preview[150] = '\0';
            strncat(preview, "...", sizeof(preview) - strlen(preview) - 1);
         } else {
            memcpy(preview, entries[i].body, blen + 1);
         }

         /* Include DB id so the LLM can target delete_sms {id: N}. */
         pos += snprintf(buf + pos, RESULT_BUF_SIZE - pos, "\n%d. [id=%lld] %s: %s\n   %s\n", i + 1,
                         (long long)entries[i].id, dir, display, preview);
      }
   }

   return buf;
}

static char *handle_status(void) {
   phone_state_t state = phone_service_get_state();
   const char *state_str[] = { "idle", "dialing", "ringing (incoming)", "active call",
                               "answering" };
   const char *st = (state >= 0 && state <= PHONE_STATE_ANSWERING) ? state_str[state] : "unknown";

   char buf[256];
   snprintf(buf, sizeof(buf), "Phone status: %s. Modem: %s.", st,
            phone_service_available() ? "online" : "offline");
   return strdup(buf);
}

/* =============================================================================
 * Main Callback
 * ============================================================================= */

static char *phone_tool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1;

   int user_id = tool_get_current_user_id();
   struct json_object *details = NULL;
   if (value && value[0]) {
      details = json_tokener_parse(value);
   }

   char *result = NULL;

   if (strcmp(action, "call") == 0) {
      result = handle_call(details, user_id);
   } else if (strcmp(action, "confirm_call") == 0) {
      result = handle_confirm_call(user_id);
   } else if (strcmp(action, "answer") == 0) {
      result = handle_answer(user_id);
   } else if (strcmp(action, "hang_up") == 0) {
      result = handle_hang_up(user_id);
   } else if (strcmp(action, "send_sms") == 0) {
      result = handle_send_sms(details, user_id);
   } else if (strcmp(action, "confirm_sms") == 0) {
      result = handle_confirm_sms(user_id);
   } else if (strcmp(action, "read_sms") == 0) {
      result = handle_read_sms(user_id);
   } else if (strcmp(action, "call_log") == 0) {
      result = handle_call_log(details, user_id);
   } else if (strcmp(action, "sms_log") == 0) {
      result = handle_sms_log(details, user_id);
   } else if (strcmp(action, "delete_sms") == 0) {
      result = handle_delete_sms(details, user_id);
   } else if (strcmp(action, "confirm_delete_sms") == 0) {
      result = handle_confirm_delete_sms(user_id);
   } else if (strcmp(action, "delete_call") == 0) {
      result = handle_delete_call(details, user_id);
   } else if (strcmp(action, "confirm_delete_call") == 0) {
      result = handle_confirm_delete_call(user_id);
   } else if (strcmp(action, "status") == 0) {
      result = handle_status();
   } else {
      char buf[320];
      snprintf(buf, sizeof(buf),
               "Error: unknown action '%s'. Valid: call, confirm_call, answer, hang_up, "
               "send_sms, confirm_sms, read_sms, call_log, sms_log, delete_sms, "
               "confirm_delete_sms, delete_call, confirm_delete_call, status",
               action);
      result = strdup(buf);
   }

   if (details) {
      json_object_put(details);
   }
   return result;
}

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

static int phone_tool_init(void) {
   int rc = phone_service_init();
   /* Forward parsed [phone] call-audio config (or defaults if the section had no
    * audio keys) so phone_service can build the bridge config on each call. */
   if (!s_audio_parsed) {
      s_audio = phone_audio_config_default();
      s_audio_parsed = true;
   }
   phone_service_set_audio_config(&s_audio);
   return rc;
}

static void phone_tool_cleanup(void) {
   phone_service_shutdown();
}

static bool phone_tool_available(void) {
   return s_config.enabled && phone_service_available();
}

/* =============================================================================
 * Config Parser
 * ============================================================================= */

static void phone_tool_config_parse(toml_table_t *table, void *config) {
   phone_tool_config_t *cfg = (phone_tool_config_t *)config;

   if (!table)
      return;

   toml_datum_t enabled = toml_bool_in(table, "enabled");
   if (enabled.ok)
      cfg->enabled = enabled.u.b;

   toml_datum_t confirm = toml_bool_in(table, "confirm_outbound");
   if (confirm.ok)
      cfg->confirm_outbound = confirm.u.b;

   toml_datum_t warn = toml_bool_in(table, "warn_on_multi_segment");
   if (warn.ok)
      cfg->warn_on_multi_segment = warn.u.b;

   toml_datum_t dlim = toml_int_in(table, "delete_rate_limit_per_hour");
   if (dlim.ok)
      cfg->delete_rate_limit_per_hour = (int)dlim.u.i;

   /* Call-audio block — one symmetric parse/write unit (see phone_audio_config.h).
    * Guarded because phone_tool_update_config (WebUI thread) writes s_audio too. */
   pthread_mutex_lock(&s_phone_tool_mutex);
   s_audio = phone_audio_config_default();
   phone_audio_config_parse(table, &s_audio);
   s_audio_parsed = true;
   pthread_mutex_unlock(&s_phone_tool_mutex);
}

/* Persist the [phone] section.  Registered as .config_writer so a WebUI settings
 * save (which truncates + rewrites dawn.toml via config_write_toml) does not drop
 * this section — a tool with a parser but no writer is silently clobbered.
 *
 * Two sources (parse<->write symmetry, or a key clobbers on every save): the four
 * base keys come from @p config (the registered phone_tool_config_t, HA-style);
 * the audio block comes from the module-static s_audio via phone_audio_config_write.
 * NOTE: hand-added but unparsed [phone] keys (user_id, retention, rate_limit_*)
 * are documented-but-unwired and NOT round-tripped (separate gap). */
static void phone_tool_config_write(void *fp, const void *config) {
   const phone_tool_config_t *cfg = (const phone_tool_config_t *)config;
   FILE *f = (FILE *)fp;

   fprintf(f, "enabled = %s\n", cfg->enabled ? "true" : "false");
   fprintf(f, "confirm_outbound = %s\n", cfg->confirm_outbound ? "true" : "false");
   fprintf(f, "warn_on_multi_segment = %s\n", cfg->warn_on_multi_segment ? "true" : "false");
   fprintf(f, "delete_rate_limit_per_hour = %d\n", cfg->delete_rate_limit_per_hour);

   /* Snapshot the audio config under the lock (update_config may write it on
    * another thread).  This runs under the registry write lock, so the order is
    * registry -> phone_tool; nothing takes phone_tool -> registry. */
   phone_audio_config_t snap;
   pthread_mutex_lock(&s_phone_tool_mutex);
   snap = s_audio;
   pthread_mutex_unlock(&s_phone_tool_mutex);
   phone_audio_config_write(f, &snap);
}

void phone_tool_update_config(const phone_audio_config_t *audio) {
   if (!audio) {
      return;
   }
   /* Clamp before storing so the persisted + echoed-back value matches what the
    * APM actually applies (e.g. a fixed gain of 60 -> 49). */
   phone_audio_config_t cfg = *audio;
   phone_apm_clamp_config(&cfg.uplink);
   phone_apm_clamp_config(&cfg.downlink);
   /* Soft-limiter gain isn't an APM knob; clamp it here (0 = bridge default). */
   if (cfg.downlink_gain < 0.0f) {
      cfg.downlink_gain = 0.0f;
   } else if (cfg.downlink_gain > PHONE_DOWNLINK_GAIN_MAX) {
      cfg.downlink_gain = PHONE_DOWNLINK_GAIN_MAX;
   }
   phone_audio_config_t snap;
   pthread_mutex_lock(&s_phone_tool_mutex);
   s_audio = cfg;
   s_audio_parsed = true;
   snap = s_audio;
   pthread_mutex_unlock(&s_phone_tool_mutex);
   /* Forward to the service, which persists it for the next call and live-applies
    * to a call in progress. */
   phone_service_set_audio_config(&snap);
}

void phone_tool_get_audio_config(phone_audio_config_t *out) {
   if (!out) {
      return;
   }
   pthread_mutex_lock(&s_phone_tool_mutex);
   *out = s_audio_parsed ? s_audio : phone_audio_config_default();
   pthread_mutex_unlock(&s_phone_tool_mutex);
}

/* =============================================================================
 * Tool Registration
 * ============================================================================= */

static const treg_param_t phone_params[] = {
   {
       .name = "action",
       .description = "Phone action: 'call' (initiate a phone call), "
                      "'confirm_call' (confirm a pending call), "
                      "'answer' (answer an incoming call), "
                      "'hang_up' (end the current call), "
                      "'send_sms' (compose an SMS for confirmation), "
                      "'confirm_sms' (send confirmed SMS), "
                      "'read_sms' (read unread text messages), "
                      "'call_log' (view recent call history), "
                      "'sms_log' (view recent text messages), "
                      "'delete_sms' (preview an SMS deletion), "
                      "'confirm_delete_sms' (confirm the pending SMS deletion), "
                      "'delete_call' (preview a call-record deletion), "
                      "'confirm_delete_call' (confirm the pending call deletion), "
                      "'status' (check phone/modem status)",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "call", "confirm_call", "answer", "hang_up", "send_sms", "confirm_sms",
                        "read_sms", "call_log", "sms_log", "delete_sms", "confirm_delete_sms",
                        "delete_call", "confirm_delete_call", "status" },
       .enum_count = 14,
   },
   {
       .name = "details",
       .description =
           "JSON object with action-specific fields (pass as JSON-encoded string).\n"
           "call {target}, confirm_call {}, answer {}, hang_up {},\n"
           "send_sms {target, body}, confirm_sms {}, read_sms {},\n"
           "call_log {count?} (default 10; rows show [id=N] for delete_call),\n"
           "sms_log {count?} (default 10; rows show [id=N] for delete_sms),\n"
           "delete_sms {id?, number?, older_than_days?} (exactly one; returns preview — "
           "call confirm_delete_sms on the NEXT user turn after they agree),\n"
           "confirm_delete_sms {} (expires 120s after preview),\n"
           "delete_call {id?, older_than_days?} (exactly one; returns preview),\n"
           "confirm_delete_call {},\n"
           "status {}.\n"
           "  target: E.164 phone number ('+16785551212') OR a contact name resolvable via "
           "the contacts system. Bare digits ('6785551212') are also accepted and normalized. "
           "The tool checks contacts itself before dialing: pass the name the user said and let "
           "the tool resolve it — you do NOT need the number. If the name is ambiguous or only a "
           "near-miss (e.g. a mis-heard surname), the tool returns candidates ('did you mean…') "
           "instead of dialing; relay them and pass the user's choice back on the next turn.\n"
           "  body: SMS message text. Concatenated SMS (multi-segment) is supported "
           "automatically; concise messages save segments.\n"
           "  IMPORTANT: after 'call' or 'send_sms' returns a preview, the LLM MUST stop "
           "and wait for the user's next turn before calling confirm_call/confirm_sms. "
           "Do not auto-confirm in the same turn.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

static const tool_metadata_t phone_metadata = {
   .name = "phone",
   .device_string = "phone",
   .topic = "dawn",
   .aliases = { "telephone", "call", "sms", "text" },
   .alias_count = 4,

   .description =
       "Make phone calls and send/receive text messages via the cellular modem. "
       "Use 'call' to initiate a call (by contact name or phone number). "
       "Use 'answer' to answer an incoming call. "
       "Use 'hang_up' to end the current call. "
       "Use 'send_sms' to compose a text message (reads back for confirmation). "
       "Use 'read_sms' to check unread text messages. "
       "Use 'call_log' or 'sms_log' to view recent history. "
       "Use 'delete_sms' or 'delete_call' to remove records — these return a preview. "
       "Call confirm_delete_sms/confirm_delete_call on the next turn if the user agrees. "
       "Pending state expires after 120 seconds. "
       "To delete multiple messages from one sender, use delete_sms with 'number' "
       "(e.g. {number: '+16786432695'}) — it's one call that deletes them all. "
       "Do NOT loop over individual ids; do NOT wrap deletes in execute_plan — "
       "the phone tool is blocked inside plans on purpose. "
       "Use 'status' to check phone and modem status. "
       "Calls and SMS require confirmation before executing (say 'confirm' after review). "
       "The 'target' field can be a contact name (resolved via contacts) or a phone number.",
   .params = phone_params,
   .param_count = 2,

   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_NETWORK | TOOL_CAP_DANGEROUS,
   .skip_followup = false,
   .default_local = true,
   .default_remote = true,

   .config = &s_config,
   .config_size = sizeof(s_config),
   .config_parser = phone_tool_config_parse,
   .config_writer = phone_tool_config_write,
   .config_section = "phone",

   .is_available = phone_tool_available,
   .init = phone_tool_init,
   .cleanup = phone_tool_cleanup,
   .callback = phone_tool_callback,
};

int phone_tool_register(void) {
   return tool_registry_register(&phone_metadata);
}
