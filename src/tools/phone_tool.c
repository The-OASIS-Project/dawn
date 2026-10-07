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

#include "core/pending_slots.h"
#include "core/tool_call_challenge.h"
#include "core/tool_call_policy.h"
#include "core/turn_origin.h"
#include "logging.h"
#include "toml.h"
#include "tools/phone_audio_config.h"
#include "tools/phone_contacts.h"
#include "tools/phone_db.h"
#include "tools/phone_service.h"
#include "tools/tool_pending.h"
#include "tools/tool_registry.h"
#include "utils/string_utils.h"

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
 *   - s_pending[] (calls, texts and deletes awaiting their confirm)
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

/* What awaits the user's confirm: a call, a text, or a delete of SMS or call
 * records, one of each per session (core/pending_slots.h).  The confirm must
 * come from the session that staged it, in the user's next turn
 * (turn_origin_check).  The TTL bounds the replay window if the user walks
 * away after the preview. */
#define PHONE_TOOL_PENDING_TTL_SEC 300 /* as long as a reply code, when one confirms it */
#define PHONE_PENDING_MAX 32           /* 4 kinds for each of 8 sessions */

enum {
   PHONE_PENDING_CALL = 1,
   PHONE_PENDING_SMS,
   PHONE_PENDING_DELETE_SMS,
   PHONE_PENDING_DELETE_CALL,
};

typedef struct {
   pending_slot_t hdr;
   char number[24];   /* call / text: the resolved number; delete: by-number criterion */
   char name[64];     /* call / text: the contact's name ("" for a number) */
   char body[1024];   /* text: what is sent */
   int64_t id;        /* delete by id (-1 otherwise) */
   time_t cutoff;     /* delete older than (0 otherwise) */
   int preview_count; /* delete: the count shown to the user */
} phone_pending_t;

PENDING_ITEM_CHECK(phone_pending_t);
static phone_pending_t s_pending[PHONE_PENDING_MAX];
PENDING_ARRAY_CHECK(s_pending);
static const pending_slots_t s_pending_slots = PENDING_SLOTS_TABLE(s_pending,
                                                                   PHONE_TOOL_PENDING_TTL_SEC);

/* Stage (replace) this session's pending item of @p kind.  NULL when every
 * slot holds another session's live item.  Caller holds s_phone_tool_mutex. */
static phone_pending_t *stage_pending_locked(const turn_origin_t *origin,
                                             int user_id,
                                             int kind,
                                             pending_stage_rc_t *rc) {
   return (phone_pending_t *)pending_slots_stage(&s_pending_slots, origin, user_id, kind,
                                                 pending_slots_now(), rc);
}

/* Drop this session's pending item of @p kind (nothing armed). */
static void drop_pending(const turn_origin_t *origin, int user_id, int kind) {
   pthread_mutex_lock(&s_phone_tool_mutex);
   pending_slots_drop(&s_pending_slots, origin, user_id, kind);
   pthread_mutex_unlock(&s_phone_tool_mutex);
}

/* A preview's closing line: the id the confirm must name. */
static char *with_pending_id(const char *preview, const char *confirm, uint32_t item_id) {
   static const char fmt[] = "%s\nIf the user says yes in their reply, call %s with pending_id "
                             "%u (this item only).";
   const size_t len = strlen(preview) + strlen(confirm) + sizeof(fmt) + 16;
   char *buf = malloc(len);
   if (buf) {
      snprintf(buf, len, fmt, preview, confirm, (unsigned)item_id);
   }
   return buf;
}

/* The pending_id a confirm names (0 when missing or not a number). */
static uint32_t pending_id_arg(struct json_object *details) {
   struct json_object *v = NULL;
   if (!details || !json_object_object_get_ex(details, "pending_id", &v) || !v) {
      return 0;
   }
   const int64_t id = json_object_get_int64(v);
   return (id > 0 && id <= UINT32_MAX) ? (uint32_t)id : 0;
}

/* Take this session's pending item of @p kind, the one the confirm names, for
 * its confirm: copied to @p out and cleared.  NULL on success, else the error
 * to return (one named wrong, or confirmed in the wrong turn, stays). */
static char *take_pending(const turn_origin_t *origin,
                          int user_id,
                          int kind,
                          uint32_t item_id,
                          const char *what,
                          phone_pending_t *out) {
   if (item_id == 0) {
      return tool_pending_missing_id(what);
   }
   turn_origin_rc_t orc = TURN_ORIGIN_OK;
   pthread_mutex_lock(&s_phone_tool_mutex);
   const pending_find_rc_t rc = pending_slots_take(&s_pending_slots, origin, user_id, kind, item_id,
                                                   pending_slots_now(), out, sizeof(*out), &orc);
   pthread_mutex_unlock(&s_phone_tool_mutex);
   if (rc == PENDING_FOUND) {
      return NULL;
   }
   if (rc == PENDING_NOT_NOW) {
      OLOG_WARNING("phone_tool: confirm of %s refused (%s)", what, turn_origin_refusal(orc));
   }
   return tool_pending_take_refusal(rc, orc, what);
}

