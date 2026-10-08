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
 * Unit tests for memory_db_admin (dawn-admin reextract):
 *   - reset_derived drops the five derived tables atomically
 *   - reset_derived honors keep_summaries
 *   - reset_derived resets users.categories_backfilled_at + embeddings_model_id
 *   - reset_derived resets conversations extraction high-water marks
 *   - reset_derived FK isolation: user A's drop doesn't touch user B's rows
 *   - estimate_reextract_cost uses the real prompt template size
 *   - estimate_reextract_cost rates_known=false for local provider
 *   - reextract worker concurrency guard (CAS busy flag)
 *   - wire payload encode/decode roundtrip
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/admin_socket.h"
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "dawn_error.h"
#include "memory/memory_db.h"
#include "memory/memory_db_admin.h"
#include "unity.h"

/* clang-format off */
static const char *DDL =
   "CREATE TABLE users ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  username TEXT UNIQUE NOT NULL,"
   "  categories_backfilled_at INTEGER DEFAULT 0,"
   "  embeddings_model_id TEXT DEFAULT NULL,"
   "  real_name TEXT DEFAULT NULL,"
   "  preferred_address TEXT DEFAULT NULL,"
   "  identity_aliases TEXT DEFAULT NULL"
   ");"
   "INSERT INTO users (id, username, categories_backfilled_at, embeddings_model_id) "
   "  VALUES (1, 'alice', 1234567890, 'bge-small');"
   "INSERT INTO users (id, username, categories_backfilled_at, embeddings_model_id) "
   "  VALUES (2, 'bob',   1234567890, 'bge-small');"

   "CREATE TABLE conversations ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER NOT NULL,"
   "  message_count INTEGER DEFAULT 0,"
   "  last_extracted_msg_count INTEGER DEFAULT 0,"
   "  last_extracted_msg_id    INTEGER NOT NULL DEFAULT 0,"
   "  extraction_attempts INTEGER DEFAULT 0,"
   "  extraction_last_attempt_at INTEGER DEFAULT 0,"
   "  is_private INTEGER DEFAULT 0,"
   "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
   ");"
   "INSERT INTO conversations (id, user_id, message_count, last_extracted_msg_count, "
   "  last_extracted_msg_id, extraction_attempts, extraction_last_attempt_at) "
   "  VALUES (10, 1, 50, 50, 999, 1, 100), (11, 1, 30, 10, 444, 2, 200), (12, 2, 20, 20, 555, 0, 0);"

   "CREATE TABLE memory_facts ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER NOT NULL,"
   "  fact_text TEXT NOT NULL,"
   "  category TEXT NOT NULL DEFAULT 'general',"
   "  embedding BLOB DEFAULT NULL,"
   "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
   ");"
   "INSERT INTO memory_facts (user_id, fact_text) VALUES "
   "  (1,'fact A1'),(1,'fact A2'),(1,'fact A3'),(2,'fact B1');"

   "CREATE TABLE memory_preferences ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER NOT NULL,"
   "  category TEXT NOT NULL,"
   "  value TEXT NOT NULL,"
   "  UNIQUE(user_id, category),"
   "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
   ");"
   "INSERT INTO memory_preferences (user_id, category, value) VALUES "
   "  (1,'tone','formal'),(1,'lang','en'),(2,'tone','casual');"

   "CREATE TABLE memory_summaries ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER NOT NULL,"
   "  session_id TEXT NOT NULL,"
   "  summary TEXT NOT NULL,"
   "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
   ");"
   "INSERT INTO memory_summaries (user_id, session_id, summary) VALUES "
   "  (1,'s1','sum1'),(1,'s2','sum2'),(2,'s3','sum3');"

   "CREATE TABLE memory_entities ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER NOT NULL,"
   "  name TEXT NOT NULL,"
   "  canonical_name TEXT NOT NULL,"
   "  entity_type TEXT NOT NULL,"
   "  embedding BLOB DEFAULT NULL,"
   "  UNIQUE(user_id, canonical_name),"
   "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
   ");"
   "INSERT INTO memory_entities (user_id, name, canonical_name, entity_type) VALUES "
   "  (1,'Pepper','pepper','person'),(1,'Friday','friday','assistant'),(2,'Tony','tony','person');"

   "CREATE TABLE memory_relations ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER NOT NULL,"
   "  subject_entity_id INTEGER NOT NULL,"
   "  relation TEXT NOT NULL,"
   "  object_entity_id INTEGER,"
   "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
   "  FOREIGN KEY (subject_entity_id) REFERENCES memory_entities(id) ON DELETE CASCADE,"
   "  FOREIGN KEY (object_entity_id) REFERENCES memory_entities(id) ON DELETE SET NULL"
   ");"
   "INSERT INTO memory_relations (user_id, subject_entity_id, relation, object_entity_id) VALUES "
   "  (1,1,'partner_of',2),(2,3,'employer_of',NULL);"

   /* v43 alias surface — reset_derived drops both tables.  Seed two rows
    * for user 1 (one active alias + one already-unlinked) and one row for
    * user 2 to exercise the FK-isolation invariant.  Plus one pending +
    * one resolved proposal for user 1. */
   "CREATE TABLE memory_entity_aliases ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER NOT NULL,"
   "  source_entity_id INTEGER,"
   "  target_entity_id INTEGER NOT NULL,"
   "  source_canonical_name TEXT NOT NULL,"
   "  target_canonical_name TEXT NOT NULL,"
   "  link_kind TEXT NOT NULL,"
   "  reason TEXT NOT NULL,"
   "  composite_score REAL,"
   "  evidence_json TEXT,"
   "  linked_at INTEGER NOT NULL,"
   "  consolidated_at INTEGER,"
   "  unlinked_at INTEGER,"
   "  unlink_reason TEXT,"
   "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
   ");"
   "INSERT INTO memory_entity_aliases (user_id, source_entity_id, target_entity_id, "
   "  source_canonical_name, target_canonical_name, link_kind, reason, linked_at, unlinked_at) "
   "  VALUES "
   "  (1, 1, 2, 'pepper', 'friday', 'soft', 'operator', 100, NULL),"
   "  (1, 2, 1, 'friday', 'pepper', 'soft', 'operator', 50, 60),"
   "  (2, 3, 3, 'tony', 'tony', 'soft', 'operator', 200, NULL);"

   "CREATE TABLE memory_entity_merge_proposals ("
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
   "  user_id INTEGER NOT NULL,"
   "  source_entity_id INTEGER NOT NULL,"
   "  target_entity_id INTEGER NOT NULL,"
   "  composite_score REAL NOT NULL,"
   "  evidence_json TEXT NOT NULL,"
   "  proposed_at INTEGER NOT NULL,"
   "  resolved_at INTEGER,"
   "  resolution TEXT,"
   "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
   ");"
   "INSERT INTO memory_entity_merge_proposals (user_id, source_entity_id, target_entity_id, "
   "  composite_score, evidence_json, proposed_at, resolved_at, resolution) VALUES "
   "  (1, 1, 2, 0.75, '{}', 100, NULL, NULL),"
   "  (1, 2, 1, 0.80, '{}', 110, 200, 'rejected'),"
   "  (2, 3, 3, 0.90, '{}', 300, NULL, NULL);";
