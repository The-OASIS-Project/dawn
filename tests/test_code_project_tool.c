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
 * code_project 'add' / 'confirm_add': a repository is staged and read back,
 * nothing is imported until the user's next turn confirms it by its
 * pending_id, and the forms a person gives a GitHub repo in resolve to one
 * clone URL.  The import service, the DB and the session are stubbed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "core/session_manager.h"
#include "core/turn_origin.h"
#include "dawn_error.h"
#include "tools/code_project_db.h"
#include "tools/code_project_git.h"
#include "tools/code_project_service.h"
#include "tools/code_project_tool.h"
#include "tools/tool_registry.h"
#include "unity.h"

/* --- the import service ------------------------------------------------------ */

static const char *g_refusal;        /* code_project_import_refusal's answer (the preview) */
static const char *g_import_refusal; /* code_project_import's refusal reason (the confirm) */
static int g_nrefusal_checks;
static bool g_repo_exists;
static char g_probed[CODE_PROJECT_URL_MAX];
static int g_nprobes;    /* code_project_git_remote_probe calls */
static int g_nbranch_ls; /* code_project_git_remote_branch_for_path calls */
static char g_tree[128]; /* the path it was asked about */
/* The remote's branches. */
static const char *const g_branches[] = { "main", "feature/x", "release/1.0", NULL };

static int g_nimports;
static char g_imp_url[CODE_PROJECT_URL_MAX];
static char g_imp_name[CODE_PROJECT_NAME_MAX];
static char g_imp_branch[CODE_PROJECT_BRANCH_MAX];
static int64_t g_imp_user;
static bool g_imp_global;

const char *code_project_import_refusal(int64_t requester_user_id,
                                        const char *source_url,
                                        const char *desired_name,
                                        const char *branch) {
   (void)requester_user_id, (void)source_url, (void)desired_name, (void)branch;
   g_nrefusal_checks++;
   return g_refusal;
}

int code_project_import(int64_t requester_user_id,
                        const char *source_url,
                        const char *desired_name,
                        const char *branch,
                        bool global,
                        int64_t *project_id_out,
                        const char **reason_out) {
   *project_id_out = 0;
   if (reason_out) {
      *reason_out = g_import_refusal;
   }
   if (g_import_refusal) {
      return FAILURE;
   }
   g_nimports++;
   g_imp_user = requester_user_id;
   snprintf(g_imp_url, sizeof(g_imp_url), "%s", source_url);
   snprintf(g_imp_name, sizeof(g_imp_name), "%s", desired_name);
   snprintf(g_imp_branch, sizeof(g_imp_branch), "%s", branch ? branch : "");
   g_imp_global = global;
   return SUCCESS;
}

int code_project_git_remote_probe(const char *url) {
   g_nprobes++;
   snprintf(g_probed, sizeof(g_probed), "%s", url);
   return g_repo_exists ? SUCCESS : FAILURE;
}

/* As the real one: the longest of g_branches the path is, or starts with. */
int code_project_git_remote_branch_for_path(const char *url,
                                            const char *path,
                                            char *branch_out,
                                            size_t branch_len) {
   g_nbranch_ls++;
   snprintf(g_probed, sizeof(g_probed), "%s", url);
   snprintf(g_tree, sizeof(g_tree), "%s", path);
   branch_out[0] = '\0';
   if (!g_repo_exists) {
      return FAILURE;
   }
   size_t best = 0;
   for (int i = 0; g_branches[i]; i++) {
      const size_t bl = strlen(g_branches[i]);
      if (bl > best && bl < branch_len && strncmp(path, g_branches[i], bl) == 0 &&
          (path[bl] == '\0' || path[bl] == '/')) {
         snprintf(branch_out, branch_len, "%s", g_branches[i]);
         best = bl;
      }
   }
   return best ? SUCCESS : CODE_GIT_NO_SUCH_BRANCH;
}

int code_project_git_branch_valid(const char *branch) {
   return branch && branch[0] ? SUCCESS : FAILURE;
}

/* --- the project DB (list / set_active / status are not under test) ---------- */

