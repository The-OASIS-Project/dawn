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
 * Summarize-missing backfill implementation.
 */

#define _GNU_SOURCE /* pthread_timedjoin_np */
#define AUTH_DB_INTERNAL_ALLOWED

#include "memory/memory_summarize_missing.h"

#include <errno.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "core/buf_printf.h"
#include "core/memory_filter.h"
#include "dawn_error.h"
#include "llm/llm_cache_monitor.h"
#include "llm/llm_interface.h"
#include "llm/llm_turn_blocks.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_extraction.h"
#include "memory/memory_extraction_input.h"
#include "memory/memory_history_loader.h"
#include "memory/memory_types.h"

/* Pacing between conversations.  Each LLM call already takes seconds; this
 * just keeps us off the rate-limit ceiling during long backfills and gives
 * concurrent live extractions a chance to slot in between. */
#define SUMMARIZE_THROTTLE_US 250000 /* 250ms */

/* Soft cap on conversations per worker run.  Acts as a defense against
 * pathological backlogs; operators staging large backfills can pass
 * max_count instead.  The internal cap is *unlimited* when max_count==0
 * — this constant only bounds the SQL list query. */
#define SUMMARIZE_QUERY_BATCH 64

/* Minimum total plain-text characters across the conversation history
 * (after stripping inline image data) for the LLM to have anything
 * summarizable.  Matches recovery.c's RECOVERY_MIN_TEXT_CHARS. */
#define SUMMARIZE_MIN_TEXT_CHARS 50

/* Hard cap on conversations processed per worker invocation when the
 * caller passes max_count == 0 ("unlimited").  Defense against an
 * admin-token-holder (compromised or malicious) launching a multi-hour
 * LLM-spend run with one packet.  Operators with a legitimate need to
 * exceed this can chain multiple runs; the worker is reentrant after
 * completion. */
#define SUMMARIZE_HARD_CAP 1000

static atomic_bool s_running = false;
static atomic_bool s_shutdown = false;
static pthread_t s_thread;

typedef struct {
   int user_id;
   uint32_t max_count;
} worker_arg_t;

/* =============================================================================
 * Missing-summary query
 *
 * Returns conversations with at least 2 messages, not private, and NO row in
 * memory_summaries linked back to them via source_conversation_id.
 *
 * Earlier-shipped paths (extraction_thread) stored summaries linked by both
 * session_id (text) and source_conversation_id (FK); newer paths only
 * populate the FK.  The FK is the authoritative link — session_id text-link
 * was kept for backward compatibility but is not relied on here.
 * ============================================================================= */

typedef struct {
   int64_t conv_id;
   int message_count;
} missing_conv_t;

static int count_missing_locked(int user_id, int *out_count) {
   const char *sql = "SELECT COUNT(*) FROM conversations c "
                     "WHERE c.user_id = ? "
                     "  AND c.is_private = 0 "
                     "  AND c.job_status IS NULL "
                     "  AND c.message_count >= 2 "
                     "  AND NOT EXISTS ("
                     "    SELECT 1 FROM memory_summaries s "
                     "    WHERE s.source_conversation_id = c.id"
                     "  )";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      OLOG_ERROR("memory_summarize_missing: count prepare failed: %s", sqlite3_errmsg(s_db.db));
      return FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   int n = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      n = sqlite3_column_int(stmt, 0);
   }
   sqlite3_finalize(stmt);
   *out_count = n;
   return SUCCESS;
}

/* Load up to `cap` missing-summary conversation IDs after `cursor`.  Caller
 * advances cursor past the last returned id to paginate.  Returns SUCCESS
 * with *out_n = 0 when no more rows. */