/* Forward decl — used in the delete-preview handlers before the definition. */
static uint32_t arm_pending_delete(const turn_origin_t *origin,
                                   int user_id,
                                   int kind,
                                   int64_t id,
                                   const char *number,
                                   time_t cutoff,
                                   int preview_count,
                                   pending_stage_rc_t *rc);

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

/* Whether a call or text previews (and waits for its confirm) rather than
 * going at once: as the call was classified at the gate, so a settings save
 * between the two can't turn a preview into a dial.  confirm_outbound outside
 * the gate. */
static bool phone_previews(void) {
   tool_action_kind_t kind = TOOL_KIND_ACT;
   if (tool_call_policy_decided(&kind)) {
      return kind == TOOL_KIND_PREPARE;
   }
   return s_config.confirm_outbound;
}

/* A call or text's recipient for this turn: NULL with @p rez ready (a number,
 * one contact, or one to confirm: *confirm), else the question to ask. */
static char *resolve_recipient(const char *target,
                               int user_id,
                               const turn_origin_t *origin,
                               int kind,
                               phone_resolve_t *rez,
                               bool *confirm) {
   phone_contacts_resolve(user_id, target, rez);
   /* Approved by the user's reply code: the code's text named them. */
   if (rez->kind == PHONE_RESOLVE_CONFIRM && session_call_code_redeemed()) {
      rez->kind = PHONE_RESOLVE_UNIQUE;
   }
   *confirm = rez->kind == PHONE_RESOLVE_CONFIRM;
   if (rez->kind == PHONE_RESOLVE_NUMBER || rez->kind == PHONE_RESOLVE_UNIQUE || *confirm) {
      return NULL;
   }
   drop_pending(origin, user_id, kind); /* disambiguate first */
   char buf[sizeof(rez->question)];
   phone_contacts_format_disambiguation(target, rez, buf, sizeof(buf));
   return strdup(buf);
}

static char *handle_call(struct json_object *details, int user_id, const turn_origin_t *origin) {
   const char *target = json_get_str(details, "target");
   if (!target || target[0] == '\0') {
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: 'target' is required (phone number or contact name)");
   }

   phone_resolve_t rez;
   bool confirm = false;
   char *ask = resolve_recipient(target, user_id, origin, PHONE_PENDING_CALL, &rez, &confirm);
   if (ask) {
      return ask;
   }
   /* A preview when previews are on, and always for a recipient to confirm:
    * the user's yes to it is the confirmation. */
   if (phone_previews() || confirm) {
      /* Arm the resolved number so confirm dials exactly what was previewed. */
      pthread_mutex_lock(&s_phone_tool_mutex);
      pending_stage_rc_t src = PENDING_STAGED;
      phone_pending_t *p = stage_pending_locked(origin, user_id, PHONE_PENDING_CALL, &src);
      const uint32_t pending_id = p ? p->hdr.item_id : 0;
      if (p) {
         snprintf(p->number, sizeof(p->number), "%s", rez.number);
         snprintf(p->name, sizeof(p->name), "%s", rez.name);
      }
      pthread_mutex_unlock(&s_phone_tool_mutex);
      if (pending_id == 0) {
         return tool_pending_stage_refusal(src, "call");
      }
      /* Room for the whole question the resolver asks. */
      char buf[sizeof(rez.name) + sizeof(rez.number) + sizeof(rez.question) + 32];
      if (rez.name[0]) {
         snprintf(buf, sizeof(buf), "About to call %s at %s. %s", rez.name, rez.number,
                  confirm ? rez.question : "Say 'confirm' to proceed.");
      } else {
         snprintf(buf, sizeof(buf), "About to call %s. %s", rez.number,
                  confirm ? rez.question : "Say 'confirm' to proceed.");
      }
      return with_pending_id(buf, "confirm_call", pending_id);
   }

   /* No confirmation: dial the number resolved above. */
   char result[RESULT_BUF_SIZE];
   int rc = phone_service_call(user_id, rez.number, result, sizeof(result));
   return phone_service_result_dup(rc, result);
}