int code_project_db_list_visible(int64_t user_id, code_project_t *out, int max, int *count) {
   (void)user_id, (void)out, (void)max;
   *count = 0;
   return AUTH_DB_SUCCESS;
}

int code_project_db_get_visible_by_name(int64_t user_id, const char *name, code_project_t *out) {
   (void)user_id, (void)name, (void)out;
   return AUTH_DB_FAILURE;
}

/* --- the session and the turn ----------------------------------------------- */

static session_t *g_session;
static uint64_t g_turn = 1;
static bool g_live = true;
static uint32_t g_answered;

session_t *session_get_command_context(void) {
   return g_session;
}
int session_effective_user_id(session_t *session) {
   (void)session;
   return 5;
}
int session_default_voice_user_id(void) {
   return 1;
}
bool scheduled_context_get(int *user_id_out) {
   (void)user_id_out;
   return false;
}
bool turn_origin_capture(turn_origin_t *out) {
   memset(out, 0, sizeof(*out));
   if (!g_live) {
      return false;
   }
   out->session_id = g_session->session_id;
   out->turn_token = g_turn;
   out->turn_number = (uint32_t)g_turn;
   return true;
}
void turn_origin_note_answered(struct session *session, uint32_t made_turn) {
   (void)session;
   g_answered = made_turn;
}

/* --- the registry ------------------------------------------------------------ */

static tool_metadata_t g_meta;
int tool_registry_register(const tool_metadata_t *metadata) {
   g_meta = *metadata;
   return 0;
}

/* --- helpers ------------------------------------------------------------------ */

static char *call(const char *action, const char *packed) {
   int respond = 0;
   char *v = packed ? strdup(packed) : NULL;
   char *out = g_meta.callback(action, v, &respond);
   free(v);
   return out;
}

static bool is_error(const char *s) {
   return s && strncmp(s, TOOL_RESULT_ERROR_MARK, strlen(TOOL_RESULT_ERROR_MARK)) == 0;
}

/* The pending_id a preview names. */
static unsigned preview_id(const char *preview) {
   static const char key[] = "pending_id ";
   const char *p = strstr(preview, key);
   return p ? (unsigned)strtoul(p + strlen(key), NULL, 10) : 0;
}

/* Stage an add for @p packed, expecting a preview; its pending_id. */
static unsigned add_ok(const char *packed) {
   char *preview = call("add", packed);
   TEST_ASSERT_FALSE_MESSAGE(is_error(preview), preview);
   const unsigned id = preview_id(preview);
   free(preview);
   TEST_ASSERT_NOT_EQUAL(0, id);
   return id;
}

static char *confirm(unsigned id) {
   char packed[48];
   snprintf(packed, sizeof(packed), "::pending_id::%u", id);
   return call("confirm_add", packed);
}

void setUp(void) {
   g_refusal = NULL;
   g_import_refusal = NULL;
   g_nrefusal_checks = 0;
   g_repo_exists = true;
   g_probed[0] = '\0';
   g_nprobes = 0;
   g_nbranch_ls = 0;
   g_tree[0] = '\0';
   g_nimports = 0;
   g_imp_url[0] = g_imp_name[0] = g_imp_branch[0] = '\0';
   g_imp_user = 0;
   g_imp_global = true;
   g_live = true;
   g_answered = 0;
   g_turn += 10; /* a fresh turn, unrelated to the last test's */
}

void tearDown(void) {
}

/* --- tests -------------------------------------------------------------------- */

/* The preview imports nothing; the yes in the next turn imports exactly what
 * was read back, owned by the user, not shared. */