static int list_missing_after_locked(int user_id,
                                     int64_t cursor,
                                     missing_conv_t *out,
                                     int cap,
                                     int *out_n) {
   *out_n = 0;
   const char *sql = "SELECT c.id, c.message_count FROM conversations c "
                     "WHERE c.user_id = ? "
                     "  AND c.is_private = 0 "
                     "  AND c.job_status IS NULL "
                     "  AND c.message_count >= 2 "
                     "  AND c.id > ? "
                     "  AND NOT EXISTS ("
                     "    SELECT 1 FROM memory_summaries s "
                     "    WHERE s.source_conversation_id = c.id"
                     "  ) "
                     "ORDER BY c.id ASC "
                     "LIMIT ?";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      OLOG_ERROR("memory_summarize_missing: list prepare failed: %s", sqlite3_errmsg(s_db.db));
      return FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, cursor);
   sqlite3_bind_int(stmt, 3, cap);
   int n = 0;
   while (n < cap && sqlite3_step(stmt) == SQLITE_ROW) {
      out[n].conv_id = sqlite3_column_int64(stmt, 0);
      out[n].message_count = sqlite3_column_int(stmt, 1);
      n++;
   }
   sqlite3_finalize(stmt);
   *out_n = n;
   return SUCCESS;
}

int memory_summarize_missing_count(int user_id, int *out_count) {
   if (out_count == NULL)
      return FAILURE;
   *out_count = 0;
   if (user_id <= 0)
      return FAILURE;

   AUTH_DB_LOCK_OR_FAIL();
   int rc = count_missing_locked(user_id, out_count);
   AUTH_DB_UNLOCK();
   return rc;
}

/* Lock-and-query helper used by the worker.  AUTH_DB_LOCK_OR_FAIL early-
 * returns FAILURE (AUTH_DB_FAILURE) if the DB isn't initialised; the
 * worker treats that as a fatal abort signal and exits cleanly. */
static int worker_list_missing(int user_id,
                               int64_t cursor,
                               missing_conv_t *batch,
                               int cap,
                               int *out_n) {
   AUTH_DB_LOCK_OR_FAIL();
   int rc = list_missing_after_locked(user_id, cursor, batch, cap, out_n);
   AUTH_DB_UNLOCK();
   return rc;
}

/* =============================================================================
 * Per-conversation summary work
 * ============================================================================= */

/* Build the anchor-date prefix string for the prompt.  Mirrors the live
 * extractor's logic so backfilled summaries resolve relative phrases
 * ("yesterday", "last month") against the conversation's own logical
 * "now". */
static void build_anchor_line(int64_t conv_id, char *out, size_t out_sz) {
   out[0] = '\0';
   if (conv_id <= 0)
      return;
   int64_t anchor_ts = ANCHOR_DATE_NONE;
   if (conv_db_get_anchor_date(conv_id, &anchor_ts) != AUTH_DB_SUCCESS)
      return;
   if (anchor_ts == ANCHOR_DATE_NONE)
      return;
   struct tm tm_utc;
   time_t ts = (time_t)anchor_ts;
   if (!gmtime_r(&ts, &tm_utc))
      return;
   char date_buf[16];
   strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm_utc);
   snprintf(out, out_sz, "Conversation anchor: %s\n\n", date_buf);
}

/* Parse the summary + title + topics out of an LLM JSON response and
 * persist them.  Skips fact/preference/entity/relation arrays even if
 * the LLM returned them — that's the whole point of summary-only.
 *
 * Returns SUCCESS if a summary was stored, FAILURE if the response was
 * unparseable or the summary field was missing/empty/blocked. */
