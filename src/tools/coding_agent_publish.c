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
 * A finished coding job's work leaving the gateway: publish pushes its branch
 * (and opens a pull request) on the user's yes to a preview DAWN renders, bound
 * to the commit it showed; close drops the work once the user is done with it,
 * at once when nothing would be lost, else on a yes to what would be.
 */

#include <ctype.h>
#include <json-c/json.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "core/pending_slots.h"
#include "core/session_manager.h"
#include "core/strbuf.h"
#include "core/tool_call_challenge.h"
#include "core/turn_origin.h"
#include "dawn_error.h"
#include "llm/llm_context_text.h"
#include "llm/llm_third_party.h"
#include "tools/coding_agent_gateway.h"
#include "tools/coding_agent_internal.h"
#include "tools/tool_pending.h"
#include "tools/tool_registry.h"
#include "utils/string_utils.h"

#define CP_PENDING_PUBLISH 1
#define CP_PENDING_CLOSE 2
#define CP_PENDING_TTL_SEC 600
#define CP_CONFIRM_TURNS 3
#define CP_PENDING_MAX 8
#define CP_HEAD_LEN 40
#define CP_BRANCH_MAX 160
#define CP_URL_MAX 200
#define CP_AGENT_TEXT_MAX 3000 /* the agent's commit subjects and paths in a preview */
#define CP_LINES_SHOWN 20      /* of each */
#define CP_SUMMARY_SHOWN 1500  /* the agent's summary (the pull request's description) */
#define CP_TASK_ID_LEN 8       /* the gateway's task ids: 8 hex digits */
/* 'coding job #N ("name")': the name plus 12 + 20 digits + 4 + NUL. */
#define CP_LABEL_MAX (CONV_TITLE_MAX + 40)

typedef struct {
   pending_slot_t hdr;
   int64_t job;
   char head[CP_HEAD_LEN + 1]; /* the commit the preview showed */
   char name[CONV_TITLE_MAX];
   char repo[CODING_AGENT_REPO_MAX];
   char branch[CP_BRANCH_MAX];
   char base[CP_BRANCH_MAX];
   char task[CP_TASK_ID_LEN + 1]; /* close of a task with no DAWN job: its id ("" for a job) */
   char label[CP_LABEL_MAX];      /* close: 'coding job #N ("name")' or 'task <id>' */
   int commits;                   /* publish: commits on the branch; close: unpublished ones */
   int files;
   int protected_paths;
   bool pr;
} cp_pending_t;

PENDING_ITEM_CHECK(cp_pending_t);
_Static_assert(CP_CONFIRM_TURNS <= TURN_ORIGIN_UNASKED_QUIET_TURNS,
               "the quiet window must cover the confirm window");
static cp_pending_t s_pending[CP_PENDING_MAX];
static pthread_mutex_t s_pending_mutex = PTHREAD_MUTEX_INITIALIZER;
PENDING_ARRAY_CHECK(s_pending);
static const pending_slots_t s_pending_slots = {
   .base = s_pending,
   .stride = sizeof(s_pending[0]),
   .count = CP_PENDING_MAX,
   .ttl_sec = CP_PENDING_TTL_SEC,
   .later_turns = CP_CONFIRM_TURNS,
};

/* Whether a preview of @p kind waits in @p session_id. */
static bool live(uint32_t session_id, int kind) {
   pthread_mutex_lock(&s_pending_mutex);
   const bool found = pending_slots_live(&s_pending_slots, session_id, kind, pending_slots_now()) !=
                      NULL;
   pthread_mutex_unlock(&s_pending_mutex);
   return found;
}

static bool is_head(const char *s) {
   size_t n = 0;
   for (; s[n] != '\0'; n++) {
      if (!isxdigit((unsigned char)s[n])) {
         return false;
      }
   }
   return n == CP_HEAD_LEN;
}

