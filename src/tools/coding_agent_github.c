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
 * The coding_agent tool's GitHub reads: a pull request's state, its review
 * threads and a check's log, read by the agent gateway with its token on any
 * repo the token can read.  The text is GitHub's, written by anyone who can
 * comment there, so these actions' results are framed as web content (the
 * tool's action-kinds table); what DAWN adds says the blockers in plain words.
 */

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "core/strbuf.h"
#include "tools/coding_agent_gateway.h"
#include "tools/coding_agent_internal.h"
#include "utils/string_utils.h"

#define CG_REF_MAX 256             /* a ref the model passes: a URL at most */
#define CG_REPO_MAX 160            /* owner/repo, or a gateway repo's name */
#define CG_CHECK_MAX 128           /* a check's name or job id */
#define CG_STATUS_MAX (16 * 1024)  /* what pr_status hands the model */
#define CG_THREADS_MAX (96 * 1024) /* what pr_threads hands the model */
#define CG_LOG_MAX (16 * 1024)     /* what pr_log hands the model (a 12 KB excerpt) */
#define CG_ROOM 1024               /* kept free for a "not shown" line */
#define CG_BODY_SHOWN 2000         /* a comment's or review's text, as one line */
#define CG_HASH_SHOWN 8            /* a commit's hash, as shown */
#define CG_NAME_SHOWN 120          /* a check's, path's or login's characters */

struct json_object *coding_github_pr_args(struct json_object *details,
                                          int user_id,
                                          char **refusal) {
   *refusal = NULL;
   const int64_t job = coding_agent_details_job(details);
   if (job != 0) {
      job_record_t rec;
      *refusal = coding_agent_check_job(job, user_id, &rec);
      struct json_object *args = *refusal ? NULL : coding_agent_job_args(job);
      if (*refusal == NULL && args == NULL) {
         *refusal = coding_agent_error("Couldn't read the coding job.");
      }
      return args;
   }
   const char *ref = coding_gateway_jstr(details, "ref");
   const char *repo = coding_gateway_jstr(details, "repo");
   if (ref[0] == '\0' || strlen(ref) > CG_REF_MAX || strlen(repo) > CG_REPO_MAX) {
      *refusal = coding_agent_error(
          "Name the pull request: {\"ref\": \"owner/repo#N\"} (or its URL, or N with \"repo\"), "
          "or {\"job\": N} for the one a coding job published.");
      return NULL;
   }
   struct json_object *args = json_object_new_object();
   json_object_object_add(args, "ref", json_object_new_string(ref));
   if (repo[0] != '\0') {
      json_object_object_add(args, "repo", json_object_new_string(repo));
   }
   return args;
}

struct json_object *coding_github_call(int user_id,
                                       const char *tool,
                                       struct json_object *args,
                                       char **refusal) {
   struct json_object *ans = NULL;
   char *err = NULL;
   if (coding_gateway_call(user_id, tool, args, &ans, &err) != CODING_GW_OK) {
      *refusal = coding_agent_error_free(err);
      return NULL;
   }
   free(err);
   return ans;
}

/* @p sb's text, or an error when it didn't fit in memory; the read is marked. */
static char *pr_result(strbuf_t *sb) {
   coding_agent_mark_read();
   char *out = strbuf_steal(sb);
   strbuf_free(sb); /* what an overflow left */
   return out ? out : coding_agent_error("The answer didn't fit in memory.");
}

static bool jbool(struct json_object *o, const char *key) {
   return json_object_get_boolean(json_object_object_get(o, key));
}

static bool check_failed(struct json_object *c) {
   return jbool(c, "failed");
}

static bool check_pending(struct json_object *c) {
   return strcmp(coding_gateway_jstr(c, "status"), "completed") != 0;
}

/* "name (job N)", or just the name for a check that isn't an Actions job. */
static void append_check_name(strbuf_t *sb, struct json_object *c) {
   strbuf_appendf(sb, "%.*s", CG_NAME_SHOWN, coding_gateway_jstr(c, "name"));
   const char *job = coding_gateway_jstr(c, "job_id");
   if (job[0] != '\0') {
      strbuf_appendf(sb, " (job %s)", job);
   }
}

