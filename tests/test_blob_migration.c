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
 * Regression test for the v68 blob-store migration applied to an EXISTING
 * pre-v68 database (not a fresh :memory: one).  Reproduces the boot path that
 * failed in the field: auth_db_create_schema() runs the full base SCHEMA_SQL
 * (CREATE ... IF NOT EXISTS) BEFORE migrations, so a base-schema index on the
 * migration-added documents.original_blob_id column failed on a real v67 DB
 * ("no such column: original_blob_id").  The index now lives only in the v68
 * migration; this test pins that ordering.
 */

#include <dirent.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h> /* mkdtemp */
#include <string.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "unity.h"

static char s_tmpdir[256];
static char s_dbpath[320];

/* Build a minimal v67-shaped DB inside an isolated temp dir: schema_version = 67
 * and a `documents` table WITHOUT original_blob_id (its v67 column set).
 * auth_db_init() then runs the base SCHEMA_SQL (documents CREATE is a no-op,
 * preserving this shape) followed by the v68 migration (adds the column +
 * index), backing up the v67 DB first into <tmpdir>/backups/. */
void setUp(void) {
   snprintf(s_tmpdir, sizeof(s_tmpdir), "/tmp/dawn_v67_mig_XXXXXX");
   TEST_ASSERT_NOT_NULL(mkdtemp(s_tmpdir));
   snprintf(s_dbpath, sizeof(s_dbpath), "%s/store.db", s_tmpdir);

   sqlite3 *db = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(s_dbpath, &db));
   char *err = NULL;
   int rc = sqlite3_exec(db,
                         "CREATE TABLE schema_version (version INTEGER);"
                         "INSERT INTO schema_version (version) VALUES (67);"
                         "CREATE TABLE documents ("
                         "  id INTEGER PRIMARY KEY,"
                         "  user_id INTEGER,"
                         "  filename TEXT NOT NULL,"
                         "  filepath TEXT NOT NULL,"
                         "  filetype TEXT NOT NULL,"
                         "  file_hash TEXT NOT NULL,"
                         "  num_chunks INTEGER NOT NULL,"
                         "  is_global INTEGER DEFAULT 0,"
                         "  created_at INTEGER NOT NULL);",
                         NULL, NULL, &err);
   if (rc != SQLITE_OK) {
      TEST_FAIL_MESSAGE(err ? err : "seed failed");
   }
   sqlite3_close(db);
}

void tearDown(void) {
   auth_db_shutdown();
   unlink(s_dbpath);
}

/* Locate the single backup file under <tmpdir>/backups/ (name derived from the
 * DB's basename, "store.db").  Returns true and fills @out on success. */
static bool find_backup(char *out, size_t out_size) {
   char backup_dir[300];
   snprintf(backup_dir, sizeof(backup_dir), "%s/backups", s_tmpdir);
   DIR *d = opendir(backup_dir);
   if (!d) {
      return false;
   }
   bool found = false;
   struct dirent *e;
   while ((e = readdir(d)) != NULL) {
      /* Backup names start with the live DB's basename ("store.db.v...") — proof
       * the path/name is derived, not hardcoded to "auth.db". */
      if (strncmp(e->d_name, "store.db.v", 10) == 0) {
         snprintf(out, out_size, "%s/%s", backup_dir, e->d_name);
         found = true;
         break;
      }
   }
   closedir(d);
   return found;
}

/* Returns true if @sql's single-int result is > 0. */
static bool scalar_true(sqlite3 *db, const char *sql) {
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
      return false;
   }
   bool ok = (sqlite3_step(st) == SQLITE_ROW) && (sqlite3_column_int(st, 0) > 0);
   sqlite3_finalize(st);
   return ok;
}

static void test_v67_db_migrates_to_v68(void) {
   /* The bug: auth_db_init() returned FAILURE here ("no such column:
    * original_blob_id") because base SCHEMA_SQL indexed a not-yet-migrated
    * column on the pre-existing documents table. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(s_dbpath));
   auth_db_shutdown();

   sqlite3 *db = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(s_dbpath, &db));

   /* Schema bumped to at least v68.  init always migrates to the current max
    * (later versions exist), so assert >= not == to stay robust across future
    * schema bumps.  The v68-specific effect is verified by the
    * blobs/original_blob_id/index assertions below. */
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK,
                         sqlite3_prepare_v2(db, "SELECT version FROM schema_version LIMIT 1", -1,
                                            &st, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
   TEST_ASSERT_TRUE_MESSAGE(sqlite3_column_int(st, 0) >= 68, "schema not migrated to >= v68");
   sqlite3_finalize(st);

   /* blobs table created, documents.original_blob_id added, and the index that
    * caused the failure now exists (created by the migration). */
   TEST_ASSERT_TRUE(
       scalar_true(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='blobs'"));
   TEST_ASSERT_TRUE(scalar_true(
       db, "SELECT COUNT(*) FROM pragma_table_info('documents') WHERE name='original_blob_id'"));
   TEST_ASSERT_TRUE(scalar_true(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND "
                                    "name='idx_documents_original_blob'"));

   sqlite3_close(db);

   /* A pre-upgrade backup exists, and it is the v67 (pre-migration) snapshot:
    * still version 67, no blobs table yet. */
   char backup[512];
   TEST_ASSERT_TRUE_MESSAGE(find_backup(backup, sizeof(backup)), "no pre-upgrade backup created");
   sqlite3 *bdb = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(backup, &bdb));
   sqlite3_stmt *bst = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(bdb,
                                                       "SELECT version FROM schema_version "
                                                       "LIMIT 1",
                                                       -1, &bst, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(bst));
   TEST_ASSERT_EQUAL_INT(67, sqlite3_column_int(bst, 0));
   sqlite3_finalize(bst);
   TEST_ASSERT_FALSE(
       scalar_true(bdb, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='blobs'"));
   sqlite3_close(bdb);
}

/* Re-running init against the now-v68 DB is a clean no-op (idempotent). */
static void test_v68_reinit_is_idempotent(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(s_dbpath));
   auth_db_shutdown();
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(s_dbpath));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_v67_db_migrates_to_v68);
   RUN_TEST(test_v68_reinit_is_idempotent);
   return UNITY_END();
}
