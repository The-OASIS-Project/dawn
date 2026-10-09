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
 * Memory-pipeline mode for bench_retrieval.
 *
 * Drives DAWN's production extraction pipeline end-to-end against a LoCoMo
 * conversation: ingests dialogs as messages with per-dia_id msg_ids, calls
 * memory_trigger_extraction() at each session boundary, polls until done,
 * dumps the resulting facts with v40 provenance.
 *
 * Phase 0 entry point. The full --memory-pipeline JSON protocol (conv_create,
 * add_message, extract, query_memory, reset_memory, snapshot_*) lands in
 * Phase 1; this file provides only the smoke-test path for now.
 *
 * =====================================================================
 *  CONTRACT: bench-only handlers MUST scope by BENCH_MP_USER_ID
 * =====================================================================
 *
 * This file links memory_callback.c, contacts_db.c, and other Layer 2
 * modules so the bench can drive the production retrieval path
 * faithfully.  That same statically-linked code can also mutate contacts
 * / entities / facts for ANY user_id — so any new handler added to this
 * file MUST pass user_id = BENCH_MP_USER_ID (or a const derived from it)
 * to every memory_*, conv_db_*, contacts_* call.  Handlers that accept
 * user_id from the JSON command would let an attacker (or a careless
 * test) exercise other users' memory stores.
 *
 * The DAWN_DAEMON_BUILD guard below catches the worst case (this file
 * being linked into the daemon binary), but it does NOT catch
 * inside-bench misuse.  When adding a handler, audit it against this
 * rule.  If a handler legitimately needs a different user_id (for
 * future multi-user bench scenarios), introduce an explicit
 * BENCH_MP_AUX_USER_ID constant, do NOT take it from the wire.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include "bench_memory_pipeline.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "bench_memory_schema.h"
#include "config/dawn_config.h"
#include "memory/memory_callback_internal.h"
#include "memory/memory_db.h"
#include "memory/memory_db_provenance.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_extraction.h"
#include "memory/memory_fact_search.h"
#include "memory/memory_graph_retrieval.h"
#include "memory/memory_types.h"
#include "prompts.h"

#ifdef DAWN_DAEMON_BUILD
#error "bench_memory_pipeline.c is a benchmark harness — do NOT link into the dawn daemon. \
The query_memory_callback handler exposes the production memory path with BENCH_MP_USER_ID; \
linking into the daemon would let any caller exercise user 1's full memory store."
#endif

#define BENCH_MP_USER_ID 1
#define BENCH_MP_POLL_USEC 250000 /* 250 ms */
#define BENCH_MP_TIMEOUT_SEC 600  /* 10 min per session */
#define BENCH_MP_FACT_LIST_CAP 500
#define BENCH_MP_DIA_MAP_CAP 4096

extern dawn_config_t g_config;
extern secrets_config_t g_secrets;
extern auth_db_state_t s_db;

typedef struct {
   char dia_id[32];
   int64_t msg_id;
} dia_map_entry_t;

static dia_map_entry_t s_dia_map[BENCH_MP_DIA_MAP_CAP];
static int s_dia_map_count = 0;

static void dia_map_clear(void) {
   s_dia_map_count = 0;
}

static void dia_map_add(const char *dia_id, int64_t msg_id) {
   if (s_dia_map_count >= BENCH_MP_DIA_MAP_CAP)
      return;
   snprintf(s_dia_map[s_dia_map_count].dia_id, sizeof(s_dia_map[0].dia_id), "%s", dia_id);
   s_dia_map[s_dia_map_count].msg_id = msg_id;
   s_dia_map_count++;
}

/* =============================================================================
 * Init / teardown — opens :memory: SQLite + applies the bench memory DDL.
 * Must run instead of (not alongside) the existing bench setup_db(), which
 * creates an incompatible users-table schema.
 * ============================================================================= */

/* Use auth_db_init() so we get full production schema + prepared statements.
 * Pure :memory: would skip the prepared-statements step, breaking conv_db_*
 * and memory_db_* helpers.  Tmpfile is cheap, gets unlinked on teardown. */
static char s_bench_db_path[64] = "";

int bench_mp_init(void) {
   /* mkstemp gives us a unique writable path; close the fd immediately so
    * sqlite can open it. */
   snprintf(s_bench_db_path, sizeof(s_bench_db_path), "/tmp/bench_mp_XXXXXX.db");
   int fd = mkstemps(s_bench_db_path, 3);
   if (fd < 0) {
      fprintf(stderr, "bench_mp_init: mkstemps failed\n");
      return 1;
   }
   close(fd);
   /* Remove the empty file so auth_db_init creates a fresh DB without
    * tripping the permission-fix path. */
   unlink(s_bench_db_path);

   if (auth_db_init(s_bench_db_path) != AUTH_DB_SUCCESS) {
      fprintf(stderr, "bench_mp_init: auth_db_init(%s) failed\n", s_bench_db_path);
      return 1;
   }

   /* Insert benchmark user — auth_db_init doesn't seed users.  Use direct
    * sqlite3_exec since auth_db's user-creation path goes through password
    * hashing we don't need. */
   char *errmsg = NULL;
   const char *insert_user =
       "INSERT OR IGNORE INTO users (id, username, password_hash, created_at) "
       "VALUES (1, 'benchmark', '', strftime('%s','now'))";
   if (sqlite3_exec(s_db.db, insert_user, NULL, NULL, &errmsg) != SQLITE_OK) {
      fprintf(stderr, "bench_mp_init: insert user failed: %s\n", errmsg ? errmsg : "?");
      sqlite3_free(errmsg);
      return 1;
   }

   /* The BENCH_MEMORY_DDL header is retained as a documented subset for the
    * eventual --memory-pipeline JSON-protocol mode; it's not exec'd here
    * because auth_db_init already applied the production schema (superset). */
   (void)BENCH_MEMORY_DDL;
   return 0;
}

void bench_mp_teardown(void) {
   if (s_db.db) {
      sqlite3_close(s_db.db);
      s_db.db = NULL;
   }
   s_db.initialized = false;
   if (s_bench_db_path[0])
      unlink(s_bench_db_path);
}

/* =============================================================================
 * Wait for extraction to complete on the given user, with timeout.
 * Mirrors src/memory/memory_recovery.c:wait_for_user_extraction() but with
 * a tighter 250ms poll since the bench is single-purpose.
 * ============================================================================= */

static int wait_for_extraction(int user_id, int timeout_sec) {
   time_t deadline = time(NULL) + timeout_sec;
   while (memory_extraction_in_progress(user_id)) {
      if (time(NULL) >= deadline) {
         fprintf(stderr, "bench_mp: extraction timed out after %ds\n", timeout_sec);
         return 1;
      }
      usleep(BENCH_MP_POLL_USEC);
   }
   return 0;
}

/* =============================================================================
 * Build conversation_history JSON from messages added so far.
 *
 * Each entry: {role, content, speaker?, id}.  Extraction's internal filter
 * (memory_extraction.c:1198-1217) skips messages with id <= last_msg_id and
 * with role=="system", so the role field stays even in cγ mode.
 *
 * cγ-bench wiring (Phase 1.5, May 2026): when content begins with a
 * "Speaker said," prefix (the LoCoMo bench-harness shape that
 * run_benchmark.py inserts), parse the leading name and emit it as a
 * "speaker" field, AND set role to a uniform "user" so the role-binary
 * prior never fires.  The probe's c3 mini-bench measured a +25pp aggregate
 * fix-rate lift across 3 models for this format vs HEAD.  Production
 * memory_extraction.c is intentionally NOT modified — it sees the extra
 * speaker field passively, and its existing role filter still works
 * because every message has role=="user" in cγ-bench mode.
 *
 * Falls through to the original role-based emission when content has no
 * "X said," prefix (production-shape input from dawn-admin add_message
 * outside bench).  Production today is single-user, so the speaker-field
 * effect doesn't apply there.
 * ============================================================================= */