static void test_add_imports_nothing_until_the_next_turn_confirms(void) {
   char *preview = call("add", "::url::The-OASIS-Project/dawn");
   TEST_ASSERT_FALSE(is_error(preview));
   TEST_ASSERT_NOT_NULL(strstr(preview, "https://github.com/The-OASIS-Project/dawn"));
   TEST_ASSERT_NOT_NULL(strstr(preview, "'dawn'"));
   TEST_ASSERT_NOT_NULL(strstr(preview, "confirm_add"));
   const unsigned id = preview_id(preview);
   free(preview);
   TEST_ASSERT_EQUAL_INT(0, g_nimports);
   const uint32_t made = (uint32_t)g_turn;

   g_turn++; /* the user's reply */
   char *done = confirm(id);
   TEST_ASSERT_FALSE_MESSAGE(is_error(done), done);
   TEST_ASSERT_NOT_NULL(strstr(done, "status"));
   free(done);
   TEST_ASSERT_EQUAL_INT(1, g_nimports);
   TEST_ASSERT_EQUAL_STRING("https://github.com/The-OASIS-Project/dawn", g_imp_url);
   TEST_ASSERT_EQUAL_STRING("dawn", g_imp_name);
   TEST_ASSERT_EQUAL_STRING("", g_imp_branch);
   TEST_ASSERT_EQUAL_INT64(5, g_imp_user);
   TEST_ASSERT_FALSE(g_imp_global);
   TEST_ASSERT_EQUAL_UINT32(made, g_answered);

   /* Consumed: a second yes imports nothing more. */
   char *again = confirm(id);
   TEST_ASSERT_TRUE(is_error(again));
   free(again);
   TEST_ASSERT_EQUAL_INT(1, g_nimports);
}

/* A confirm in the turn that staged it, or naming another id, imports nothing
 * and leaves the staged import for the right one. */
static void test_confirm_needs_the_next_turn_and_the_id(void) {
   const unsigned id = add_ok("::url::o/r");
   char *same = confirm(id);
   TEST_ASSERT_TRUE(is_error(same));
   free(same);
   g_turn++;
   char *other = confirm(id + 1);
   TEST_ASSERT_TRUE(is_error(other));
   free(other);
   char *none = call("confirm_add", NULL);
   TEST_ASSERT_TRUE(is_error(none));
   free(none);
   TEST_ASSERT_EQUAL_INT(0, g_nimports);
   char *right = confirm(id);
   TEST_ASSERT_FALSE(is_error(right));
   free(right);
   TEST_ASSERT_EQUAL_INT(1, g_nimports);
}

/* A yes two turns later is not the reply to that preview. */
static void test_confirm_after_the_conversation_moved_on(void) {
   const unsigned id = add_ok("::url::o/r");
   g_turn += 2;
   char *late = confirm(id);
   TEST_ASSERT_TRUE(is_error(late));
   free(late);
   TEST_ASSERT_EQUAL_INT(0, g_nimports);
}

/* Each form a person copies a GitHub repo in clones the same URL; the name
 * comes from the repo, in DAWN's charset. */
static void test_github_forms_resolve_to_one_clone_url(void) {
   static const struct {
      const char *given;
      const char *url;
      const char *name;
   } cases[] = {
      { "o/r", "https://github.com/o/r", "r" },
      { "  o/r/  ", "https://github.com/o/r", "r" },
      { "https://github.com/o/r.git", "https://github.com/o/r", "r" },
      { "https://github.com/o/socket.io/tree/main/src", "https://github.com/o/socket.io",
        "socket-io" },
      { "http://www.github.com/o/r?tab=readme", "https://github.com/o/r", "r" },
      { "github.com/o/r", "https://github.com/o/r", "r" },
      { "git@github.com:o/r.git", "https://github.com/o/r", "r" },
      { "https://github.com/o/.github", "https://github.com/o/.github", "github" },
      { "https://gitlab.com/group/proj.git", "https://gitlab.com/group/proj.git", "proj" },
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      setUp();
      char packed[160];
      snprintf(packed, sizeof(packed), "::url::%s", cases[i].given);
      const unsigned id = add_ok(packed);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(cases[i].url, g_probed, cases[i].given);
      g_turn++;
      free(confirm(id));
      TEST_ASSERT_EQUAL_STRING_MESSAGE(cases[i].url, g_imp_url, cases[i].given);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(cases[i].name, g_imp_name, cases[i].given);
   }
}