/* clang-format on */

static int count_rows(const char *sql, int param) {
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      return -1;
   }
   sqlite3_bind_int(stmt, 1, param);
   int n = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      n = sqlite3_column_int(stmt, 0);
   }
   sqlite3_finalize(stmt);
   return n;
}

static int64_t scalar_int64(const char *sql, int param) {
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      return -1;
   }
   sqlite3_bind_int(stmt, 1, param);
   int64_t v = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      v = sqlite3_column_int64(stmt, 0);
   }
   sqlite3_finalize(stmt);
   return v;
}

static char *scalar_text(const char *sql, int param) {
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      return NULL;
   }
   sqlite3_bind_int(stmt, 1, param);
   char *out = NULL;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      const unsigned char *t = sqlite3_column_text(stmt, 0);
      if (t)
         out = strdup((const char *)t);
   }
   sqlite3_finalize(stmt);
   return out;
}

static void open_db(void) {
   if (sqlite3_open(":memory:", &s_db.db) != SQLITE_OK) {
      fprintf(stderr, "open failed\n");
      exit(1);
   }
   sqlite3_exec(s_db.db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
   char *err = NULL;
   if (sqlite3_exec(s_db.db, DDL, NULL, NULL, &err) != SQLITE_OK) {
      fprintf(stderr, "DDL failed: %s\n", err ? err : "?");
      sqlite3_free(err);
      exit(1);
   }
   s_db.initialized = true;
}

static void close_db(void) {
   s_db.initialized = false;
   sqlite3_close(s_db.db);
   s_db.db = NULL;
}

void setUp(void) {
   memset(&g_config, 0, sizeof(g_config));
   g_config.memory.recovery_max_attempts = 2;
   open_db();
}
void tearDown(void) {
   close_db();
}

/* ============================================================================
 * reset_derived
 * ============================================================================ */

void test_reset_derived_drops_all_five_tables_for_user_a(void) {
   memory_db_admin_reset_counts_t c = { 0 };
   TEST_ASSERT_EQUAL(SUCCESS, memory_db_admin_reset_derived(1, false, &c));
   TEST_ASSERT_EQUAL(3, c.facts_deleted);
   TEST_ASSERT_EQUAL(2, c.preferences_deleted);
   TEST_ASSERT_EQUAL(2, c.summaries_deleted);
   TEST_ASSERT_EQUAL(2, c.entities_deleted);
   TEST_ASSERT_EQUAL(1, c.relations_deleted);
   TEST_ASSERT_EQUAL(2, c.conversations_reset);
   /* v43 drop-list: both alias rows for user 1 (active + unlinked) and
    * both proposal rows (pending + resolved) get dropped — reset is
    * unconditional, not filtered by unlinked_at / resolved_at. */
   TEST_ASSERT_EQUAL(2, c.aliases_deleted);
   TEST_ASSERT_EQUAL(2, c.merge_proposals_deleted);

   /* Verify rows are gone. */
   TEST_ASSERT_EQUAL(0, count_rows("SELECT COUNT(*) FROM memory_facts WHERE user_id = ?", 1));
   TEST_ASSERT_EQUAL(0, count_rows("SELECT COUNT(*) FROM memory_preferences WHERE user_id = ?", 1));
   TEST_ASSERT_EQUAL(0, count_rows("SELECT COUNT(*) FROM memory_summaries WHERE user_id = ?", 1));
   TEST_ASSERT_EQUAL(0, count_rows("SELECT COUNT(*) FROM memory_entities WHERE user_id = ?", 1));
   TEST_ASSERT_EQUAL(0, count_rows("SELECT COUNT(*) FROM memory_relations WHERE user_id = ?", 1));
   TEST_ASSERT_EQUAL(0,
                     count_rows("SELECT COUNT(*) FROM memory_entity_aliases WHERE user_id = ?", 1));
   TEST_ASSERT_EQUAL(0, count_rows("SELECT COUNT(*) FROM memory_entity_merge_proposals "
                                   "WHERE user_id = ?",
                                   1));
}

void test_reset_derived_does_not_touch_other_users(void) {
   memory_db_admin_reset_counts_t c = { 0 };
   TEST_ASSERT_EQUAL(SUCCESS, memory_db_admin_reset_derived(1, false, &c));

   /* User 2 (bob) is untouched. */
   TEST_ASSERT_EQUAL(1, count_rows("SELECT COUNT(*) FROM memory_facts WHERE user_id = ?", 2));
   TEST_ASSERT_EQUAL(1, count_rows("SELECT COUNT(*) FROM memory_preferences WHERE user_id = ?", 2));
   TEST_ASSERT_EQUAL(1, count_rows("SELECT COUNT(*) FROM memory_summaries WHERE user_id = ?", 2));
   TEST_ASSERT_EQUAL(1, count_rows("SELECT COUNT(*) FROM memory_entities WHERE user_id = ?", 2));
   TEST_ASSERT_EQUAL(1, count_rows("SELECT COUNT(*) FROM memory_relations WHERE user_id = ?", 2));
   /* v43: user 2's alias + proposal rows survive the user-1 reset. */
   TEST_ASSERT_EQUAL(1,
                     count_rows("SELECT COUNT(*) FROM memory_entity_aliases WHERE user_id = ?", 2));
   TEST_ASSERT_EQUAL(1, count_rows("SELECT COUNT(*) FROM memory_entity_merge_proposals "
                                   "WHERE user_id = ?",
                                   2));
}

void test_reset_derived_keep_summaries_keeps_summaries(void) {
   memory_db_admin_reset_counts_t c = { 0 };
   TEST_ASSERT_EQUAL(SUCCESS, memory_db_admin_reset_derived(1, /*keep_summaries=*/true, &c));
   TEST_ASSERT_EQUAL(0, c.summaries_deleted);
   /* Summaries for user 1 are still present. */
   TEST_ASSERT_EQUAL(2, count_rows("SELECT COUNT(*) FROM memory_summaries WHERE user_id = ?", 1));
   /* But facts/prefs/entities/relations are still gone. */
   TEST_ASSERT_EQUAL(0, count_rows("SELECT COUNT(*) FROM memory_facts WHERE user_id = ?", 1));
}

void test_reset_derived_resets_user_flags(void) {
   memory_db_admin_reset_counts_t c = { 0 };
   TEST_ASSERT_EQUAL(SUCCESS, memory_db_admin_reset_derived(1, false, &c));

   TEST_ASSERT_EQUAL(0, scalar_int64("SELECT categories_backfilled_at FROM users WHERE id = ?", 1));
   char *embed = scalar_text("SELECT embeddings_model_id FROM users WHERE id = ?", 1);
   TEST_ASSERT_NULL(embed); /* NULL after reset */

   /* Other user's flags untouched. */
   TEST_ASSERT_EQUAL(1234567890,
                     scalar_int64("SELECT categories_backfilled_at FROM users WHERE id = ?", 2));
   char *embed2 = scalar_text("SELECT embeddings_model_id FROM users WHERE id = ?", 2);
   TEST_ASSERT_NOT_NULL(embed2);
   TEST_ASSERT_EQUAL_STRING("bge-small", embed2);
   free(embed2);
}

void test_reset_derived_resets_conversation_high_water_marks(void) {
   memory_db_admin_reset_counts_t c = { 0 };
   TEST_ASSERT_EQUAL(SUCCESS, memory_db_admin_reset_derived(1, false, &c));

   /* Both user 1's conversations should now have last_extracted_* = 0. */
   sqlite3_stmt *stmt = NULL;
   TEST_ASSERT_EQUAL(SQLITE_OK,
                     sqlite3_prepare_v2(s_db.db,
                                        "SELECT last_extracted_msg_count, last_extracted_msg_id, "
                                        "extraction_attempts, extraction_last_attempt_at "
                                        "FROM conversations WHERE user_id = 1 ORDER BY id",
                                        -1, &stmt, NULL));
   int rows = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW) {
      TEST_ASSERT_EQUAL(0, sqlite3_column_int(stmt, 0));
      TEST_ASSERT_EQUAL(0, sqlite3_column_int64(stmt, 1));
      TEST_ASSERT_EQUAL(0, sqlite3_column_int(stmt, 2));
      TEST_ASSERT_EQUAL(0, sqlite3_column_int64(stmt, 3));
      rows++;
   }
   sqlite3_finalize(stmt);
   TEST_ASSERT_EQUAL(2, rows);

   /* User 2's conversation untouched. */
   TEST_ASSERT_EQUAL(
       20, scalar_int64("SELECT last_extracted_msg_count FROM conversations WHERE user_id = ?", 2));
}

