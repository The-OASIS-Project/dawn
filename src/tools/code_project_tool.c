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
 * Native LLM tool `code_project`: list visible projects, set the per-session
 * active project, report a project's status, and add a repository (a GitHub
 * owner/repo or an HTTPS git URL).  'add' only stages the import and reads it
 * back; nothing is cloned or indexed until the user's reply confirms it
 * ('confirm_add'), which hands it to code_project_import() (clone + code-graph
 * index on the service's worker).  Linked local checkouts, delete and global
 * imports stay with the operator (WebUI / dawn-admin).
 */

#include "tools/code_project_tool.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "auth/auth_db.h"
#include "core/pending_slots.h"
#include "core/session_manager.h"
#include "core/turn_origin.h"
#include "logging.h"
#include "tools/code_project_db.h"
#include "tools/code_project_git.h"
#include "tools/code_project_service.h"
#include "tools/tool_pending.h"
#include "tools/tool_registry.h"

static char *cp_list(int64_t uid) {
   code_project_t list[CODE_PROJECTS_MAX];
   int n = 0;
   if (code_project_db_list_visible(uid, list, CODE_PROJECTS_MAX, &n) != AUTH_DB_SUCCESS) {
      return strdup(TOOL_RESULT_ERROR_MARK "Could not list code projects.");
   }
   if (n == 0) {
      return strdup("No code projects are available to you.");
   }
   char buf[2048];
   int off = snprintf(buf, sizeof(buf), "Available code projects:\n");
   for (int i = 0; i < n && off < (int)sizeof(buf); i++) {
      off += snprintf(buf + off, sizeof(buf) - off, "- %s (%s)%s\n", list[i].name, list[i].status,
                      list[i].is_global ? " [global]" : "");
   }
   return strdup(buf);
}

/* "No such project" message that lists the caller's visible projects so the LLM
 * can self-correct (wrong name, or a stale one). Falls back to the bare message
 * if the list can't be built. */
static char *cp_no_such_project_msg(int64_t uid) {
   code_project_t list[CODE_PROJECTS_MAX];
   int n = 0;
   if (code_project_db_list_visible(uid, list, CODE_PROJECTS_MAX, &n) != AUTH_DB_SUCCESS ||
       n == 0) {
      return strdup(TOOL_RESULT_ERROR_MARK "No such project is available to you.");
   }
   char buf[2048];
   int off = snprintf(buf, sizeof(buf),
                      TOOL_RESULT_ERROR_MARK
                      "No such project is available to you. Available projects: ");
   for (int i = 0; i < n && off < (int)sizeof(buf); i++) {
      off += snprintf(buf + off, sizeof(buf) - off, "%s%s", i > 0 ? ", " : "", list[i].name);
   }
   if (off < (int)sizeof(buf)) {
      snprintf(buf + off, sizeof(buf) - off, ".");
   }
   return strdup(buf);
}

static char *cp_set_active(int64_t uid, const char *name) {
   if (name == NULL || name[0] == '\0') {
      return strdup("Specify a project name to activate.");
   }
   code_project_t p;
   if (code_project_db_get_visible_by_name(uid, name, &p) != AUTH_DB_SUCCESS) {
      return cp_no_such_project_msg(uid);
   }
   session_t *s = session_get_command_context();
   if (s == NULL) {
      return strdup(TOOL_RESULT_ERROR_MARK "No active session to set the project on.");
   }
   pthread_mutex_lock(&s->llm_config_mutex);
   s->active_project_id = p.id;
   snprintf(s->active_project_name, sizeof(s->active_project_name), "%s", p.name);
   pthread_mutex_unlock(&s->llm_config_mutex);

   char msg[160];
   snprintf(msg, sizeof(msg), "Active code project set to '%s'.", p.name);
   return strdup(msg);
}

static char *cp_status(int64_t uid, const char *name) {
   char resolved[CODE_PROJECT_NAME_MAX] = { 0 };
   if (name != NULL && name[0] != '\0') {
      snprintf(resolved, sizeof(resolved), "%s", name);
   } else {
      session_t *s = session_get_command_context();
      if (s != NULL) {
         pthread_mutex_lock(&s->llm_config_mutex);
         snprintf(resolved, sizeof(resolved), "%s", s->active_project_name);
         pthread_mutex_unlock(&s->llm_config_mutex);
      }
   }
   if (resolved[0] == '\0') {
      return strdup("No active project. Use action 'set_active' first.");
   }
   code_project_t p;
   if (code_project_db_get_visible_by_name(uid, resolved, &p) != AUTH_DB_SUCCESS) {
      return cp_no_such_project_msg(uid);
   }
   char msg[512];
   snprintf(msg, sizeof(msg), "Project '%s': status=%s%s%s", p.name, p.status,
            p.status_msg[0] ? ", " : "", p.status_msg);
   return strdup(msg);
}

/* =============================================================================
 * add / confirm_add: an import staged for the user's yes (core/pending_slots.h)
 * ============================================================================= */

#define CP_ADD_PENDING 1
#define CP_ADD_MAX_PENDING 16
#define CP_ADD_EXPIRY_SEC 300 /* as long as a reply code, when one confirms it */
#define CP_MSG_MAX 1024

typedef struct {
   pending_slot_t hdr;
   char url[CODE_PROJECT_URL_MAX];
   char name[CODE_PROJECT_NAME_MAX];
   char branch[CODE_PROJECT_BRANCH_MAX]; /* "" = the remote's default branch */
} cp_add_pending_t;

PENDING_ITEM_CHECK(cp_add_pending_t);
static cp_add_pending_t s_pending[CP_ADD_MAX_PENDING];
static pthread_mutex_t s_pending_mutex = PTHREAD_MUTEX_INITIALIZER;
PENDING_ARRAY_CHECK(s_pending);
static const pending_slots_t s_pending_slots = PENDING_SLOTS_TABLE(s_pending, CP_ADD_EXPIRY_SEC);

/* GitHub's own charsets: an owner is letters, digits and '-' (at most 39); a
 * repo letters, digits, '.', '_' and '-' (at most 100), never "." or "..". */
static bool gh_part_valid(const char *s, size_t n, bool repo) {
   if (n == 0 || n > (repo ? 100u : 39u)) {
      return false;
   }
   if (repo && ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))) {
      return false;
   }
   for (size_t i = 0; i < n; i++) {
      const unsigned char c = (unsigned char)s[i];
      if (!(isalnum(c) || c == '-' || (repo && (c == '.' || c == '_')))) {
         return false;
      }
   }
   return true;
}