/* The blockers in plain words, on one line: failing and running required
 * checks, behind its base, conflicts, review state, Copilot, open threads. */
static void append_blockers(strbuf_t *sb, struct json_object *ans) {
   struct json_object *pr = json_object_object_get(ans, "pr");
   struct json_object *checks = json_object_object_get(ans, "checks");
   const size_t nc = json_object_is_type(checks, json_type_array) ? json_object_array_length(checks)
                                                                  : 0;
   const char *state = coding_gateway_jstr(pr, "state");
   if (strcmp(state, "merged") == 0 || strcmp(state, "closed") == 0) {
      strbuf_appendf(sb, "It is %s.\n", state);
      return;
   }
   strbuf_t b;
   strbuf_init(&b, 256);
   if (jbool(pr, "draft")) {
      strbuf_append(&b, "It is a draft. ");
   }
   for (size_t i = 0; i < nc; i++) {
      struct json_object *c = json_object_array_get_idx(checks, i);
      if (jbool(c, "required") && (check_failed(c) || check_pending(c))) {
         strbuf_append(&b, check_failed(c) ? "Required check failing: "
                                           : "Required check not finished: ");
         append_check_name(&b, c);
         strbuf_append(&b, ". ");
      }
   }
   const char *base = coding_gateway_jstr(pr, "base");
   if (jbool(pr, "behind")) {
      strbuf_appendf(&b, "Behind %.*s. ", CG_NAME_SHOWN, base);
   }
   if (strcmp(coding_gateway_jstr(pr, "mergeable"), "conflicting") == 0) {
      strbuf_appendf(&b, "Has merge conflicts with %.*s. ", CG_NAME_SHOWN, base);
   }
   const char *decision = coding_gateway_jstr(pr, "review_decision");
   if (strcmp(decision, "changes_requested") == 0) {
      strbuf_append(&b, "Changes requested. ");
   } else if (strcmp(decision, "review_required") == 0) {
      strbuf_append(&b, "Needs an approving review. ");
   }
   const int open = json_object_get_int(
       json_object_object_get(json_object_object_get(ans, "threads"), "unresolved"));
   if (open > 0) {
      strbuf_appendf(&b, "%d unresolved thread%s. ", open, open == 1 ? "" : "s");
   }
   /* Right after its base moves, GitHub takes a moment to work this out. */
   const char *merge_state = coding_gateway_jstr(pr, "merge_state");
   if (strcmp(merge_state, "unknown") == 0) {
      strbuf_appendf(&b,
                     "GitHub is still working out whether it is behind %.*s or conflicts; "
                     "ask again in a moment. ",
                     CG_NAME_SHOWN, base);
   } else if (strbuf_len(&b) == 0) {
      /* Only these let it merge; "blocked" can come from a rule DAWN doesn't see. */
      const bool mergeable = strcmp(merge_state, "clean") == 0 ||
                             strcmp(merge_state, "unstable") == 0 ||
                             strcmp(merge_state, "has_hooks") == 0;
      strbuf_appendf(&b,
                     mergeable ? "Nothing DAWN can see blocks it (GitHub's merge state: %.*s). "
                               : "GitHub's merge state is %.*s, from a rule DAWN can't see. ",
                     CG_NAME_SHOWN, merge_state);
   }
   /* Copilot's review isn't a blocker, so it comes after the verdict. */
   const char *copilot = coding_gateway_jstr(ans, "copilot");
   if (strcmp(copilot, "requested") == 0) {
      strbuf_append(&b, "Copilot's review is requested. ");
   } else if (strcmp(copilot, "reviewed_head") == 0) {
      strbuf_append(&b, "Copilot reviewed the current head: no new commits since. ");
   } else if (strcmp(copilot, "reviewed_earlier") == 0) {
      strbuf_append(&b, "Copilot reviewed an earlier commit. ");
   }
   /* Each sentence ends ". ": the line without its last space. */
   if (strbuf_len(&b) > 0) {
      strbuf_append_n(sb, strbuf_str(&b), strbuf_len(&b) - 1);
   }
   strbuf_append(sb, "\n");
   strbuf_free(&b);
}