/* What names no repository is refused before anything is probed or staged. */
static void test_what_names_no_repo_is_refused(void) {
   static const char *const bad[] = {
      "::url::",      "::url::dawn",
      "::url::a/b/c", "::url::gitlab.com/g/p",
      "::url::o/r e", "::url::https://github.com/o",
      "::url::o/..",  NULL,
   };
   for (int i = 0; bad[i]; i++) {
      char *r = call("add", bad[i]);
      TEST_ASSERT_TRUE_MESSAGE(is_error(r), bad[i]);
      free(r);
   }
   char *r = call("add", NULL);
   TEST_ASSERT_TRUE(is_error(r));
   free(r);
   TEST_ASSERT_EQUAL_STRING("", g_probed);
}

/* A name and branch the user gives are what is imported. */
static void test_name_and_branch_are_kept(void) {
   const unsigned id = add_ok("mine::url::o/r::branch::dev");
   g_turn++;
   free(confirm(id));
   TEST_ASSERT_EQUAL_STRING("mine", g_imp_name);
   TEST_ASSERT_EQUAL_STRING("dev", g_imp_branch);
}

/* What the import would refuse (an admin-only DAWN, a taken name) is told at
 * the preview, which stages nothing. */
static void test_a_refused_import_is_never_staged(void) {
   g_refusal = "Adding code projects is restricted to administrators on this DAWN.";
   char *r = call("add", "::url::o/r");
   TEST_ASSERT_TRUE(is_error(r));
   TEST_ASSERT_NOT_NULL(strstr(r, "administrators"));
   free(r);
   TEST_ASSERT_EQUAL_STRING("", g_probed);
   g_turn++;
   for (unsigned id = 1; id < 64; id++) {
      free(confirm(id));
   }
   TEST_ASSERT_EQUAL_INT(0, g_nimports);
}

/* A repository that isn't there is told at the preview, not after the yes. */
static void test_a_missing_repo_is_never_staged(void) {
   g_repo_exists = false;
   char *r = call("add", "::url::o/typo");
   TEST_ASSERT_TRUE(is_error(r));
   TEST_ASSERT_NOT_NULL(strstr(r, "https://github.com/o/typo"));
   free(r);
}

/* Refused at the confirm (a project of that name appeared meanwhile): told,
 * not imported. */
static void test_confirm_rechecks_the_import(void) {
   const unsigned id = add_ok("::url::o/r");
   const int checks = g_nrefusal_checks;
   g_import_refusal = "A code project with that name already exists.";
   g_turn++;
   char *r = confirm(id);
   TEST_ASSERT_TRUE(is_error(r));
   TEST_ASSERT_NOT_NULL(strstr(r, "already exists")); /* the import's own reason */
   free(r);
   TEST_ASSERT_EQUAL_INT(0, g_nimports);
   /* The confirm asks the import once; it doesn't check separately first. */
   TEST_ASSERT_EQUAL_INT(checks, g_nrefusal_checks);
}

/* A /tree/ URL with no branch given tracks the branch it names: the longest
 * of the remote's branches the path starts with, so a branch with a '/' and a
 * folder under a branch both resolve.  The read-back names it. */
static void test_tree_url_names_the_branch(void) {
   static const struct {
      const char *given;
      const char *branch;
   } cases[] = {
      { "https://github.com/o/r/tree/feature/x", "feature/x" },
      { "https://github.com/o/r/tree/main/src/lib/", "main" },
      { "https://github.com/o/r/tree/release/1.0/docs?plain=1", "release/1.0" },
      { "github.com/o/r/tree/main", "main" },
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      setUp();
      char packed[160];
      snprintf(packed, sizeof(packed), "::url::%s", cases[i].given);
      char *preview = call("add", packed);
      TEST_ASSERT_FALSE_MESSAGE(is_error(preview), preview);
      char named[64];
      snprintf(named, sizeof(named), "branch '%s', from the URL", cases[i].branch);
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(preview, named), preview);
      const unsigned id = preview_id(preview);
      free(preview);
      TEST_ASSERT_EQUAL_STRING("https://github.com/o/r", g_probed);
      TEST_ASSERT_EQUAL_INT(0, g_nprobes); /* the branch lookup proved it exists */
      g_turn++;
      free(confirm(id));
      TEST_ASSERT_EQUAL_STRING_MESSAGE(cases[i].branch, g_imp_branch, cases[i].given);
      TEST_ASSERT_EQUAL_STRING("https://github.com/o/r", g_imp_url);
   }
}