static char *handle_confirm_call(struct json_object *details,
                                 int user_id,
                                 const turn_origin_t *origin) {
   phone_pending_t p;
   char *err = take_pending(origin, user_id, PHONE_PENDING_CALL, pending_id_arg(details), "call",
                            &p);
   if (err) {
      return err;
   }
   char result[RESULT_BUF_SIZE];
   int rc = phone_service_call(user_id, p.number, result, sizeof(result));
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

static char *handle_send_sms(struct json_object *details,
                             int user_id,
                             const turn_origin_t *origin) {
   const char *target = json_get_str(details, "target");
   const char *body = json_get_str(details, "body");

   if (!target || target[0] == '\0') {
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: 'target' is required (phone number or contact name)");
   }
   if (!body || body[0] == '\0') {
      return strdup(TOOL_RESULT_ERROR_MARK "Error: 'body' is required (message text)");
   }
   phone_resolve_t rez;
   bool confirm = false;
   char *ask = resolve_recipient(target, user_id, origin, PHONE_PENDING_SMS, &rez, &confirm);
   if (ask) {
      return ask;
   }
   /* A preview when previews are on, and always for a recipient to confirm. */
   const bool previews = phone_previews() || confirm;
   if (previews && strlen(body) >= sizeof(((phone_pending_t *)0)->body)) {
      /* Stored cut short, the text sent wouldn't be the one previewed. */
      return strdup(TOOL_RESULT_ERROR_MARK "Error: the message is too long to preview; keep it "
                                           "under about 1000 bytes (fewer with emoji or "
                                           "non-Latin text), or send it as two messages.");
   }

   if (previews) {
      /* Arm the resolved number so confirm sends exactly what was previewed. */
      pthread_mutex_lock(&s_phone_tool_mutex);
      pending_stage_rc_t src = PENDING_STAGED;
      phone_pending_t *p = stage_pending_locked(origin, user_id, PHONE_PENDING_SMS, &src);
      const uint32_t pending_id = p ? p->hdr.item_id : 0;
      if (p) {
         snprintf(p->number, sizeof(p->number), "%s", rez.number);
         snprintf(p->name, sizeof(p->name), "%s", rez.name);
         snprintf(p->body, sizeof(p->body), "%s", body);
      }
      pthread_mutex_unlock(&s_phone_tool_mutex);
      if (pending_id == 0) {
         return tool_pending_stage_refusal(src, "text");
      }

      char recipient[96];
      if (rez.name[0]) {
         snprintf(recipient, sizeof(recipient), "%s (%s)", rez.name, rez.number);
      } else {
         snprintf(recipient, sizeof(recipient), "%s", rez.number);
      }
      int segs = s_config.warn_on_multi_segment ? estimate_sms_segments(body) : 1;
      /* Room for the whole body: the preview shows exactly what is sent. */
      char buf[sizeof(((phone_pending_t *)0)->body) + 768];
      char multi[64] = "";
      if (segs > 1) {
         snprintf(multi, sizeof(multi), " This will send as %d text messages.", segs);
      }
      snprintf(buf, sizeof(buf), "About to send SMS to %s: \"%s\".%s %s", recipient, body, multi,
               confirm ? rez.question : "Say 'confirm' to send.");
      return with_pending_id(buf, "confirm_sms", pending_id);
   }

   /* No confirmation: text the number resolved above. */
   char result[RESULT_BUF_SIZE];
   int rc = phone_service_send_sms(user_id, rez.number, body, result, sizeof(result));
   return phone_service_result_dup(rc, result);
}

static char *handle_confirm_sms(struct json_object *details,
                                int user_id,
                                const turn_origin_t *origin) {
   phone_pending_t p;
   char *err = take_pending(origin, user_id, PHONE_PENDING_SMS, pending_id_arg(details), "text",
                            &p);
   if (err) {
      return err;
   }
   char result[RESULT_BUF_SIZE];
   int rc = phone_service_send_sms(user_id, p.number, p.body, result, sizeof(result));
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
 * Delete Handlers — two-step confirmation.
 *
 * Flow:
 *   1. LLM calls delete_sms / delete_call with {id} OR {number} OR
 *      {older_than_days}. Exactly one criterion required.
 *   2. Handler looks up match count (0 → immediate error, don't arm pending).
 *   3. Preview is returned; this session's pending deletion of that kind is
 *      armed with the criteria (stage_pending_locked).
 *   4. LLM calls confirm_delete_sms / confirm_delete_call in the user's next
 *      turn.  Handler takes the session's pending item (TTL + turn origin,
 *      take_pending), checks the hourly rate limit, executes DELETE.
 * ============================================================================= */

/* Convert timestamp → compact yyyy-mm-dd string for previews. */
static void format_short_date(time_t t, char *out, size_t out_size) {
   struct tm tm_buf;
   localtime_r(&t, &tm_buf);
   strftime(out, out_size, "%Y-%m-%d", &tm_buf);
}

static char *handle_delete_sms(struct json_object *details,
                               int user_id,
                               const turn_origin_t *origin) {
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
         drop_pending(origin, user_id, PHONE_PENDING_DELETE_SMS);
         snprintf(buf, sizeof(buf), "Error: SMS record #%lld not found.", (long long)id);
         return strdup(buf);
      }
      if (rc != PHONE_DB_SUCCESS) {
         drop_pending(origin, user_id, PHONE_PENDING_DELETE_SMS);
         return strdup(TOOL_RESULT_ERROR_MARK "Error: database error looking up SMS record.");
      }

      /* Truncate body preview */
      char preview[128] = "";
      size_t blen = strlen(match.body);
      if (blen > 100) {
         memcpy(preview, match.body, 100);
         memcpy(preview + 100, "...", sizeof("...")); /* 3 chars + NUL, fits in [128] */
      } else {
         memcpy(preview, match.body, blen + 1);
      }
      char date[32];
      format_short_date(match.timestamp, date, sizeof(date));
      const char *dir = match.direction == PHONE_DIR_INCOMING ? "in" : "out";
      const char *name = match.contact_name[0] ? match.contact_name : match.number;

      pending_stage_rc_t src = PENDING_STAGED;
      const uint32_t pending_id = arm_pending_delete(origin, user_id, PHONE_PENDING_DELETE_SMS, id,
                                                     NULL, 0, 1, &src);
      if (pending_id == 0) {
         return tool_pending_stage_refusal(src, "deletion");
      }

      snprintf(buf, sizeof(buf),
               "About to delete SMS #%lld [%s %s, %s]: \"%s\". Say 'confirm' to delete.",
               (long long)id, dir, name, date, preview);
      return with_pending_id(buf, "confirm_delete_sms", pending_id);
   }

   if (num_obj) {
      const char *number = json_object_get_string(num_obj);
      if (!number || !*number) {
         drop_pending(origin, user_id, PHONE_PENDING_DELETE_SMS);
         return strdup(TOOL_RESULT_ERROR_MARK "Error: 'number' must be a non-empty phone number.");
      }
      if (strlen(number) >= sizeof(((phone_pending_t *)0)->number)) {
         /* Stored cut short, the confirm would delete by another string. */
         drop_pending(origin, user_id, PHONE_PENDING_DELETE_SMS);
         return strdup(TOOL_RESULT_ERROR_MARK "Error: 'number' is too long for a phone number.");
      }
      int match_count = 0;
      if (phone_db_sms_log_count_by_number(user_id, number, &match_count) != PHONE_DB_SUCCESS) {
         drop_pending(origin, user_id, PHONE_PENDING_DELETE_SMS);
         return strdup(TOOL_RESULT_ERROR_MARK "Error: database error counting SMS by number.");
      }
      if (match_count == 0) {
         drop_pending(origin, user_id, PHONE_PENDING_DELETE_SMS);
         snprintf(buf, sizeof(buf), "No SMS records match number %s.", number);
         return strdup(buf);
      }

      pending_stage_rc_t src = PENDING_STAGED;
      const uint32_t pending_id = arm_pending_delete(origin, user_id, PHONE_PENDING_DELETE_SMS, -1,
                                                     number, 0, match_count, &src);
      if (pending_id == 0) {
         return tool_pending_stage_refusal(src, "deletion");
      }

      snprintf(buf, sizeof(buf),
               "About to delete %d SMS message(s) matching number %s. "
               "Say 'confirm' to delete all %d.",
               match_count, number, match_count);
      return with_pending_id(buf, "confirm_delete_sms", pending_id);
   }

   /* older_than_days */
   int days = json_object_get_int(older_obj);
   if (days <= 0) {
      drop_pending(origin, user_id, PHONE_PENDING_DELETE_SMS);
      return strdup(TOOL_RESULT_ERROR_MARK "Error: 'older_than_days' must be a positive integer.");
   }
   time_t cutoff = time(NULL) - (time_t)days * 86400;

   int match_count = 0;
   if (phone_db_sms_log_count_older_than(user_id, cutoff, &match_count) != PHONE_DB_SUCCESS) {
      drop_pending(origin, user_id, PHONE_PENDING_DELETE_SMS);
      return strdup(TOOL_RESULT_ERROR_MARK "Error: database error counting older SMS.");
   }
   if (match_count == 0) {
      drop_pending(origin, user_id, PHONE_PENDING_DELETE_SMS);
      snprintf(buf, sizeof(buf), "No SMS records older than %d days.", days);
      return strdup(buf);
   }

   pending_stage_rc_t src = PENDING_STAGED;
   const uint32_t pending_id = arm_pending_delete(origin, user_id, PHONE_PENDING_DELETE_SMS, -1,
                                                  NULL, cutoff, match_count, &src);
   if (pending_id == 0) {
      return tool_pending_stage_refusal(src, "deletion");
   }

   char cutoff_date[32];
   format_short_date(cutoff, cutoff_date, sizeof(cutoff_date));
   snprintf(buf, sizeof(buf),
            "About to delete %d SMS record(s) older than %d days (before %s). "
            "Say 'confirm' to delete.",
            match_count, days, cutoff_date);
   return with_pending_id(buf, "confirm_delete_sms", pending_id);
}

