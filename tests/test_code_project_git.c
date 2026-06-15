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
 * Unit tests for the libgit2 clone wrapper. Builds a small source repo with
 * libgit2 at runtime (no fixture binary, no git subprocess), clones it, and
 * checks the success / failure / removal paths.
 */

#define _GNU_SOURCE

#include <git2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dawn_error.h"
#include "tools/code_project_git.h"
#include "unity.h"

static char g_base[256]; /* mkdtemp scratch dir */
static char g_src[320];  /* source repo */

static void make_source_repo(const char *path) {
   git_libgit2_init();
   git_repository *repo = NULL;
   TEST_ASSERT_EQUAL_INT(0, git_repository_init(&repo, path, 0));

   char fp[400];
   snprintf(fp, sizeof(fp), "%s/README.md", path);
   FILE *f = fopen(fp, "w");
   TEST_ASSERT_NOT_NULL(f);
   fputs("hello world\n", f);
   fclose(f);

   git_index *idx = NULL;
   TEST_ASSERT_EQUAL_INT(0, git_repository_index(&idx, repo));
   TEST_ASSERT_EQUAL_INT(0, git_index_add_bypath(idx, "README.md"));
   TEST_ASSERT_EQUAL_INT(0, git_index_write(idx));
   git_oid tree_oid;
   TEST_ASSERT_EQUAL_INT(0, git_index_write_tree(&tree_oid, idx));
   git_tree *tree = NULL;
   TEST_ASSERT_EQUAL_INT(0, git_tree_lookup(&tree, repo, &tree_oid));
   git_signature *sig = NULL;
   TEST_ASSERT_EQUAL_INT(0, git_signature_now(&sig, "Test", "test@example.com"));
   git_oid commit_oid;
   TEST_ASSERT_EQUAL_INT(0, git_commit_create_v(&commit_oid, repo, "HEAD", sig, sig, NULL, "init",
                                                tree, 0));

   git_signature_free(sig);
   git_tree_free(tree);
   git_index_free(idx);
   git_repository_free(repo);
   git_libgit2_shutdown();
}

void setUp(void) {
   /* Hold a libgit2 init for the whole test (refcounted, nests with the helper
    * init/shutdown pairs) so fetch/remote ops aren't left uninitialized — mirrors
    * the daemon's code_project_git_global_init() at startup. */
   code_project_git_global_init();
   snprintf(g_base, sizeof(g_base), "/tmp/dawn_git_test_XXXXXX");
   TEST_ASSERT_NOT_NULL(mkdtemp(g_base));
   snprintf(g_src, sizeof(g_src), "%s/src", g_base);
   make_source_repo(g_src);
}

void tearDown(void) {
   code_project_git_remove(g_base);
   code_project_git_global_shutdown();
}

static int path_exists(const char *p) {
   struct stat sb;
   return stat(p, &sb) == 0;
}

static void test_clone_success(void) {
   char dst[400];
   snprintf(dst, sizeof(dst), "%s/dst", g_base);
   code_git_clone_opts_t opts = {
      .source_url = g_src,
      .local_path = dst,
      .max_size_bytes = 100 * 1024 * 1024,
      .max_file_count = 100000,
      .max_path_depth = 20,
      .clone_depth = 0,
   };
   TEST_ASSERT_EQUAL_INT(SUCCESS, code_project_git_clone(&opts));

   char readme[480];
   snprintf(readme, sizeof(readme), "%s/README.md", dst);
   TEST_ASSERT_TRUE(path_exists(readme));
}

static void test_clone_bad_url_fails(void) {
   char dst[400];
   snprintf(dst, sizeof(dst), "%s/dst_bad", g_base);
   code_git_clone_opts_t opts = {
      .source_url = "/nonexistent/definitely/not/a/repo",
      .local_path = dst,
      .max_size_bytes = 0,
      .max_file_count = 0,
      .max_path_depth = 0,
      .clone_depth = 0,
   };
   TEST_ASSERT_EQUAL_INT(FAILURE, code_project_git_clone(&opts));
}

static void test_remove(void) {
   char dst[400];
   snprintf(dst, sizeof(dst), "%s/dst_rm", g_base);
   code_git_clone_opts_t opts = { .source_url = g_src, .local_path = dst, .max_path_depth = 20 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, code_project_git_clone(&opts));
   TEST_ASSERT_TRUE(path_exists(dst));
   TEST_ASSERT_EQUAL_INT(SUCCESS, code_project_git_remove(dst));
   TEST_ASSERT_FALSE(path_exists(dst));
}