static int store_summary_only(int user_id,
                              int64_t conv_id,
                              const char *response,
                              int message_count,
                              int duration_seconds) {
   struct json_object *root = memory_extraction_parse_json(response);
   if (root == NULL) {
      OLOG_WARNING("memory_summarize_missing: unparseable LLM response for conv %lld",
                   (long long)conv_id);
      return FAILURE;
   }

   struct json_object *summary_obj = NULL;
   if (!json_object_object_get_ex(root, "summary", &summary_obj)) {
      OLOG_WARNING("memory_summarize_missing: no 'summary' field for conv %lld",
                   (long long)conv_id);
      json_object_put(root);
      return FAILURE;
   }
   const char *summary = json_object_get_string(summary_obj);
   if (summary == NULL || summary[0] == '\0') {
      OLOG_WARNING("memory_summarize_missing: empty summary for conv %lld", (long long)conv_id);
      json_object_put(root);
      return FAILURE;
   }
   /* No substring filter on summary text — LLM-paraphrased.
    * See atlas INJECTION_FILTER.md §"Filtering at the trust boundary,
    * not after paraphrase" for the trust-tier rationale. */

   /* Topics — LLM-emitted strings, no substring filter for the same
    * trust-tier reason as the summary. */
   char topics[MEMORY_TOPICS_MAX] = { 0 };
   struct json_object *topics_arr = NULL;
   if (json_object_object_get_ex(root, "topics", &topics_arr) &&
       json_object_is_type(topics_arr, json_type_array)) {
      int n = json_object_array_length(topics_arr);
      size_t toff = 0;
      size_t trem = MEMORY_TOPICS_MAX;
      for (int i = 0; i < n && trem > 1; i++) {
         const char *t = json_object_get_string(json_object_array_get_idx(topics_arr, i));
         if (t && t[0] != '\0') {
            if (toff > 0) {
               BUF_PRINTF(topics, toff, trem, ", ");
            }
            BUF_PRINTF(topics, toff, trem, "%s", t);
         }
      }
   }

   /* Synthetic session_id so the summary is distinguishable from one
    * produced by live extraction; the FK back to conversations is the
    * authoritative link. */
   char session_id[MEMORY_SESSION_ID_MAX];
   snprintf(session_id, sizeof(session_id), "summarize_missing_%lld", (long long)conv_id);

   /* Provenance covers the full range of stored messages.  msg_id_start=1
    * is a deliberate "from the beginning" sentinel — the live extractor
    * uses (last_extracted_msg_id+1) for incremental coverage, but a
    * backfilled summary necessarily spans the whole conversation. */
   memory_provenance_t prov = { .conv_id = conv_id, .msg_id_start = 1, .msg_id_end = 0 };
   int64_t max_msg_id = 0;
   if (conv_db_get_max_msg_id(conv_id, user_id, &max_msg_id) == AUTH_DB_SUCCESS && max_msg_id > 0) {
      prov.msg_id_end = max_msg_id;
   }

   /* Inherit the conversation's created_at so backfilled summaries land at
    * the conv's actual time, not the worker run time.  Keeps recency-based
    * retrieval signals correct after a backfill pass.  NOT_FOUND is silent
    * (the eligibility query already requires the conv exists; a race with
    * deletion is acceptable); FAILURE surfaces because it usually signals
    * a real DB issue worth investigating. */
   int64_t conv_created_at = 0;
   int lookup_rc = conv_db_get_created_at(conv_id, &conv_created_at);
   if (lookup_rc != AUTH_DB_SUCCESS && lookup_rc != AUTH_DB_NOT_FOUND) {
      OLOG_WARNING(
          "memory_summarize_missing: conv_db_get_created_at failed for conv %lld (rc=%d) — "
          "summary will use NOW() instead of conv time",
          (long long)conv_id, lookup_rc);
   }

   int64_t summary_id = 0;
   int rc = memory_db_summary_create_at(user_id, session_id, summary, topics, "neutral",
                                        message_count, duration_seconds,
                                        prov.msg_id_end > 0 ? &prov : NULL, conv_created_at,
                                        &summary_id);
   if (rc != MEMORY_DB_SUCCESS) {
      OLOG_WARNING("memory_summarize_missing: store failed for conv %lld (rc=%d)",
                   (long long)conv_id, rc);
      json_object_put(root);
      return FAILURE;
   }

   /* Embed-at-create — same pattern as live extraction.  Failure is silent
    * (the row exists; the recompute worker will backfill the embedding on
    * next boot or model swap). */
   if (summary_id > 0) {
      (void)memory_embeddings_embed_and_store_summary(user_id, summary_id, summary);
   }

   /* Title backfill intentionally omitted — existing conversations almost
    * always already have titles (set during the original session or by
    * recovery extraction), and the operator-visible goal of summarize-
    * missing is the summary row, not retitling.  Live extraction in
    * memory_extraction.c still handles title auto-rename. */

   json_object_put(root);
   return SUCCESS;
}

/* Run one conversation through the summary-only pipeline.  Returns
 * SUCCESS if a summary row landed, FAILURE on any earlier-stage abort. */