/* A pull request's address as GitHub gives it: https://github.com/<o>/<r>/pull/<n>. */
bool coding_publish_pr_url_ok(const char *url) {
   static const char prefix[] = "https://github.com/";
   if (url == NULL || strncmp(url, prefix, sizeof(prefix) - 1) != 0 || strlen(url) >= CP_URL_MAX) {
      return false;
   }
   const char *p = url + sizeof(prefix) - 1;
   for (int part = 0; part < 2; part++) { /* owner, then repo (which may hold . and _) */
      const char *start = p;
      while (isalnum((unsigned char)*p) || *p == '-' || (part == 1 && (*p == '.' || *p == '_'))) {
         p++;
      }
      if (p == start || *p != '/') {
         return false;
      }
      p++;
   }
   if (strncmp(p, "pull/", 5) != 0) {
      return false;
   }
   p += 5;
   const char *digits = p;
   while (isdigit((unsigned char)*p)) {
      p++;
   }
   return p > digits && *p == '\0';
}

/* The gateway's record of @p job's task (owned by the caller), or NULL with
 * the error result in *@p err. */
static struct json_object *task_of(int user_id, int64_t job, char **err) {
   struct json_object *ans = NULL;
   char *e = NULL;
   if (coding_gateway_call(user_id, "task_list", coding_agent_job_args(job), &ans, &e) !=
       CODING_GW_OK) {
      *err = coding_agent_error_free(e);
      return NULL;
   }
   free(e);
   if (!json_object_is_type(json_object_object_get(ans, "task"), json_type_object)) {
      json_object_put(ans);
      *err = coding_agent_error("The coding-agent gateway has no record of that job.");
      return NULL;
   }
   return ans;
}

/* A gateway string copied whole into @p out, or "" when it doesn't fit. */
static void copy_whole(char *out, size_t len, const char *s) {
   if (snprintf(out, len, "%s", s) >= (int)len) {
      out[0] = '\0';
   }
}

/* ========== publish ========== */

/* The agent's commit subjects and changed paths: its words, cleaned and framed. */
static char *agent_lines(struct json_object *changes, const char *summary) {
   strbuf_t sb;
   strbuf_init_with_max(&sb, 512, CP_AGENT_TEXT_MAX + CP_SUMMARY_SHOWN + 64);
   if (summary != NULL && summary[0] != '\0') {
      /* What the pull request's description will say: public once published. */
      char shown[CP_SUMMARY_SHOWN + 8];
      snprintf(shown, sizeof(shown), "%.*s", CP_SUMMARY_SHOWN, summary);
      utf8_trim_incomplete(shown);
      strbuf_appendf(&sb, "Pull request description:\n%s%s\n", shown,
                     strlen(summary) > CP_SUMMARY_SHOWN ? " [...]" : "");
   }
   struct json_object *commits = json_object_object_get(changes, "commits");
   struct json_object *files = json_object_object_get(changes, "files");
   const size_t nc = json_object_is_type(commits, json_type_array)
                         ? json_object_array_length(commits)
                         : 0;
   const size_t nf = json_object_is_type(files, json_type_array) ? json_object_array_length(files)
                                                                 : 0;
   strbuf_append(&sb, "Commits:\n");
   for (size_t i = 0; i < nc && i < CP_LINES_SHOWN; i++) {
      char line[160];
      str_excerpt_line(coding_gateway_jstr(json_object_array_get_idx(commits, i), "subject"), 120,
                       line, sizeof(line));
      strbuf_appendf(&sb, "- %s\n", line);
   }
   strbuf_append(&sb, "Files:\n");
   for (size_t i = 0; i < nf && i < CP_LINES_SHOWN; i++) {
      struct json_object *f = json_object_array_get_idx(files, i);
      char path[200];
      str_excerpt_line(coding_gateway_jstr(f, "path"), 160, path, sizeof(path));
      strbuf_appendf(&sb, "- %s%s\n", path,
                     json_object_get_boolean(json_object_object_get(f, "protected"))
                         ? " (protected)"
                         : "");
   }
   if (nf > CP_LINES_SHOWN) {
      strbuf_appendf(&sb, "(%zu more files)\n", nf - CP_LINES_SHOWN);
   }
   char *body = llm_context_neutralize_owned(strbuf_steal(&sb));
   char *framed = body ? llm_third_party_frame(TOOL_FRAME_AGENT, NULL, body) : NULL;
   free(body);
   return framed;
}