/* open_validate accepts a repo at exactly the path and (via NO_SEARCH) rejects a
 * plain dir and a subdirectory of a repo (no parent-search to an ancestor .git). */
static void test_open_validate(void) {
   TEST_ASSERT_EQUAL_INT(SUCCESS, code_project_git_open_validate(g_src));
   TEST_ASSERT_EQUAL_INT(FAILURE, code_project_git_open_validate(g_base)); /* plain dir */
   char sub[400];
   snprintf(sub, sizeof(sub), "%s/subdir", g_src);
   TEST_ASSERT_EQUAL_INT(0, mkdir(sub, 0755));
   TEST_ASSERT_EQUAL_INT(FAILURE, code_project_git_open_validate(sub)); /* NO_SEARCH */
}

/* Add a commit on a new branch @p branch (creating refs/heads/<branch>) to the
 * repo at @p repo_path, introducing @p fname so a checkout is observable. */
static void add_branch_commit(const char *repo_path, const char *branch, const char *fname) {
   /* libgit2 is already initialized for the test's lifetime by setUp()'s
    * code_project_git_global_init(); no nested init/shutdown needed here. */
   git_repository *repo = NULL;
   TEST_ASSERT_EQUAL_INT(0, git_repository_open(&repo, repo_path));

   char fp[512];
   snprintf(fp, sizeof(fp), "%s/%s", repo_path, fname);
   FILE *f = fopen(fp, "w");
   TEST_ASSERT_NOT_NULL(f);
   fputs("feature\n", f);
   fclose(f);

   git_index *idx = NULL;
   TEST_ASSERT_EQUAL_INT(0, git_repository_index(&idx, repo));
   TEST_ASSERT_EQUAL_INT(0, git_index_add_bypath(idx, fname));
   TEST_ASSERT_EQUAL_INT(0, git_index_write(idx));
   git_oid tree_oid;
   TEST_ASSERT_EQUAL_INT(0, git_index_write_tree(&tree_oid, idx));
   git_tree *tree = NULL;
   TEST_ASSERT_EQUAL_INT(0, git_tree_lookup(&tree, repo, &tree_oid));

   git_oid parent_oid;
   TEST_ASSERT_EQUAL_INT(0, git_reference_name_to_id(&parent_oid, repo, "HEAD"));
   git_commit *parent = NULL;
   TEST_ASSERT_EQUAL_INT(0, git_commit_lookup(&parent, repo, &parent_oid));
   git_signature *sig = NULL;
   TEST_ASSERT_EQUAL_INT(0, git_signature_now(&sig, "Test", "test@example.com"));

   char ref[256];
   snprintf(ref, sizeof(ref), "refs/heads/%s", branch);
   git_oid commit_oid;
   TEST_ASSERT_EQUAL_INT(0, git_commit_create_v(&commit_oid, repo, ref, sig, sig, NULL, "feat",
                                                tree, 1, parent));

   git_signature_free(sig);
   git_commit_free(parent);
   git_tree_free(tree);
   git_index_free(idx);
   git_repository_free(repo);
}

/* Clone, then fetch + check out a branch that exists only on the remote — the
 * branch-switch path. current_branch must report it and its file must appear. */
static void test_fetch_checkout_branch(void) {
   char dst[400];
   snprintf(dst, sizeof(dst), "%s/dst_fc", g_base);
   code_git_clone_opts_t co = { .source_url = g_src, .local_path = dst, .max_path_depth = 20 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, code_project_git_clone(&co));

   add_branch_commit(g_src, "feature", "FEATURE.md"); /* remote-only after the clone */

   code_git_fetch_opts_t fo = { .local_path = dst,
                                .branch = "feature",
                                .clone_depth = 0,
                                .max_path_depth = 20 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, code_project_git_fetch_checkout(&fo));

   char br[128];
   TEST_ASSERT_EQUAL_INT(SUCCESS, code_project_git_current_branch(dst, br, sizeof(br)));
   TEST_ASSERT_EQUAL_STRING("feature", br);

   char feat[480];
   snprintf(feat, sizeof(feat), "%s/FEATURE.md", dst);
   TEST_ASSERT_TRUE(path_exists(feat));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_clone_success);
   RUN_TEST(test_clone_bad_url_fails);
   RUN_TEST(test_remove);
   RUN_TEST(test_open_validate);
   RUN_TEST(test_fetch_checkout_branch);
   return UNITY_END();
}