/* Parse a leading "<Name> said," prefix from content.  Writes the name
 * into out (capacity out_cap) and returns true on success.  Restricted to
 * a leading capital letter + alpha/apostrophe/hyphen body so an
 * accidentally-matching production phrase like "i said," doesn't trip it
 * (the existing bench harness inserts properly-capitalised speaker names;
 * production raw user text would not). */
static bool parse_speaker_prefix(const char *content, char *out, size_t out_cap) {
   if (!content || !out || out_cap == 0)
      return false;
   if (!(content[0] >= 'A' && content[0] <= 'Z'))
      return false;
   const char *said = strstr(content, " said,");
   if (!said || said == content)
      return false;
   size_t namelen = (size_t)(said - content);
   if (namelen == 0 || namelen >= out_cap)
      return false;
   for (size_t i = 0; i < namelen; i++) {
      char ch = content[i];
      bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '\'' || ch == '-';
      if (!ok)
         return false;
   }
   memcpy(out, content, namelen);
   out[namelen] = '\0';
   return true;
}

static struct json_object *build_history_for_conv(int64_t conv_id, int *count_out) {
   sqlite3_stmt *stmt = NULL;
   const char *sql =
       "SELECT id, role, content FROM messages WHERE conversation_id = ? AND kind IS NULL "
       "ORDER BY id ASC";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      fprintf(stderr, "bench_mp: prepare history failed: %s\n", sqlite3_errmsg(s_db.db));
      return NULL;
   }
   sqlite3_bind_int64(stmt, 1, conv_id);

   struct json_object *arr = json_object_new_array();
   int n = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW) {
      int64_t msg_id = sqlite3_column_int64(stmt, 0);
      const char *role = (const char *)sqlite3_column_text(stmt, 1);
      const char *content = (const char *)sqlite3_column_text(stmt, 2);
      struct json_object *msg = json_object_new_object();

      char speaker[64] = "";
      bool have_speaker = parse_speaker_prefix(content, speaker, sizeof(speaker));

      if (have_speaker) {
         /* cγ-bench: uniform role + named speaker (matches the probe's
          * approach-D shape that delivered +25pp c3 lift).  role stays a
          * non-system value so memory_extraction.c's filter accepts it. */
         json_object_object_add(msg, "role", json_object_new_string("user"));
         json_object_object_add(msg, "content", json_object_new_string(content ? content : ""));
         json_object_object_add(msg, "speaker", json_object_new_string(speaker));
      } else {
         /* Production-shape fallback: keep the original role binary. */
         json_object_object_add(msg, "role", json_object_new_string(role ? role : ""));
         json_object_object_add(msg, "content", json_object_new_string(content ? content : ""));
      }
      json_object_object_add(msg, "id", json_object_new_int64(msg_id));
      json_object_array_add(arr, msg);
      n++;
   }
   sqlite3_finalize(stmt);
   if (count_out)
      *count_out = n;
   return arr;
}

/* =============================================================================
 * Smoke-test runner.  Reads LoCoMo JSON, picks conv_idx, ingests dialogs as
 * messages, fires extraction at each session boundary, dumps facts at end.
 * ============================================================================= */

static const char *role_for_speaker(const char *speaker, const char *first_speaker) {
   /* LoCoMo has two human speakers; DAWN's prompt wants user/assistant.
    * Map first-introduced speaker to "user", the second to "assistant". */
   if (!speaker || !first_speaker)
      return "user";
   return strcmp(speaker, first_speaker) == 0 ? "user" : "assistant";
}