static int count_protected(struct json_object *changes) {
   struct json_object *files = json_object_object_get(changes, "files");
   int n = 0;
   for (size_t i = 0;
        json_object_is_type(files, json_type_array) && i < json_object_array_length(files); i++) {
      n += json_object_get_boolean(
          json_object_object_get(json_object_array_get_idx(files, i), "protected"));
   }
   return n;
}

char *coding_publish_propose(struct json_object *details,
                             int user_id,
                             const turn_origin_t *origin) {
   const int64_t job = coding_agent_details_job(details);
   job_record_t rec;
   char *bad = coding_agent_check_job(job, user_id, &rec);
   if (bad != NULL) {
      return bad;
   }
   struct json_object *jpr = NULL;
   const bool pr = !json_object_object_get_ex(details, "pr", &jpr) || json_object_get_boolean(jpr);
   if (strcmp(rec.job_status, "done") != 0) {
      char msg[160];
      snprintf(msg, sizeof(msg),
               "Coding job #%lld is %s; only a job that finished its work is published.",
               (long long)job, rec.job_status);
      return coding_agent_error(msg);
   }
   char *err = NULL;
   struct json_object *ans = task_of(user_id, job, &err);
   if (ans == NULL) {
      return err;
   }
   struct json_object *task = json_object_object_get(ans, "task");
   struct json_object *published = json_object_object_get(task, "published");
   struct json_object *jleft = json_object_object_get(ans, "unpublished_commits");
   const char *pr_url = coding_gateway_jstr(published, "pr_url");
   if (coding_gateway_jstr(task, "closed")[0] != '\0') { /* when it was closed; null if not */
      json_object_put(ans);
      return coding_agent_error("That job was closed; its work is gone from the gateway.");
   }
   const bool had_pr = coding_publish_pr_url_ok(pr_url);
   if (json_object_is_type(published, json_type_object) &&
       json_object_is_type(jleft, json_type_int) && json_object_get_int(jleft) == 0 &&
       (had_pr || !pr)) {
      char msg[CP_URL_MAX + 96];
      if (coding_publish_pr_url_ok(pr_url)) {
         snprintf(msg, sizeof(msg), "Coding job #%lld is already published: %s", (long long)job,
                  pr_url);
      } else {
         snprintf(msg, sizeof(msg), "Coding job #%lld's branch is already pushed; nothing new.",
                  (long long)job);
      }
      json_object_put(ans);
      return coding_agent_error(msg); /* nothing to publish: nothing staged */
   }
   cp_pending_t want = { .job = job, .pr = pr };
   copy_whole(want.repo, sizeof(want.repo), coding_gateway_jstr(task, "repo"));
   copy_whole(want.branch, sizeof(want.branch), coding_gateway_jstr(task, "branch"));
   copy_whole(want.base, sizeof(want.base), coding_gateway_jstr(task, "base"));
   const bool again = had_pr; /* new commits go to the PR it has (a new one if that ended) */
   char pr_url_copy[CP_URL_MAX];
   copy_whole(pr_url_copy, sizeof(pr_url_copy), had_pr ? pr_url : "");
   char *summary = strdup(coding_gateway_jstr(task, "summary"));
   json_object_put(ans);

   struct json_object *changes = NULL;
   if (coding_gateway_call(user_id, "task_changes", coding_agent_job_args(job), &changes, &err) !=
       CODING_GW_OK) {
      free(summary);
      return coding_agent_error_free(err);
   }
   free(err);
   const char *head = coding_gateway_jstr(changes, "head");
   struct json_object *commits = json_object_object_get(changes, "commits");
   struct json_object *files = json_object_object_get(changes, "files");
   want.commits = json_object_is_type(commits, json_type_array)
                      ? (int)json_object_array_length(commits)
                      : 0;
   if (!is_head(head) || want.repo[0] == '\0' || want.branch[0] == '\0' || want.base[0] == '\0' ||
       want.commits == 0) {
      json_object_put(changes);
      free(summary);
      return coding_agent_error(want.commits == 0
                                    ? "That job has no commits to publish."
                                    : "The gateway's record of that job can't be shown; nothing "
                                      "was staged.");
   }
   safe_strscpy(want.head, head);
   safe_strscpy(want.name, rec.title);
   const int nfiles = json_object_is_type(files, json_type_array)
                          ? (int)json_object_array_length(files)
                          : 0;
   const int nprot = count_protected(changes);
   want.files = nfiles;
   want.protected_paths = nprot;
   char *agent = agent_lines(changes, pr ? summary : NULL);
   free(summary);
   json_object_put(changes);
   /* Read before this preview marks the turn (its own words don't count here). */
   const bool read_before = coding_agent_turn_read(origin);
   coding_agent_mark_read(); /* the preview carries the agent's words */

   if (read_before && live(origin->session_id, CP_PENDING_PUBLISH)) {
      free(agent);
      return coding_agent_error("A publish is already waiting for the user's answer; it can't be "
                                "replaced in a turn that read the coding agent's words.");
   }
   pending_stage_rc_t rc = PENDING_STAGED;
   pthread_mutex_lock(&s_pending_mutex);
   cp_pending_t *p = (cp_pending_t *)pending_slots_stage(&s_pending_slots, origin, user_id,
                                                         CP_PENDING_PUBLISH, pending_slots_now(),
                                                         &rc);
   const uint32_t id = p ? p->hdr.item_id : 0;
   if (p != NULL) {
      const pending_slot_t hdr = p->hdr;
      *p = want;
      p->hdr = hdr;
   }
   pthread_mutex_unlock(&s_pending_mutex);
   if (id == 0) {
      free(agent);
      return tool_pending_stage_refusal(rc, "publish");
   }
   char count[16]; /* the gateway lists at most 100 */
   if (want.commits >= 100) {
      safe_strscpy(count, "100+");
   } else {
      snprintf(count, sizeof(count), "%d", want.commits);
   }
   strbuf_t sb;
   strbuf_init(&sb, 1024);
   strbuf_appendf(&sb,
                  "About to publish coding job #%lld (\"%s\"): push branch %s of %s at commit "
                  "%.12s (%s commit%s, %d file%s changed",
                  (long long)job, rec.title, want.branch, want.repo, want.head, count,
                  want.commits == 1 ? "" : "s", nfiles, nfiles == 1 ? "" : "s");
   if (nprot > 0) {
      strbuf_appendf(&sb, ", %d protected path%s among them", nprot, nprot == 1 ? "" : "s");
   }
   if (!pr && had_pr) {
      strbuf_appendf(&sb, "); the new commits join its open pull request %s.", pr_url_copy);
   } else if (!pr) {
      strbuf_append(&sb, "), the branch only, no pull request.");
   } else if (again) {
      strbuf_appendf(&sb,
                     "), adding the new commits to its pull request against %s (a new one "
                     "if that was merged or closed).",
                     want.base);
   } else {
      strbuf_appendf(&sb, ") and open a pull request against %s.", want.base);
   }
   if (pr) {
      strbuf_appendf(&sb,
                     " The pull request is titled \"%s\" (the job's name); its description is "
                     "the agent's summary, shown below: read it to the user too, since it will "
                     "be public.",
                     rec.title);
   }
   strbuf_appendf(&sb,
                  " Read this to the user (the protected paths too) and ask them to confirm; if "
                  "they say yes in their reply, call coding_agent with action 'confirm_publish' "
                  "and arguments {\"id\": %u}. The agent's commits and files, its own words:\n%s",
                  (unsigned)id, agent ? agent : "(can't be shown)");
   free(agent);
   return strbuf_steal(&sb);
}