/* The checks: failing and unfinished ones by name, passing ones counted. */
static void append_checks(strbuf_t *sb, struct json_object *ans) {
   struct json_object *checks = json_object_object_get(ans, "checks");
   const size_t nc = json_object_is_type(checks, json_type_array) ? json_object_array_length(checks)
                                                                  : 0;
   const char *note = coding_gateway_jstr(ans, "checks_note");
   if (note[0] != '\0') {
      strbuf_appendf(sb, "Checks: %s.\n", note);
      return;
   }
   strbuf_appendf(sb, "Checks: %s in all.\n", coding_gateway_jstr(ans, "checks_total"));
   size_t passed = 0;
   for (size_t i = 0; i < nc; i++) {
      struct json_object *c = json_object_array_get_idx(checks, i);
      if (!check_failed(c) && !check_pending(c)) {
         passed++;
         continue;
      }
      strbuf_append(sb, "- ");
      append_check_name(sb, c);
      strbuf_appendf(sb, ": %s%s\n",
                     check_pending(c) ? "not finished" : coding_gateway_jstr(c, "conclusion"),
                     jbool(c, "required") ? " (required)" : "");
   }
   strbuf_appendf(sb, "%zu of the %zu listed passed (or were skipped).\n", passed, nc);
}

/* Reviews, requested reviewers, the threads, and whether a job of the user's
 * published it (the agent can work on it then). */
static void append_reviews(strbuf_t *sb, struct json_object *ans) {
   struct json_object *reviews = json_object_object_get(ans, "reviews");
   const size_t nr = json_object_is_type(reviews, json_type_array)
                         ? json_object_array_length(reviews)
                         : 0;
   if (nr > 0) {
      strbuf_append(sb, "Latest reviews:");
      for (size_t i = 0; i < nr; i++) {
         struct json_object *r = json_object_array_get_idx(reviews, i);
         strbuf_appendf(sb, " %.*s %s on %.*s%s", CG_NAME_SHOWN, coding_gateway_jstr(r, "login"),
                        coding_gateway_jstr(r, "state"), CG_HASH_SHOWN,
                        coding_gateway_jstr(r, "commit"), i + 1 < nr ? ";" : ".\n");
      }
   }
   struct json_object *requested = json_object_object_get(ans, "requested");
   const size_t nq = json_object_is_type(requested, json_type_array)
                         ? json_object_array_length(requested)
                         : 0;
   for (size_t i = 0; i < nq; i++) {
      strbuf_appendf(sb, "%s%.*s%s", i == 0 ? "Review requested from: " : ", ", CG_NAME_SHOWN,
                     json_object_get_string(json_object_array_get_idx(requested, i)),
                     i + 1 < nq ? "" : ".\n");
   }
   struct json_object *threads = json_object_object_get(ans, "threads");
   strbuf_appendf(sb, "Review threads: %s unresolved, %s resolved (pr_threads reads them).\n",
                  coding_gateway_jstr(threads, "unresolved"),
                  coding_gateway_jstr(threads, "resolved"));
   int64_t job = 0;
   coding_gateway_ref_job(coding_gateway_jstr(ans, "job"), &job);
   const char *repo = coding_gateway_jstr(ans, "configured_repo");
   if (job > 0) {
      strbuf_appendf(sb, "Coding job #%lld published it.\n", (long long)job);
   } else if (repo[0] != '\0') {
      strbuf_appendf(sb,
                     "It is in the coding agent's repo %.*s; no coding job of yours "
                     "published it.\n",
                     CG_NAME_SHOWN, repo);
   } else {
      strbuf_append(sb, "It isn't in a repo the coding agent works in.\n");
   }
}