int bench_mp_run_smoke(const char *locomo_path, int conv_idx) {
   /* 1. Load LoCoMo JSON */
   struct json_object *root = json_object_from_file(locomo_path);
   if (!root || !json_object_is_type(root, json_type_array)) {
      fprintf(stderr, "bench_mp: failed to load LoCoMo JSON from %s\n", locomo_path);
      if (root)
         json_object_put(root);
      return 1;
   }
   if (conv_idx < 0 || conv_idx >= (int)json_object_array_length(root)) {
      fprintf(stderr, "bench_mp: conv_idx %d out of range (have %d)\n", conv_idx,
              (int)json_object_array_length(root));
      json_object_put(root);
      return 1;
   }

   struct json_object *entry = json_object_array_get_idx(root, conv_idx);
   struct json_object *conv = NULL;
   if (!json_object_object_get_ex(entry, "conversation", &conv))
      conv = entry;

   /* 2. Create DAWN conversation */
   int64_t conv_id = 0;
   if (conv_db_create_with_origin(BENCH_MP_USER_ID, "smoke", "voice", &conv_id) !=
       AUTH_DB_SUCCESS) {
      fprintf(stderr, "bench_mp: conv_db_create_with_origin failed\n");
      json_object_put(root);
      return 1;
   }
   fprintf(stderr, "bench_mp: created conv_id=%" PRId64 "\n", conv_id);

   /* 3. Iterate sessions; ingest dialogs; trigger extraction at each boundary */
   dia_map_clear();
   char first_speaker[64] = "";
   int total_msgs = 0;
   int total_extractions = 0;
   time_t total_start = time(NULL);

   for (int session_n = 1;; session_n++) {
      char key[32];
      snprintf(key, sizeof(key), "session_%d", session_n);
      struct json_object *session = NULL;
      if (!json_object_object_get_ex(conv, key, &session))
         break;
      if (!json_object_is_type(session, json_type_array))
         continue;

      /* Ingest all dialogs in this session */
      size_t n_dialogs = json_object_array_length(session);
      for (size_t i = 0; i < n_dialogs; i++) {
         struct json_object *d = json_object_array_get_idx(session, i);
         struct json_object *dia_id_obj = NULL, *speaker_obj = NULL, *text_obj = NULL;
         json_object_object_get_ex(d, "dia_id", &dia_id_obj);
         json_object_object_get_ex(d, "speaker", &speaker_obj);
         json_object_object_get_ex(d, "text", &text_obj);
         const char *dia_id = dia_id_obj ? json_object_get_string(dia_id_obj) : "";
         const char *speaker = speaker_obj ? json_object_get_string(speaker_obj) : "?";
         const char *text = text_obj ? json_object_get_string(text_obj) : "";

         if (first_speaker[0] == '\0')
            snprintf(first_speaker, sizeof(first_speaker), "%s", speaker);
         const char *role = role_for_speaker(speaker, first_speaker);

         char content[2048];
         snprintf(content, sizeof(content), "%s said, \"%s\"", speaker, text);

         int64_t msg_id = 0;
         if (conv_db_add_message_ex(conv_id, BENCH_MP_USER_ID, role, content, &msg_id) !=
             AUTH_DB_SUCCESS) {
            fprintf(stderr, "bench_mp: conv_db_add_message_ex failed for %s\n", dia_id);
            continue;
         }
         dia_map_add(dia_id, msg_id);
         total_msgs++;

         /* Print mapping to stderr so the smoke caller can sanity-check */
         fprintf(stderr, "  %s -> msg_id=%" PRId64 "\n", dia_id, msg_id);
      }

      /* Trigger extraction at session boundary.  Build fresh history each time;
       * memory_extraction filters internally on last_extracted_msg_id. */
      int message_count = 0;
      struct json_object *history = build_history_for_conv(conv_id, &message_count);
      if (!history) {
         fprintf(stderr, "bench_mp: build_history failed\n");
         json_object_put(root);
         return 1;
      }

      char session_id_buf[64];
      snprintf(session_id_buf, sizeof(session_id_buf), "smoke_conv%d_s%d", conv_idx, session_n);

      time_t t_start = time(NULL);
      fprintf(stderr, "bench_mp: triggering extraction at session %d (%d total msgs)\n", session_n,
              message_count);

      int rc = memory_trigger_extraction(BENCH_MP_USER_ID, conv_id, session_id_buf, history,
                                         message_count, 0, NULL);
      json_object_put(history);

      if (rc != 0) {
         fprintf(stderr, "bench_mp: memory_trigger_extraction returned %d at session %d\n", rc,
                 session_n);
         /* Some return codes are benign (no new messages, too few messages) — keep going */
         continue;
      }

      if (wait_for_extraction(BENCH_MP_USER_ID, BENCH_MP_TIMEOUT_SEC) != 0) {
         fprintf(stderr, "bench_mp: extraction stuck at session %d\n", session_n);
         json_object_put(root);
         return 1;
      }

      total_extractions++;
      time_t elapsed = time(NULL) - t_start;
      fprintf(stderr, "bench_mp: session %d extraction done (%lds)\n", session_n, (long)elapsed);
   }

   /* 4. Dump facts with provenance */
   memory_fact_t *facts = calloc(BENCH_MP_FACT_LIST_CAP, sizeof(*facts));
   if (!facts) {
      fprintf(stderr, "bench_mp: calloc failed\n");
      json_object_put(root);
      return 1;
   }
   int fact_count = 0;
   if (memory_db_fact_list(BENCH_MP_USER_ID, facts, BENCH_MP_FACT_LIST_CAP, 0, &fact_count) !=
       MEMORY_DB_SUCCESS) {
      fprintf(stderr, "bench_mp: memory_db_fact_list failed\n");
      free(facts);
      json_object_put(root);
      return 1;
   }

   /* Batch-load provenance */
   int64_t *fact_ids = calloc((size_t)fact_count, sizeof(int64_t));
   int64_t *prov_conv_ids = calloc((size_t)fact_count, sizeof(int64_t));
   int64_t *prov_starts = calloc((size_t)fact_count, sizeof(int64_t));
   int64_t *prov_ends = calloc((size_t)fact_count, sizeof(int64_t));
   if (!fact_ids || !prov_conv_ids || !prov_starts || !prov_ends) {
      fprintf(stderr, "bench_mp: provenance calloc failed\n");
      free(facts);
      free(fact_ids);
      free(prov_conv_ids);
      free(prov_starts);
      free(prov_ends);
      json_object_put(root);
      return 1;
   }
   for (int i = 0; i < fact_count; i++)
      fact_ids[i] = facts[i].id;
   memory_db_facts_get_sources(BENCH_MP_USER_ID, fact_ids, fact_count, prov_conv_ids, prov_starts,
                               prov_ends);

   /* Phase 0 validation: aggregate counts for the new schema's load-bearing
    * fields.  Computed via direct SQL since memory_fact_t doesn't carry
    * subject_entity_id and memory_relation_t doesn't carry fact_id; these
    * counts answer "did the parser refactor populate the FKs correctly?". */
   int facts_with_subject = 0;
   int facts_no_subject = 0;
   int relations_total = 0;
   int relations_with_fact = 0;
   int relations_no_fact = 0;
   {
      sqlite3_stmt *stmt = NULL;
      if (sqlite3_prepare_v2(s_db.db,
                             "SELECT "
                             "  SUM(CASE WHEN subject_entity_id IS NOT NULL THEN 1 ELSE 0 END), "
                             "  SUM(CASE WHEN subject_entity_id IS NULL THEN 1 ELSE 0 END) "
                             "FROM memory_facts WHERE user_id = ?",
                             -1, &stmt, NULL) == SQLITE_OK) {
         sqlite3_bind_int(stmt, 1, BENCH_MP_USER_ID);
         if (sqlite3_step(stmt) == SQLITE_ROW) {
            facts_with_subject = sqlite3_column_int(stmt, 0);
            facts_no_subject = sqlite3_column_int(stmt, 1);
         }
         sqlite3_finalize(stmt);
      }
      if (sqlite3_prepare_v2(s_db.db,
                             "SELECT "
                             "  COUNT(*), "
                             "  SUM(CASE WHEN fact_id IS NOT NULL THEN 1 ELSE 0 END), "
                             "  SUM(CASE WHEN fact_id IS NULL THEN 1 ELSE 0 END) "
                             "FROM memory_relations WHERE user_id = ?",
                             -1, &stmt, NULL) == SQLITE_OK) {
         sqlite3_bind_int(stmt, 1, BENCH_MP_USER_ID);
         if (sqlite3_step(stmt) == SQLITE_ROW) {
            relations_total = sqlite3_column_int(stmt, 0);
            relations_with_fact = sqlite3_column_int(stmt, 1);
            relations_no_fact = sqlite3_column_int(stmt, 2);
         }
         sqlite3_finalize(stmt);
      }
   }

   /* Output JSON summary */
   time_t total_elapsed = time(NULL) - total_start;
   printf("{\n");
   printf("  \"mode\": \"memory-pipeline-smoke\",\n");
   printf("  \"conv_idx\": %d,\n", conv_idx);
   printf("  \"conv_id\": %" PRId64 ",\n", conv_id);
   printf("  \"total_messages\": %d,\n", total_msgs);
   printf("  \"total_extractions\": %d,\n", total_extractions);
   printf("  \"total_seconds\": %ld,\n", (long)total_elapsed);
   printf("  \"extraction_provider\": \"%s\",\n", g_config.memory.extraction_provider);
   printf("  \"extraction_model\": \"%s\",\n", g_config.memory.extraction_model);
   printf("  \"fact_count\": %d,\n", fact_count);
   printf("  \"phase0_facts_with_subject_entity_id\": %d,\n", facts_with_subject);
   printf("  \"phase0_facts_null_subject_entity_id\": %d,\n", facts_no_subject);
   printf("  \"phase0_relations_total\": %d,\n", relations_total);
   printf("  \"phase0_relations_linked_to_fact\": %d,\n", relations_with_fact);
   printf("  \"phase0_relations_unlinked\": %d,\n", relations_no_fact);
   printf("  \"facts\": [\n");
   for (int i = 0; i < fact_count; i++) {
      const char *fact_text_esc = json_object_to_json_string(
          json_object_new_string(facts[i].fact_text));
      printf("    {\"id\": %" PRId64 ", \"text\": %s, \"category\": \"%s\", "
             "\"conv_id\": %" PRId64 ", \"msg_start\": %" PRId64 ", \"msg_end\": %" PRId64 "}%s\n",
             facts[i].id, fact_text_esc, facts[i].category, prov_conv_ids[i], prov_starts[i],
             prov_ends[i], i + 1 < fact_count ? "," : "");
   }
   printf("  ]\n");
   printf("}\n");

   free(facts);
   free(fact_ids);
   free(prov_conv_ids);
   free(prov_starts);
   free(prov_ends);
   json_object_put(root);
   return 0;
}

/* =============================================================================
 * JSON-protocol dispatcher (Phase 1).
 *
 * Each handler reads its inputs from `cmd` and writes a single JSON line to
 * stdout — same wire convention as handle_add/handle_query/handle_reset in
 * bench_retrieval.c.  Returns 1 if the command was recognised, 0 otherwise.
 * ============================================================================= */

static void respond_error(const char *message) {
   fprintf(stdout, "{\"status\":\"error\",\"message\":\"%s\"}\n", message ? message : "?");
   fflush(stdout);
}

/* Reverse: msg_id → dia_id (used by query_memory to compute covered_dia_ids
 * for retrieved facts whose provenance range covers stored msg_ids). */
static const char *dia_map_reverse(int64_t msg_id) {
   for (int i = 0; i < s_dia_map_count; i++) {
      if (s_dia_map[i].msg_id == msg_id)
         return s_dia_map[i].dia_id;
   }
   return NULL;
}

