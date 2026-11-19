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
#include <strings.h>

#include "auth/auth_db.h"
#include "core/conv_event.h"
#include "core/event_payload.h"
#include "core/job_report.h"
#include "core/pending_slots.h"
#include "core/session_manager.h"
#include "core/strbuf.h"
#include "core/tool_call_challenge.h"
#include "core/turn_origin.h"
#include "dawn_error.h"
#include "llm/llm_context_text.h"
#include "llm/llm_third_party.h"
#include "tools/coding_agent_card.h"
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
#define CP_HEAD_LEN CODING_HEAD_LEN
#define CP_BRANCH_MAX 160
#define CP_URL_MAX 200
#define CP_LINES_SHOWN 20      /* commits, files and history-only paths in a preview, each */
#define CP_DESCRIBE_EXCERPT 90 /* the user's own title or description in a reply-code line */
#define CP_PATH_SHOWN 160      /* a path's excerpt in such a line */
#define CP_SUBJECT_SHOWN 120   /* a commit subject's excerpt */
#define CP_LINE_MAX 200        /* one of those lines: the longest excerpt plus its marks */
#define CP_EXCERPT_MARK 8      /* room an excerpt keeps for its cut mark */
/* The commits, files and history-only lists with their headings and "more" lines. */
#define CP_AGENT_TEXT_MAX (3 * (CP_LINES_SHOWN * CP_LINE_MAX + 64))
#define CP_SUMMARY_SHOWN 1500 /* the agent's summary (the pull request's description) */
#define CP_TITLE_SHOWN 160    /* the pull request's title the agent wrote */
#define CP_TASK_ID_LEN 8      /* the gateway's task ids: 8 hex digits */
#define CP_LOST_TEXT_MAX 160  /* "its 2 unpublished commits and 3 files with ..." */
#define CP_BODY_REFS_MAX 20   /* mentions, and issues a merge closes, named in a preview */
#define CP_MENTION_MAX 80     /* "@owner/team": longer than any GitHub handle */
#define CP_ISSUE_REF_MAX 120  /* "owner/repo#N" */
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
   int check_paths;
   bool pr;
   bool gone; /* close: the gateway has no record of the task (its state was lost) */
   char body_sha[CODING_SHA256_LEN + 1];   /* publish: the description shown ("" if unknown) */
   char pr_title[CODING_PR_TITLE_MAX + 1]; /* publish: the user's own ("": the agent's) */
   char pr_desc[CODING_PR_DESC_MAX + 1];   /* publish: the user's own ("": the agent's) */
   bool edit_open; /* publish: that text replaces the job's open pull request's */
   bool text_only; /* publish (edit_open): nothing new to push, only that text changes */
   char replaces_sha[CODING_SHA256_LEN + 1]; /* edit_open: the description it replaces */
   char pr_url[CP_URL_MAX];                  /* edit_open: that pull request */
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

ca_confirm_kind_t coding_publish_staged(uint32_t id, const turn_origin_t *origin, int user_id) {
   int kind = 0;
   pthread_mutex_lock(&s_pending_mutex);
   const bool found = pending_slots_kind_of(&s_pending_slots, origin, user_id, id,
                                            pending_slots_now(), &kind);
   pthread_mutex_unlock(&s_pending_mutex);
   if (!found) {
      return CA_CONFIRM_NONE;
   }
   return kind == CP_PENDING_CLOSE ? CA_CONFIRM_CLOSE : CA_CONFIRM_PUBLISH;
}

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