static int process_one_conv(int user_id,
                            int64_t conv_id,
                            int message_count,
                            const llm_resolved_config_t *cfg) {
   size_t text_len = 0;
   struct json_object *history = memory_history_load_from_db(conv_id, user_id, &text_len);
   if (history == NULL) {
      OLOG_WARNING("memory_summarize_missing: load history failed for conv %lld",
                   (long long)conv_id);
      return FAILURE;
   }
   if (text_len < SUMMARIZE_MIN_TEXT_CHARS) {
      OLOG_INFO("memory_summarize_missing: conv %lld only %zu chars text, skipping",
                (long long)conv_id, text_len);
      json_object_put(history);
      return FAILURE;
   }

   /* What extraction would send: the rows checked to be this conversation's,
    * DAWN's injected context and a turn's reasoning left out. */
   struct json_object *input = memory_extraction_build_input(user_id, conv_id, history, 0);
   json_object_put(history);
   history = input ? llm_history_strip_internal(input) : NULL;
   json_object_put(input);
   if (history == NULL) {
      OLOG_WARNING("memory_summarize_missing: conv %lld refused for summarizing",
                   (long long)conv_id);
      return FAILURE;
   }

   const char *history_json = json_object_to_json_string_ext(history, JSON_C_TO_STRING_PLAIN);
   if (history_json == NULL) {
      OLOG_WARNING("memory_summarize_missing: history JSON encode failed for conv %lld",
                   (long long)conv_id);
      json_object_put(history);
      return FAILURE;
   }

   char anchor_line[64] = "";
   build_anchor_line(conv_id, anchor_line, sizeof(anchor_line));

   /* The canonical extraction prompt takes four %s args: anchor, conversation,
    * existing profile, expiry block.  The profile is empty and the expiry
    * block left out: only the summary matters here. */
   const char *empty_profile = "(none)";
   size_t prompt_sz = strlen(MEMORY_EXTRACTION_PROMPT_TEMPLATE) + strlen(anchor_line) +
                      strlen(history_json) + strlen(empty_profile) + 100;
   char *prompt = malloc(prompt_sz);
   if (prompt == NULL) {
      OLOG_ERROR("memory_summarize_missing: OOM prompt for conv %lld", (long long)conv_id);
      json_object_put(history);
      return FAILURE;
   }
   snprintf(prompt, prompt_sz, MEMORY_EXTRACTION_PROMPT_TEMPLATE, anchor_line, history_json,
            empty_profile, "");

   /* Build a one-message wrapper for llm_chat_completion_with_config. */
   struct json_object *llm_history = json_object_new_array();
   struct json_object *user_msg = json_object_new_object();
   json_object_object_add(user_msg, "role", json_object_new_string("user"));
   json_object_object_add(user_msg, "content", json_object_new_string(prompt));
   json_object_array_add(llm_history, user_msg);

   const int kind_prev = llm_cache_monitor_push_kind(LLM_CALL_EXTRACTION);
   char *response = llm_chat_completion_with_config(llm_history, prompt, cfg);
   llm_cache_monitor_pop_kind(kind_prev);

   json_object_put(llm_history);
   free(prompt);
   json_object_put(history);

   if (response == NULL) {
      OLOG_WARNING("memory_summarize_missing: LLM returned no response for conv %lld",
                   (long long)conv_id);
      return FAILURE;
   }

   int rc = store_summary_only(user_id, conv_id, response, message_count, 0);
   free(response);
   return rc;
}

/* =============================================================================
 * Worker thread
 * ============================================================================= */