static int handle_conv_create(struct json_object *cmd) {
   const char *title = "bench";
   struct json_object *title_obj = NULL;
   if (json_object_object_get_ex(cmd, "title", &title_obj))
      title = json_object_get_string(title_obj);

   int64_t conv_id = 0;
   if (conv_db_create_with_origin(BENCH_MP_USER_ID, title, "voice", &conv_id) != AUTH_DB_SUCCESS) {
      respond_error("conv_db_create_with_origin failed");
      return 1;
   }

   /* Optional anchor_date override (v42).  When the bench passes a LoCoMo
    * session_X_date_time epoch, the extraction prompt resolves relative
    * temporal phrases against it instead of bench wall-clock. */
   struct json_object *anchor_obj = NULL;
   if (json_object_object_get_ex(cmd, "anchor_date", &anchor_obj)) {
      int64_t anchor_ts = json_object_get_int64(anchor_obj);
      if (anchor_ts > 0) {
         conv_db_force_anchor_date_unsafe(conv_id, BENCH_MP_USER_ID, anchor_ts);
      }
   }

   fprintf(stdout, "{\"status\":\"ok\",\"conv_id\":%" PRId64 "}\n", conv_id);
   fflush(stdout);
   return 1;
}

static int handle_add_message(struct json_object *cmd) {
   struct json_object *conv_obj = NULL, *dia_obj = NULL, *role_obj = NULL, *content_obj = NULL;
   if (!json_object_object_get_ex(cmd, "conv_id", &conv_obj) ||
       !json_object_object_get_ex(cmd, "dia_id", &dia_obj) ||
       !json_object_object_get_ex(cmd, "role", &role_obj) ||
       !json_object_object_get_ex(cmd, "content", &content_obj)) {
      respond_error("add_message: missing conv_id/dia_id/role/content");
      return 1;
   }
   int64_t conv_id = json_object_get_int64(conv_obj);
   const char *dia_id = json_object_get_string(dia_obj);
   const char *role = json_object_get_string(role_obj);
   const char *content = json_object_get_string(content_obj);

   int64_t msg_id = 0;
   if (conv_db_add_message_ex(conv_id, BENCH_MP_USER_ID, role, content, &msg_id) !=
       AUTH_DB_SUCCESS) {
      respond_error("conv_db_add_message_ex failed");
      return 1;
   }
   dia_map_add(dia_id, msg_id);
   fprintf(stdout, "{\"status\":\"ok\",\"msg_id\":%" PRId64 ",\"dia_id\":\"%s\"}\n", msg_id,
           dia_id);
   fflush(stdout);
   return 1;
}

/* Lightweight COUNT(*) helper — memory_db_fact_list with max=0 evaluates as
 * SQL LIMIT 0 and always returns count=0, so we can't use it for counting. */
static int count_table_for_user(const char *table, int user_id) {
   char sql[128];
   snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE user_id = ?", table);
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_int(stmt, 1, user_id);
   int count = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      count = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return count;
}

static int handle_extract(struct json_object *cmd) {
   struct json_object *conv_obj = NULL, *sess_obj = NULL, *to_obj = NULL;
   if (!json_object_object_get_ex(cmd, "conv_id", &conv_obj) ||
       !json_object_object_get_ex(cmd, "session_id", &sess_obj)) {
      respond_error("extract: missing conv_id/session_id");
      return 1;
   }
   int64_t conv_id = json_object_get_int64(conv_obj);
   const char *session_id = json_object_get_string(sess_obj);
   int timeout_sec = BENCH_MP_TIMEOUT_SEC;
   if (json_object_object_get_ex(cmd, "timeout_ms", &to_obj)) {
      int ms = json_object_get_int(to_obj);
      if (ms > 0)
         timeout_sec = ms / 1000;
   }

   /* Optional per-session anchor override (v42).  LoCoMo conversations span
    * months across sessions, so each session's extraction needs its own anchor
    * for relative-phrase resolution.  Bench harness sends session_X_date_time
    * here; we update the conversation row before triggering extraction.
    *
    * Note: this overwrites the row's anchor_date per session — the value is
    * NOT preserved across sessions in the row.  A bench abort mid-conversation
    * leaves anchor_date set to the last-completed session.  Cache-miss reruns
    * recreate the conversation cleanly so this is benign in practice. */
   struct json_object *anchor_obj = NULL;
   if (json_object_object_get_ex(cmd, "anchor_date", &anchor_obj)) {
      int64_t anchor_ts = json_object_get_int64(anchor_obj);
      if (anchor_ts > 0) {
         conv_db_force_anchor_date_unsafe(conv_id, BENCH_MP_USER_ID, anchor_ts);
      }
   }

   int facts_before = count_table_for_user("memory_facts", BENCH_MP_USER_ID);
   int entities_before = count_table_for_user("memory_entities", BENCH_MP_USER_ID);

   int message_count = 0;
   struct json_object *history = build_history_for_conv(conv_id, &message_count);
   if (!history) {
      respond_error("build_history_for_conv failed");
      return 1;
   }

   time_t t_start = time(NULL);
   int rc = memory_trigger_extraction(BENCH_MP_USER_ID, conv_id, session_id, history, message_count,
                                      0, NULL);
   json_object_put(history);

   if (rc != 0) {
      /* Skip codes are benign (no new messages, too few). Report 0/0/0 deltas. */
      fprintf(stdout,
              "{\"status\":\"ok\",\"duration_ms\":0,\"facts_added\":0,"
              "\"entities_added\":0,\"trigger_rc\":%d}\n",
              rc);
      fflush(stdout);
      return 1;
   }

   if (wait_for_extraction(BENCH_MP_USER_ID, timeout_sec) != 0) {
      respond_error("extraction timed out");
      return 1;
   }

   int facts_after = count_table_for_user("memory_facts", BENCH_MP_USER_ID);
   int entities_after = count_table_for_user("memory_entities", BENCH_MP_USER_ID);

   long duration_ms = (long)(time(NULL) - t_start) * 1000;
   fprintf(stdout,
           "{\"status\":\"ok\",\"duration_ms\":%ld,\"facts_added\":%d,"
           "\"entities_added\":%d,\"facts_total\":%d,\"entities_total\":%d}\n",
           duration_ms, facts_after - facts_before, entities_after - entities_before, facts_after,
           entities_after);
   fflush(stdout);
   return 1;
}