/* A project name from a repo's name: DAWN's charset (code_project_import) has
 * no '.', so "socket.io" becomes "socket-io"; leading punctuation is dropped. */
static void name_from_repo(const char *repo, size_t n, char *out, size_t out_len) {
   size_t o = 0;
   for (size_t i = 0; i < n && o + 1 < out_len && o < 63; i++) {
      const unsigned char c = (unsigned char)repo[i];
      if (o == 0 && !isalnum(c)) {
         continue;
      }
      out[o++] = (isalnum(c) || c == '_' || c == '-') ? (char)c : '-';
   }
   out[o] = '\0';
}

static int hex_value(char c) {
   if (c >= '0' && c <= '9') {
      return c - '0';
   }
   c = (char)tolower((unsigned char)c);
   return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

/* Decode the @p n bytes at @p src's %XX escapes into @p out (room for @p n + 1
 * bytes; decoding never lengthens), length to @p out_n.  False for a '%'
 * without two hex digits after it, or one that decodes to NUL. */
static bool percent_decode(const char *src, size_t n, char *out, size_t *out_n) {
   size_t o = 0;
   for (size_t i = 0; i < n; i++) {
      if (src[i] != '%') {
         out[o++] = src[i];
         continue;
      }
      const int hi = i + 2 < n ? hex_value(src[i + 1]) : -1;
      const int lo = hi >= 0 ? hex_value(src[i + 2]) : -1;
      if (lo < 0 || (hi == 0 && lo == 0)) {
         return false;
      }
      out[o++] = (char)(hi * 16 + lo);
      i += 2;
   }
   out[o] = '\0';
   *out_n = o;
   return true;
}

/* The clone URL and a default project name for the repository the user named.
 * A GitHub repo in any form a person copies (owner/repo, github.com/owner/repo,
 * a web URL of any page under the repo, its HTTPS or SSH clone address) becomes
 * https://github.com/owner/repo; any other https URL is kept as given, for the
 * import's host allowlist to decide.  A GitHub web URL under /tree/ also gives
 * what follows it in @p tree ("main/src", "feature/x"): a branch, maybe with a
 * folder after it, which only the remote's branches can tell apart.  False when
 * it names no repository. */
static bool cp_resolve_repo(const char *in,
                            char *url,
                            size_t url_len,
                            char *name,
                            size_t name_len,
                            char *tree,
                            size_t tree_len) {
   tree[0] = '\0';
   static const char *const gh_prefixes[] = {
      "https://github.com/",    "http://github.com/", "https://www.github.com/",
      "http://www.github.com/", "github.com/",        "www.github.com/",
      "ssh://git@github.com/",  "git@github.com:",
   };
   while (isspace((unsigned char)*in)) {
      in++;
   }
   size_t len = strlen(in);
   while (len > 0 && isspace((unsigned char)in[len - 1])) {
      len--;
   }
   char buf[CODE_PROJECT_URL_MAX];
   if (len == 0 || len >= sizeof(buf)) {
      return false;
   }
   memcpy(buf, in, len);
   buf[len] = '\0';

   const char *rest = NULL;
   for (size_t i = 0; i < sizeof(gh_prefixes) / sizeof(gh_prefixes[0]); i++) {
      const size_t pl = strlen(gh_prefixes[i]);
      if (strncasecmp(buf, gh_prefixes[i], pl) == 0) {
         rest = buf + pl;
         break;
      }
   }
   /* Bare owner/repo: no scheme, nothing after the repo. */
   const bool shorthand = rest == NULL && strchr(buf, ':') == NULL;
   if (shorthand) {
      rest = buf;
   }

   if (rest != NULL) {
      const char *slash = strchr(rest, '/');
      if (slash == NULL) {
         return false;
      }
      const char *repo = slash + 1;
      size_t rn = strcspn(repo, "/?#");
      if (shorthand && repo[rn] != '\0' && strcmp(repo + rn, "/") != 0) {
         return false; /* "a/b/c" isn't a GitHub repo */
      }
      if (rn > 4 && strncasecmp(repo + rn - 4, ".git", 4) == 0) {
         rn -= 4;
      }
      const size_t on = (size_t)(slash - rest);
      if (!gh_part_valid(rest, on, false) || !gh_part_valid(repo, rn, true)) {
         return false;
      }
      const int n = snprintf(url, url_len, "https://github.com/%.*s/%.*s", (int)on, rest, (int)rn,
                             repo);
      if (n < 0 || (size_t)n >= url_len) {
         return false;
      }
      name_from_repo(repo, rn, name, name_len);
      static const char tree_seg[] = "/tree/";
      if (!shorthand && strncmp(repo + rn, tree_seg, sizeof(tree_seg) - 1) == 0) {
         /* Branch names are matched as git spells them, so decode the URL's
          * spelling first ("feature%2Fx" is feature/x). */
         const char *t = repo + rn + sizeof(tree_seg) - 1;
         char decoded[CODE_PROJECT_URL_MAX]; /* never longer than @p in */
         size_t tn = 0;
         if (!percent_decode(t, strcspn(t, "?#"), decoded, &tn)) {
            return false;
         }
         while (tn > 0 && decoded[tn - 1] == '/') {
            tn--;
         }
         /* Only a branch's length of it matters: with @p tree one past the
          * longest branch, a cut never ends exactly where a chosen branch does. */
         if (tn >= tree_len) {
            tn = tree_len - 1;
         }
         memcpy(tree, decoded, tn);
         tree[tn] = '\0';
      }
      return true;
   }

   /* Another host's clone URL: its last path segment names it. */
   snprintf(url, url_len, "%s", buf);
   size_t end = strcspn(buf, "?#");
   while (end > 0 && buf[end - 1] == '/') {
      end--;
   }
   if (end > 4 && strncasecmp(buf + end - 4, ".git", 4) == 0) {
      end -= 4;
   }
   size_t start = end;
   while (start > 0 && buf[start - 1] != '/') {
      start--;
   }
   name_from_repo(buf + start, end - start, name, name_len);
   return true;
}

static void cp_branch_phrase(const char *branch, bool from_url, char *out, size_t out_len) {
   if (branch[0] != '\0') {
      snprintf(out, out_len, "branch '%s'%s", branch, from_url ? ", from the URL" : "");
   } else {
      snprintf(out, out_len, "its default branch");
   }
}

/* add: check everything the import will check, confirm the repository exists,
 * and stage it.  Nothing is cloned or indexed here. */
static char *cp_add_request(int64_t uid, const char *value, const turn_origin_t *origin) {
   char given[CODE_PROJECT_URL_MAX] = "";
   char name[CODE_PROJECT_NAME_MAX] = "";
   char branch[CODE_PROJECT_BRANCH_MAX] = "";
   tool_param_extract_custom(value, "url", given, sizeof(given));
   tool_param_extract_base(value, name, sizeof(name));
   tool_param_extract_custom(value, "branch", branch, sizeof(branch));

   char url[CODE_PROJECT_URL_MAX];
   char derived[CODE_PROJECT_NAME_MAX];
   char tree[CODE_PROJECT_BRANCH_MAX + 1];
   if (!cp_resolve_repo(given, url, sizeof(url), derived, sizeof(derived), tree, sizeof(tree))) {
      return strdup(TOOL_RESULT_ERROR_MARK "Give the repository in 'url' as owner/repo (a GitHub "
                                           "repository) or as its https:// git URL.");
   }
   if (name[0] == '\0') {
      snprintf(name, sizeof(name), "%s", derived);
   }

   char msg[CP_MSG_MAX];
   const char *why = code_project_import_refusal(uid, url, name, branch);
   if (why != NULL) {
      snprintf(msg, sizeof(msg), TOOL_RESULT_ERROR_MARK "Can't add %s as '%s': %s", url, name, why);
      return strdup(msg);
   }
   /* Asking the user to approve a clone that can't happen wastes their yes:
    * a typo'd or private repository is caught here (the worker probes again).
    * A /tree/ URL with no branch given names the branch to track; looking it up
    * among the remote's branches proves the repository exists too. */
   const bool from_url = branch[0] == '\0' && tree[0] != '\0';
   const int found = from_url ? code_project_git_remote_branch_for_path(url, tree, branch,
                                                                        sizeof(branch))
                              : code_project_git_remote_probe(url);
   if (found == CODE_GIT_NO_SUCH_BRANCH ||
       (found == SUCCESS && from_url && code_project_git_branch_valid(branch) != SUCCESS)) {
      snprintf(msg, sizeof(msg),
               TOOL_RESULT_ERROR_MARK "No branch of %s matches '%s' from the URL. Ask the user "
                                      "which branch to track and give it in 'branch'.",
               url, tree);
      return strdup(msg);
   }
   if (found != SUCCESS) {
      snprintf(msg, sizeof(msg),
               TOOL_RESULT_ERROR_MARK "No public repository was found at %s (or its host isn't "
                                      "reachable). Check the owner and repository name.",
               url);
      return strdup(msg);
   }

   pending_stage_rc_t src = PENDING_STAGED;
   pthread_mutex_lock(&s_pending_mutex);
   cp_add_pending_t *p = (cp_add_pending_t *)pending_slots_stage(&s_pending_slots, origin, (int)uid,
                                                                 CP_ADD_PENDING,
                                                                 pending_slots_now(), &src);
   const uint32_t pending_id = p ? p->hdr.item_id : 0;
   if (p) {
      snprintf(p->url, sizeof(p->url), "%s", url);
      snprintf(p->name, sizeof(p->name), "%s", name);
      snprintf(p->branch, sizeof(p->branch), "%s", branch);
   }
   pthread_mutex_unlock(&s_pending_mutex);
   if (pending_id == 0) {
      return tool_pending_stage_refusal(src, "code project import");
   }

   char which[CODE_PROJECT_BRANCH_MAX + 32];
   cp_branch_phrase(branch, from_url, which, sizeof(which));
   snprintf(msg, sizeof(msg),
            "Ready to add %s (%s) as the code project '%s': DAWN will clone it and index it into "
            "the code graph, visible to this user only. Nothing has been cloned yet. Ask the user "
            "to confirm; if they say yes in their reply, call code_project with action "
            "'confirm_add' and pending_id %u (this import only).",
            url, which, name, (unsigned)pending_id);
   return strdup(msg);
}

static uint32_t cp_pending_id(const char *value) {
   char id_str[16] = "";
   if (tool_param_extract_custom(value, "pending_id", id_str, sizeof(id_str)) && id_str[0]) {
      const long long n = strtoll(id_str, NULL, 10);
      return (n > 0 && n <= UINT32_MAX) ? (uint32_t)n : 0;
   }
   return 0;
}

/* confirm_add: the user's yes to the staged import.  The import re-checks
 * everything (a project of that name may have appeared meanwhile). */
static char *cp_confirm_add(int64_t uid, const char *value, const turn_origin_t *origin) {
   const uint32_t pending_id = cp_pending_id(value);
   if (pending_id == 0) {
      return tool_pending_missing_id("code project import");
   }
   cp_add_pending_t p;
   turn_origin_rc_t orc = TURN_ORIGIN_OK;
   pthread_mutex_lock(&s_pending_mutex);
   const pending_find_rc_t rc = pending_slots_take(&s_pending_slots, origin, (int)uid,
                                                   CP_ADD_PENDING, pending_id, pending_slots_now(),
                                                   &p, sizeof(p), &orc);
   pthread_mutex_unlock(&s_pending_mutex);
   if (rc != PENDING_FOUND) {
      if (rc == PENDING_NOT_NOW) {
         OLOG_WARNING("code_project: confirm_add refused (%s)", turn_origin_refusal(orc));
      }
      return tool_pending_take_refusal(rc, orc, "code project import");
   }
   turn_origin_note_answered(session_get_command_context(), p.hdr.origin.turn_number);

   char msg[CP_MSG_MAX];
   int64_t id = 0;
   const char *why = NULL;
   if (code_project_import(uid, p.url, p.name, p.branch, false, &id, &why) != SUCCESS) {
      snprintf(msg, sizeof(msg), TOOL_RESULT_ERROR_MARK "Couldn't add '%s': %s", p.name,
               why ? why : "the import was refused.");
      return strdup(msg);
   }
   OLOG_INFO("code_project: user %lld added '%s' from %s", (long long)uid, p.name, p.url);
   snprintf(msg, sizeof(msg),
            "Adding '%s': DAWN is cloning %s and indexing it into the code graph in the "
            "background (a large repository can take several minutes). Check progress with "
            "action 'status' and name '%s'; once it's ready, 'set_active' it to ask about its "
            "code.",
            p.name, p.url, p.name);
   return strdup(msg);
}

/* What confirm_add, waiting for the user's reply code, would carry out
 * (tool_metadata_t describe_call): the import its pending_id names, in this
 * session.  Other actions take the default description. */
static int code_project_describe_call(const char *action,
                                      const char *value,
                                      char *out,
                                      size_t out_len,
                                      int *valid_for_sec) {
   if (strcmp(action, "confirm_add") != 0) {
      return TOOL_DESCRIBE_DEFAULT;
   }
   const uint32_t id = cp_pending_id(value);
   turn_origin_t origin;
   if (id == 0 || !turn_origin_capture(&origin)) {
      return FAILURE;
   }
   int n = -1;
   pthread_mutex_lock(&s_pending_mutex);
   pending_slot_t *slot = NULL;
   if (pending_slots_find(&s_pending_slots, &origin, tool_get_current_user_id(), CP_ADD_PENDING, id,
                          pending_slots_now(), &slot) == PENDING_FOUND) {
      const cp_add_pending_t *p = (const cp_add_pending_t *)slot;
      char which[CODE_PROJECT_BRANCH_MAX + 32];
      cp_branch_phrase(p->branch, false, which, sizeof(which));
      n = snprintf(out, out_len, "clone %s (%s) as the code project '%s' and index it", p->url,
                   which, p->name);
      *valid_for_sec = pending_slots_valid_for(&s_pending_slots, slot, pending_slots_now());
   }
   pthread_mutex_unlock(&s_pending_mutex);
   return (n > 0 && (size_t)n < out_len) ? SUCCESS : FAILURE;
}

static char *code_project_callback(const char *action, char *value, int *should_respond) {
   if (should_respond != NULL) {
      *should_respond = 1;
   }
   int64_t uid = tool_get_current_user_id();
   if (uid <= 0)
      return strdup(TOOL_GUEST_REFUSAL);

   if (action == NULL || action[0] == '\0' || strcmp(action, "list") == 0) {
      return cp_list(uid);
   }
   const bool adds = strcmp(action, "add") == 0 || strcmp(action, "confirm_add") == 0;
   if (adds) {
      /* Adding needs the user in a live conversation, and its confirm their
       * next turn in the session that staged it. */
      turn_origin_t origin;
      if (!turn_origin_capture(&origin)) {
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Adding a code project needs the user in a live conversation, and this "
                       "request came from a background job or an automated turn. Tell the user "
                       "which repository you would add, and let them ask for it.");
      }
      return strcmp(action, "add") == 0 ? cp_add_request(uid, value, &origin)
                                        : cp_confirm_add(uid, value, &origin);
   }

   char name[CODE_PROJECT_NAME_MAX] = "";
   tool_param_extract_base(value, name, sizeof(name));
   if (strcmp(action, "set_active") == 0) {
      return cp_set_active(uid, name);
   }
   if (strcmp(action, "status") == 0) {
      return cp_status(uid, name);
   }
   return strdup(TOOL_RESULT_ERROR_MARK "Unknown action. Use 'list', 'set_active', 'status', "
                                        "'add' or 'confirm_add'.");
}