/* The slot @p kind named by @p details, taken for its confirm. */
static char *take(struct json_object *details,
                  int user_id,
                  const turn_origin_t *origin,
                  int kind,
                  const char *what,
                  cp_pending_t *out) {
   const uint32_t id = coding_agent_details_id(details);
   if (id == 0) {
      return tool_pending_missing_id(what);
   }
   if (coding_agent_turn_read(origin)) {
      char msg[160];
      snprintf(msg, sizeof(msg),
               "This turn read the coding agent's words, so the %s can't be confirmed in it. Ask "
               "the user again.",
               what);
      return coding_agent_error(msg);
   }
   turn_origin_rc_t orc = TURN_ORIGIN_OK;
   pthread_mutex_lock(&s_pending_mutex);
   const pending_find_rc_t rc = pending_slots_take(&s_pending_slots, origin, user_id, kind, id,
                                                   pending_slots_now(), out, sizeof(*out), &orc);
   pthread_mutex_unlock(&s_pending_mutex);
   if (rc != PENDING_FOUND) {
      return tool_pending_take_refusal(rc, orc, what);
   }
   turn_origin_note_answered(session_get_command_context(), out->hdr.origin.turn_number);
   return NULL;
}

char *coding_publish_confirm(struct json_object *details,
                             int user_id,
                             const turn_origin_t *origin) {
   cp_pending_t p = { 0 };
   char *bad = take(details, user_id, origin, CP_PENDING_PUBLISH, "publish", &p);
   if (bad != NULL) {
      return bad;
   }
   struct json_object *args = coding_agent_job_args(p.job);
   if (args != NULL) {
      json_object_object_add(args, "head", json_object_new_string(p.head));
      json_object_object_add(args, "open_pr", json_object_new_boolean(p.pr));
      /* The job's name, not the task text's first line (written to the agent). */
      json_object_object_add(args, "title", json_object_new_string(p.name));
   }
   struct json_object *ans = NULL;
   char *err = NULL;
   if (coding_gateway_call(user_id, "task_publish", args, &ans, &err) != CODING_GW_OK) {
      return coding_agent_error_free(err);
   }
   free(err);
   const char *url = coding_gateway_jstr(ans, "pr_url");
   char pushed[CP_BRANCH_MAX + CODING_AGENT_REPO_MAX + 8];
   copy_whole(pushed, sizeof(pushed), coding_gateway_jstr(ans, "pushed"));
   char buf[sizeof(pushed) + CP_URL_MAX + CONV_TITLE_MAX + 96];
   if (coding_publish_pr_url_ok(url)) {
      snprintf(buf, sizeof(buf), "Published coding job #%lld (\"%s\"): pushed %s; pull request: %s",
               (long long)p.job, p.name, pushed[0] ? pushed : p.branch, url);
   } else {
      snprintf(buf, sizeof(buf), "Published coding job #%lld (\"%s\"): pushed %s%s",
               (long long)p.job, p.name, pushed[0] ? pushed : p.branch,
               p.pr ? " (the pull request's address couldn't be shown)" : ".");
   }
   json_object_put(ans);
   return strdup(buf);
}

