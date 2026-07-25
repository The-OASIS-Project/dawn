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
 * WebUI background-job frames (background jobs Phase 2 — §6.4).
 *
 * THE LIFETIME SPLIT.  Jobs reach the browser through three frames, split by how
 * long the thing they describe lives — not by which widget consumes them:
 *
 *   jobs_snapshot      the ACTIVE set, in full, on (re)connect
 *   job_update         one job's row, on every lifecycle transition
 *   list_jobs_response a keyset-paginated page of TERMINAL jobs (history)
 *
 * This replaces an earlier pair of frames that pushed server-computed per-parent
 * COUNTS (`job_activity` / `jobs_activity_snapshot`).  Counts are recoverable
 * from rows; rows are not recoverable from counts — so shipping both would have
 * meant two computations of one truth, on two code paths, able to disagree.  The
 * client now derives the "N jobs running" pill counts by grouping the active set
 * on parent_id.
 *
 * That derivation is only sound because the active set is BOUNDED: job_manager
 * gates every reservation on the global `max_active_jobs` (clamped <= 256) before
 * the per-user cap, so a user's active jobs fit in one frame and the snapshot is
 * a complete set rather than a page.  History has no such bound, which is exactly
 * why it is a separate, paginated frame that must never feed a count.  If the
 * ceiling below is ever actually hit, the frame says so (`truncated`) instead of
 * silently under-reporting.
 *
 * Lives outside webui_broadcasts.c (already large, and this is a domain surface
 * with its own contract) — same shape as webui_phone.c.
 */

#include <json-c/json.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/job_manager.h"
#include "logging.h"
#include "utils/string_utils.h"
#include "webui/webui_internal.h"
#include "webui/webui_send.h"

/* Hard ceiling on rows in one jobs_snapshot.  Sized past the largest active set
 * any config can produce (max_active_jobs clamps to 256), so `truncated` is a
 * can't-happen backstop rather than a routine outcome. */
#define JOBS_SNAPSHOT_HARD_MAX 512

/* Terminal jobs per list_jobs page, and the default when a client asks for none. */
#define JOBS_HISTORY_PAGE_MAX 50
#define JOBS_HISTORY_PAGE_DEFAULT 25

/* =============================================================================
 * Row serialization
 * ============================================================================= */

/* One job row as the client sees it.  Free-text fields are LLM-authored (the
 * spawn `title`) or internal (`job_error` is one of a handful of DAWN literals),
 * so they are sanitized before entering a WS text frame: a single invalid UTF-8
 * byte fails the frame (RFC 6455 §5.6) and wedges the connection. */
static json_object *job_to_json(const job_record_t *r) {
   char title[CONV_TITLE_MAX];
   char error[JOB_ERROR_MAX];
   char deliver_to[JOB_DELIVER_TO_MAX];
   char status[JOB_STATUS_MAX];
   char spawn_mode[JOB_SPAWN_MODE_MAX];
   char on_complete[JOB_ON_COMPLETE_MAX];
   snprintf(title, sizeof(title), "%s", r->title);
   snprintf(error, sizeof(error), "%s", r->job_error);
   snprintf(deliver_to, sizeof(deliver_to), "%s", r->deliver_to);
   snprintf(status, sizeof(status), "%s", r->job_status);
   snprintf(spawn_mode, sizeof(spawn_mode), "%s", r->spawn_mode);
   snprintf(on_complete, sizeof(on_complete), "%s", r->on_complete);
   /* Every string field, including the three that are allowlist-constrained at
    * today's write paths.  Sanitizing only the "untrusted" ones would leave a
    * rule to remember: relax one allowlist later (Phase 3 adds on_complete
    * variants) and a single bad byte fails the whole WS text frame per RFC 6455
    * §5.6 and wedges the connection.  Fails-safe beats a comment. */
   sanitize_utf8_for_json(title);
   sanitize_utf8_for_json(error);
   sanitize_utf8_for_json(deliver_to);
   sanitize_utf8_for_json(status);
   sanitize_utf8_for_json(spawn_mode);
   sanitize_utf8_for_json(on_complete);

   json_object *j = json_object_new_object();
   json_object_object_add(j, "conversation_id", json_object_new_int64(r->id));
   json_object_object_add(j, "parent_id", json_object_new_int64(r->parent_id));
   json_object_object_add(j, "title", json_object_new_string(title));
   json_object_object_add(j, "status", json_object_new_string(status));
   json_object_object_add(j, "spawn_mode", json_object_new_string(spawn_mode));
   json_object_object_add(j, "on_complete", json_object_new_string(on_complete));
   /* CP4's Resume affordance needs to know whether the follow-up already fired;
    * cheaper to ship now than to version the frame later. */
   json_object_object_add(j, "on_complete_fired", json_object_new_boolean(r->on_complete_fired));
   json_object_object_add(j, "spawn_depth", json_object_new_int(r->spawn_depth));
   json_object_object_add(j, "reinvoke_count", json_object_new_int(r->reinvoke_count));
   json_object_object_add(j, "created_at", json_object_new_int64((int64_t)r->created_at));
   json_object_object_add(j, "started_at", json_object_new_int64((int64_t)r->started_at));
   json_object_object_add(j, "finished_at", json_object_new_int64((int64_t)r->finished_at));
   /* Omitted rather than sent empty: absent reads as "none" at every consumer,
    * where "" would have to be special-cased by each of them. */
   if (deliver_to[0]) {
      json_object_object_add(j, "deliver_to", json_object_new_string(deliver_to));
   }
   if (error[0]) {
      json_object_object_add(j, "error", json_object_new_string(error));
   }
   return j;
}