void test_reset_derived_invalid_user_fails(void) {
   memory_db_admin_reset_counts_t c = { 0 };
   TEST_ASSERT_EQUAL(FAILURE, memory_db_admin_reset_derived(0, false, &c));
   TEST_ASSERT_EQUAL(FAILURE, memory_db_admin_reset_derived(-1, false, &c));
   /* counts must be zeroed on failure. */
   TEST_ASSERT_EQUAL(0, c.facts_deleted);
}

/* ============================================================================
 * cost estimate
 * ============================================================================ */

void test_estimate_uses_real_template_size(void) {
   /* Configure the daemon for Claude Haiku — rates_known should be true. */
   strncpy(g_config.memory.extraction_provider, "claude",
           sizeof(g_config.memory.extraction_provider) - 1);
   strncpy(g_config.memory.extraction_model, "claude-haiku-4-5",
           sizeof(g_config.memory.extraction_model) - 1);

   memory_db_admin_cost_estimate_t est = { 0 };
   TEST_ASSERT_EQUAL(SUCCESS, memory_db_admin_estimate_reextract_cost(1, &est));
   /* Estimator sizes the POST-RESET work: every non-private user conv with
    * message_count >= 2.  User 1 has conv 10 (50 msgs) + conv 11 (30 msgs);
    * the existing extraction state of those convs is ignored — they are
    * both going to be re-extracted after the reset. */
   TEST_ASSERT_EQUAL(2, est.conv_count);
   TEST_ASSERT_EQUAL(80, est.message_count); /* 50 + 30 */
   TEST_ASSERT_TRUE(est.rates_known);
   TEST_ASSERT_EQUAL_STRING("claude", est.provider);
   TEST_ASSERT_EQUAL_STRING("claude-haiku-4-5", est.model);
   TEST_ASSERT_TRUE(est.cost_high_usd > 0.0);
   TEST_ASSERT_TRUE(est.cost_high_usd >= est.cost_low_usd);
}