/* ========== close ========== */

/* The gateway's arguments naming what a close drops: a job's task, or a task by id. */
static struct json_object *close_args(int64_t job, const char *task) {
   if (task[0] == '\0') {
      return coding_agent_job_args(job);
   }
   struct json_object *args = json_object_new_object();
   json_object_object_add(args, "task_id", json_object_new_string(task));
   return args;
}

static bool is_task_id(const char *s) {
   size_t n = 0;
   for (; s[n] != '\0'; n++) {
      if (!isxdigit((unsigned char)s[n]) || isupper((unsigned char)s[n])) {
         return false;
      }
   }
   return n == CP_TASK_ID_LEN;
}

/* What a close names in @p want: a coding job ({"job": N}), or a task of the
 * user's that no DAWN job tracks ({"task": "<id>"}: from before tasks carried
 * one).  *@p open: that task still runs, so it is stopped first (it has no job
 * to cancel).  NULL, or the refusal. */
static char *close_target(struct json_object *details,
                          int user_id,
                          cp_pending_t *want,
                          bool *open) {
   *open = false;
   struct json_object *jtask = NULL;
   if (!json_object_object_get_ex(details, "task", &jtask)) {
      job_record_t rec;
      want->job = coding_agent_details_job(details);
      char *bad = coding_agent_check_job(want->job, user_id, &rec);
      if (bad != NULL) {
         return bad;
      }
      if (strcmp(rec.job_status, "queued") == 0 || strcmp(rec.job_status, "running") == 0) {
         return coding_agent_error("That job is still running; cancel it first (the job tool's "
                                   "cancel), then close it.");
      }
      safe_strscpy(want->name, rec.title);
      snprintf(want->label, sizeof(want->label), "coding job #%lld (\"%s\")", (long long)want->job,
               want->name);
      /* Ended here while its task never stopped (the gateway was away when it
       * was cancelled): stopped first, as closing a task with no job is. */
      struct json_object *ans = NULL;
      char *err = NULL;
      if (coding_gateway_call(user_id, "task_list", coding_agent_job_args(want->job), &ans, &err) ==
          CODING_GW_OK) {
         const char *status = coding_gateway_jstr(json_object_object_get(ans, "task"), "status");
         *open = strcmp(status, "working") == 0 || strcmp(status, "input_required") == 0;
      }
      json_object_put(ans);
      free(err);
      return NULL;
   }
   const char *task = json_object_get_string(jtask);
   if (task == NULL || !is_task_id(task)) {
      return coding_agent_error("Name the task by the id the list shows, as a string: "
                                "{\"task\": \"281935db\"}.");
   }
   struct json_object *args = json_object_new_object();
   json_object_object_add(args, "task_id", json_object_new_string(task));
   struct json_object *ans = NULL;
   char *err = NULL;
   if (coding_gateway_call(user_id, "task_list", args, &ans, &err) != CODING_GW_OK) {
      return coding_agent_error_free(err); /* the gateway checks it is the user's */
   }
   free(err);
   struct json_object *t = json_object_object_get(ans, "task");
   int64_t job = 0;
   job_record_t rec;
   /* Only a clean "no such job" makes it an orphan: a read that failed might
    * hide the job it belongs to, which its job's cancel must stop. */
   const int ref_rc = coding_gateway_ref_job(coding_gateway_jstr(t, "client_ref"), &job);
   const int job_rc = (ref_rc == SUCCESS && job > 0) ? conv_db_job_get(job, user_id, &rec)
                                                     : AUTH_DB_NOT_FOUND;
   const char *status = coding_gateway_jstr(t, "status");
   *open = strcmp(status, "working") == 0 || strcmp(status, "input_required") == 0;
   json_object_put(ans);
   if (ref_rc != SUCCESS || (job_rc != AUTH_DB_SUCCESS && job_rc != AUTH_DB_NOT_FOUND)) {
      return coding_agent_error("Couldn't check whether that task belongs to a job; nothing was "
                                "changed. Try again.");
   }
   if (job_rc == AUTH_DB_SUCCESS) {
      char msg[96];
      snprintf(msg, sizeof(msg), "That task is coding job #%lld; close it by its number.",
               (long long)job);
      return coding_agent_error(msg);
   }
   safe_strscpy(want->task, task);
   snprintf(want->label, sizeof(want->label), "task %s", task);
   return NULL;
}