static void *worker_fn(void *arg) {
   worker_arg_t *warg = (worker_arg_t *)arg;
   int user_id = warg->user_id;
   uint32_t max_count = warg->max_count;
   free(warg);

   OLOG_INFO("memory_summarize_missing: start user=%d max=%u", user_id, max_count);

   llm_resolved_config_t cfg;
   char model_buf[LLM_MODEL_NAME_MAX];
   char endpoint_buf[MEMORY_EXTRACTION_ENDPOINT_BUF_MIN];
   if (memory_extraction_resolve_config(&cfg, model_buf, sizeof(model_buf), endpoint_buf,
                                        sizeof(endpoint_buf),
                                        "memory_summarize_missing") != SUCCESS) {
      OLOG_ERROR("memory_summarize_missing: resolve_config failed");
      atomic_store(&s_running, false);
      return NULL;
   }

   OLOG_INFO("memory_summarize_missing: using provider=%s model=%s",
             g_config.memory.extraction_provider,
             g_config.memory.extraction_model[0] ? g_config.memory.extraction_model : "(default)");

   int total_seen = 0;
   int total_stored = 0;
   int total_failed = 0;
   int64_t cursor = 0;
   missing_conv_t batch[SUMMARIZE_QUERY_BATCH];

   while (!atomic_load(&s_shutdown)) {
      if (max_count > 0 && (uint32_t)total_seen >= max_count) {
         break;
      }

      int n = 0;
      int rc = worker_list_missing(user_id, cursor, batch, SUMMARIZE_QUERY_BATCH, &n);
      if (rc != SUCCESS) {
         OLOG_ERROR("memory_summarize_missing: list query failed; aborting");
         break;
      }
      if (n <= 0) {
         OLOG_INFO("memory_summarize_missing: backlog drained for user %d", user_id);
         break;
      }

      for (int i = 0; i < n && !atomic_load(&s_shutdown); i++) {
         if (max_count > 0 && (uint32_t)total_seen >= max_count) {
            break;
         }
         total_seen++;
         cursor = batch[i].conv_id;

         int srv = process_one_conv(user_id, batch[i].conv_id, batch[i].message_count, &cfg);
         if (srv == SUCCESS) {
            total_stored++;
            OLOG_INFO(
                "memory_summarize_missing: stored summary for conv %lld (%d/%d done, %d failed)",
                (long long)batch[i].conv_id, total_stored, total_seen, total_failed);
         } else {
            total_failed++;
         }

         if ((total_seen % 10) == 0) {
            OLOG_INFO("memory_summarize_missing: progress user=%d seen=%d stored=%d failed=%d",
                      user_id, total_seen, total_stored, total_failed);
         }

         usleep(SUMMARIZE_THROTTLE_US);
      }

      if (n < SUMMARIZE_QUERY_BATCH) {
         /* Last partial page — no more rows. */
         break;
      }
   }

   OLOG_INFO("memory_summarize_missing: complete user=%d seen=%d stored=%d failed=%d", user_id,
             total_seen, total_stored, total_failed);

   atomic_store(&s_running, false);
   return NULL;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int memory_summarize_missing_start(int user_id, uint32_t max_count) {
   if (user_id <= 0) {
      return FAILURE;
   }
   bool expected = false;
   if (!atomic_compare_exchange_strong(&s_running, &expected, true)) {
      OLOG_WARNING("memory_summarize_missing: already running");
      return FAILURE;
   }

   /* Clamp the "unlimited" sentinel to a hard cap so a single packet
    * can't enqueue arbitrarily many LLM calls.  Documented at
    * SUMMARIZE_HARD_CAP above. */
   if (max_count == 0)
      max_count = SUMMARIZE_HARD_CAP;

   worker_arg_t *warg = malloc(sizeof(*warg));
   if (warg == NULL) {
      atomic_store(&s_running, false);
      return FAILURE;
   }
   warg->user_id = user_id;
   warg->max_count = max_count;

   atomic_store(&s_shutdown, false);

   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

   if (pthread_create(&s_thread, &attr, worker_fn, warg) != 0) {
      OLOG_ERROR("memory_summarize_missing: pthread_create failed");
      free(warg);
      atomic_store(&s_running, false);
      pthread_attr_destroy(&attr);
      return FAILURE;
   }

   pthread_attr_destroy(&attr);
   return SUCCESS;
}

bool memory_summarize_missing_is_running(void) {
   return atomic_load(&s_running);
}

void memory_summarize_missing_stop(void) {
   if (!atomic_load(&s_running)) {
      return;
   }
   atomic_store(&s_shutdown, true);

   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_sec += 5;

   int ret = pthread_timedjoin_np(s_thread, NULL, &ts);
   if (ret == ETIMEDOUT) {
      OLOG_WARNING("memory_summarize_missing: thread did not stop in 5s, cancelling");
      pthread_cancel(s_thread);
      pthread_join(s_thread, NULL);
   }
   atomic_store(&s_running, false);
}