/* Arm this session's pending deletion of @p kind.  Its pending_id, or 0 when
 * every slot holds another session's live item. */
static uint32_t arm_pending_delete(const turn_origin_t *origin,
                                   int user_id,
                                   int kind,
                                   int64_t id,
                                   const char *number,
                                   time_t cutoff,
                                   int preview_count,
                                   pending_stage_rc_t *rc) {
   pthread_mutex_lock(&s_phone_tool_mutex);
   phone_pending_t *p = stage_pending_locked(origin, user_id, kind, rc);
   const uint32_t pending_id = p ? p->hdr.item_id : 0;
   if (p) {
      p->id = id;
      snprintf(p->number, sizeof(p->number), "%s", number ? number : "");
      p->cutoff = cutoff;
      p->preview_count = preview_count;
   }
   pthread_mutex_unlock(&s_phone_tool_mutex);
   return pending_id;
}

static char *handle_confirm_delete_sms(struct json_object *details,
                                       int user_id,
                                       const turn_origin_t *origin) {
   phone_pending_t p;
   char *err = take_pending(origin, user_id, PHONE_PENDING_DELETE_SMS, pending_id_arg(details),
                            "SMS deletion", &p);
   if (err)
      return err;

   if (!check_delete_rate_limit(user_id)) {
      OLOG_WARNING("phone_tool: user=%d delete rate-limited (>%d/hour)", user_id,
                   s_config.delete_rate_limit_per_hour);
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: too many deletions in the last hour. Try again later.");
   }

   /* The staged criteria, taken from the slot (a later arm can't alter them). */
   const int64_t snap_id = p.id;
   const char *snap_number = p.number;
   const time_t snap_cutoff = p.cutoff;

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

   return strdup(buf);
}