char *coding_github_pr_status(struct json_object *details, int user_id) {
   char *refusal = NULL;
   struct json_object *args = coding_github_pr_args(details, user_id, &refusal);
   struct json_object *ans = args ? coding_github_call(user_id, "pr_status", args, &refusal) : NULL;
   if (ans == NULL) {
      return refusal;
   }
   struct json_object *pr = json_object_object_get(ans, "pr");
   strbuf_t sb;
   strbuf_init_with_max(&sb, 1024, CG_STATUS_MAX);
   const char *state = coding_gateway_jstr(pr, "state");
   strbuf_appendf(&sb, "Pull request %s/%s#%s: %s\n%s%s by %.*s, %.*s <- %.*s at %.*s. %s\n",
                  coding_gateway_jstr(pr, "owner"), coding_gateway_jstr(pr, "repo"),
                  coding_gateway_jstr(pr, "number"), coding_gateway_jstr(pr, "title"),
                  jbool(pr, "draft") ? "Draft, " : "", state, CG_NAME_SHOWN,
                  coding_gateway_jstr(pr, "author"), CG_NAME_SHOWN, coding_gateway_jstr(pr, "base"),
                  CG_NAME_SHOWN, coding_gateway_jstr(pr, "head_ref"), CG_HASH_SHOWN,
                  coding_gateway_jstr(pr, "head_sha"), coding_gateway_jstr(pr, "url"));
   append_blockers(&sb, ans);
   append_checks(&sb, ans);
   append_reviews(&sb, ans);
   json_object_put(ans);
   return pr_result(&sb);
}

/* One comment as a line: its author (a bot marked) and its text. */
static void append_comment(strbuf_t *sb, struct json_object *c, const char *indent) {
   char body[CG_BODY_SHOWN + 64];
   str_excerpt_line(coding_gateway_jstr(c, "body"), CG_BODY_SHOWN, body, sizeof(body));
   strbuf_appendf(sb, "%s%.*s%s: %s\n", indent, CG_NAME_SHOWN, coding_gateway_jstr(c, "author"),
                  jbool(c, "bot") ? " (bot)" : "", body);
}

static void append_thread(strbuf_t *sb, struct json_object *t) {
   strbuf_appendf(sb, "%s) comment_id %s, %.*s", coding_gateway_jstr(t, "n"),
                  coding_gateway_jstr(t, "comment_id"), CG_NAME_SHOWN,
                  coding_gateway_jstr(t, "path"));
   const char *line = coding_gateway_jstr(t, "line");
   if (line[0] != '\0') {
      strbuf_appendf(sb, ":%s", line);
   }
   strbuf_appendf(sb, "%s%s", jbool(t, "outdated") ? " (outdated)" : "",
                  jbool(t, "resolved_by_copilot") ? " (resolved by Copilot)"
                  : jbool(t, "resolved")          ? " (resolved)"
                                                  : "");
   const char *repeat = coding_gateway_jstr(t, "repeat_of");
   if (repeat[0] != '\0') {
      strbuf_appendf(sb, " (repeats comment_id %s, already answered)", repeat);
   }
   strbuf_append(sb, "\n");
   struct json_object *comments = json_object_object_get(t, "comments");
   const size_t n = json_object_is_type(comments, json_type_array)
                        ? json_object_array_length(comments)
                        : 0;
   for (size_t i = 0; i < n; i++) {
      append_comment(sb, json_object_array_get_idx(comments, i), "   ");
      const int total = json_object_get_int(json_object_object_get(t, "comments_total"));
      if (i == 0 && total > (int)n) {
         strbuf_appendf(sb, "   (%d comments between not shown)\n", total - (int)n);
      }
   }
}

/* Copilot's latest review, and whether it found anything new: a re-review whose
 * threads only repeat ones already answered likely has nothing new to fix. */
static void append_copilot_round(strbuf_t *sb, struct json_object *round) {
   if (!json_object_is_type(round, json_type_object)) {
      return;
   }
   const int threads = json_object_get_int(json_object_object_get(round, "threads"));
   const int fresh = json_object_get_int(json_object_object_get(round, "new"));
   strbuf_appendf(sb,
                  "Copilot's latest review (of %.*s): %d thread%s, %d new, %s repeating "
                  "answered ones.\n",
                  CG_HASH_SHOWN, coding_gateway_jstr(round, "commit"), threads,
                  threads == 1 ? "" : "s", fresh, coding_gateway_jstr(round, "repeats"));
   if (jbool(round, "rereview") && fresh == 0) {
      strbuf_append(sb, "Copilot's re-review found nothing new: likely ready for the user's "
                        "review.\n");
   }
}