static int handle_query_memory(struct json_object *cmd) {
   struct json_object *text_obj = NULL, *topk_obj = NULL;
   if (!json_object_object_get_ex(cmd, "text", &text_obj)) {
      respond_error("query_memory: missing text");
      return 1;
   }
   const char *query_text = json_object_get_string(text_obj);
   int top_k = 10;
   if (json_object_object_get_ex(cmd, "top_k", &topk_obj))
      top_k = json_object_get_int(topk_obj);
   if (top_k <= 0)
      top_k = 10;
   if (top_k > MEMORY_SEARCH_HYBRID_MAX)
      top_k = MEMORY_SEARCH_HYBRID_MAX;

   /* Delegate to the unified retrieval primitive — same code path
    * memory_action_search uses in production.  This closes the prior
    * bench-vs-production drift (different hybrid call shape, different
    * graph-merge implementation, missing score floor) flagged by the
    * May 14, 2026 architecture review. */
   memory_fact_t *facts = calloc((size_t)top_k, sizeof(*facts));
   float *scores = calloc((size_t)top_k, sizeof(*scores));
   if (!facts || !scores) {
      free(facts);
      free(scores);
      respond_error("alloc failed");
      return 1;
   }
   int n_results = 0;
   memory_search_execute(BENCH_MP_USER_ID, query_text, 0, facts, scores, top_k, &n_results);

   /* Batch-load provenance for the surviving facts. */
   int64_t *fact_ids = calloc((size_t)n_results, sizeof(int64_t));
   int64_t *prov_convs = calloc((size_t)n_results, sizeof(int64_t));
   int64_t *prov_starts = calloc((size_t)n_results, sizeof(int64_t));
   int64_t *prov_ends = calloc((size_t)n_results, sizeof(int64_t));
   if (n_results > 0 && (!fact_ids || !prov_convs || !prov_starts || !prov_ends)) {
      free(facts);
      free(scores);
      free(fact_ids);
      free(prov_convs);
      free(prov_starts);
      free(prov_ends);
      respond_error("alloc failed");
      return 1;
   }
   for (int i = 0; i < n_results; i++)
      fact_ids[i] = facts[i].id;
   if (n_results > 0)
      memory_db_facts_get_sources(BENCH_MP_USER_ID, fact_ids, n_results, prov_convs, prov_starts,
                                  prov_ends);

   /* Emit results.  covered_dia_ids[]: for each retrieved fact, every dia_id
    * whose msg_id is in [msg_start, msg_end].  Empty list if range is zeroed. */
   fprintf(stdout, "{\"status\":\"ok\",\"results\":[");
   for (int i = 0; i < n_results; i++) {
      struct json_object *text_str = json_object_new_string(facts[i].fact_text);
      const char *text_json = json_object_to_json_string(text_str);

      fprintf(stdout, "%s{\"fact_id\":%" PRId64 ",\"score\":%.6f,\"text\":%s,", i ? "," : "",
              facts[i].id, (double)scores[i], text_json);
      fprintf(stdout, "\"conv_id\":%" PRId64 ",\"msg_start\":%" PRId64 ",\"msg_end\":%" PRId64 ",",
              prov_convs[i], prov_starts[i], prov_ends[i]);
      fprintf(stdout, "\"covered_dia_ids\":[");
      bool first = true;
      if (prov_starts[i] > 0 && prov_ends[i] >= prov_starts[i]) {
         for (int64_t mid = prov_starts[i]; mid <= prov_ends[i]; mid++) {
            const char *did = dia_map_reverse(mid);
            if (!did)
               continue;
            fprintf(stdout, "%s\"%s\"", first ? "" : ",", did);
            first = false;
         }
      }
      fprintf(stdout, "]}");
      json_object_put(text_str);
   }
   fprintf(stdout, "]}\n");
   fflush(stdout);

   free(facts);
   free(scores);
   free(fact_ids);
   free(prov_convs);
   free(prov_starts);
   free(prov_ends);
   return 1;
}

/* query_graph_only {text}
 *
 * Diagnostic command for Phase 1A reachability profiling.  Returns ONLY
 * the entity-graph candidate set for the query — no hybrid retrieval,
 * no merge, no floor.  Same JSON shape as query_memory (results[] with
 * fact_id / score / text / conv_id / msg_start / msg_end /
 * covered_dia_ids) so the Python analysis can re-use the same parser.
 *
 * Used to answer "of the cat-3 misses, how many are graph-reachable
 * via fact-linked relations from query proper nouns?" — the diagnostic
 * that informs whether the right graph policy can rescue the misses.
 *
 * Reports an empty results[] when seeds is empty (no proper nouns
 * resolve to known entities).  No 'status: error' for that case —
 * it's a normal "no graph signal for this query" outcome.
 */
static int handle_query_graph_only(struct json_object *cmd) {
   struct json_object *text_obj = NULL;
   if (!json_object_object_get_ex(cmd, "text", &text_obj)) {
      respond_error("query_graph_only: missing text");
      return 1;
   }
   const char *query_text = json_object_get_string(text_obj);
   if (!query_text || !*query_text) {
      respond_error("query_graph_only: empty text");
      return 1;
   }

   int64_t seeds[MEMORY_GRAPH_MAX_SEED_CANDIDATES];
   int seed_count = 0;
   memory_graph_extract_seed_entities(BENCH_MP_USER_ID, query_text, seeds,
                                      MEMORY_GRAPH_MAX_SEED_CANDIDATES, &seed_count);

   /* Generous cap — the diagnostic wants the FULL reachable fact set,
    * not the production fan-out cap.  At LoCoMo scale (~250 facts per
    * conv) this won't exceed BENCH_MP_FACT_LIST_CAP. */
   const int cap = BENCH_MP_FACT_LIST_CAP;
   memory_fact_t *gfacts = calloc((size_t)cap, sizeof(*gfacts));
   float *gscores = calloc((size_t)cap, sizeof(*gscores));
   int gcount = 0;
   if (!gfacts || !gscores) {
      free(gfacts);
      free(gscores);
      respond_error("alloc failed");
      return 1;
   }
   if (seed_count > 0) {
      memory_graph_expand_fact_linked(BENCH_MP_USER_ID, seeds, seed_count, gfacts, gscores, cap,
                                      &gcount);
   }

   /* Provenance lookup */
   int64_t *prov_convs = calloc((size_t)gcount, sizeof(int64_t));
   int64_t *prov_starts = calloc((size_t)gcount, sizeof(int64_t));
   int64_t *prov_ends = calloc((size_t)gcount, sizeof(int64_t));
   int64_t *fact_ids = calloc((size_t)gcount, sizeof(int64_t));
   if (gcount > 0 && (!prov_convs || !prov_starts || !prov_ends || !fact_ids)) {
      free(gfacts);
      free(gscores);
      free(prov_convs);
      free(prov_starts);
      free(prov_ends);
      free(fact_ids);
      respond_error("alloc failed");
      return 1;
   }
   for (int i = 0; i < gcount; i++)
      fact_ids[i] = gfacts[i].id;
   if (gcount > 0)
      memory_db_facts_get_sources(BENCH_MP_USER_ID, fact_ids, gcount, prov_convs, prov_starts,
                                  prov_ends);

   fprintf(stdout, "{\"status\":\"ok\",\"seed_count\":%d,\"results\":[", seed_count);
   for (int i = 0; i < gcount; i++) {
      struct json_object *text_str = json_object_new_string(gfacts[i].fact_text);
      const char *text_json = json_object_to_json_string(text_str);
      fprintf(stdout, "%s{\"fact_id\":%" PRId64 ",\"score\":%.6f,\"text\":%s,", i ? "," : "",
              gfacts[i].id, (double)gscores[i], text_json);
      fprintf(stdout, "\"conv_id\":%" PRId64 ",\"msg_start\":%" PRId64 ",\"msg_end\":%" PRId64 ",",
              prov_convs[i], prov_starts[i], prov_ends[i]);
      fprintf(stdout, "\"covered_dia_ids\":[");
      bool first = true;
      if (prov_starts[i] > 0 && prov_ends[i] >= prov_starts[i]) {
         for (int64_t mid = prov_starts[i]; mid <= prov_ends[i]; mid++) {
            const char *did = dia_map_reverse(mid);
            if (!did)
               continue;
            fprintf(stdout, "%s\"%s\"", first ? "" : ",", did);
            first = false;
         }
      }
      fprintf(stdout, "]}");
      json_object_put(text_str);
   }
   fprintf(stdout, "]}\n");
   fflush(stdout);

   free(gfacts);
   free(gscores);
   free(prov_convs);
   free(prov_starts);
   free(prov_ends);
   free(fact_ids);
   return 1;
}

/* query_graph_two_hop {text}
 *
 * Diagnostic: 2-hop graph reachability ceiling.  Same response shape as
 * query_graph_only but expands the seed set to include 1-hop entity
 * neighbors before running memory_graph_expand_fact_linked.  Used to
 * measure how many cat-3 misses become reachable when retrieval walks
 * 2 hops from query-extracted seeds.
 *
 * Algorithm:
 *   1. Extract proper-noun seeds (Phase 1A path).
 *   2. For each seed, list relations as subject AND object →
 *      collect neighbor entity IDs (skip object_entity_id=0 literals).
 *   3. Union seeds + 1-hop neighbors → expanded seed set, deduplicated.
 *   4. Call memory_graph_expand_fact_linked on the expanded set.
 *   5. Return facts + covered_dia_ids in the same shape as
 *      query_graph_only, plus "expanded_seed_count" surfacing
 *      |seeds ∪ 1-hop neighbors|.
 */