static char *handle_delete_call(struct json_object *details,
                                int user_id,
                                const turn_origin_t *origin) {
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
         drop_pending(origin, user_id, PHONE_PENDING_DELETE_CALL);
         snprintf(buf, sizeof(buf), "Error: call record #%lld not found.", (long long)id);
         return strdup(buf);
      }
      if (rc != PHONE_DB_SUCCESS) {
         drop_pending(origin, user_id, PHONE_PENDING_DELETE_CALL);
         return strdup(TOOL_RESULT_ERROR_MARK "Error: database error looking up call record.");
      }

      char date[32];
      format_short_date(match.timestamp, date, sizeof(date));
      const char *dir = match.direction == PHONE_DIR_INCOMING ? "in" : "out";
      const char *name = match.contact_name[0] ? match.contact_name : match.number;

      pending_stage_rc_t src = PENDING_STAGED;
      const uint32_t pending_id = arm_pending_delete(origin, user_id, PHONE_PENDING_DELETE_CALL, id,
                                                     NULL, 0, 1, &src);
      if (pending_id == 0) {
         return tool_pending_stage_refusal(src, "deletion");
      }

      snprintf(buf, sizeof(buf),
               "About to delete call #%lld [%s %s, %s, %ds]. Say 'confirm' to delete.",
               (long long)id, dir, name, date, match.duration_sec);
      return with_pending_id(buf, "confirm_delete_call", pending_id);
   }

   /* older_than_days */
   int days = json_object_get_int(older_obj);
   if (days <= 0) {
      drop_pending(origin, user_id, PHONE_PENDING_DELETE_CALL);
      return strdup(TOOL_RESULT_ERROR_MARK "Error: 'older_than_days' must be a positive integer.");
   }
   time_t cutoff = time(NULL) - (time_t)days * 86400;
   int match_count = 0;
   if (phone_db_call_log_count_older_than(user_id, cutoff, &match_count) != PHONE_DB_SUCCESS) {
      drop_pending(origin, user_id, PHONE_PENDING_DELETE_CALL);
      return strdup(TOOL_RESULT_ERROR_MARK "Error: database error counting older calls.");
   }
   if (match_count == 0) {
      drop_pending(origin, user_id, PHONE_PENDING_DELETE_CALL);
      snprintf(buf, sizeof(buf), "No call records older than %d days.", days);
      return strdup(buf);
   }

   pending_stage_rc_t src = PENDING_STAGED;
   const uint32_t pending_id = arm_pending_delete(origin, user_id, PHONE_PENDING_DELETE_CALL, -1,
                                                  NULL, cutoff, match_count, &src);
   if (pending_id == 0) {
      return tool_pending_stage_refusal(src, "deletion");
   }

   char cutoff_date[32];
   format_short_date(cutoff, cutoff_date, sizeof(cutoff_date));
   snprintf(buf, sizeof(buf),
            "About to delete %d call record(s) older than %d days (before %s). "
            "Say 'confirm' to delete.",
            match_count, days, cutoff_date);
   return with_pending_id(buf, "confirm_delete_call", pending_id);
}