/* "its 2 unpublished commits and 3 files with uncommitted changes" (either part may be absent). */
static void lost_text(char *out, size_t len, int commits, int files) {
   char a[64] = "", b[64] = "";
   if (commits > 0) {
      snprintf(a, sizeof(a), "%d unpublished commit%s", commits, commits == 1 ? "" : "s");
   }
   if (files > 0) {
      snprintf(b, sizeof(b), "%d file%s with uncommitted changes", files, files == 1 ? "" : "s");
   }
   snprintf(out, len, "its %s%s%s", a, a[0] && b[0] ? " and " : "", b);
}

char *coding_close(struct json_object *details, int user_id, const turn_origin_t *origin) {
   cp_pending_t want = { .job = 0 };
   bool open = false;
   char *bad = close_target(details, user_id, &want, &open);
   if (bad != NULL) {
      return bad;
   }
   /* Before anything is stopped: a refused close leaves nothing behind. */
   if (coding_agent_turn_read(origin) && live(origin->session_id, CP_PENDING_CLOSE)) {
      return coding_agent_error("A close is already waiting for the user's answer; it can't be "
                                "replaced in a turn that read the coding agent's words.");
   }
   struct json_object *ans = NULL;
   char *err = NULL;
   if (open) { /* no job to cancel: the user asking to close it is asking to stop it */
      if (coding_gateway_call(user_id, "task_cancel", close_args(want.job, want.task), &ans,
                              &err) != CODING_GW_OK) {
         return coding_agent_error_free(err);
      }
      open = strcmp(coding_gateway_jstr(ans, "status"), "cancelled") == 0; /* it was running */
      json_object_put(ans);
      ans = NULL;
      free(err);
      err = NULL;
   }
   if (coding_gateway_call(user_id, "task_close", close_args(want.job, want.task), &ans, &err) !=
       CODING_GW_OK) {
      return coding_agent_error_free(err);
   }
   free(err);
   char buf[CP_LABEL_MAX + CP_BRANCH_MAX + 256];
   if (json_object_get_boolean(json_object_object_get(ans, "closed"))) {
      json_object_put(ans);
      snprintf(buf, sizeof(buf),
               "Closed %s%s: its copy of the repo and its branch are gone from the gateway.",
               want.label, open ? " (stopped first)" : "");
      return strdup(buf);
   }
   /* Work would go (commits never published, edits never committed): only on
    * the user's yes to exactly that.  No commits: the head confirmed is "". */
   const char *head = coding_gateway_jstr(ans, "head");
   want.commits = json_object_get_int(json_object_object_get(ans, "unpublished_commits"));
   want.files = json_object_get_int(json_object_object_get(ans, "uncommitted"));
   copy_whole(want.branch, sizeof(want.branch), coding_gateway_jstr(ans, "branch"));
   const bool head_ok = want.commits > 0 ? is_head(head) : head[0] == '\0';
   if (head_ok) {
      safe_strscpy(want.head, head);
   }
   json_object_put(ans);
   if (!head_ok || want.commits < 0 || want.files < 0 || want.commits + want.files == 0) {
      return coding_agent_error("The gateway's answer can't be shown; nothing was closed.");
   }
   pending_stage_rc_t rc = PENDING_STAGED;
   pthread_mutex_lock(&s_pending_mutex);
   cp_pending_t *p = (cp_pending_t *)pending_slots_stage(&s_pending_slots, origin, user_id,
                                                         CP_PENDING_CLOSE, pending_slots_now(),
                                                         &rc);
   const uint32_t id = p ? p->hdr.item_id : 0;
   if (p != NULL) {
      const pending_slot_t hdr = p->hdr;
      *p = want;
      p->hdr = hdr;
   }
   pthread_mutex_unlock(&s_pending_mutex);
   if (id == 0) {
      return tool_pending_stage_refusal(rc, "close");
   }
   /* A question to the user, as a prepare is: DAWN asks nothing else meanwhile. */
   turn_origin_note_prepare(session_get_command_context());
   char lost[160];
   lost_text(lost, sizeof(lost), want.commits, want.files);
   snprintf(buf, sizeof(buf),
            "Closing %s%s deletes %s on branch %s%s%.12s%s; nothing else has them. Read this to "
            "the user and ask them to confirm; if they say yes in their reply, call coding_agent "
            "with action 'confirm_close' and arguments {\"id\": %u}.",
            want.label, open ? " (now stopped)" : "", lost,
            want.branch[0] ? want.branch : "(its branch)", want.head[0] ? " (at " : "", want.head,
            want.head[0] ? ")" : "", (unsigned)id);
   return strdup(buf);
}