/* Wrap a payload in a typed frame and hand it to @p conn's response queue. */
static void send_frame(ws_connection_t *conn, const char *type, json_object *payload) {
   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string(type));
   json_object_object_add(root, "payload", payload);
   send_json_response(conn, root);
   json_object_put(root);
}

/* =============================================================================
 * Outbound: job_update (lifecycle delta)
 * ============================================================================= */

/* Strong override of the weak seam in job_manager.c.  Fans one job row out to
 * the owner's authenticated browser sessions; clients upsert it into their
 * active set and drop it when it arrives terminal. */
void webui_broadcast_job_update(int user_id, const job_record_t *rec) {
   if (user_id <= 0 || !rec) {
      return;
   }

   json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("job_update"));
   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "job", job_to_json(rec));
   json_object_object_add(root, "payload", payload);
   /* PLAIN to match every other broadcast in the WebUI layer — the default
    * (_SPACED) would ship ~35 bytes of gratuitous whitespace per frame per
    * connection and make the job frames the odd family on the wire. */
   const char *json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
   if (!json_str) {
      /* Serialization OOM.  Dropping the frame costs a client a stale row until
       * its next snapshot; strdup(NULL) inside the registry walk would take the
       * daemon down while holding the connection mutex. */
      json_object_put(root);
      return;
   }

   int sent = 0;
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (!conn || !conn->session) {
         continue;
      }
      /* Browser sessions only, owner only.  A job row carries the user's own
       * prompt-derived title, so a non-positive owner id sends to nobody rather
       * than fanning it across every account. */
      if (!conn->authenticated || conn->is_satellite || conn->auth_user_id != user_id) {
         continue;
      }
      char *json_copy = strdup(json_str);
      if (!json_copy) {
         continue;
      }
      ws_response_t resp = { .session = conn->session,
                             .type = WS_RESP_JSON,
                             .generic_json = { .json = json_copy } };
      queue_response(&resp);
      sent++;
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);
   json_object_put(root);

   if (sent > 0) {
      OLOG_INFO("WebUI: job_update conv %lld (%s) to %d client(s)", (long long)rec->id,
                rec->job_status, sent);
   }
}

/* =============================================================================
 * Inbound: jobs_request -> jobs_snapshot (the complete active set)
 * ============================================================================= */