/* A /tree/ path is matched as git spells branch names: its %XX escapes are
 * decoded once first, an encoded '/' included. */
static void test_tree_url_branch_is_percent_decoded(void) {
   static const struct {
      const char *given;
      const char *path; /* what the remote's branches are matched against */
      const char *branch;
   } cases[] = {
      { "https://github.com/o/r/tree/feature%2Fx/src", "feature/x/src", "feature/x" },
      { "https://github.com/o/r/tree/feature%2fx", "feature/x", "feature/x" },
      { "https://github.com/o/r/tree/release%2F1.0%2F", "release/1.0", "release/1.0" },
      { "https://github.com/o/r/tree/main/src%20dir", "main/src dir", "main" },
      /* Decoded once: "%252F" is a literal "%2F", not a '/'. */
      { "https://github.com/o/r/tree/main/a%252Fb", "main/a%2Fb", "main" },
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      setUp();
      char packed[160];
      snprintf(packed, sizeof(packed), "::url::%s", cases[i].given);
      const unsigned id = add_ok(packed);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(cases[i].path, g_tree, cases[i].given);
      g_turn++;
      free(confirm(id));
      TEST_ASSERT_EQUAL_STRING_MESSAGE(cases[i].branch, g_imp_branch, cases[i].given);
   }
}

/* A malformed escape, or one that decodes to NUL, names no branch: refused
 * before the remote is asked, with nothing staged. */
static void test_tree_url_bad_percent_encoding_is_refused(void) {
   static const char *const bad[] = {
      "::url::https://github.com/o/r/tree/feature%2",    /* cut short */
      "::url::https://github.com/o/r/tree/feature%",     /* bare '%' */
      "::url::https://github.com/o/r/tree/feature%zz/x", /* not hex */
      "::url::https://github.com/o/r/tree/feature%g1",   /* first digit not hex */
      "::url::https://github.com/o/r/tree/main%00/src",  /* encoded NUL */
      NULL,
   };
   for (int i = 0; bad[i]; i++) {
      char *r = call("add", bad[i]);
      TEST_ASSERT_TRUE_MESSAGE(is_error(r), bad[i]);
      free(r);
   }
   TEST_ASSERT_EQUAL_INT(0, g_nbranch_ls);
   TEST_ASSERT_EQUAL_INT(0, g_nprobes);
   g_turn++;
   for (unsigned id = 1; id < 64; id++) {
      free(confirm(id));
   }
   TEST_ASSERT_EQUAL_INT(0, g_nimports);
}

/* An explicit branch wins over the URL's, and the remote isn't asked about it. */
static void test_explicit_branch_beats_the_url(void) {
   char *preview = call("add", "::url::https://github.com/o/r/tree/feature/x::branch::dev");
   TEST_ASSERT_FALSE(is_error(preview));
   TEST_ASSERT_NOT_NULL(strstr(preview, "(branch 'dev')"));
   const unsigned id = preview_id(preview);
   free(preview);
   TEST_ASSERT_EQUAL_INT(0, g_nbranch_ls);
   TEST_ASSERT_EQUAL_INT(1, g_nprobes);
   g_turn++;
   free(confirm(id));
   TEST_ASSERT_EQUAL_STRING("dev", g_imp_branch);
}

/* A /tree/ path no branch matches is told, with nothing staged; an
 * unreachable repo still reads as missing. */
static void test_tree_url_without_a_matching_branch(void) {
   char *r = call("add", "::url::https://github.com/o/r/tree/gone/src");
   TEST_ASSERT_TRUE(is_error(r));
   TEST_ASSERT_NOT_NULL(strstr(r, "'gone/src'"));
   TEST_ASSERT_NOT_NULL(strstr(r, "'branch'"));
   free(r);
   g_repo_exists = false;
   r = call("add", "::url::https://github.com/o/r/tree/main");
   TEST_ASSERT_TRUE(is_error(r));
   TEST_ASSERT_NOT_NULL(strstr(r, "No public repository"));
   free(r);
   g_turn++;
   for (unsigned id = 1; id < 64; id++) {
      free(confirm(id));
   }
   TEST_ASSERT_EQUAL_INT(0, g_nimports);
}

