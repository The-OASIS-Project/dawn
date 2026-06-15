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
 * Unit tests for code_project_db (schema v56): create/get/list/visibility/
 * update/delete, plus FK CASCADE on user delete.
 */

#include <sqlite3.h>
#include <string.h>

#include "auth/auth_db.h"
#include "tools/code_project_db.h"
#include "unity.h"

/* From auth_db_migrations_v66.c (linked via AUTH_DB_SOURCES) — exercised directly
 * to prove the ALTERs are idempotent under the hard-gating migration ladder. */
int auth_db_migrations_v66(sqlite3 *db);

static int64_t g_alice;
static int64_t g_bob;

void setUp(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("alice", "h", true));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("bob", "h", false));
   auth_user_t u;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user("alice", &u));
   g_alice = u.id;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user("bob", &u));
   g_bob = u.id;
}

void tearDown(void) {
   auth_db_shutdown();
}

static int64_t make_project(const char *name, int64_t owner, bool global) {
   code_project_t p = { 0 };
   snprintf(p.name, sizeof(p.name), "%s", name);
   snprintf(p.source_url, sizeof(p.source_url), "https://github.com/o/%s", name);
   snprintf(p.local_path, sizeof(p.local_path), "/var/lib/dawn/source/%s", name);
   p.user_id = owner;
   p.imported_by = owner;
   p.is_global = global;
   snprintf(p.status, sizeof(p.status), "pending");
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, code_project_db_create(&p, &id));
   TEST_ASSERT_TRUE(id > 0);
   return id;
}

static void test_create_and_get(void) {
   int64_t id = make_project("repo1", g_alice, false);
   code_project_t got;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, code_project_db_get(id, &got));
   TEST_ASSERT_EQUAL_STRING("repo1", got.name);
   TEST_ASSERT_EQUAL_INT64(g_alice, got.user_id);
   TEST_ASSERT_FALSE(got.is_global);
   TEST_ASSERT_EQUAL_STRING("pending", got.status);

   code_project_t by_name;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, code_project_db_get_by_name("repo1", &by_name));
   TEST_ASSERT_EQUAL_INT64(id, by_name.id);

   code_project_t missing;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, code_project_db_get_by_name("nope", &missing));
}

static void test_visibility(void) {
   make_project("alice_priv", g_alice, false);
   make_project("shared", g_alice, true); /* global */
   make_project("bob_priv", g_bob, false);

   code_project_t list[CODE_PROJECTS_MAX];
   int count = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         code_project_db_list_visible(g_bob, list, CODE_PROJECTS_MAX, &count));
   /* bob sees his own + the global, not alice's private. */
   TEST_ASSERT_EQUAL_INT(2, count);
   bool saw_shared = false, saw_bob = false, saw_alice_priv = false;
   for (int i = 0; i < count; i++) {
      if (strcmp(list[i].name, "shared") == 0)
         saw_shared = true;
      if (strcmp(list[i].name, "bob_priv") == 0)
         saw_bob = true;
      if (strcmp(list[i].name, "alice_priv") == 0)
         saw_alice_priv = true;
   }
   TEST_ASSERT_TRUE(saw_shared);
   TEST_ASSERT_TRUE(saw_bob);
   TEST_ASSERT_FALSE(saw_alice_priv);

   bool vis = true;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, code_project_db_check_visible(g_bob, "alice_priv", &vis));
   TEST_ASSERT_FALSE(vis);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, code_project_db_check_visible(g_bob, "shared", &vis));
   TEST_ASSERT_TRUE(vis);
}

static void test_update_and_delete(void) {
   int64_t id = make_project("repo2", g_alice, false);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, code_project_db_update_status(id, "ready", "indexed ok"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, code_project_db_set_indexed_at(id, 1234567890));
   code_project_t got;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, code_project_db_get(id, &got));
   TEST_ASSERT_EQUAL_STRING("ready", got.status);
   TEST_ASSERT_EQUAL_INT64(1234567890, got.indexed_at);

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, code_project_db_set_global(id, true));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, code_project_db_get(id, &got));
   TEST_ASSERT_TRUE(got.is_global);

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, code_project_db_delete(id));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, code_project_db_get(id, &got));
}

/* Deleting the owner cascades the project away (ON DELETE CASCADE). */
static void test_cascade_on_user_delete(void) {
   int64_t id = make_project("bob_repo", g_bob, false);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_delete_user("bob"));
   code_project_t got;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, code_project_db_get(id, &got));
}

/* branch/kind/graph_name round-trip + setters; also asserts the pre-existing
 * columns still read correctly (read_row positional-index stability). */