void test_estimate_haiku_5_5_has_its_own_rate(void) {
   strncpy(g_config.memory.extraction_provider, "claude",
           sizeof(g_config.memory.extraction_provider) - 1);
   strncpy(g_config.memory.extraction_model, "claude-haiku-4-5",
           sizeof(g_config.memory.extraction_model) - 1);
   memory_db_admin_cost_estimate_t old_est = { 0 };
   TEST_ASSERT_EQUAL(SUCCESS, memory_db_admin_estimate_reextract_cost(1, &old_est));
   TEST_ASSERT_TRUE(old_est.cost_high_usd > 0.0);

   strncpy(g_config.memory.extraction_model, "claude-haiku-5-5",
           sizeof(g_config.memory.extraction_model) - 1);
   memory_db_admin_cost_estimate_t est = { 0 };
   TEST_ASSERT_EQUAL(SUCCESS, memory_db_admin_estimate_reextract_cost(1, &est));
   TEST_ASSERT_TRUE(est.rates_known);
   /* A tenth of Haiku 4.5's rate, not the generic Claude fallback's. */
   TEST_ASSERT_DOUBLE_WITHIN(old_est.cost_high_usd * 0.001, old_est.cost_high_usd / 10.0,
                             est.cost_high_usd);
}

void test_estimate_unknown_local_provider_marks_uncosted(void) {
   strncpy(g_config.memory.extraction_provider, "local",
           sizeof(g_config.memory.extraction_provider) - 1);
   strncpy(g_config.memory.extraction_model, "qwen3-4b",
           sizeof(g_config.memory.extraction_model) - 1);

   memory_db_admin_cost_estimate_t est = { 0 };
   TEST_ASSERT_EQUAL(SUCCESS, memory_db_admin_estimate_reextract_cost(1, &est));
   TEST_ASSERT_FALSE(est.rates_known);
   TEST_ASSERT_EQUAL_DOUBLE(0.0, est.cost_high_usd);
   TEST_ASSERT_EQUAL_DOUBLE(0.0, est.cost_low_usd);
}