#define BENCH_MP_TWO_HOP_MAX_EXPANDED_SEEDS 256
#define BENCH_MP_TWO_HOP_NEIGHBORS_PER_SEED 64

static bool expanded_set_contains(const int64_t *set, int n, int64_t id) {
   for (int i = 0; i < n; i++) {
      if (set[i] == id)
         return true;
   }
   return false;
}

static int handle_query_graph_two_hop(struct json_object *cmd) {
   struct json_object *text_obj = NULL;
   if (!json_object_object_get_ex(cmd, "text", &text_obj)) {
      respond_error("query_graph_two_hop: missing text");
      return 1;
   }
   const char *query_text = json_object_get_string(text_obj);
   if (!query_text || !*query_text) {
      respond_error("query_graph_two_hop: empty text");
      return 1;
   }

   int64_t seeds[MEMORY_GRAPH_MAX_SEED_CANDIDATES];
   int seed_count = 0;
   memory_graph_extract_seed_entities(BENCH_MP_USER_ID, query_text, seeds,
                                      MEMORY_GRAPH_MAX_SEED_CANDIDATES, &seed_count);

   /* Expand seeds: union(seeds, 1-hop entity neighbors via subject AND object). */
   int64_t expanded[BENCH_MP_TWO_HOP_MAX_EXPANDED_SEEDS];
   int expanded_n = 0;
   for (int s = 0; s < seed_count && expanded_n < BENCH_MP_TWO_HOP_MAX_EXPANDED_SEEDS; s++) {
      if (!expanded_set_contains(expanded, expanded_n, seeds[s]))
         expanded[expanded_n++] = seeds[s];
   }

   memory_relation_t *neighbors = calloc((size_t)BENCH_MP_TWO_HOP_NEIGHBORS_PER_SEED,
                                         sizeof(*neighbors));
   if (!neighbors) {
      respond_error("alloc failed");
      return 1;
   }

   for (int s = 0; s < seed_count && expanded_n < BENCH_MP_TWO_HOP_MAX_EXPANDED_SEEDS; s++) {
      int got = 0;
      /* Subject direction: this seed's outgoing edges → object entities */
      if (memory_db_relation_list_by_subject(BENCH_MP_USER_ID, seeds[s], neighbors,
                                             BENCH_MP_TWO_HOP_NEIGHBORS_PER_SEED,
                                             &got) == MEMORY_DB_SUCCESS) {
         for (int i = 0; i < got && expanded_n < BENCH_MP_TWO_HOP_MAX_EXPANDED_SEEDS; i++) {
            int64_t oid = neighbors[i].object_entity_id;
            if (oid <= 0)
               continue;
            if (!expanded_set_contains(expanded, expanded_n, oid))
               expanded[expanded_n++] = oid;
         }
      }
      /* Object direction: this seed's incoming edges → subject entities */
      got = 0;
      if (memory_db_relation_list_by_object(BENCH_MP_USER_ID, seeds[s], neighbors,
                                            BENCH_MP_TWO_HOP_NEIGHBORS_PER_SEED,
                                            &got) == MEMORY_DB_SUCCESS) {
         for (int i = 0; i < got && expanded_n < BENCH_MP_TWO_HOP_MAX_EXPANDED_SEEDS; i++) {
            int64_t sid = neighbors[i].subject_entity_id;
            if (sid <= 0)
               continue;
            if (!expanded_set_contains(expanded, expanded_n, sid))
               expanded[expanded_n++] = sid;
         }
      }
   }
   free(neighbors);

   const int cap = BENCH_MP_FACT_LIST_CAP;
   memory_fact_t *gfacts = calloc((size_t)cap, sizeof(*gfacts));
   float *gscores = calloc((size_t)cap, sizeof(*gscores));
   int gcount = 0;
   if (!gfacts || !gscores) {
      free(gfacts);
      free(gscores);
      respond_error("alloc failed");
      return 1;
   }
   if (expanded_n > 0) {
      memory_graph_expand_fact_linked(BENCH_MP_USER_ID, expanded, expanded_n, gfacts, gscores, cap,
                                      &gcount);
   }

   /* Provenance lookup (same shape as query_graph_only) */
   int64_t *prov_convs = calloc((size_t)gcount, sizeof(int64_t));
   int64_t *prov_starts = calloc((size_t)gcount, sizeof(int64_t));
   int64_t *prov_ends = calloc((size_t)gcount, sizeof(int64_t));
   int64_t *fact_ids = calloc((size_t)gcount, sizeof(int64_t));
   if (gcount > 0 && (!prov_convs || !prov_starts || !prov_ends || !fact_ids)) {
      free(gfacts);
      free(gscores);
      free(prov_convs);
      free(prov_starts);
      free(prov_ends);
      free(fact_ids);
      respond_error("alloc failed");
      return 1;
   }
   for (int i = 0; i < gcount; i++)
      fact_ids[i] = gfacts[i].id;
   if (gcount > 0)
      memory_db_facts_get_sources(BENCH_MP_USER_ID, fact_ids, gcount, prov_convs, prov_starts,
                                  prov_ends);

   fprintf(stdout, "{\"status\":\"ok\",\"seed_count\":%d,\"expanded_seed_count\":%d,\"results\":[",
           seed_count, expanded_n);
   for (int i = 0; i < gcount; i++) {
      struct json_object *text_str = json_object_new_string(gfacts[i].fact_text);
      const char *text_json = json_object_to_json_string(text_str);
      fprintf(stdout, "%s{\"fact_id\":%" PRId64 ",\"score\":%.6f,\"text\":%s,", i ? "," : "",
              gfacts[i].id, (double)gscores[i], text_json);
      fprintf(stdout, "\"conv_id\":%" PRId64 ",\"msg_start\":%" PRId64 ",\"msg_end\":%" PRId64 ",",
              prov_convs[i], prov_starts[i], prov_ends[i]);
      fprintf(stdout, "\"covered_dia_ids\":[");
      bool first = true;
      if (prov_starts[i] > 0 && prov_ends[i] >= prov_starts[i]) {
         for (int64_t mid = prov_starts[i]; mid <= prov_ends[i]; mid++) {
            const char *did = dia_map_reverse(mid);
            if (!did)
               continue;
            fprintf(stdout, "%s\"%s\"", first ? "" : ",", did);
            first = false;
         }
      }
      fprintf(stdout, "]}");
      json_object_put(text_str);
   }
   fprintf(stdout, "]}\n");
   fflush(stdout);

   free(gfacts);
   free(gscores);
   free(prov_convs);
   free(prov_starts);
   free(prov_ends);
   free(fact_ids);
   return 1;
}

/* query_memory_callback {query, with_source, source_budget}
 * Calls the production memory_action_search() path so the bench measures what
 * voice DAWN users actually receive when the LLM tool fires.  source_budget=0
 * uses the production default (MEMORY_SOURCE_BUDGET_CHARS); >0 overrides it
 * for sweep passes.  Returns the formatted memory tool output as a JSON
 * string under "text". */
static int handle_query_memory_callback(struct json_object *cmd) {
   struct json_object *query_obj = NULL, *ws_obj = NULL, *budget_obj = NULL;
   if (!json_object_object_get_ex(cmd, "query", &query_obj)) {
      respond_error("query_memory_callback: missing query");
      return 1;
   }
   const char *query_text = json_object_get_string(query_obj);
   if (!query_text || !*query_text) {
      respond_error("query_memory_callback: empty query");
      return 1;
   }

   bool with_source = false;
   if (json_object_object_get_ex(cmd, "with_source", &ws_obj))
      with_source = json_object_get_boolean(ws_obj);

   int source_budget = 0;
   if (json_object_object_get_ex(cmd, "source_budget", &budget_obj))
      source_budget = json_object_get_int(budget_obj);
   if (source_budget < 0)
      source_budget = 0;

   /* Production path: same call as memoryCallback("search", ...) takes after
    * value-string parsing.  No since_ts / category / as_of / include_historical
    * for the LoCoMo bench questions — bench uses the simplest production flow. */
   char *text = memory_action_search(BENCH_MP_USER_ID, query_text,
                                     /* since_ts */ 0,
                                     /* category */ NULL,
                                     /* as_of_ts */ 0,
                                     /* include_historical */ false, with_source, source_budget);
   if (!text) {
      respond_error("query_memory_callback: memory_action_search returned NULL");
      return 1;
   }

   struct json_object *text_str = json_object_new_string(text);
   const char *text_json = json_object_to_json_string(text_str);
   fprintf(stdout, "{\"status\":\"ok\",\"text\":%s}\n", text_json);
   fflush(stdout);
   json_object_put(text_str);
   free(text);
   return 1;
}