static char *handle_confirm_delete_call(struct json_object *details,
                                        int user_id,
                                        const turn_origin_t *origin) {
   phone_pending_t p;
   char *err = take_pending(origin, user_id, PHONE_PENDING_DELETE_CALL, pending_id_arg(details),
                            "call-record deletion", &p);
   if (err)
      return err;

   if (!check_delete_rate_limit(user_id)) {
      OLOG_WARNING("phone_tool: user=%d delete rate-limited (>%d/hour)", user_id,
                   s_config.delete_rate_limit_per_hour);
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: too many deletions in the last hour. Try again later.");
   }

   /* The staged criteria, taken from the slot (a later arm can't alter them). */
   const int64_t snap_id = p.id;
   const time_t snap_cutoff = p.cutoff;

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
         char preview[200] = "";
         size_t blen = strlen(entries[i].body);
         if (blen > 150) {
            memcpy(preview, entries[i].body, 150);
            memcpy(preview + 150, "...", sizeof("...")); /* 3 chars + NUL, fits in [200] */
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

static const tool_action_kind_entry_t s_phone_action_kinds[] = {
   { "read_sms", TOOL_KIND_READ, NULL },
   { "call_log", TOOL_KIND_READ, NULL },
   { "sms_log", TOOL_KIND_READ, NULL },
   { "status", TOOL_KIND_READ, NULL },
   { "call", TOOL_KIND_PREPARE, "confirm_call" },
   { "send_sms", TOOL_KIND_PREPARE, "confirm_sms" },
   { "delete_sms", TOOL_KIND_PREPARE, "confirm_delete_sms" },
   { "delete_call", TOOL_KIND_PREPARE, "confirm_delete_call" },
   { "confirm_call", TOOL_KIND_ACT, NULL },
   { "confirm_sms", TOOL_KIND_ACT, NULL },
   { "confirm_delete_sms", TOOL_KIND_ACT, NULL },
   { "confirm_delete_call", TOOL_KIND_ACT, NULL },
};

/* The actions that call, answer, hang up, text or delete: every listed action
 * that isn't a read, and answer / hang_up (unlisted, so they act). */
static bool phone_action_acts(const char *action) {
   for (int i = 0; i < TOOL_KIND_COUNT(s_phone_action_kinds); i++) {
      if (strcmp(action, s_phone_action_kinds[i].action) == 0)
         return s_phone_action_kinds[i].kind != TOOL_KIND_READ;
   }
   return strcmp(action, "answer") == 0 || strcmp(action, "hang_up") == 0;
}

static char *phone_tool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1;

   int user_id = tool_get_current_user_id();
   if (user_id <= 0)
      return strdup(TOOL_GUEST_REFUSAL);

   /* What calls, texts or deletes needs the user in a live conversation, with
    * or without confirm_outbound: not a background job, a re-engaged turn or
    * an MQTT message, where the request may come from content the model read.
    * A confirm must come from the session that staged it, in the user's next
    * turn (turn_origin_t). */
   turn_origin_t origin = { 0 };
   if (phone_action_acts(action) && !turn_origin_capture(&origin)) {
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: calls, texts and deletes need the user in a live conversation, and "
                    "this request came from a background job or an automated turn. Tell the "
                    "user what you would do, and let them ask for it.");
   }
   struct json_object *details = NULL;
   if (value && value[0]) {
      details = json_tokener_parse(value);
   }

   char *result = NULL;

   if (strcmp(action, "call") == 0) {
      result = handle_call(details, user_id, &origin);
   } else if (strcmp(action, "confirm_call") == 0) {
      result = handle_confirm_call(details, user_id, &origin);
   } else if (strcmp(action, "answer") == 0) {
      result = handle_answer(user_id);
   } else if (strcmp(action, "hang_up") == 0) {
      result = handle_hang_up(user_id);
   } else if (strcmp(action, "send_sms") == 0) {
      result = handle_send_sms(details, user_id, &origin);
   } else if (strcmp(action, "confirm_sms") == 0) {
      result = handle_confirm_sms(details, user_id, &origin);
   } else if (strcmp(action, "read_sms") == 0) {
      result = handle_read_sms(user_id);
   } else if (strcmp(action, "call_log") == 0) {
      result = handle_call_log(details, user_id);
   } else if (strcmp(action, "sms_log") == 0) {
      result = handle_sms_log(details, user_id);
   } else if (strcmp(action, "delete_sms") == 0) {
      result = handle_delete_sms(details, user_id, &origin);
   } else if (strcmp(action, "confirm_delete_sms") == 0) {
      result = handle_confirm_delete_sms(details, user_id, &origin);
   } else if (strcmp(action, "delete_call") == 0) {
      result = handle_delete_call(details, user_id, &origin);
   } else if (strcmp(action, "confirm_delete_call") == 0) {
      result = handle_confirm_delete_call(details, user_id, &origin);
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
       .name = "arguments",
       .description =
           "JSON object of the action's arguments, passed as a JSON-encoded string.  "
           "Omit entirely for an action that takes no arguments; never fill it with a "
           "description or rationale.\n"
           "call {target}, confirm_call {pending_id}, answer {}, hang_up {},\n"
           "send_sms {target, body}, confirm_sms {pending_id}, read_sms {},\n"
           "call_log {count?} (default 10; rows show [id=N] for delete_call),\n"
           "sms_log {count?} (default 10; rows show [id=N] for delete_sms),\n"
           "delete_sms {id?, number?, older_than_days?} (exactly one; returns preview — "
           "call confirm_delete_sms on the NEXT user turn after they agree),\n"
           "confirm_delete_sms {pending_id} (expires 5 minutes after preview),\n"
           "delete_call {id?, older_than_days?} (exactly one; returns preview),\n"
           "confirm_delete_call {pending_id},\n"
           "status {}.\n"
           "  target: E.164 phone number ('+16785551212') OR a contact name resolvable via "
           "the contacts system. Bare digits ('6785551212') are also accepted and normalized. "
           "The tool checks contacts itself before dialing: pass the name exactly as the user "
           "said it (never a correction or a guess of yours; for a relationship such as 'my "
           "wife', the name of the person you know it means; with the label if the user gave one, "
           "'Bob Smith mobile') and let the tool resolve it — you do NOT need the number, so "
           "don't look the person up first. If the name is ambiguous or only a near-miss (e.g. a "
           "mis-heard surname), the tool returns candidates ('did you mean…') instead of "
           "dialing; relay them and pass the user's choice back on the next turn. If the "
           "preview asks whether it's the right person, ask the user.\n"
           "  body: SMS message text. Concatenated SMS (multi-segment) is supported "
           "automatically; concise messages save segments.\n"
           "  pending_id: the number a preview (call, send_sms, delete_*) returns; its confirm "
           "must name it, and only in the user's reply to that preview.\n"
           "  IMPORTANT: after 'call' or 'send_sms' returns a preview, the LLM MUST stop "
           "and wait for the user's next turn before calling confirm_call/confirm_sms. "
           "Do not auto-confirm in the same turn.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};


/* A call or a text is a preview only while confirm_outbound is on; with it off,
 * it dials or sends at once.  Read once, here: the handler follows the kind
 * decided at the gate (phone_previews), not the flag. */
static tool_action_kind_t phone_classify_call(const char *device,
                                              const char *action,
                                              const char *value,
                                              tool_action_kind_t listed) {
   (void)device;
   (void)value;
   if (listed == TOOL_KIND_PREPARE && !s_config.confirm_outbound && action &&
       (strcmp(action, "call") == 0 || strcmp(action, "send_sms") == 0)) {
      return TOOL_KIND_ACT;
   }
   return listed;
}

/* A staged item, described from the item itself (the slot the confirm names). */
static int describe_pending(struct json_object *details,
                            int kind,
                            char *out,
                            size_t out_len,
                            int *valid_for_sec) {
   turn_origin_t origin;
   if (!turn_origin_capture(&origin)) {
      return FAILURE;
   }
   const int user_id = tool_get_current_user_id();
   int n = -1;
   pthread_mutex_lock(&s_phone_tool_mutex);
   pending_slot_t *slot = NULL;
   if (pending_slots_find(&s_pending_slots, &origin, user_id, kind, pending_id_arg(details),
                          pending_slots_now(), &slot) == PENDING_FOUND &&
       pending_id_arg(details) != 0) {
      const phone_pending_t *p = (const phone_pending_t *)slot;
      char text[320];
      if (valid_for_sec) {
         *valid_for_sec = pending_slots_valid_for(&s_pending_slots, slot, pending_slots_now());
      }
      char date[32];
      switch (kind) {
         case PHONE_PENDING_CALL:
            n = snprintf(out, out_len, "call %s%s%s%s", p->name, p->name[0] ? " (" : "", p->number,
                         p->name[0] ? ")" : "");
            break;
         case PHONE_PENDING_SMS:
            str_excerpt_line(p->body, 200, text, sizeof(text));
            n = snprintf(out, out_len, "text %s%s%s%s: \"%s\"", p->name, p->name[0] ? " (" : "",
                         p->number, p->name[0] ? ")" : "", text);
            break;
         default: {
            const char *what = kind == PHONE_PENDING_DELETE_SMS ? "text message" : "call";
            if (p->id >= 0) {
               n = snprintf(out, out_len, "delete %s record #%lld", what, (long long)p->id);
            } else if (p->number[0]) {
               n = snprintf(out, out_len, "delete %d %s records with %s", p->preview_count, what,
                            p->number);
            } else {
               format_short_date(p->cutoff, date, sizeof(date));
               n = snprintf(out, out_len, "delete %d %s records from before %s", p->preview_count,
                            what, date);
            }
            break;
         }
      }
   }
   pthread_mutex_unlock(&s_phone_tool_mutex);
   return (n > 0 && (size_t)n < out_len) ? SUCCESS : FAILURE;
}

/* What a call that waits for the user's reply code does (tool_metadata_t
 * describe_call): a confirm from the item it carries out; a call or text
 * that goes at once (confirm_outbound off) from its target and body. */
static int phone_describe_call(const char *action,
                               const char *value,
                               char *out,
                               size_t out_len,
                               int *valid_for_sec) {
   struct json_object *details = value && value[0] ? json_tokener_parse(value) : NULL;
   int rc = FAILURE;
   int n = -1;
   bool handled = true;
   char text[320];
   if (strcmp(action, "confirm_call") == 0) {
      rc = describe_pending(details, PHONE_PENDING_CALL, out, out_len, valid_for_sec);
   } else if (strcmp(action, "confirm_sms") == 0) {
      rc = describe_pending(details, PHONE_PENDING_SMS, out, out_len, valid_for_sec);
   } else if (strcmp(action, "confirm_delete_sms") == 0) {
      rc = describe_pending(details, PHONE_PENDING_DELETE_SMS, out, out_len, valid_for_sec);
   } else if (strcmp(action, "confirm_delete_call") == 0) {
      rc = describe_pending(details, PHONE_PENDING_DELETE_CALL, out, out_len, valid_for_sec);
   } else if (strcmp(action, "answer") == 0 || strcmp(action, "hang_up") == 0) {
      /* The call it acts on, so a code can't answer or end another one. */
      phone_call_notif_status_t status;
      char number[64] = "";
      char name[128] = "";
      int64_t call_id = 0;
      if (phone_service_get_call_snapshot(&status, number, sizeof(number), name, sizeof(name),
                                          &call_id, NULL) &&
          (strcmp(action, "hang_up") == 0 || status == PHONE_CALL_NOTIF_RINGING)) {
         /* A ring doesn't last: the code goes with it. */
         if (strcmp(action, "answer") == 0) {
            *valid_for_sec = TOOL_CALL_CHALLENGE_MIN_SEC;
         }
         n = snprintf(out, out_len, "%s the call %s %s%s%s%s (#%lld)",
                      strcmp(action, "answer") == 0 ? "answer" : "hang up",
                      strcmp(action, "answer") == 0 ? "from" : "with", name[0] ? name : "",
                      name[0] ? " (" : "", number[0] ? number : "an unknown number",
                      name[0] ? ")" : "", (long long)call_id);
      } else if (strcmp(action, "hang_up") == 0 &&
                 phone_service_get_state() == PHONE_STATE_DIALING) {
         n = snprintf(out, out_len, "hang up the call being dialed");
      } else {
         snprintf(out, out_len, "there's no %s call",
                  strcmp(action, "answer") == 0 ? "ringing" : "current");
      }
   } else if ((strcmp(action, "call") == 0 || strcmp(action, "send_sms") == 0) && details) {
      /* Going at once (confirm_outbound off): the number the target resolves
       * to, as the handler will dial or text it.  One the user should confirm
       * is fine here: the code's text is that confirmation. */
      const char *target = json_get_str(details, "target");
      phone_resolve_t rez;
      memset(&rez, 0, sizeof(rez));
      if (target && target[0]) {
         phone_contacts_resolve(tool_get_current_user_id(), target, &rez);
      }
      if (target && target[0] &&
          (rez.kind == PHONE_RESOLVE_NUMBER || rez.kind == PHONE_RESOLVE_UNIQUE ||
           rez.kind == PHONE_RESOLVE_CONFIRM)) {
         char who[160];
         snprintf(who, sizeof(who), "%s%s%s%s", rez.name[0] ? rez.name : "",
                  rez.name[0] ? " (" : "", rez.number, rez.name[0] ? ")" : "");
         if (strcmp(action, "call") == 0) {
            n = snprintf(out, out_len, "call %s", who);
         } else {
            str_excerpt_line(json_get_str(details, "body"), 200, text, sizeof(text));
            n = snprintf(out, out_len, "text %s: \"%s\"", who, text);
         }
      } else if (target && target[0]) {
         /* What the voice path says too: which contacts it could be. */
         phone_contacts_format_disambiguation(target, &rez, out, out_len);
      } else {
         snprintf(out, out_len, "it doesn't say who to %s",
                  strcmp(action, "call") == 0 ? "call" : "text");
      }
   } else {
      handled = false;
   }
   if (n >= 0) {
      rc = ((size_t)n < out_len) ? SUCCESS : FAILURE;
   } else if (!handled) {
      rc = TOOL_DESCRIBE_DEFAULT;
   }
   if (details) {
      json_object_put(details);
   }
   return rc;
}

static const tool_metadata_t phone_metadata = {
   .name = "phone",
   .action_kinds = s_phone_action_kinds,
   .action_kind_count = TOOL_KIND_COUNT(s_phone_action_kinds),
   .classify_call = phone_classify_call,
   .describe_call = phone_describe_call,
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
       "Pending state expires after 5 minutes. "
       "To delete multiple messages from one sender, use delete_sms with 'number' "
       "(e.g. {number: '+14045550142'}) — it's one call that deletes them all. "
       "Do NOT loop over individual ids; do NOT wrap deletes in execute_plan — "
       "the phone tool is blocked inside plans on purpose. "
       "Use 'status' to check phone and modem status. "
       "Calls and SMS require confirmation before executing (say 'confirm' after review). "
       "The 'target' field can be a contact name (resolved via contacts) or a phone number.",
   .params = phone_params,
   .param_count = TOOL_PARAM_COUNT(phone_params),

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