void test_estimate_after_reset_includes_all_conversations(void) {
   strncpy(g_config.memory.extraction_provider, "claude",
           sizeof(g_config.memory.extraction_provider) - 1);
   strncpy(g_config.memory.extraction_model, "claude-haiku-4-5",
           sizeof(g_config.memory.extraction_model) - 1);

   /* Pre-reset estimate. */
   memory_db_admin_cost_estimate_t before = { 0 };
   TEST_ASSERT_EQUAL(SUCCESS, memory_db_admin_estimate_reextract_cost(1, &before));

   /* Reset, then estimate again — must match pre-reset because the
    * estimator is reset-state-independent (it sizes the post-reset
    * work in both cases). */
   memory_db_admin_reset_counts_t c = { 0 };
   TEST_ASSERT_EQUAL(SUCCESS, memory_db_admin_reset_derived(1, false, &c));

   memory_db_admin_cost_estimate_t after = { 0 };
   TEST_ASSERT_EQUAL(SUCCESS, memory_db_admin_estimate_reextract_cost(1, &after));
   TEST_ASSERT_EQUAL(2, after.conv_count);     /* both user 1 convs */
   TEST_ASSERT_EQUAL(80, after.message_count); /* 50 + 30 */
   TEST_ASSERT_EQUAL(before.conv_count, after.conv_count);
   TEST_ASSERT_EQUAL(before.message_count, after.message_count);
}