static int handle_reset_memory(struct json_object *cmd) {
   (void)cmd;
   /* Drop facts/preferences/relations/entities/summaries for the bench user.
    * Conversations are kept for the run; orchestrator drives reset between
    * LoCoMo conversations by creating new conv_ids — that's cheap and avoids
    * having to mass-delete via raw SQL. */
   int rc = memory_db_delete_user_memories(BENCH_MP_USER_ID);
   /* delete_user_memories now invalidates the embedding caches itself, so no
    * separate call here.  dia_map is bench-side state with no production
    * analogue. */
   dia_map_clear();
   fprintf(stdout, "{\"status\":\"ok\",\"rc\":%d}\n", rc);
   fflush(stdout);
   return 1;
}

/* =============================================================================
 * Snapshot save / load (Phase 2.1).
 *
 * Caches the post-extraction memory state (facts, entities, relations,
 * summaries, preferences, conversations, messages) plus the in-process
 * dia_id<->msg_id map.  Lets a re-run skip the expensive extraction LLM calls
 * and jump straight to query_memory.
 *
 * snapshot_save  {db_path, map_path}  → VACUUM INTO db_path; write map_path JSON
 * snapshot_load  {db_path, map_path}  → shutdown DB, copy db_path over bench
 *                                       tmpfile, re-init, load map_path JSON
 *
 * The orchestrator (Python) is responsible for choosing the cache key and
 * paths so we don't have to plumb hashing into C.  We just save/restore.
 * ============================================================================= */

static int file_exists(const char *path) {
   struct stat st;
   return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* messages table has no user_id column; count via join on conversations. */
static int count_messages_for_user(int user_id) {
   const char *sql = "SELECT COUNT(*) FROM messages m "
                     "JOIN conversations c ON c.id = m.conversation_id "
                     "WHERE c.user_id = ? AND m.kind IS NULL";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_int(stmt, 1, user_id);
   int count = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      count = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return count;
}

/* Counts per BENCH_MP_USER_ID.  Used in snapshot responses for the orchestrator
 * to verify a cache load actually populated the expected tables. */
static void counts_for_response(int *facts_out, int *entities_out, int *convs_out, int *msgs_out) {
   if (facts_out)
      *facts_out = count_table_for_user("memory_facts", BENCH_MP_USER_ID);
   if (entities_out)
      *entities_out = count_table_for_user("memory_entities", BENCH_MP_USER_ID);
   if (convs_out)
      *convs_out = count_table_for_user("conversations", BENCH_MP_USER_ID);
   if (msgs_out)
      *msgs_out = count_messages_for_user(BENCH_MP_USER_ID);
}

/* Copy file via streaming read/write.  Used by snapshot_load to overwrite the
 * bench tmpfile with the cached snapshot before re-initializing auth_db.
 * Returns 0 on success, 1 on failure. */
static int copy_file(const char *src, const char *dst) {
   FILE *in = fopen(src, "rb");
   if (!in)
      return 1;
   FILE *out = fopen(dst, "wb");
   if (!out) {
      fclose(in);
      return 1;
   }
   char buf[64 * 1024];
   size_t n;
   int rc = 0;
   while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
      if (fwrite(buf, 1, n, out) != n) {
         rc = 1;
         break;
      }
   }
   if (ferror(in))
      rc = 1;
   fclose(in);
   if (fclose(out) != 0)
      rc = 1;
   return rc;
}

/* SHA-256 of the live MEMORY_EXTRACTION_PROMPT_TEMPLATE body, lowercase hex.
 * Computed once on first call and cached.  Used by both the ready-message
 * publisher (so the Python harness can mix it into its cache key) and by the
 * snapshot save/load path (so a stale snapshot is detected at load time
 * instead of silently corrupting bench numbers).
 *
 * See benchmarks/run_benchmark.py for the matching consumer. */
const char *bench_mp_extraction_prompt_sha256(void) {
   static char hex[SHA256_DIGEST_LENGTH * 2 + 1] = { 0 };
   static int computed = 0;
   if (computed)
      return hex;
   const char *tmpl = MEMORY_EXTRACTION_PROMPT_TEMPLATE;
   size_t tmpl_len = tmpl ? strlen(tmpl) : 0;
   unsigned char digest[SHA256_DIGEST_LENGTH];
   SHA256((const unsigned char *)(tmpl ? tmpl : ""), tmpl_len, digest);
   for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
      snprintf(hex + i * 2, 3, "%02x", digest[i]);
   }
   hex[SHA256_DIGEST_LENGTH * 2] = '\0';
   computed = 1;
   return hex;
}

static int handle_snapshot_save(struct json_object *cmd) {
   struct json_object *db_obj = NULL, *map_obj = NULL;
   if (!json_object_object_get_ex(cmd, "db_path", &db_obj) ||
       !json_object_object_get_ex(cmd, "map_path", &map_obj)) {
      respond_error("snapshot_save: missing db_path/map_path");
      return 1;
   }
   const char *db_path = json_object_get_string(db_obj);
   const char *map_path = json_object_get_string(map_obj);
   if (!db_path || !*db_path || !map_path || !*map_path) {
      respond_error("snapshot_save: empty db_path/map_path");
      return 1;
   }

   /* VACUUM INTO is atomic and produces a clean single-file DB (no -wal/-shm
    * sidecars).  Force overwrite by unlinking first; SQLite returns
    * SQLITE_ERROR if the destination exists. */
   unlink(db_path);

   /* SQL-quote the path: bind args aren't allowed in VACUUM INTO. */
   sqlite3_stmt *stmt = NULL;
   const char *sql = "VACUUM INTO ?";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      respond_error("snapshot_save: prepare VACUUM INTO failed");
      return 1;
   }
   sqlite3_bind_text(stmt, 1, db_path, -1, SQLITE_STATIC);
   int step_rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   if (step_rc != SQLITE_DONE) {
      char err[256];
      snprintf(err, sizeof(err), "snapshot_save: VACUUM INTO failed: %s", sqlite3_errmsg(s_db.db));
      respond_error(err);
      return 1;
   }

   /* Persist dia_map + extraction prompt fingerprint.  Wrapper-object shape:
    *   {extraction_prompt_sha256: "<hex>",
    *    dia_map: [{dia_id, msg_id}, ...]}
    * The wrapper lets snapshot_load refuse a stale snapshot whose prompt
    * differs from the running code's prompt (otherwise the harness would
    * compare apples to oranges).  Bare-array files from older bench builds
    * still parse (back-compat in handle_snapshot_load below). */
   struct json_object *arr = json_object_new_array();
   for (int i = 0; i < s_dia_map_count; i++) {
      struct json_object *e = json_object_new_object();
      json_object_object_add(e, "dia_id", json_object_new_string(s_dia_map[i].dia_id));
      json_object_object_add(e, "msg_id", json_object_new_int64(s_dia_map[i].msg_id));
      json_object_array_add(arr, e);
   }
   struct json_object *wrapper = json_object_new_object();
   json_object_object_add(wrapper, "extraction_prompt_sha256",
                          json_object_new_string(bench_mp_extraction_prompt_sha256()));
   json_object_object_add(wrapper, "dia_map", arr); /* takes ownership */
   int wrote = json_object_to_file_ext(map_path, wrapper, JSON_C_TO_STRING_PLAIN);
   json_object_put(wrapper);
   if (wrote != 0) {
      unlink(db_path);
      respond_error("snapshot_save: write map_path failed");
      return 1;
   }

   int facts = 0, entities = 0, convs = 0, msgs = 0;
   counts_for_response(&facts, &entities, &convs, &msgs);
   fprintf(stdout,
           "{\"status\":\"ok\",\"facts\":%d,\"entities\":%d,\"conversations\":%d,"
           "\"messages\":%d,\"dia_count\":%d}\n",
           facts, entities, convs, msgs, s_dia_map_count);
   fflush(stdout);
   return 1;
}