static bool is_sha256(const char *s) {
   size_t n = 0;
   for (; s != NULL && s[n] != '\0'; n++) {
      if (!isxdigit((unsigned char)s[n])) {
         return false;
      }
   }
   return n == CODING_SHA256_LEN;
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

/* The agent's commit subjects and changed paths (none when !@p work: nothing is
 * pushed), under the pull request's @p title and @p summary: its words, cleaned
 * and framed. */
static char *agent_lines(struct json_object *changes,
                         const char *title,
                         const char *summary,
                         bool whole,
                         bool work) {
   strbuf_t sb;
   strbuf_init_with_max(
       &sb, 512, CP_AGENT_TEXT_MAX + CODING_PR_DESC_MAX + CP_SUMMARY_SHOWN + CP_TITLE_SHOWN + 256);
   if (title != NULL && title[0] != '\0') {
      char line[CP_TITLE_SHOWN];
      str_excerpt_line(title, CP_TITLE_SHOWN - CP_EXCERPT_MARK, line, sizeof(line));
      strbuf_appendf(&sb, "Pull request title:\n%s\n", line);
   }
   if (summary != NULL && summary[0] != '\0' && whole) {
      strbuf_appendf(&sb, "Pull request description:\n%s\n", summary);
   } else if (summary != NULL && summary[0] != '\0') {
      /* What the pull request's description will say: public once published. */
      char shown[CP_SUMMARY_SHOWN + 8];
      snprintf(shown, sizeof(shown), "%.*s", CP_SUMMARY_SHOWN, summary);
      utf8_trim_incomplete(shown);
      strbuf_appendf(&sb, "Pull request description:\n%s%s\n", shown,
                     strlen(summary) > CP_SUMMARY_SHOWN ? " [...]" : "");
   }
   struct json_object *commits = work ? json_object_object_get(changes, "commits") : NULL;
   struct json_object *files = work ? json_object_object_get(changes, "files") : NULL;
   const size_t nc = json_object_is_type(commits, json_type_array)
                         ? json_object_array_length(commits)
                         : 0;
   const size_t nf = json_object_is_type(files, json_type_array) ? json_object_array_length(files)
                                                                 : 0;
   if (work) {
      strbuf_append(&sb, "Commits:\n");
   }
   for (size_t i = 0; i < nc && i < CP_LINES_SHOWN; i++) {
      char line[CP_LINE_MAX];
      str_excerpt_line(coding_gateway_jstr(json_object_array_get_idx(commits, i), "subject"),
                       CP_SUBJECT_SHOWN, line, sizeof(line));
      strbuf_appendf(&sb, "- %s\n", line);
   }
   if (work) {
      strbuf_append(&sb, "Files:\n");
   }
   for (size_t i = 0; i < nf && i < CP_LINES_SHOWN; i++) {
      struct json_object *f = json_object_array_get_idx(files, i);
      char path[CP_LINE_MAX];
      str_excerpt_line(coding_gateway_jstr(f, "path"), CP_PATH_SHOWN, path, sizeof(path));
      strbuf_appendf(&sb, "- %s%s\n", path, coding_file_check(f) ? " (to check)" : "");
   }
   if (nf > CP_LINES_SHOWN) {
      strbuf_appendf(&sb, "(%zu more files)\n", nf - CP_LINES_SHOWN);
   }
   /* Changed in a commit and changed back: public in the branch's history too. */
   struct json_object *hist = work ? json_object_object_get(changes, "history_only") : NULL;
   const size_t nh = json_object_is_type(hist, json_type_array) ? json_object_array_length(hist)
                                                                : 0;
   if (nh > 0) {
      strbuf_append(&sb, "Changed only in commits (not in the final diff):\n");
   }
   for (size_t i = 0; i < nh && i < CP_LINES_SHOWN; i++) {
      struct json_object *f = json_object_array_get_idx(hist, i);
      char path[CP_LINE_MAX];
      str_excerpt_line(coding_gateway_jstr(f, "path"), CP_PATH_SHOWN, path, sizeof(path));
      strbuf_appendf(&sb, "- %s%s\n", path, coding_file_check(f) ? " (to check)" : "");
   }
   if (nh > CP_LINES_SHOWN) {
      strbuf_appendf(&sb, "(%zu more)\n", nh - CP_LINES_SHOWN);
   }
   char *body = llm_context_neutralize_owned(strbuf_steal(&sb));
   char *framed = body ? llm_third_party_frame(TOOL_FRAME_AGENT, NULL, body) : NULL;
   free(body);
   return framed;
}

static bool is_space(char c) {
   return c == ' ' || (c >= '\t' && c <= '\r');
}

static size_t rtrim_len(const char *s, size_t n) {
   while (n > 0 && is_space(s[n - 1])) {
      n--;
   }
   return n;
}

/* The length of @p text without trailing white space and one trailing @p footer
 * (NULL: none), as the gateway takes a description before adding its footer:
 * text that reuses the current description gets the footer once. */
static size_t footless_len(const char *text, const char *footer) {
   size_t n = rtrim_len(text, strlen(text));
   if (footer == NULL) {
      return n;
   }
   while (is_space(*footer)) {
      footer++;
   }
   const size_t f = rtrim_len(footer, strlen(footer));
   if (f > 0 && n >= f && memcmp(text + n - f, footer, f) == 0) {
      n = rtrim_len(text, n - f);
   }
   return n;
}

/* The open pull request's description as GitHub has it, whole up to what a new
 * one may be (the user's text is rebuilt from it), without the gateway's
 * @p footer: written by anyone who can edit it there, so framed as a page's
 * text, never the user's. */
static char *github_description(const char *body, const char *footer) {
   const size_t n = footless_len(body, footer);
   const bool cut = n > CODING_PR_DESC_MAX;
   char *shown = strndup(body, cut ? CODING_PR_DESC_MAX : n);
   if (shown == NULL) {
      return NULL;
   }
   utf8_trim_incomplete(shown);
   strbuf_t sb;
   strbuf_init(&sb, strlen(shown) + 160);
   strbuf_appendf(&sb,
                  "The pull request's current description on GitHub, without the gateway's "
                  "closing line%s:\n%s%s\n",
                  cut ? " (cut: too long to show whole)" : "", shown[0] ? shown : "(empty)",
                  cut ? " [...]" : "");
   free(shown);
   char *text = llm_context_neutralize_owned(strbuf_steal(&sb));
   char *framed = text ? llm_third_party_frame(TOOL_FRAME_WEB, NULL, text) : NULL;
   free(text);
   return framed;
}

/* Files to check that a push makes public: in the final diff, and in commits
 * only (changed, then changed back: still in the branch's history).  Counted;
 * with @p sb, also named in DAWN's words ("`path` (why); ..."), the first few
 * that are plainly paths. */
static int check_paths(struct json_object *changes, strbuf_t *sb) {
   static const char *const k_lists[] = { "files", "history_only" };
   int n = 0, shown = 0;
   for (size_t l = 0; l < sizeof(k_lists) / sizeof(k_lists[0]); l++) {
      struct json_object *arr = json_object_object_get(changes, k_lists[l]);
      for (size_t i = 0;
           json_object_is_type(arr, json_type_array) && i < json_object_array_length(arr); i++) {
         struct json_object *f = json_object_array_get_idx(arr, i);
         if (!coding_file_check(f)) {
            continue;
         }
         n++;
         const char *path = coding_gateway_jstr(f, "path");
         if (sb != NULL && shown < CODING_CHECK_SHOWN && coding_check_path_shown(path)) {
            strbuf_appendf(sb, "%s`%s`", shown ? "; " : "", path);
            const char *why = coding_gateway_jstr(f, "why");
            if (coding_check_why_shown(why)) {
               strbuf_appendf(sb, " (%s)", why);
            }
            shown++;
         }
      }
   }
   if (sb != NULL && n > shown) {
      strbuf_appendf(sb, "%s%d %s", shown ? "; and " : "", n - shown, shown ? "more" : "not shown");
   }
   return n;
}

/* What publishing a job would do, as the gateway has it: the checks every way
 * of publishing makes, before any preview.  The caller frees it (plan_free). */
typedef struct {
   cp_pending_t want; /* job, name, repo, branch, base, head, commits, files, files to check */
   job_record_t rec;
   bool had_pr; /* an earlier publish opened a pull request */
   char pr_url[CP_URL_MAX];
   char *summary;               /* the agent's, raw */
   struct json_object *changes; /* task_changes' answer */
} cp_plan_t;

static void plan_free(cp_plan_t *plan) {
   free(plan->summary);
   plan->summary = NULL;
   json_object_put(plan->changes);
   plan->changes = NULL;
}

/* The user's own title and/or description for a new pull request (NULL: the
 * agent's), into @p want: the title as one line (runs of white space as one
 * space), the description without trailing white space or the gateway's
 * @p footer (NULL: unknown) it adds again.  Refused, never cut, when empty or
 * too long: they confirm this text.  NULL, or the refusal. */
static char *own_text(const char *title, const char *desc, const char *footer, cp_pending_t *want) {
   if (title != NULL) {
      char line[CODING_PR_TITLE_MAX + 1];
      size_t n = 0;
      bool space = false, bad = false;
      for (const unsigned char *c = (const unsigned char *)title; *c && !bad; c++) {
         if (*c == ' ' || (*c >= '\t' && *c <= '\r')) {
            space = n > 0;
            continue;
         }
         bad = *c < ' ' || *c == 0x7f;
         if (space && n < CODING_PR_TITLE_MAX) {
            line[n++] = ' ';
         }
         space = false;
         if (n < CODING_PR_TITLE_MAX) {
            line[n] = (char)*c;
         }
         n++;
      }
      if (bad || n == 0 || n > CODING_PR_TITLE_MAX) {
         char msg[96];
         snprintf(msg, sizeof(msg), "The pull request title must be one line of 1 to %d bytes.",
                  CODING_PR_TITLE_MAX);
         return coding_agent_error(msg);
      }
      line[n] = '\0';
      safe_strscpy(want->pr_title, line);
   }
   if (desc != NULL) {
      const size_t n = footless_len(desc, footer);
      if (n == 0 || n > CODING_PR_DESC_MAX) {
         char msg[96];
         snprintf(msg, sizeof(msg), "The pull request description must be 1 to %d bytes.",
                  CODING_PR_DESC_MAX);
         return coding_agent_error(msg);
      }
      memcpy(want->pr_desc, desc, n);
      want->pr_desc[n] = '\0';
   }
   return NULL;
}

/* @p job, the user's coding job that finished its work: NULL with @p rec, or
 * the refusal. */
static char *publishable_job(int user_id, int64_t job, job_record_t *rec) {
   char *bad = coding_agent_check_job(job, user_id, rec);
   if (bad != NULL || strcmp(rec->job_status, "done") == 0) {
      return bad;
   }
   char msg[160];
   snprintf(msg, sizeof(msg),
            "Coding job #%lld is %s; only a job that finished its work is published.",
            (long long)job, rec->job_status);
   return coding_agent_error(msg);
}

/* NULL with @p out filled (to free), or the refusal.  @p edit: the user's own
 * title or description comes with it, which replace the job's open pull
 * request's (want.edit_open, with nothing new to push too: want.text_only). */
static char *publish_plan(int user_id, int64_t job, bool pr, bool edit, cp_plan_t *out) {
   memset(out, 0, sizeof(*out));
   char *bad = publishable_job(user_id, job, &out->rec);
   if (bad != NULL) {
      return bad;
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
   out->had_pr = coding_publish_pr_url_ok(pr_url);
   const bool nothing_new = json_object_is_type(published, json_type_object) &&
                            json_object_is_type(jleft, json_type_int) &&
                            json_object_get_int(jleft) == 0;
   /* Nothing new: a publish only changes the text of a pull request still open. */
   if (nothing_new && (out->had_pr || !pr) && !(out->had_pr && edit)) {
      char msg[CP_URL_MAX + 96];
      if (out->had_pr) {
         snprintf(msg, sizeof(msg), "Coding job #%lld is already published: %s", (long long)job,
                  pr_url);
      } else {
         snprintf(msg, sizeof(msg), "Coding job #%lld's branch is already pushed; nothing new.",
                  (long long)job);
      }
      json_object_put(ans);
      return coding_agent_error(msg); /* nothing to publish: nothing staged */
   }
   cp_pending_t *want = &out->want;
   want->job = job;
   want->pr = pr;
   copy_whole(want->repo, sizeof(want->repo), coding_gateway_jstr(task, "repo"));
   copy_whole(want->branch, sizeof(want->branch), coding_gateway_jstr(task, "branch"));
   copy_whole(want->base, sizeof(want->base), coding_gateway_jstr(task, "base"));
   copy_whole(out->pr_url, sizeof(out->pr_url), out->had_pr ? pr_url : "");
   out->summary = strdup(coding_gateway_jstr(task, "summary"));
   json_object_put(ans);

   /* Text for a pull request the job has: replaces that one's while it is open. */
   const bool ask_open = edit && out->had_pr;
   struct json_object *cargs = coding_agent_job_args(job);
   if (cargs != NULL && ask_open) {
      json_object_object_add(cargs, "open_pr", json_object_new_boolean(true));
   }
   if (coding_gateway_call(user_id, "task_changes", cargs, &out->changes, &err) != CODING_GW_OK) {
      plan_free(out);
      return coding_agent_error_free(err);
   }
   free(err);
   struct json_object *open = json_object_object_get(out->changes, "open_pr");
   const char *open_err = coding_gateway_jstr(out->changes, "open_pr_error");
   const bool is_open = json_object_is_type(open, json_type_object);
   /* An older gateway ignores the question and leaves the key out. */
   if (ask_open && open_err[0] == '\0' &&
       !json_object_object_get_ex(out->changes, "open_pr", NULL)) {
      plan_free(out);
      return coding_agent_error("The coding-agent gateway is too old to change an open pull "
                                "request's text; reinstall it. Nothing was staged.");
   }
   /* Merged or closed with new commits: they open a new one, with this text. */
   if (ask_open && !is_open && (open_err[0] != '\0' || nothing_new)) {
      char msg[CP_URL_MAX + 256];
      if (open_err[0] != '\0') {
         snprintf(msg, sizeof(msg),
                  "Coding job #%lld's pull request couldn't be read on GitHub (%.120s); nothing "
                  "was staged.",
                  (long long)job, open_err);
      } else {
         snprintf(msg, sizeof(msg),
                  "Coding job #%lld's pull request %s was merged or closed, and there is nothing "
                  "new to publish.",
                  (long long)job, out->pr_url);
      }
      plan_free(out);
      return coding_agent_error(msg);
   }
   if (ask_open && is_open) {
      out->want.edit_open = true;
      out->want.text_only = nothing_new;
      safe_strscpy(out->want.pr_url, out->pr_url);
      const char *osha = coding_gateway_jstr(open, "body_sha256");
      if (is_sha256(osha)) {
         safe_strscpy(out->want.replaces_sha, osha);
      }
   }
   const char *head = coding_gateway_jstr(out->changes, "head");
   struct json_object *commits = json_object_object_get(out->changes, "commits");
   struct json_object *files = json_object_object_get(out->changes, "files");
   want->commits = json_object_is_type(commits, json_type_array)
                       ? (int)json_object_array_length(commits)
                       : 0;
   if (!is_head(head) || want->repo[0] == '\0' || want->branch[0] == '\0' ||
       want->base[0] == '\0' || want->commits == 0) {
      plan_free(out);
      return coding_agent_error(want->commits == 0
                                    ? "That job has no commits to publish."
                                    : "The gateway's record of that job can't be shown; nothing "
                                      "was staged.");
   }
   safe_strscpy(want->head, head);
   safe_strscpy(want->name, out->rec.title);
   want->files = json_object_is_type(files, json_type_array) ? (int)json_object_array_length(files)
                                                             : 0;
   want->check_paths = check_paths(out->changes, NULL);
   const char *sha = coding_gateway_jstr(out->changes, "pr_body_sha256");
   if (pr && is_sha256(sha)) {
      safe_strscpy(want->body_sha, sha); /* the chat's yes binds the description too */
   }
   return NULL;
}

char *coding_publish_propose(struct json_object *details,
                             int user_id,
                             const turn_origin_t *origin) {
   const int64_t job = coding_agent_details_job(details);
   struct json_object *jpr = NULL;
   const bool pr = !json_object_object_get_ex(details, "pr", &jpr) || json_object_get_boolean(jpr);
   struct json_object *jt = NULL, *jd = NULL;
   const char *own_title = json_object_object_get_ex(details, "title", &jt)
                               ? json_object_get_string(jt)
                               : NULL;
   const char *own_desc = json_object_object_get_ex(details, "description", &jd)
                              ? json_object_get_string(jd)
                              : NULL;
   /* A NUL inside would cut the text short, unseen. */
   if ((own_title && (size_t)json_object_get_string_len(jt) != strlen(own_title)) ||
       (own_desc && (size_t)json_object_get_string_len(jd) != strlen(own_desc))) {
      return coding_agent_error("The pull request title or description holds a NUL character.");
   }
   if ((own_title || own_desc) && !pr) {
      return coding_agent_error("A title or description is for a pull request; this publish "
                                "opens none.");
   }
   cp_plan_t plan;
   char *bad = publish_plan(user_id, job, pr, own_title || own_desc, &plan);
   if (bad != NULL) {
      return bad;
   }
   bad = own_text(own_title, own_desc, coding_gateway_jstr(plan.changes, "pr_footer"), &plan.want);
   if (bad != NULL) {
      plan_free(&plan);
      return bad;
   }
   const bool edit_open = plan.want.edit_open;
   if (plan.want.pr_desc[0] != '\0' || edit_open) {
      plan.want.body_sha[0] = '\0'; /* the user's own needs no binding; nor an edit, the agent's */
   }
   if (edit_open && plan.want.pr_desc[0] != '\0' && plan.want.replaces_sha[0] == '\0') {
      plan_free(&plan);
      return coding_agent_error("The coding-agent gateway can't show the pull request's current "
                                "description exactly; nothing was staged.");
   }
   const cp_pending_t want = plan.want;
   const bool again = plan.had_pr; /* new commits go to the PR it has (a new one if that ended) */
   const int nfiles = want.files;
   const int ncheck = want.check_paths;
   /* What a pull request would say: the title and description the agent wrote for
    * it (the gateway's, as it would send them), else its report.  Its words: in
    * the frame, never in DAWN's own sentence. */
   const char *agent_title = coding_gateway_jstr(plan.changes, "pr_title");
   const char *body = coding_gateway_jstr(plan.changes, "pr_body");
   const bool by_agent = agent_title[0] != '\0';
   /* The user's own text, as the gateway will send it: the description with its
    * footer, whole (they confirm all of it). */
   char *own_body = NULL;
   if (want.pr_desc[0] != '\0') {
      strbuf_t ob;
      strbuf_init(&ob, strlen(want.pr_desc) + 64);
      strbuf_appendf(&ob, "%s%s", want.pr_desc, coding_gateway_jstr(plan.changes, "pr_footer"));
      own_body = strbuf_steal(&ob);
   }
   /* The title the gateway will use: the user's, the agent's, else the job's name;
    * of an open pull request's, only what changes. */
   const char *shown_title = want.pr_title[0] ? want.pr_title
                             : edit_open      ? NULL
                             : agent_title[0] ? agent_title
                                              : plan.rec.title;
   const char *shown_body = own_body ? own_body : edit_open ? NULL : body[0] ? body : plan.summary;
   char *agent = agent_lines(plan.changes, pr ? shown_title : NULL, pr ? shown_body : NULL,
                             own_body != NULL, !want.text_only);
   free(own_body);
   /* The description it replaces: GitHub's, never framed as the user's. */
   char *github = NULL;
   if (edit_open && want.pr_desc[0] != '\0') {
      github = github_description(
          coding_gateway_jstr(json_object_object_get(plan.changes, "open_pr"), "body"),
          coding_gateway_jstr(plan.changes, "pr_footer"));
   }
   if (agent == NULL || (edit_open && want.pr_desc[0] != '\0' && github == NULL)) {
      /* What can't be shown isn't staged: the yes would be to text nobody read. */
      free(agent);
      free(github);
      plan_free(&plan);
      return coding_agent_error("The publish preview is too large to show; nothing was staged.");
   }
   char title[sizeof(plan.rec.title)];
   safe_strscpy(title, plan.rec.title);
   char pr_url_copy[CP_URL_MAX];
   safe_strscpy(pr_url_copy, plan.pr_url);
   strbuf_t cn; /* the files to check, named */
   strbuf_init(&cn, 256);
   check_paths(plan.changes, &cn);
   plan_free(&plan);
   /* Read before this preview marks the turn (its own words don't count here). */
   const bool read_before = coding_agent_turn_read(origin);
   coding_agent_mark_read(); /* the preview carries the agent's words */

   if (read_before && live(origin->session_id, CP_PENDING_PUBLISH)) {
      free(agent);
      free(github);
      strbuf_free(&cn);
      return coding_agent_error("A publish is already waiting for the user's answer; it can't be "
                                "replaced in a turn that read the coding agent's words.");
   }
   pending_stage_rc_t rc = PENDING_STAGED;
   pthread_mutex_lock(&s_pending_mutex);
   cp_pending_t *p = (cp_pending_t *)pending_slots_stage(&s_pending_slots, origin, user_id,
                                                         CP_PENDING_PUBLISH, pending_slots_now(),
                                                         &rc);
   const uint32_t id = p ? p->hdr.item_id : 0;
   const uint32_t replaced = p ? p->hdr.replaced_turn : 0;
   if (p != NULL) {
      const pending_slot_t hdr = p->hdr;
      *p = want;
      p->hdr = hdr;
   }
   pthread_mutex_unlock(&s_pending_mutex);
   coding_agent_preview_replaced(replaced);
   if (id == 0) {
      free(agent);
      free(github);
      strbuf_free(&cn);
      return tool_pending_stage_refusal(rc, "publish");
   }
   const bool both = want.pr_title[0] && want.pr_desc[0];
   const char *parts = both ? "title and description" : want.pr_title[0] ? "title" : "description";
   char count[16];
   if (want.commits >= CODING_COMMITS_LISTED) {
      snprintf(count, sizeof(count), "%d+", CODING_COMMITS_LISTED);
   } else {
      snprintf(count, sizeof(count), "%d", want.commits);
   }
   strbuf_t sb;
   strbuf_init(&sb, 1024);
   if (want.text_only) {
      strbuf_appendf(&sb,
                     "About to change coding job #%lld (\"%s\")'s open pull request %s: replace "
                     "its %s (no new commits to push).",
                     (long long)job, title, pr_url_copy, parts);
   } else {
      strbuf_appendf(&sb,
                     "About to publish coding job #%lld (\"%s\"): push branch %s of %s at commit "
                     "%.12s (%s commit%s, %d file%s changed",
                     (long long)job, title, want.branch, want.repo, want.head, count,
                     want.commits == 1 ? "" : "s", nfiles, nfiles == 1 ? "" : "s");
   }
   if (want.text_only) {
      /* said above */
   } else if (edit_open) {
      strbuf_appendf(&sb,
                     "), adding the new commits to its open pull request %s and replacing "
                     "its %s.",
                     pr_url_copy, parts);
   } else if (!pr && again) {
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
   if (!want.text_only && ncheck > 0) {
      strbuf_appendf(&sb, " %d file%s to check before publishing: %s.", ncheck,
                     ncheck == 1 ? "" : "s", strbuf_len(&cn) ? strbuf_str(&cn) : "");
   }
   if (edit_open) {
      strbuf_appendf(&sb,
                     " The new %s, given with this publish, %s shown below%s: read %s to the "
                     "user too, since %s public.",
                     parts, both ? "are" : "is",
                     want.pr_desc[0] ? " in full, with the line the gateway adds" : "",
                     both ? "them" : "it", both ? "they will be" : "it will be");
      if (want.pr_desc[0]) {
         strbuf_append(&sb, " The description it replaces is shown last, as GitHub has it now "
                            "(anyone who can edit the pull request may have written it); if it "
                            "changes before the user's yes, nothing is replaced.");
      }
   } else if (pr && (want.pr_title[0] || want.pr_desc[0])) {
      strbuf_appendf(&sb,
                     " The pull request's %s given with this publish (%s the agent's), shown "
                     "below%s: read %s to the user too, since %s public.",
                     both               ? "title and description are the ones"
                     : want.pr_title[0] ? "title is the one"
                                        : "description is the one",
                     both               ? "not"
                     : want.pr_title[0] ? "the description is"
                                        : "the title is",
                     want.pr_desc[0] ? " in full, with the line the gateway adds" : "",
                     both ? "them" : "it", both ? "they will be" : "it will be");
   } else if (pr && by_agent) {
      strbuf_append(&sb, " The pull request's title and description are the agent's, shown "
                         "below: read them to the user too, since they will be public.");
   } else if (pr) {
      strbuf_appendf(&sb,
                     " The pull request is titled \"%s\" (the job's name); its description is "
                     "the agent's summary, shown below: read it to the user too, since it will "
                     "be public.",
                     title);
   }
   strbuf_appendf(
       &sb,
       " Read this to the user%s and ask them to confirm; if they say yes in their "
       "reply, call coding_agent with action 'confirm' and arguments "
       "{\"ids\": [%u]}. %s\n%s%s%s",
       want.text_only ? "" : " (the files to check too)", (unsigned)id,
       want.text_only ? "The new text:" : "The agent's commits and files, its own words:", agent,
       github ? "\n" : "", github ? github : "");
   free(agent);
   free(github);
   strbuf_free(&cn);
   return strbuf_steal(&sb);
}

/* Slot @p id of @p kind, taken for its confirm. */
static char *take(uint32_t id,
                  int user_id,
                  const turn_origin_t *origin,
                  int kind,
                  const char *what,
                  cp_pending_t *out) {
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

/* Push what @p p names (bound to its head and, when given, the description
 * hash shown) and say what happened.  *@p pr_url (when not NULL): the pull
 * request's address on success, to free. */
static char *publish_run(int user_id, const cp_pending_t *p, const char *body_sha, char **pr_url) {
   struct json_object *args = coding_agent_job_args(p->job);
   if (args != NULL) {
      json_object_object_add(args, "head", json_object_new_string(p->head));
      json_object_object_add(args, "open_pr", json_object_new_boolean(p->pr));
      /* The job's name, not the task text's first line (written to the agent). */
      json_object_object_add(args, "title", json_object_new_string(p->name));
      if (p->pr_title[0] != '\0') {
         json_object_object_add(args, "pr_title", json_object_new_string(p->pr_title));
      }
      if (p->pr_desc[0] != '\0') {
         json_object_object_add(args, "pr_description", json_object_new_string(p->pr_desc));
      }
      if (body_sha != NULL && p->pr_desc[0] == '\0') { /* the user's own needs no binding */
         json_object_object_add(args, "expect_body_sha256", json_object_new_string(body_sha));
      }
      /* An open pull request's description: replaced only while it is the one shown. */
      if (p->edit_open && p->pr_desc[0] != '\0') {
         json_object_object_add(args, "replaces_body_sha256",
                                json_object_new_string(p->replaces_sha));
      }
      /* Shown as an edit: one merged or closed since is refused, never replaced. */
      if (p->edit_open) {
         json_object_object_add(args, "edit_open_pr", json_object_new_boolean(true));
      }
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
   const char *where = pushed[0] ? pushed : (p->branch[0] ? p->branch : "its branch");
   char buf[sizeof(pushed) + CP_URL_MAX + CONV_TITLE_MAX + 160];
   /* What of an open pull request's text was replaced (none: it already said that). */
   struct json_object *jparts = json_object_object_get(ans, "pr_updated");
   char updated[64] = "";
   for (size_t i = 0; json_object_is_type(jparts, json_type_array) &&
                      i < json_object_array_length(jparts) && i < 2;
        i++) {
      const char *part = json_object_get_string(json_object_array_get_idx(jparts, i));
      if (part != NULL && (strcmp(part, "title") == 0 || strcmp(part, "description") == 0)) {
         snprintf(updated + strlen(updated), sizeof(updated) - strlen(updated), "%s%s",
                  updated[0] ? " and " : "", part);
      }
   }
   if (json_object_is_type(jparts, json_type_array) && coding_publish_pr_url_ok(url)) {
      char what[96];
      if (updated[0] != '\0') {
         snprintf(what, sizeof(what), "updated the pull request's %s", updated);
      } else {
         snprintf(what, sizeof(what), "the pull request already had this text");
      }
      if (pushed[0] != '\0') {
         snprintf(buf, sizeof(buf), "Published coding job #%lld (\"%s\"): pushed %s; %s: %s",
                  (long long)p->job, p->name, pushed, what, url);
      } else {
         snprintf(buf, sizeof(buf), "Coding job #%lld (\"%s\"): %s: %s", (long long)p->job, p->name,
                  what, url);
      }
      if (pr_url != NULL) {
         *pr_url = strdup(url);
      }
   } else if (coding_publish_pr_url_ok(url)) {
      snprintf(buf, sizeof(buf), "Published coding job #%lld (\"%s\"): pushed %s; pull request: %s",
               (long long)p->job, p->name, where, url);
      if (pr_url != NULL) {
         *pr_url = strdup(url);
      }
   } else {
      snprintf(buf, sizeof(buf), "Published coding job #%lld (\"%s\"): pushed %s%s",
               (long long)p->job, p->name, where,
               p->pr ? " (the pull request's address couldn't be shown)" : ".");
   }
   json_object_put(ans);
   return strdup(buf);
}

char *coding_publish_confirm(uint32_t id, int user_id, const turn_origin_t *origin) {
   cp_pending_t p = { 0 };
   char *bad = take(id, user_id, origin, CP_PENDING_PUBLISH, "publish", &p);
   if (bad != NULL) {
      return bad;
   }
   return publish_run(user_id, &p, p.body_sha[0] ? p.body_sha : NULL, NULL);
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

/* A close of coding job @p job: the user's, finished.  @p rec receives its
 * record.  *@p open: its task still runs (the gateway was away when the job
 * was cancelled), so it is stopped first.  NULL, or the refusal. */
static char *close_target_job(int user_id,
                              int64_t job,
                              cp_pending_t *want,
                              bool *open,
                              job_record_t *rec) {
   *open = false;
   want->job = job;
   char *bad = coding_agent_check_job(job, user_id, rec);
   if (bad != NULL) {
      return bad;
   }
   if (strcmp(rec->job_status, "queued") == 0 || strcmp(rec->job_status, "running") == 0) {
      return coding_agent_error("That job is still running; cancel it first (the job tool's "
                                "cancel), then close it.");
   }
   safe_strscpy(want->name, rec->title);
   snprintf(want->label, sizeof(want->label), "coding job #%lld (\"%s\")", (long long)job,
            want->name);
   struct json_object *ans = NULL;
   char *err = NULL;
   if (coding_gateway_call(user_id, "task_list", coding_agent_job_args(job), &ans, &err) ==
       CODING_GW_OK) {
      struct json_object *task = json_object_object_get(ans, "task");
      *open = coding_task_status_open(coding_gateway_jstr(task, "status"));
      want->gone = !json_object_is_type(task, json_type_object);
   }
   json_object_put(ans);
   free(err);
   return NULL;
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
      return close_target_job(user_id, coding_agent_details_job(details), want, open, &rec);
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
   *open = coding_task_status_open(status);
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

/* A close's first step: the task stopped first when it still runs (*@p
 * stopped: it was), then closed when nothing would be lost (*@p closed), else
 * @p want filled with what would be (commits, files, branch, the head to bind).
 * NULL, or the refusal. */
/* @p job's task is gone from the gateway (its state was lost): nothing is held
 * there, so its close is recorded as the gateway's would be, and the job can be
 * deleted. */
static void record_gone(int64_t job, int user_id) {
   static const char *const fields[] = { "reason", NULL };
   struct json_object *data = json_object_new_object();
   json_object_object_add(data, "reason", json_object_new_string("gone"));
   conv_event_emit(job, user_id, CONV_EVENT_AGENT_REQUEST,
                   event_payload_agent(EVENT_GW_CLOSED, 0, (int64_t)time(NULL), data, fields,
                                       NULL));
   json_object_put(data);
}

static char *close_first(int user_id, cp_pending_t *want, bool open, bool *closed, bool *stopped) {
   *closed = false;
   *stopped = false;
   if (want->gone && want->job > 0) {
      record_gone(want->job, user_id);
      *closed = true;
      return NULL;
   }
   struct json_object *ans = NULL;
   char *err = NULL;
   if (open) { /* no job to cancel: the user asking to close it is asking to stop it */
      if (coding_gateway_call(user_id, "task_cancel", close_args(want->job, want->task), &ans,
                              &err) != CODING_GW_OK) {
         return coding_agent_error_free(err);
      }
      *stopped = strcmp(coding_gateway_jstr(ans, "status"), "cancelled") == 0; /* it was running */
      json_object_put(ans);
      ans = NULL;
      free(err);
      err = NULL;
   }
   if (coding_gateway_call(user_id, "task_close", close_args(want->job, want->task), &ans, &err) !=
       CODING_GW_OK) {
      return coding_agent_error_free(err);
   }
   free(err);
   if (json_object_get_boolean(json_object_object_get(ans, "closed"))) {
      json_object_put(ans);
      *closed = true;
      return NULL;
   }
   /* Work would go (commits never published, edits never committed): only on
    * the user's yes to exactly that.  No commits: the head confirmed is "". */
   const char *head = coding_gateway_jstr(ans, "head");
   want->commits = json_object_get_int(json_object_object_get(ans, "unpublished_commits"));
   want->files = json_object_get_int(json_object_object_get(ans, "uncommitted"));
   copy_whole(want->branch, sizeof(want->branch), coding_gateway_jstr(ans, "branch"));
   const bool head_ok = want->commits > 0 ? is_head(head) : head[0] == '\0';
   if (head_ok) {
      safe_strscpy(want->head, head);
   }
   json_object_put(ans);
   if (!head_ok || want->commits < 0 || want->files < 0 || want->commits + want->files == 0) {
      return coding_agent_error("The gateway's answer can't be shown; nothing was closed.");
   }
   return NULL;
}

/* Close what @p want names only while its branch is at @p head ("" with no
 * commits).  NULL with *@p closed, or the refusal. */
static char *close_at(int user_id, const cp_pending_t *want, const char *head, bool *closed) {
   *closed = false;
   struct json_object *args = close_args(want->job, want->task);
   if (args != NULL) {
      json_object_object_add(args, "head", json_object_new_string(head));
   }
   struct json_object *ans = NULL;
   char *err = NULL;
   if (coding_gateway_call(user_id, "task_close", args, &ans, &err) != CODING_GW_OK) {
      return coding_agent_error_free(err);
   }
   free(err);
   *closed = json_object_get_boolean(json_object_object_get(ans, "closed"));
   json_object_put(ans);
   return NULL;
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
   bool closed = false;
   bool stopped = false;
   bad = close_first(user_id, &want, open, &closed, &stopped);
   if (bad != NULL) {
      return bad;
   }
   char buf[CP_LABEL_MAX + CP_BRANCH_MAX + 256];
   if (closed) {
      snprintf(buf, sizeof(buf),
               "Closed %s%s: its copy of the repo and its branch are gone from the gateway.",
               want.label, stopped ? " (stopped first)" : "");
      return strdup(buf);
   }
   pending_stage_rc_t rc = PENDING_STAGED;
   pthread_mutex_lock(&s_pending_mutex);
   cp_pending_t *p = (cp_pending_t *)pending_slots_stage(&s_pending_slots, origin, user_id,
                                                         CP_PENDING_CLOSE, pending_slots_now(),
                                                         &rc);
   const uint32_t id = p ? p->hdr.item_id : 0;
   const uint32_t replaced = p ? p->hdr.replaced_turn : 0;
   if (p != NULL) {
      const pending_slot_t hdr = p->hdr;
      *p = want;
      p->hdr = hdr;
   }
   pthread_mutex_unlock(&s_pending_mutex);
   coding_agent_preview_replaced(replaced);
   if (id == 0) {
      return tool_pending_stage_refusal(rc, "close");
   }
   /* A question to the user, as a prepare is: DAWN asks nothing else meanwhile. */
   turn_origin_note_prepare(session_get_command_context());
   char lost[CP_LOST_TEXT_MAX];
   lost_text(lost, sizeof(lost), want.commits, want.files);
   /* Grown, not fixed: a cut here would lose the id the confirm needs. */
   strbuf_t sb;
   strbuf_init(&sb, 512);
   strbuf_appendf(&sb,
                  "Closing %s%s deletes %s on branch %s%s%.12s%s; nothing else has them. Read this "
                  "to the user and ask them to confirm; if they say yes in their reply, call "
                  "coding_agent with action 'confirm' and arguments {\"ids\": [%u]}.",
                  want.label, stopped ? " (now stopped)" : "", lost,
                  want.branch[0] ? want.branch : "(its branch)", want.head[0] ? " (at " : "",
                  want.head, want.head[0] ? ")" : "", (unsigned)id);
   return strbuf_steal(&sb);
}

char *coding_close_confirm(uint32_t id, int user_id, const turn_origin_t *origin) {
   cp_pending_t p = { 0 };
   char *bad = take(id, user_id, origin, CP_PENDING_CLOSE, "close", &p);
   if (bad != NULL) {
      return bad;
   }
   bool closed = false;
   bad = close_at(user_id, &p, p.head, &closed);
   if (bad != NULL) {
      return bad;
   }
   if (!closed) {
      return coding_agent_error("It changed since the user was asked; nothing was closed.");
   }
   char lost[CP_LOST_TEXT_MAX];
   char buf[CP_LABEL_MAX + sizeof(lost) + 64];
   lost_text(lost, sizeof(lost), p.commits, p.files);
   snprintf(buf, sizeof(buf), "Closed %s: %s, its copy of the repo and its branch are gone.",
            p.label, lost);
   return strdup(buf);
}

/* ========== the WebUI's buttons: the user's click is the confirm ========== */


/* A staged chat publish or close of @p job (@p kind, or any with 0), made moot
 * by a click. */
static void drop_staged(int64_t job, int kind) {
   turn_origin_t dropped[CP_PENDING_MAX];
   int n = 0;
   pthread_mutex_lock(&s_pending_mutex);
   for (int i = 0; i < CP_PENDING_MAX; i++) {
      if (s_pending[i].hdr.active && (kind == 0 || s_pending[i].hdr.kind == kind) &&
          s_pending[i].job == job) {
         dropped[n++] = s_pending[i].hdr.origin;
         pending_slots_clear(&s_pending_slots, &s_pending[i].hdr);
      }
   }
   pthread_mutex_unlock(&s_pending_mutex);
   /* Its preview is settled by the click: it no longer holds back what DAWN
    * would post unasked in the chat that staged it. */
   for (int i = 0; i < n; i++) {
      session_t *s = session_get(dropped[i].session_id);
      if (s != NULL) {
         turn_origin_note_answered(s, dropped[i].turn_number);
         session_release(s);
      }
   }
}

/* What a click did, said in the job's conversation, as a card's answer is: the
 * chat (and the model reading it) would otherwise not know. */
static void post_result(int64_t parent, int user_id, const char *text) {
   if (parent <= 0) {
      return;
   }
   char *said = llm_context_neutralize(text);
   job_report_post(parent, user_id, said ? said : text);
   free(said);
}

static void add_unique(struct json_object *arr, const char *s, size_t len) {
   const size_t n = json_object_array_length(arr);
   if (n >= CP_BODY_REFS_MAX) {
      return;
   }
   for (size_t i = 0; i < n; i++) {
      const char *have = json_object_get_string(json_object_array_get_idx(arr, i));
      if (strlen(have) == len && strncmp(have, s, len) == 0) {
         return;
      }
   }
   json_object_array_add(arr, json_object_new_string_len(s, (int)len));
}

static bool word_char(char c) {
   return isalnum((unsigned char)c) || c == '_';
}

/* What a pull request's description does on GitHub beyond being read: the
 * accounts it mentions (notified), and the issues it closes when merged. */
static void body_refs(const char *body, struct json_object *mentions, struct json_object *closes) {
   static const char *const k_keywords[] = { "close", "closes",  "closed",   "fix",     "fixes",
                                             "fixed", "resolve", "resolves", "resolved" };
   for (const char *p = body; *p != '\0'; p++) {
      if (*p == '@' && (p == body || !word_char(p[-1]))) {
         const char *q = p + 1;
         while (isalnum((unsigned char)*q) || *q == '-' || *q == '/') {
            q++;
         }
         if (q - p > 1 && q - p <= CP_MENTION_MAX) {
            add_unique(mentions, p, (size_t)(q - p));
         }
         continue;
      }
      if (p != body && word_char(p[-1])) {
         continue;
      }
      for (size_t k = 0; k < sizeof(k_keywords) / sizeof(k_keywords[0]); k++) {
         const size_t kl = strlen(k_keywords[k]);
         if (strncasecmp(p, k_keywords[k], kl) != 0 || word_char(p[kl])) {
            continue;
         }
         const char *q = p + kl;
         if (*q == ':') {
            q++;
         }
         while (*q == ' ') {
            q++;
         }
         static const char gh[] = "https://github.com/";
         if (strncmp(q, gh, sizeof(gh) - 1) == 0) { /* .../owner/repo/issues/N */
            const char *u = q;
            while (*u != '\0' && !isspace((unsigned char)*u)) {
               u++;
            }
            while (u > q && strchr(".,;:)!?", u[-1]) != NULL) { /* the sentence's, not the URL's */
               u--;
            }
            const size_t len = (size_t)(u - q);
            char tok[CP_ISSUE_REF_MAX + 1];
            if (len > 0 && len <= CP_ISSUE_REF_MAX) {
               memcpy(tok, q, len);
               tok[len] = '\0';
               if ((strstr(tok, "/issues/") || strstr(tok, "/pull/")) &&
                   isdigit((unsigned char)tok[len - 1])) {
                  add_unique(closes, q, len);
               }
            }
            break;
         }
         const char *ref = q; /* #N, or owner/repo#N */
         while (isalnum((unsigned char)*q) || *q == '-' || *q == '_' || *q == '.' || *q == '/') {
            q++;
         }
         if (*q == '#' && isdigit((unsigned char)q[1])) {
            q++;
            while (isdigit((unsigned char)*q)) {
               q++;
            }
            if (q - ref <= CP_ISSUE_REF_MAX) {
               add_unique(closes, ref, (size_t)(q - ref));
            }
         }
         break;
      }
   }
}

struct json_object *coding_publish_preview(int user_id, int64_t job, char **err_out) {
   *err_out = NULL;
   cp_plan_t plan;
   char *bad = publish_plan(user_id, job, true, false, &plan);
   if (bad != NULL) {
      *err_out = bad;
      return NULL;
   }
   const char *body = coding_gateway_jstr(plan.changes, "pr_body");
   const char *sha = coding_gateway_jstr(plan.changes, "pr_body_sha256");
   if (!is_sha256(sha)) {
      plan_free(&plan);
      *err_out = coding_agent_error("The coding-agent gateway can't show the exact pull request "
                                    "description; update it, then publish from here.");
      return NULL;
   }
   const cp_pending_t *w = &plan.want;
   struct json_object *o = json_object_new_object();
   json_object_object_add(o, "head", json_object_new_string(w->head));
   json_object_object_add(o, "repo", json_object_new_string(w->repo));
   json_object_object_add(o, "branch", json_object_new_string(w->branch));
   json_object_object_add(o, "base", json_object_new_string(w->base));
   json_object_object_add(o, "commits", json_object_new_int(w->commits));
   json_object_object_add(o, "commits_more",
                          json_object_new_boolean(w->commits >= CODING_COMMITS_LISTED));
   json_object_object_add(o, "files", json_object_new_int(w->files));
   struct json_object *total = json_object_object_get(plan.changes, "files_total");
   json_object_object_add(o, "files_total",
                          json_object_new_int(json_object_is_type(total, json_type_int)
                                                  ? json_object_get_int(total)
                                                  : w->files));
   struct json_object *check = json_object_new_array();
   struct json_object *files = json_object_object_get(plan.changes, "files");
   for (size_t i = 0;
        json_object_is_type(files, json_type_array) && i < json_object_array_length(files); i++) {
      struct json_object *f = json_object_array_get_idx(files, i);
      if (coding_file_check(f)) {
         struct json_object *c = json_object_new_object();
         json_object_object_add(c, "path", json_object_new_string(coding_gateway_jstr(f, "path")));
         json_object_object_add(c, "why", json_object_new_string(coding_gateway_jstr(f, "why")));
         json_object_array_add(check, c);
      }
   }
   json_object_object_add(o, "to_check", check);
   struct json_object *history = json_object_object_get(plan.changes, "history_only");
   json_object_object_add(o, "history_only",
                          json_object_is_type(history, json_type_array) ? json_object_get(history)
                                                                        : json_object_new_array());
   json_object_object_add(o, "workflows",
                          json_object_new_boolean(json_object_get_boolean(
                              json_object_object_get(plan.changes, "workflows"))));
   const char *agent_title = coding_gateway_jstr(plan.changes, "pr_title");
   json_object_object_add(o, "pr_title",
                          json_object_new_string(agent_title[0] ? agent_title : w->name));
   json_object_object_add(o, "pr_title_by_agent", json_object_new_boolean(agent_title[0] != '\0'));
   json_object_object_add(o, "pr_body", json_object_new_string(body));
   json_object_object_add(o, "pr_body_sha256", json_object_new_string(sha));
   /* What the gateway adds to a description the user writes themselves. */
   json_object_object_add(o, "pr_footer",
                          json_object_new_string(coding_gateway_jstr(plan.changes, "pr_footer")));
   struct json_object *mentions = json_object_new_array();
   struct json_object *closes = json_object_new_array();
   body_refs(body, mentions, closes);
   json_object_object_add(o, "mentions", mentions);
   json_object_object_add(o, "closes", closes);
   if (plan.had_pr) {
      json_object_object_add(o, "pr_url", json_object_new_string(plan.pr_url));
   }
   plan_free(&plan);
   return o;
}

char *coding_publish_click(int user_id,
                           int64_t job,
                           const char *head,
                           const char *body_sha,
                           const char *pr_title,
                           const char *pr_description,
                           char **pr_url_out) {
   *pr_url_out = NULL;
   job_record_t rec;
   char *bad = publishable_job(user_id, job, &rec);
   if (bad != NULL) {
      return bad;
   }
   /* The agent's description is bound by its hash; the user's own needs none. */
   if (head == NULL || !is_head(head) || (pr_description == NULL && !is_sha256(body_sha))) {
      return coding_agent_error("What was shown can't be matched; open the preview again.");
   }
   cp_pending_t p = { .job = job, .pr = true };
   bad = own_text(pr_title, pr_description, NULL, &p);
   if (bad != NULL) {
      return bad;
   }
   safe_strscpy(p.head, head);
   safe_strscpy(p.name, rec.title);
   char *res = publish_run(user_id, &p, body_sha, pr_url_out);
   if (!tool_result_is_error(res)) {
      drop_staged(job, CP_PENDING_PUBLISH);
      /* Also on the gateway's replay of an earlier publish: that one's answer
       * may never have reached DAWN, and a line said twice beats none. */
      post_result(rec.parent_id, user_id, res);
   }
   return res;
}

struct json_object *coding_close_click(int user_id,
                                       int64_t job,
                                       bool confirm,
                                       const char *head,
                                       char **err_out) {
   *err_out = NULL;
   cp_pending_t want = { .job = 0 };
   bool open = false;
   job_record_t rec;
   char *bad = close_target_job(user_id, job, &want, &open, &rec);
   if (bad == NULL && confirm && (head == NULL || (head[0] != '\0' && !is_head(head)))) {
      bad = coding_agent_error("What was shown can't be matched; close it again.");
   }
   bool closed = false;
   bool stopped = false;
   if (bad == NULL) {
      bad = confirm ? close_at(user_id, &want, head, &closed)
                    : close_first(user_id, &want, open, &closed, &stopped);
   }
   if (bad == NULL && confirm && !closed) {
      bad = coding_agent_error("It changed since it was shown; nothing was closed.");
   }
   if (bad != NULL) {
      *err_out = bad;
      return NULL;
   }
   char buf[CP_LABEL_MAX + CP_BRANCH_MAX + 256];
   struct json_object *o = json_object_new_object();
   json_object_object_add(o, "closed", json_object_new_boolean(closed));
   if (closed) {
      snprintf(buf, sizeof(buf),
               confirm ? "Closed %s%s: its unpublished work, its copy of the repo and its branch "
                         "are gone."
                       : "Closed %s%s: its copy of the repo and its branch are gone from the "
                         "gateway.",
               want.label, stopped ? " (stopped first)" : "");
      drop_staged(job, 0);                      /* a staged publish of it can't happen now either */
      post_result(rec.parent_id, user_id, buf); /* also on a replay, as publish */
   } else {
      char lost[CP_LOST_TEXT_MAX];
      lost_text(lost, sizeof(lost), want.commits, want.files);
      snprintf(buf, sizeof(buf), "Closing %s deletes %s; nothing else has them.", want.label, lost);
      struct json_object *nc = json_object_new_object();
      json_object_object_add(nc, "commits", json_object_new_int(want.commits));
      json_object_object_add(nc, "files", json_object_new_int(want.files));
      json_object_object_add(nc, "branch", json_object_new_string(want.branch));
      json_object_object_add(nc, "head", json_object_new_string(want.head));
      json_object_object_add(o, "needs_confirm", nc);
   }
   json_object_object_add(o, "message", json_object_new_string(buf));
   return o;
}

/* ========== reply codes: DAWN's facts only, never the agent's words ========== */

int coding_publish_describe(uint32_t id,
                            bool close,
                            char *out,
                            size_t out_len,
                            int *valid_for_sec) {
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
         char lost[CP_LOST_TEXT_MAX];
         lost_text(lost, sizeof(lost), p->commits, p->files);
         n = snprintf(out, out_len, "close %s, deleting %s", p->label, lost);
      } else {
         if (p->text_only) {
            n = snprintf(out, out_len, "change coding job #%lld (\"%s\")'s open pull request %s",
                         (long long)p->job, p->name, p->pr_url);
         } else {
            n = snprintf(out, out_len,
                         "publish coding job #%lld (\"%s\"): push %s of %s at %.12s (%d commit%s, "
                         "%d file%s, %d to check)%s%s",
                         (long long)p->job, p->name, p->branch, p->repo, p->head, p->commits,
                         p->commits == 1 ? "" : "s", p->files, p->files == 1 ? "" : "s",
                         p->check_paths,
                         p->edit_open ? " to its open pull request "
                         : p->pr      ? " and open a pull request against "
                                      : ", no pull request",
                         p->edit_open ? p->pr_url
                         : p->pr      ? p->base
                                      : "");
         }
         /* Text the model wrote that goes public: shown here, in DAWN's line,
          * not left to its read-back. */
         if (n > 0 && (size_t)n < out_len && p->pr_title[0]) {
            char t[CP_DESCRIBE_EXCERPT];
            str_excerpt_line(p->pr_title, sizeof(t) - CP_EXCERPT_MARK, t, sizeof(t));
            n += snprintf(out + n, out_len - (size_t)n,
                          p->edit_open ? ", its title becoming \"%s\"" : ", titled \"%s\"", t);
         }
         if (n > 0 && (size_t)n < out_len && p->pr_desc[0]) {
            char d[CP_DESCRIBE_EXCERPT];
            str_excerpt_line(p->pr_desc, sizeof(d) - CP_EXCERPT_MARK, d, sizeof(d));
            n += snprintf(out + n, out_len - (size_t)n,
                          p->edit_open ? ", its description replaced by %zu bytes that begin \"%s\""
                                       : ", with a description of %zu bytes that begins \"%s\"",
                          strlen(p->pr_desc), d);
         }
      }
      *valid_for_sec = pending_slots_valid_for(&s_pending_slots, slot, pending_slots_now());
   }
   pthread_mutex_unlock(&s_pending_mutex);
   return (n > 0 && (size_t)n < out_len) ? SUCCESS : FAILURE;
}