/* ============================================================================
 * worker concurrency
 * ============================================================================ */

void test_worker_is_running_starts_false(void) {
   TEST_ASSERT_FALSE(memory_db_admin_reextract_is_running());
}

/* ============================================================================
 * wire payload encode/decode roundtrip
 * ============================================================================ */

/* Re-implement encode on the test side so the test isn't dependent on
 * the dawn-admin client binary linking in.  This mirrors the wire format
 * defined in admin_socket.h (MEMORY_REEXTRACT payload). */
static uint16_t encode_payload(uint8_t flags,
                               const char *backup_path,
                               uint32_t max_cost_micros,
                               const char *username,
                               uint8_t *out,
                               size_t out_size) {
   size_t bplen = backup_path ? strlen(backup_path) : 0;
   size_t ulen = username ? strlen(username) : 0;
   size_t total = 1 + 2 + bplen + 4 + 1 + ulen;
   if (total > out_size)
      return 0;
   size_t off = 0;
   out[off++] = flags;
   uint16_t bplen_le = (uint16_t)bplen;
   memcpy(out + off, &bplen_le, sizeof(bplen_le));
   off += sizeof(bplen_le);
   if (bplen > 0) {
      memcpy(out + off, backup_path, bplen);
      off += bplen;
   }
   memcpy(out + off, &max_cost_micros, sizeof(max_cost_micros));
   off += sizeof(max_cost_micros);
   out[off++] = (uint8_t)ulen;
   if (ulen > 0) {
      memcpy(out + off, username, ulen);
      off += ulen;
   }
   return (uint16_t)off;
}

void test_payload_fits_under_admin_max(void) {
   uint8_t buf[ADMIN_MSG_MAX_PAYLOAD];
   /* Worst case lengths. */
   char path[ADMIN_REEXTRACT_BACKUP_PATH_MAX + 1];
   memset(path, 'a', ADMIN_REEXTRACT_BACKUP_PATH_MAX);
   path[ADMIN_REEXTRACT_BACKUP_PATH_MAX] = '\0';
   char user[ADMIN_REEXTRACT_USERNAME_MAX + 1];
   memset(user, 'u', ADMIN_REEXTRACT_USERNAME_MAX);
   user[ADMIN_REEXTRACT_USERNAME_MAX] = '\0';

   uint16_t n = encode_payload(ADMIN_REEXTRACT_FLAG_CONFIRM | ADMIN_REEXTRACT_FLAG_KEEP_SUMMARIES,
                               path, 1000000, user, buf, sizeof(buf));
   TEST_ASSERT_TRUE(n > 0);
   TEST_ASSERT_LESS_OR_EQUAL_size_t(ADMIN_MSG_MAX_PAYLOAD, n);
   /* 1 + 2 + 200 + 4 + 1 + 32 = 240 */
   TEST_ASSERT_EQUAL(240, n);
}

void test_payload_minimum_username_only(void) {
   uint8_t buf[ADMIN_MSG_MAX_PAYLOAD];
   uint16_t n = encode_payload(0, NULL, 0, "x", buf, sizeof(buf));
   TEST_ASSERT_EQUAL(1 + 2 + 0 + 4 + 1 + 1, n);
   TEST_ASSERT_EQUAL(0, buf[0]); /* flags */
   TEST_ASSERT_EQUAL(0, buf[1]); /* bplen lo */
   TEST_ASSERT_EQUAL(0, buf[2]); /* bplen hi */
   TEST_ASSERT_EQUAL(0, buf[3]); /* cost low */
   TEST_ASSERT_EQUAL(1, buf[7]); /* uname len */
   TEST_ASSERT_EQUAL('x', buf[8]);
}

/* ============================================================================
 * memory_db_undo_extraction_attempt — transient-failure rollback
 * ============================================================================ */

/* Helper: read (extraction_attempts, extraction_last_attempt_at) for a conv. */
static void read_attempt_state(int64_t conv_id, int *attempts, int64_t *last_at) {
   sqlite3_stmt *stmt = NULL;
   const char *sql = "SELECT extraction_attempts, extraction_last_attempt_at "
                     "FROM conversations WHERE id = ?";
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL));
   sqlite3_bind_int64(stmt, 1, conv_id);
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(stmt));
   if (attempts)
      *attempts = sqlite3_column_int(stmt, 0);
   if (last_at)
      *last_at = sqlite3_column_int64(stmt, 1);
   sqlite3_finalize(stmt);
}