static int handle_snapshot_load(struct json_object *cmd) {
   struct json_object *db_obj = NULL, *map_obj = NULL;
   if (!json_object_object_get_ex(cmd, "db_path", &db_obj) ||
       !json_object_object_get_ex(cmd, "map_path", &map_obj)) {
      respond_error("snapshot_load: missing db_path/map_path");
      return 1;
   }
   const char *db_path = json_object_get_string(db_obj);
   const char *map_path = json_object_get_string(map_obj);
   if (!db_path || !*db_path || !map_path || !*map_path) {
      respond_error("snapshot_load: empty db_path/map_path");
      return 1;
   }
   if (!file_exists(db_path) || !file_exists(map_path)) {
      respond_error("snapshot_load: snapshot files not found");
      return 1;
   }

   /* Pre-flight: parse map_path BEFORE the destructive teardown so we can
    * validate the extraction-prompt fingerprint and bail with no side effects
    * on mismatch.  Wrapper shape:
    *   {extraction_prompt_sha256: "<hex>", dia_map: [...]}
    * A bare array (legacy pre-fingerprint snapshots) is rejected loudly — we
    * deliberately refuse silent fall-through to re-extraction because a fresh
    * extraction costs real $ in LLM calls and the user expected a cache hit. */
   struct json_object *map_root = json_object_from_file(map_path);
   if (!map_root) {
      respond_error("snapshot_load: parse map_path failed");
      return 1;
   }
   struct json_object *dia_arr = NULL;
   const char *stored_hash = NULL;
   if (json_object_is_type(map_root, json_type_object)) {
      struct json_object *jhash = NULL, *jarr = NULL;
      if (json_object_object_get_ex(map_root, "extraction_prompt_sha256", &jhash))
         stored_hash = json_object_get_string(jhash);
      if (json_object_object_get_ex(map_root, "dia_map", &jarr) &&
          json_object_is_type(jarr, json_type_array)) {
         dia_arr = jarr;
      }
   } else if (json_object_is_type(map_root, json_type_array)) {
      /* Legacy bare-array snapshot (pre-fingerprint).  Reject loudly. */
      json_object_put(map_root);
      char err[512];
      snprintf(err, sizeof(err),
               "snapshot_load: legacy snapshot lacks extraction_prompt_sha256 "
               "(map_path=%s).  This snapshot predates the prompt-hash cache "
               "key and CANNOT be safely reused — its extracted facts may "
               "have come from a different extraction prompt.  Re-run without "
               "this cache dir, or delete the snapshot files.",
               map_path);
      respond_error(err);
      return 1;
   }
   if (!stored_hash || !dia_arr) {
      json_object_put(map_root);
      respond_error("snapshot_load: map_path missing extraction_prompt_sha256 "
                    "or dia_map");
      return 1;
   }
   const char *current_hash = bench_mp_extraction_prompt_sha256();
   if (strcmp(stored_hash, current_hash) != 0) {
      char err[768];
      snprintf(err, sizeof(err),
               "snapshot_load: extraction prompt hash mismatch.  "
               "snapshot=%s current=%s map_path=%s.  Snapshot was produced "
               "under a different extraction prompt; reusing it would compare "
               "apples to oranges.  Use a fresh --cache-dir or delete the "
               "stale snapshot files.",
               stored_hash, current_hash, map_path);
      json_object_put(map_root);
      respond_error(err);
      return 1;
   }

   /* Tear down current DB cleanly (finalizes all 43 prepared statements,
    * checkpoints WAL, closes the handle).  s_db.initialized goes false. */
   auth_db_shutdown();

   /* Replace the bench tmpfile with the cached snapshot. */
   unlink(s_bench_db_path);
   if (copy_file(db_path, s_bench_db_path) != 0) {
      respond_error("snapshot_load: copy snapshot failed");
      return 1;
   }

   /* Re-init auth_db.  This restores all prepared statements pointing at the
    * loaded data and re-applies any forward schema migrations (the cached DB
    * may be from an older bench commit). */
   if (auth_db_init(s_bench_db_path) != AUTH_DB_SUCCESS) {
      respond_error("snapshot_load: auth_db_init failed");
      return 1;
   }

   /* Invalidate in-memory fact + entity embedding caches.  Without this,
    * hybrid_search uses the previous snapshot's cached embeddings even though
    * the SQLite tables now hold the newly-loaded snapshot's facts — every
    * post-first-snapshot query returns stale results and recall collapses. */
   memory_embeddings_invalidate_all();

   /* Restore dia_map.  map_root + dia_arr were parsed and validated in the
    * pre-flight block above; dia_arr is a borrowed reference owned by
    * map_root and stays valid until we put map_root. */
   dia_map_clear();
   size_t n = json_object_array_length(dia_arr);
   for (size_t i = 0; i < n; i++) {
      struct json_object *e = json_object_array_get_idx(dia_arr, i);
      struct json_object *did = NULL, *mid = NULL;
      if (!json_object_object_get_ex(e, "dia_id", &did) ||
          !json_object_object_get_ex(e, "msg_id", &mid))
         continue;
      const char *dia_id = json_object_get_string(did);
      int64_t msg_id = json_object_get_int64(mid);
      if (dia_id && *dia_id)
         dia_map_add(dia_id, msg_id);
   }
   json_object_put(map_root);

   int facts = 0, entities = 0, convs = 0, msgs = 0;
   counts_for_response(&facts, &entities, &convs, &msgs);
   fprintf(stdout,
           "{\"status\":\"ok\",\"facts\":%d,\"entities\":%d,\"conversations\":%d,"
           "\"messages\":%d,\"dia_count\":%d}\n",
           facts, entities, convs, msgs, s_dia_map_count);
   fflush(stdout);
   return 1;
}

int bench_mp_dispatch(struct json_object *cmd) {
   struct json_object *cmd_obj = NULL;
   if (!json_object_object_get_ex(cmd, "cmd", &cmd_obj))
      return 0;
   const char *cmd_str = json_object_get_string(cmd_obj);
   if (!cmd_str)
      return 0;

   if (strcmp(cmd_str, "conv_create") == 0)
      return handle_conv_create(cmd);
   if (strcmp(cmd_str, "add_message") == 0)
      return handle_add_message(cmd);
   if (strcmp(cmd_str, "extract") == 0)
      return handle_extract(cmd);
   if (strcmp(cmd_str, "query_memory_callback") == 0)
      return handle_query_memory_callback(cmd);

   if (strcmp(cmd_str, "query_graph_only") == 0)
      return handle_query_graph_only(cmd);
   if (strcmp(cmd_str, "query_graph_two_hop") == 0)
      return handle_query_graph_two_hop(cmd);
   if (strcmp(cmd_str, "query_memory") == 0)
      return handle_query_memory(cmd);
   if (strcmp(cmd_str, "reset_memory") == 0)
      return handle_reset_memory(cmd);
   if (strcmp(cmd_str, "snapshot_save") == 0)
      return handle_snapshot_save(cmd);
   if (strcmp(cmd_str, "snapshot_load") == 0)
      return handle_snapshot_load(cmd);

   return 0;
}