char *coding_github_pr_threads(struct json_object *details, int user_id) {
   char *refusal = NULL;
   struct json_object *args = coding_github_pr_args(details, user_id, &refusal);
   if (args != NULL && jbool(details, "all")) {
      json_object_object_add(args, "all", json_object_new_boolean(true));
   }
   struct json_object *ans = args ? coding_github_call(user_id, "pr_threads", args, &refusal)
                                  : NULL;
   if (ans == NULL) {
      return refusal;
   }
   struct json_object *threads = json_object_object_get(ans, "threads");
   const size_t n = json_object_is_type(threads, json_type_array)
                        ? json_object_array_length(threads)
                        : 0;
   strbuf_t sb;
   strbuf_init_with_max(&sb, 4096, CG_THREADS_MAX);
   strbuf_appendf(&sb, "%s review threads at head %.*s: %s%s.\n",
                  jbool(details, "all") ? "All" : "Unresolved", CG_HASH_SHOWN,
                  coding_gateway_jstr(ans, "head_sha"), coding_gateway_jstr(ans, "total"),
                  jbool(ans, "truncated") ? " (not all shown: GitHub's answer was cut)" : "");
   append_copilot_round(&sb, json_object_object_get(ans, "copilot_round"));
   size_t i = 0;
   for (; i < n && strbuf_len(&sb) < CG_THREADS_MAX - CG_ROOM; i++) {
      append_thread(&sb, json_object_array_get_idx(threads, i));
   }
   if (i < n) {
      strbuf_appendf(&sb, "(%zu more threads not shown)\n", n - i);
   }
   static const struct {
      const char *key, *title;
   } more[] = { { "reviews", "Reviews with a comment, oldest first:" },
                { "comments", "The conversation's newest comments, oldest first:" } };
   for (size_t m = 0; m < sizeof(more) / sizeof(more[0]); m++) {
      struct json_object *list = json_object_object_get(ans, more[m].key);
      const size_t k = json_object_is_type(list, json_type_array) ? json_object_array_length(list)
                                                                  : 0;
      for (size_t j = 0; j < k && strbuf_len(&sb) < CG_THREADS_MAX - CG_ROOM; j++) {
         struct json_object *c = json_object_array_get_idx(list, j);
         if (j == 0) {
            strbuf_appendf(&sb, "%s\n", more[m].title);
         }
         const char *state = coding_gateway_jstr(c, "state");
         if (state[0] != '\0') {
            strbuf_appendf(&sb, "- [%s] ", state);
            append_comment(&sb, c, "");
         } else {
            append_comment(&sb, c, "- ");
         }
      }
   }
   json_object_put(ans);
   return pr_result(&sb);
}

char *coding_github_pr_log(struct json_object *details, int user_id) {
   const char *check = coding_gateway_jstr(details, "check");
   if (check[0] == '\0' || strlen(check) > CG_CHECK_MAX) {
      return coding_agent_error("Name the check: {\"check\": \"<a check's name or job number, "
                                "as pr_status lists it>\"}, with the pull request's ref or job.");
   }
   char *refusal = NULL;
   struct json_object *args = coding_github_pr_args(details, user_id, &refusal);
   if (args != NULL) {
      json_object_object_add(args, "check", json_object_new_string(check));
   }
   struct json_object *ans = args ? coding_github_call(user_id, "pr_check_log", args, &refusal)
                                  : NULL;
   if (ans == NULL) {
      return refusal;
   }
   strbuf_t sb;
   strbuf_init_with_max(&sb, 4096, CG_LOG_MAX);
   strbuf_appendf(&sb,
                  "Log of check %.*s (%s), %s\nAn excerpt (its error lines, then its end) "
                  "of %s bytes read%s:\n%s\n",
                  CG_NAME_SHOWN, coding_gateway_jstr(ans, "name"),
                  coding_gateway_jstr(ans, "conclusion"), coding_gateway_jstr(ans, "url"),
                  coding_gateway_jstr(ans, "log_bytes_read"),
                  jbool(ans, "truncated") ? " (the log is longer)" : "",
                  coding_gateway_jstr(ans, "excerpt"));
   json_object_put(ans);
   return pr_result(&sb);
}