/* Standard rollback path: conv was stamped with attempts=2, last_attempt_at=200
 * (matches the DDL seed for conv 11).  Undo drops attempts to 1 and clears
 * last_attempt_at to 0 so the recovery scan's re-eligibility predicate
 * (`extraction_last_attempt_at < updated_at`) admits the conv again. */
static void test_undo_extraction_attempt_decrements_counter(void) {
   int attempts_before = -1;
   int64_t last_at_before = -1;
   read_attempt_state(11, &attempts_before, &last_at_before);
   TEST_ASSERT_EQUAL_INT(2, attempts_before);
   TEST_ASSERT_EQUAL_INT64(200, last_at_before);

   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_undo_extraction_attempt(11));

   int attempts_after = -1;
   int64_t last_at_after = -1;
   read_attempt_state(11, &attempts_after, &last_at_after);
   TEST_ASSERT_EQUAL_INT(1, attempts_after);
   TEST_ASSERT_EQUAL_INT64(0, last_at_after);
}

/* Floor at zero: a transient failure on the very first attempt (attempts=0
 * before record_attempt stamps it to 1) followed by undo MUST land at 0,
 * not wrap to a negative.  Seed conv 12 starts at attempts=0; calling undo
 * directly on it (without a preceding stamp) verifies the MAX(0, ...). */
static void test_undo_extraction_attempt_clamps_at_zero(void) {
   int attempts_before = -1;
   read_attempt_state(12, &attempts_before, NULL);
   TEST_ASSERT_EQUAL_INT(0, attempts_before);

   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_undo_extraction_attempt(12));

   int attempts_after = -1;
   int64_t last_at_after = -1;
   read_attempt_state(12, &attempts_after, &last_at_after);
   TEST_ASSERT_EQUAL_INT(0, attempts_after);
   TEST_ASSERT_EQUAL_INT64(0, last_at_after);
}

/* Idempotency: calling undo twice in a row leaves the conv in the same
 * state as one call (counter floored, last_at zeroed). */
static void test_undo_extraction_attempt_is_idempotent(void) {
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_undo_extraction_attempt(10));
   int attempts_after_one = -1;
   read_attempt_state(10, &attempts_after_one, NULL);
   TEST_ASSERT_EQUAL_INT(0, attempts_after_one); /* seeded at 1, decremented to 0 */

   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_undo_extraction_attempt(10));
   int attempts_after_two = -1;
   int64_t last_at_after_two = -1;
   read_attempt_state(10, &attempts_after_two, &last_at_after_two);
   TEST_ASSERT_EQUAL_INT(0, attempts_after_two); /* clamp held */
   TEST_ASSERT_EQUAL_INT64(0, last_at_after_two);
}

/* ============================================================================
 * Test runner
 * ============================================================================ */

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_reset_derived_drops_all_five_tables_for_user_a);
   RUN_TEST(test_reset_derived_does_not_touch_other_users);
   RUN_TEST(test_reset_derived_keep_summaries_keeps_summaries);
   RUN_TEST(test_reset_derived_resets_user_flags);
   RUN_TEST(test_reset_derived_resets_conversation_high_water_marks);
   RUN_TEST(test_reset_derived_invalid_user_fails);
   RUN_TEST(test_estimate_uses_real_template_size);
   RUN_TEST(test_estimate_haiku_5_5_has_its_own_rate);
   RUN_TEST(test_estimate_unknown_local_provider_marks_uncosted);
   RUN_TEST(test_estimate_after_reset_includes_all_conversations);
   RUN_TEST(test_worker_is_running_starts_false);
   RUN_TEST(test_payload_fits_under_admin_max);
   RUN_TEST(test_payload_minimum_username_only);
   RUN_TEST(test_undo_extraction_attempt_decrements_counter);
   RUN_TEST(test_undo_extraction_attempt_clamps_at_zero);
   RUN_TEST(test_undo_extraction_attempt_is_idempotent);
   return UNITY_END();
}