char *coding_close_confirm(struct json_object *details, int user_id, const turn_origin_t *origin) {
   cp_pending_t p = { 0 };
   char *bad = take(details, user_id, origin, CP_PENDING_CLOSE, "close", &p);
   if (bad != NULL) {
      return bad;
   }
   struct json_object *args = close_args(p.job, p.task);
   if (args != NULL) {
      json_object_object_add(args, "head", json_object_new_string(p.head));
   }
   struct json_object *ans = NULL;
   char *err = NULL;
   if (coding_gateway_call(user_id, "task_close", args, &ans, &err) != CODING_GW_OK) {
      return coding_agent_error_free(err);
   }
   free(err);
   const bool closed = json_object_get_boolean(json_object_object_get(ans, "closed"));
   json_object_put(ans);
   if (!closed) {
      return coding_agent_error("It changed since the user was asked; nothing was closed.");
   }
   char lost[160];
   char buf[CP_LABEL_MAX + sizeof(lost) + 64];
   lost_text(lost, sizeof(lost), p.commits, p.files);
   snprintf(buf, sizeof(buf), "Closed %s: %s, its copy of the repo and its branch are gone.",
            p.label, lost);
   return strdup(buf);
}

/* ========== reply codes: DAWN's facts only, never the agent's words ========== */