void webui_jobs_send_snapshot(ws_connection_t *conn) {
   if (!conn || !conn->session || !conn->authenticated || conn->is_satellite ||
       conn->auth_user_id <= 0) {
      return;
   }

   /* Sized from the caps that actually bound the set, not from a guess: nothing
    * can be running past max_active_jobs, nor queued past max_queued_per_user.
    * Heap, not stack — a job_record_t is ~700 bytes and this runs on the lws
    * service thread. */
   int cap = g_config.jobs.max_active_jobs + g_config.jobs.max_queued_per_user;
   if (cap < 1) {
      cap = 1;
   }
   if (cap > JOBS_SNAPSHOT_HARD_MAX) {
      cap = JOBS_SNAPSHOT_HARD_MAX;
   }
   /* Ask for one row MORE than the caps permit.  Reading exactly `cap` rows is
    * the legitimately-full case, not a truncated one; without the probe row a
    * user running at their configured limit would be told their counts are
    * unreliable every single connect. */
   int query_max = cap + 1;
   job_record_t *rows = calloc((size_t)query_max, sizeof(*rows));
   if (!rows) {
      OLOG_ERROR("webui_jobs: snapshot alloc failed (%d rows)", query_max);
      return;
   }

   int n = 0;
   if (conv_db_job_list_active_by_user(conn->auth_user_id, rows, query_max, &n) !=
       AUTH_DB_SUCCESS) {
      free(rows);
      OLOG_WARNING("webui_jobs: active-list read failed for user %d", conn->auth_user_id);
      return;
   }

   /* The client derives pill counts from this array, so a short read has to be
    * visible: `truncated` means "counts are lower bounds", never a silent
    * undercount.  Reaching the probe row means more active jobs exist than the
    * current caps allow — normally impossible, but `max_active_jobs` is
    * runtime-mutable, so lowering it below the number already running produces
    * exactly this transiently, until those jobs drain. */
   bool truncated = (n == query_max);

   json_object *payload = json_object_new_object();
   json_object *arr = json_object_new_array();
   /* Ship at most `cap` rows: the extra row exists only to detect truncation. */
   int emit = truncated ? cap : n;
   for (int i = 0; i < emit; i++) {
      json_object_array_add(arr, job_to_json(&rows[i]));
   }
   json_object_object_add(payload, "jobs", arr);
   json_object_object_add(payload, "truncated", json_object_new_boolean(truncated));
   if (truncated) {
      OLOG_WARNING("webui_jobs: user %d has more than %d active jobs — snapshot truncated",
                   conn->auth_user_id, cap);
   }
   send_frame(conn, "jobs_snapshot", payload);

   free(rows);
}

/* =============================================================================
 * Inbound: list_jobs -> list_jobs_response (paginated history)
 * ============================================================================= */

void webui_jobs_send_history(ws_connection_t *conn,
                             int64_t before_created_at,
                             int64_t before_id,
                             int limit) {
   if (!conn || !conn->session || !conn->authenticated || conn->is_satellite ||
       conn->auth_user_id <= 0) {
      return;
   }
   if (limit <= 0) {
      limit = JOBS_HISTORY_PAGE_DEFAULT;
   }
   if (limit > JOBS_HISTORY_PAGE_MAX) {
      limit = JOBS_HISTORY_PAGE_MAX;
   }

   job_record_t *rows = calloc((size_t)limit, sizeof(*rows));
   if (!rows) {
      OLOG_ERROR("webui_jobs: history alloc failed (%d rows)", limit);
      return;
   }

   int n = 0;
   if (conv_db_job_list_history_by_user(conn->auth_user_id, before_created_at, before_id, rows,
                                        limit, &n) != AUTH_DB_SUCCESS) {
      free(rows);
      OLOG_WARNING("webui_jobs: history read failed for user %d", conn->auth_user_id);
      return;
   }

   json_object *payload = json_object_new_object();
   json_object *arr = json_object_new_array();
   for (int i = 0; i < n; i++) {
      json_object_array_add(arr, job_to_json(&rows[i]));
   }
   json_object_object_add(payload, "jobs", arr);
   /* A full page means there MAY be more; the cursor is the last row's sort key,
    * so the follow-up request resumes exactly where this page stopped even if
    * jobs finished in between.  Exactly-full-and-no-more costs one empty page. */
   json_object_object_add(payload, "has_more", json_object_new_boolean(n == limit));
   if (n > 0) {
      json_object_object_add(payload, "next_before_created_at",
                             json_object_new_int64((int64_t)rows[n - 1].created_at));
      json_object_object_add(payload, "next_before_id", json_object_new_int64(rows[n - 1].id));
   }
   send_frame(conn, "list_jobs_response", payload);

   free(rows);
}