static void test_branch_kind_graph_roundtrip(void) {
   code_project_t p = { 0 };
   snprintf(p.name, sizeof(p.name), "proj1");
   snprintf(p.source_url, sizeof(p.source_url), "https://github.com/o/proj1");
   snprintf(p.local_path, sizeof(p.local_path), "/var/lib/dawn/source/proj1");
   p.user_id = g_alice;
   p.imported_by = g_alice;
   snprintf(p.status, sizeof(p.status), "pending");
   snprintf(p.branch, sizeof(p.branch), "develop");
   snprintf(p.kind, sizeof(p.kind), "clone");
   snprintf(p.graph_name, sizeof(p.graph_name), "var-lib-dawn-source-proj1");
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, code_project_db_create(&p, &id));

   code_project_t got;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, code_project_db_get(id, &got));
   TEST_ASSERT_EQUAL_STRING("develop", got.branch);
   TEST_ASSERT_EQUAL_STRING("clone", got.kind);
   TEST_ASSERT_EQUAL_STRING("var-lib-dawn-source-proj1", got.graph_name);
   /* Old fields must still read at their original indices. */
   TEST_ASSERT_EQUAL_STRING("proj1", got.name);
   TEST_ASSERT_EQUAL_STRING("https://github.com/o/proj1", got.source_url);
   TEST_ASSERT_EQUAL_STRING("/var/lib/dawn/source/proj1", got.local_path);
   TEST_ASSERT_EQUAL_INT64(g_alice, got.user_id);

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, code_project_db_set_branch(id, "main"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, code_project_db_set_graph_name(id, "newslug"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, code_project_db_get(id, &got));
   TEST_ASSERT_EQUAL_STRING("main", got.branch);
   TEST_ASSERT_EQUAL_STRING("newslug", got.graph_name);
}

/* kind defaults to "clone" when the caller leaves it empty. */
static void test_default_kind_clone(void) {
   code_project_t p = { 0 };
   snprintf(p.name, sizeof(p.name), "defk");
   snprintf(p.source_url, sizeof(p.source_url), "https://github.com/o/defk");
   snprintf(p.local_path, sizeof(p.local_path), "/var/lib/dawn/source/defk");
   p.user_id = g_alice;
   snprintf(p.status, sizeof(p.status), "pending");
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, code_project_db_create(&p, &id));
   code_project_t got;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, code_project_db_get(id, &got));
   TEST_ASSERT_EQUAL_STRING("clone", got.kind);
}

/* An out-of-domain kind is rejected in C (no CHECK constraint). */
static void test_invalid_kind_rejected(void) {
   code_project_t p = { 0 };
   snprintf(p.name, sizeof(p.name), "badk");
   snprintf(p.source_url, sizeof(p.source_url), "https://github.com/o/badk");
   snprintf(p.local_path, sizeof(p.local_path), "/var/lib/dawn/source/badk");
   p.user_id = g_alice;
   snprintf(p.kind, sizeof(p.kind), "bogus");
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_FAILURE, code_project_db_create(&p, &id));
}

/* v66 must be re-runnable: ALTER ADD COLUMN errors on a duplicate, and the
 * migration ladder hard-gates the version bump on failure — a non-idempotent v66
 * would wedge the schema on the next boot. Build a v65-shape table, migrate twice,
 * and confirm both calls succeed, the columns exist, and a pre-existing row gets
 * the kind='clone' default. */
static void test_v66_migration_idempotent(void) {
   sqlite3 *db = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(":memory:", &db));
   const char *v65 =
       "CREATE TABLE code_projects (id INTEGER PRIMARY KEY AUTOINCREMENT,"
       "name TEXT NOT NULL UNIQUE, source_url TEXT NOT NULL, local_path TEXT NOT NULL,"
       "user_id INTEGER, is_global INTEGER NOT NULL DEFAULT 0,"
       "status TEXT NOT NULL DEFAULT 'pending', status_msg TEXT,"
       "created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL,"
       "indexed_at INTEGER, imported_by INTEGER);";
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(db, v65, NULL, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_OK,
                         sqlite3_exec(db,
                                      "INSERT INTO code_projects "
                                      "(name,source_url,local_path,created_at,updated_at) "
                                      "VALUES ('old','https://x/y','/p',0,0);",
                                      NULL, NULL, NULL));

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_migrations_v66(db));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_migrations_v66(db)); /* re-run: must not wedge */

   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(
       SQLITE_OK,
       sqlite3_prepare_v2(db, "SELECT kind, branch, graph_name FROM code_projects WHERE name='old'",
                          -1, &st, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
   TEST_ASSERT_EQUAL_STRING("clone", (const char *)sqlite3_column_text(st, 0));
   sqlite3_finalize(st);
   sqlite3_close(db);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_create_and_get);
   RUN_TEST(test_visibility);
   RUN_TEST(test_update_and_delete);
   RUN_TEST(test_cascade_on_user_delete);
   RUN_TEST(test_branch_kind_graph_roundtrip);
   RUN_TEST(test_default_kind_clone);
   RUN_TEST(test_invalid_kind_rejected);
   RUN_TEST(test_v66_migration_idempotent);
   return UNITY_END();
}