int coding_publish_describe(const char *value,
                            bool close,
                            char *out,
                            size_t out_len,
                            int *valid_for_sec) {
   struct json_object *details = tool_parse_details(value, true);
   const uint32_t id = coding_agent_details_id(details);
   json_object_put(details);
   turn_origin_t origin;
   int n = -1;
   snprintf(out, out_len, "that %s isn't waiting any more; ask for it again",
            close ? "close" : "publish");
   if (id == 0 || !turn_origin_capture(&origin)) {
      return FAILURE;
   }
   pthread_mutex_lock(&s_pending_mutex);
   pending_slot_t *slot = NULL;
   if (pending_slots_find(&s_pending_slots, &origin, tool_get_current_user_id(),
                          close ? CP_PENDING_CLOSE : CP_PENDING_PUBLISH, id, pending_slots_now(),
                          &slot) == PENDING_FOUND) {
      const cp_pending_t *p = (const cp_pending_t *)slot;
      if (close) {
         char lost[160];
         lost_text(lost, sizeof(lost), p->commits, p->files);
         n = snprintf(out, out_len, "close %s, deleting %s", p->label, lost);
      } else {
         n = snprintf(out, out_len,
                      "publish coding job #%lld (\"%s\"): push %s of %s at %.12s (%d commit%s, "
                      "%d file%s, %d protected)%s%s",
                      (long long)p->job, p->name, p->branch, p->repo, p->head, p->commits,
                      p->commits == 1 ? "" : "s", p->files, p->files == 1 ? "" : "s",
                      p->protected_paths,
                      p->pr ? " and open a pull request against " : ", no pull request",
                      p->pr ? p->base : "");
      }
      *valid_for_sec = pending_slots_valid_for(&s_pending_slots, slot, pending_slots_now());
   }
   pthread_mutex_unlock(&s_pending_mutex);
   return (n > 0 && (size_t)n < out_len) ? SUCCESS : FAILURE;
}