static const treg_param_t s_params[] = {
   {
       .name = "action",
       .description = "list (show projects), set_active (switch active project), status, add "
                      "(prepare adding a repository: it is NOT cloned until the user confirms), "
                      "or confirm_add (carry out the add the user just approved)",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "list", "set_active", "status", "add", "confirm_add" },
       .enum_count = 5,
   },
   {
       .name = "name",
       .description = "Project name (required for set_active; optional for status; for add, the "
                      "name to give the new project, default the repository's name)",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
   {
       .name = "url",
       .description = "Required by add: the repository, as GitHub owner/repo (e.g. "
                      "'torvalds/linux'), a github.com URL, or an https:// git clone URL",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "url",
   },
   {
       .name = "branch",
       .description = "Optional for add: the branch to track. Without it, a github.com "
                      ".../tree/<branch> URL's branch is used, else the repository's default "
                      "branch",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "branch",
   },
   {
       .name = "pending_id",
       .description = "Required by confirm_add: the pending_id the add preview returned. Only "
                      "that import is carried out.",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "pending_id",
   },
};

static const tool_action_kind_entry_t s_meta_action_kinds[] = {
   { "list", TOOL_KIND_READ, NULL },        { "status", TOOL_KIND_READ, NULL },
   { "set_active", TOOL_KIND_STATE, NULL }, { "add", TOOL_KIND_PREPARE, "confirm_add" },
   { "confirm_add", TOOL_KIND_ACT, NULL },
};

static const tool_metadata_t s_meta = {
   .name = "code_project",
   .results = TOOL_RESULTS_OUTSIDE,
   .describe_call = code_project_describe_call,
   .action_kinds = s_meta_action_kinds,
   .action_kind_count = TOOL_KIND_COUNT(s_meta_action_kinds),
   .description = "Manage code projects: list available repos, set the active project for code "
                  "questions, check indexing status, or add a repository (a GitHub owner/repo or "
                  "git URL) so it is cloned and indexed into the code graph. Adding always asks "
                  "the user to confirm first.",
   .params = s_params,
   .param_count = TOOL_PARAM_COUNT(s_params),
   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_NONE,
   .default_local = true,
   .default_remote = true,
   .callback = code_project_callback,
};

int code_project_tool_register(void) {
   return tool_registry_register(&s_meta);
}