/* Other GitHub pages (issues, blob) aren't a branch: the default branch. */
static void test_other_pages_take_the_default_branch(void) {
   const unsigned id = add_ok("::url::https://github.com/o/r/issues/12");
   TEST_ASSERT_EQUAL_INT(0, g_nbranch_ls);
   g_turn++;
   free(confirm(id));
   TEST_ASSERT_EQUAL_STRING("", g_imp_branch);
}

/* Not from a live conversation (a job, an automated turn): neither stages
 * nor confirms. */
static void test_needs_a_live_conversation(void) {
   g_live = false;
   char *r = call("add", "::url::o/r");
   TEST_ASSERT_TRUE(is_error(r));
   free(r);
   TEST_ASSERT_EQUAL_STRING("", g_probed);
}

/* The reply-code text names what the confirm clones, from the staged item. */
static void test_describe_confirm_add(void) {
   const unsigned id = add_ok("::url::o/r::branch::dev");
   g_turn++;
   char packed[48];
   snprintf(packed, sizeof(packed), "::pending_id::%u", id);
   char out[256];
   int valid = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         g_meta.describe_call("confirm_add", packed, out, sizeof(out), &valid));
   TEST_ASSERT_NOT_NULL(strstr(out, "https://github.com/o/r"));
   TEST_ASSERT_NOT_NULL(strstr(out, "'dev'"));
   TEST_ASSERT_GREATER_THAN_INT(0, valid);
   snprintf(packed, sizeof(packed), "::pending_id::%u", id + 1);
   TEST_ASSERT_EQUAL_INT(FAILURE,
                         g_meta.describe_call("confirm_add", packed, out, sizeof(out), &valid));
   TEST_ASSERT_EQUAL_INT(TOOL_DESCRIBE_DEFAULT,
                         g_meta.describe_call("add", "::url::o/r", out, sizeof(out), &valid));
}

/* add is a preview its confirm carries out (the registry's table). */
static void test_kinds(void) {
   bool add = false, confirm_add = false;
   for (int i = 0; i < g_meta.action_kind_count; i++) {
      const tool_action_kind_entry_t *e = &g_meta.action_kinds[i];
      if (strcmp(e->action, "add") == 0) {
         add = e->kind == TOOL_KIND_PREPARE && strcmp(e->confirm, "confirm_add") == 0;
      } else if (strcmp(e->action, "confirm_add") == 0) {
         confirm_add = e->kind == TOOL_KIND_ACT;
      }
   }
   TEST_ASSERT_TRUE(add);
   TEST_ASSERT_TRUE(confirm_add);
}

int main(void) {
   g_session = calloc(1, sizeof(session_t));
   g_session->type = SESSION_TYPE_WEBUI;
   g_session->session_id = 7;
   TEST_ASSERT_EQUAL_INT(0, code_project_tool_register());
   UNITY_BEGIN();
   RUN_TEST(test_add_imports_nothing_until_the_next_turn_confirms);
   RUN_TEST(test_confirm_needs_the_next_turn_and_the_id);
   RUN_TEST(test_confirm_after_the_conversation_moved_on);
   RUN_TEST(test_github_forms_resolve_to_one_clone_url);
   RUN_TEST(test_what_names_no_repo_is_refused);
   RUN_TEST(test_name_and_branch_are_kept);
   RUN_TEST(test_a_refused_import_is_never_staged);
   RUN_TEST(test_a_missing_repo_is_never_staged);
   RUN_TEST(test_confirm_rechecks_the_import);
   RUN_TEST(test_tree_url_names_the_branch);
   RUN_TEST(test_tree_url_branch_is_percent_decoded);
   RUN_TEST(test_tree_url_bad_percent_encoding_is_refused);
   RUN_TEST(test_explicit_branch_beats_the_url);
   RUN_TEST(test_tree_url_without_a_matching_branch);
   RUN_TEST(test_other_pages_take_the_default_branch);
   RUN_TEST(test_needs_a_live_conversation);
   RUN_TEST(test_describe_confirm_add);
   RUN_TEST(test_kinds);
   const int rc = UNITY_END();
   free(g_session);
   return rc;
}
