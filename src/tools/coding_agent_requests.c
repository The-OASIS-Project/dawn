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
 * Answering what a coding task waits on.  Allowing a host or a tool widens
 * what the agent reaches, so it is a preview DAWN renders from the gateway's
 * open request and the user's yes (approve, then confirm_approve), for that
 * task or, when the user asks, every task on its repo.  Declining and
 * answering a question reach no further, so they act at once, as does
 * removing a standing approval (grants lists them).
 */

#include <json-c/json.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "core/pending_slots.h"
#include "core/session_history.h"
#include "core/session_manager.h"
#include "core/strbuf.h"
#include "core/tool_call_challenge.h"
#include "core/turn_origin.h"
#include "dawn_error.h"
#include "tools/coding_agent_gateway.h"
#include "tools/coding_agent_internal.h"
#include "tools/tool_pending.h"
#include "tools/tool_registry.h"
#include "utils/string_utils.h"

#define CR_PENDING_APPROVE 1
#define CR_PENDING_TTL_SEC 600
#define CR_CONFIRM_TURNS 3 /* turns after the preview that may confirm it */
#define CR_PENDING_MAX 8
#define CR_VALUE_MAX 256   /* a request's value (a host is at most 253) */
#define CR_KIND_MAX 16     /* domain, tool, question */
#define CR_ANSWER_MAX 2000 /* an answer to the agent's question */
#define CR_LISTED_MAX 8    /* open requests named when the call must pick one */

typedef struct {
   pending_slot_t hdr;
   int64_t job;
   int64_t request_id;
   char kind[CR_KIND_MAX];
   char value[CR_VALUE_MAX];
   char line[CODING_GATEWAY_REQUEST_LINE_MAX]; /* what it asks, in DAWN's words */
   char title[CONV_TITLE_MAX];
   /* The job's repo ("" when it can't be shown: then only this task), and
    * whether the preview allows it for every task there. */
   char repo[CODING_AGENT_REPO_MAX];
   bool for_repo;
   /* Staged by DAWN when the job started waiting, not asked for: confirmed
    * only by the user's very next message, in that conversation.  DAWN's
    * question offers both scopes; the confirm names the one chosen. */
   bool unasked;
   int64_t conv;
} cr_pending_t;

PENDING_ITEM_CHECK(cr_pending_t);
/* An approval the user may still answer holds DAWN's unasked ones back. */
_Static_assert(CR_CONFIRM_TURNS <= TURN_ORIGIN_UNASKED_QUIET_TURNS,
               "the quiet window must cover the confirm window");
static cr_pending_t s_pending[CR_PENDING_MAX];
static pthread_mutex_t s_pending_mutex = PTHREAD_MUTEX_INITIALIZER;
PENDING_ARRAY_CHECK(s_pending);
/* As with a delegate: the user may ask about it before agreeing; the id binds
 * the confirm to the request they were shown. */
static const pending_slots_t s_pending_slots = {
   .base = s_pending,
   .stride = sizeof(s_pending[0]),
   .count = CR_PENDING_MAX,
   .ttl_sec = CR_PENDING_TTL_SEC,
   .later_turns = CR_CONFIRM_TURNS,
};

/* ========== The open requests ========== */

/* Job @p job's task as task_list returns it ({task, pending}); NULL with the
 * error result in @p err_out. */
static struct json_object *fetch_open(int user_id, int64_t job, char **err_out) {
   struct json_object *ans = NULL;
   char *err = NULL;
   if (coding_gateway_call(user_id, "task_list", coding_agent_job_args(job), &ans, &err) !=
       CODING_GW_OK) {
      *err_out = coding_agent_error_free(err);
      return NULL;
   }
   if (!json_object_is_type(json_object_object_get(ans, "task"), json_type_object)) {
      json_object_put(ans);
      *err_out = coding_agent_error("The coding-agent gateway has no record of that job.");
      return NULL;
   }
   return ans;
}

static bool kind_in(const char *kind, const char *const *kinds) {
   for (; *kinds != NULL; kinds++) {
      if (strcmp(kind, *kinds) == 0) {
         return true;
      }
   }
   return false;
}

static int64_t request_id(struct json_object *req) {
   struct json_object *jid = NULL;
   return json_object_object_get_ex(req, "id", &jid) ? json_object_get_int64(jid) : 0;
}

bool coding_agent_repo_name_ok(const char *repo) {
   const size_t n = repo ? strlen(repo) : 0;
   return n > 0 && n < CODING_AGENT_REPO_MAX &&
          strspn(repo, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") == n &&
          strcmp(repo, ".") != 0 && strcmp(repo, "..") != 0;
}

/* @p repo when coding_agent_repo_name_ok(), else "". */
static void copy_repo(char *out, size_t out_len, const char *repo) {
   snprintf(out, out_len, "%s", coding_agent_repo_name_ok(repo) ? repo : "");
}

/* The scope a call names ({"scope": "task"|"repo"}): SUCCESS with *repo_out,
 * or FAILURE for anything else.  Absent: @p dflt. */
static int parse_scope(struct json_object *details, bool dflt, bool *repo_out) {
   struct json_object *js = NULL;
   if (!json_object_object_get_ex(details, "scope", &js)) {
      *repo_out = dflt;
      return SUCCESS;
   }
   const char *s = json_object_get_string(js);
   if (s != NULL && (strcmp(s, "task") == 0 || strcmp(s, "repo") == 0)) {
      *repo_out = strcmp(s, "repo") == 0;
      return SUCCESS;
   }
   return FAILURE;
}

/* What a confirm of @p p allows: the preview's scope, or for DAWN's own
 * question the one the user chose.  NULL, or the error result. */
static char *confirm_scope(const cr_pending_t *p, struct json_object *details, bool *repo_out) {
   bool repo = false;
   if (parse_scope(details, p->for_repo, &repo) != SUCCESS) {
      return coding_agent_error("scope is 'task' or 'repo'.");
   }
   if (!p->unasked && repo != p->for_repo) {
      return coding_agent_error("Confirm what the preview showed; for the other scope, call "
                                "approve again with it.");
   }
   if (repo && p->repo[0] == '\0') {
      return coding_agent_error("That job's repo can't be named, so it can be allowed for "
                                "this task only.");
   }
   *repo_out = repo;
   return NULL;
}

/* "for this task only" or "for every task on <repo>" into @p out. */
static void scope_text(char *out, size_t out_len, bool repo, const char *name) {
   if (repo) {
      snprintf(out, out_len, "for every task on %s", name);
   } else {
      snprintf(out, out_len, "for this task only");
   }
}

/* The open request of one of @p kinds that the call names ({"request": N}),
 * or the only one; NULL with the error result in @p err_out (none open, or
 * several and none named: they are listed).  Borrowed from @p ans. */
static struct json_object *pick(struct json_object *ans,
                                struct json_object *details,
                                const char *const *kinds,
                                int64_t job,
                                char **err_out) {
   struct json_object *jreq = NULL;
   const int64_t named = json_object_object_get_ex(details, "request", &jreq)
                             ? json_object_get_int64(jreq)
                             : 0;
   struct json_object *pending = json_object_object_get(ans, "pending");
   const size_t n = json_object_is_type(pending, json_type_array)
                        ? json_object_array_length(pending)
                        : 0;
   struct json_object *found = NULL;
   size_t matching = 0;
   for (size_t i = 0; i < n; i++) {
      struct json_object *r = json_object_array_get_idx(pending, i);
      if (!kind_in(coding_gateway_jstr(r, "kind"), kinds)) {
         continue;
      }
      matching++;
      if (named == 0 || request_id(r) == named) {
         found = found ? found : r;
      }
   }
   if (found != NULL && (named != 0 || matching == 1)) {
      return found;
   }
   strbuf_t sb;
   strbuf_init(&sb, 256);
   if (matching == 0 && n > 0 && named == 0) {
      strbuf_appendf(&sb, "Coding job #%lld waits on something else: %s.", (long long)job,
                     kind_in("question", kinds) ? "a host or tool takes approve or deny"
                                                : "a question takes answer");
   } else if (matching == 0 || named != 0) {
      strbuf_appendf(&sb, "Coding job #%lld has no such open request.", (long long)job);
   } else {
      strbuf_appendf(&sb, "Coding job #%lld has %zu open requests; name one with \"request\":",
                     (long long)job, matching);
      size_t listed = 0;
      for (size_t i = 0; i < n && listed < CR_LISTED_MAX; i++) {
         struct json_object *r = json_object_array_get_idx(pending, i);
         char line[CODING_GATEWAY_REQUEST_LINE_MAX];
         if (kind_in(coding_gateway_jstr(r, "kind"), kinds)) {
            if (!coding_gateway_request_line(r, line, sizeof(line))) {
               snprintf(line, sizeof(line), "a request DAWN can't show");
            }
            strbuf_appendf(&sb, " %lld (%s);", (long long)request_id(r), line);
            listed++;
         }
      }
   }
   *err_out = coding_agent_error_free(strbuf_steal(&sb));
   return NULL;
}

#define CR_NEXT_MAX 128

/* Send the user's @p decision (or @p answer) on request @p rid of job @p job;
 * what happens next, from the task's status after it, into @p next. */
static int reply(int user_id,
                 int64_t job,
                 int64_t rid,
                 const char *decision,
                 const char *answer,
                 char *next,
                 size_t next_len,
                 char **err) {
   struct json_object *args = coding_agent_job_args(job);
   if (args == NULL) {
      *err = strdup("Couldn't read the coding job.");
      return CODING_GW_FAILED;
   }
   json_object_object_add(args, "request_id", json_object_new_int64(rid));
   if (decision != NULL) {
      json_object_object_add(args, "decision", json_object_new_string(decision));
   }
   if (answer != NULL) {
      json_object_object_add(args, "answer", json_object_new_string(answer));
   }
   struct json_object *ans = NULL;
   const int rc = coding_gateway_call(user_id, "task_reply", args, &ans, err);
   const char *status = coding_gateway_jstr(ans, "status");
   if (strcmp(status, "working") == 0) {
      snprintf(next, next_len, "The agent picks up again.");
   } else if (strcmp(status, "input_required") == 0) {
      snprintf(next, next_len, "It still waits on another request; status shows it.");
   } else {
      snprintf(next, next_len, "It goes on when the gateway has room (now: %.32s).",
               status[0] ? status : "unknown");
   }
   json_object_put(ans);
   return rc;
}

/* ========== approve: a preview, then the user's yes ========== */

static const char *const k_reach[] = { "domain", "tool", NULL };

static const cr_pending_t *approval_live_locked(uint32_t sid, time_t now);

char *coding_requests_approve(struct json_object *details,
                              int user_id,
                              const turn_origin_t *origin) {
   /* A preview may follow reading the agent's words: it shows only the checked
    * host or tool, and allows nothing.  Its confirm needs the user's yes in a
    * turn that read none (confirm_approve). */
   const int64_t job = coding_agent_details_job(details);
   job_record_t rec;
   char *bad = coding_agent_check_job(job, user_id, &rec);
   if (bad != NULL) {
      return bad;
   }
   /* The message answering DAWN's own question (same job and request, or its
    * approval number given as the request, in that conversation) confirms
    * that, not a new preview. */
   struct json_object *jreq = NULL;
   const int64_t named = json_object_object_get_ex(details, "request", &jreq)
                             ? json_object_get_int64(jreq)
                             : 0;
   session_t *ctx = session_get_command_context();
   const int64_t conv = ctx ? session_turn_conversation(ctx) : 0;
   uint32_t asked = 0;
   char asked_line[CODING_GATEWAY_REQUEST_LINE_MAX] = "";
   pthread_mutex_lock(&s_pending_mutex);
   const cr_pending_t *live = approval_live_locked(origin->session_id, pending_slots_now());
   if (live != NULL && live->unasked && live->job == job && live->conv == conv &&
       origin->turn_number == live->hdr.origin.turn_number + 1 &&
       (named == 0 || named == live->request_id || named == (int64_t)live->hdr.item_id)) {
      asked = live->hdr.item_id;
      safe_strscpy(asked_line, live->line);
   }
   pthread_mutex_unlock(&s_pending_mutex);
   if (asked != 0) {
      char msg[CODING_GATEWAY_REQUEST_LINE_MAX + 256];
      snprintf(msg, sizeof(msg),
               "DAWN already asked the user about this (approval %u: %s): if their message "
               "agrees, call confirm_approve {\"id\": %u} (with \"scope\": \"repo\" if they "
               "chose every task on the repo).",
               (unsigned)asked, asked_line, (unsigned)asked);
      return coding_agent_error(msg);
   }
   bool for_repo = false;
   if (parse_scope(details, false, &for_repo) != SUCCESS) {
      return coding_agent_error("scope is 'task' (the default) or 'repo'.");
   }
   char *err = NULL;
   struct json_object *ans = fetch_open(user_id, job, &err);
   if (ans == NULL) {
      return err;
   }
   struct json_object *req = pick(ans, details, k_reach, job, &err);
   if (req == NULL) {
      json_object_put(ans);
      return err;
   }
   char line[CODING_GATEWAY_REQUEST_LINE_MAX];
   if (!coding_gateway_request_line(req, line, sizeof(line))) {
      json_object_put(ans);
      return coding_agent_error("DAWN can't check what that request asks for, so it can't be "
                                "approved here; decline it, or answer the agent another way.");
   }
   char repo[CODING_AGENT_REPO_MAX];
   copy_repo(repo, sizeof(repo), coding_gateway_jstr(json_object_object_get(ans, "task"), "repo"));
   if (for_repo && repo[0] == '\0') {
      json_object_put(ans);
      return coding_agent_error("That job's repo can't be named, so it can be allowed for this "
                                "task only.");
   }
   pending_stage_rc_t rc = PENDING_STAGED;
   pthread_mutex_lock(&s_pending_mutex);
   cr_pending_t *p = (cr_pending_t *)pending_slots_stage(&s_pending_slots, origin, user_id,
                                                         CR_PENDING_APPROVE, pending_slots_now(),
                                                         &rc);
   const uint32_t id = p ? p->hdr.item_id : 0;
   if (p != NULL) {
      p->job = job;
      p->request_id = request_id(req);
      safe_strscpy(p->kind, coding_gateway_jstr(req, "kind"));
      safe_strscpy(p->value, coding_gateway_jstr(json_object_object_get(req, "payload"), "value"));
      safe_strscpy(p->line, line);
      safe_strscpy(p->title, rec.title);
      safe_strscpy(p->repo, repo);
      p->for_repo = for_repo;
   }
   pthread_mutex_unlock(&s_pending_mutex);
   json_object_put(ans);
   if (id == 0) {
      return tool_pending_stage_refusal(rc, "approval");
   }
   char scope[CODING_AGENT_REPO_MAX + 32];
   scope_text(scope, sizeof(scope), for_repo, repo);
   strbuf_t sb;
   strbuf_init(&sb, 512);
   strbuf_appendf(&sb,
                  "About to allow coding job #%lld (%s) to %s, %s. Ask the user to confirm; if "
                  "they say yes in their reply, call coding_agent with action 'confirm_approve' "
                  "and arguments {\"id\": %u} (this one only).",
                  (long long)job, rec.title, line, scope, (unsigned)id);
   return strbuf_steal(&sb);
}

char *coding_requests_confirm_approve(struct json_object *details,
                                      int user_id,
                                      const turn_origin_t *origin) {
   const uint32_t id = coding_agent_details_id(details);
   if (id == 0) {
      return tool_pending_missing_id("approval");
   }
   /* Not in a turn that read the agent's report: a yes there could be its words. */
   if (coding_agent_turn_read(origin)) {
      return coding_agent_error("This turn read the coding agent's report, so the approval "
                                "can't be confirmed in it. Ask the user again.");
   }
   cr_pending_t p;
   turn_origin_rc_t orc = TURN_ORIGIN_OK;
   bool for_repo = false;
   char *bad = NULL;
   pthread_mutex_lock(&s_pending_mutex);
   /* A scope the approval can't take is refused first, leaving it to answer. */
   pending_slot_t *slot = NULL;
   if (pending_slots_find(&s_pending_slots, origin, user_id, CR_PENDING_APPROVE, id,
                          pending_slots_now(), &slot) == PENDING_FOUND) {
      bad = confirm_scope((const cr_pending_t *)slot, details, &for_repo);
   }
   const pending_find_rc_t rc = bad ? PENDING_FOUND
                                    : pending_slots_take(&s_pending_slots, origin, user_id,
                                                         CR_PENDING_APPROVE, id,
                                                         pending_slots_now(), &p, sizeof(p), &orc);
   pthread_mutex_unlock(&s_pending_mutex);
   if (bad != NULL) {
      return bad;
   }
   if (rc != PENDING_FOUND) {
      return tool_pending_take_refusal(rc, orc, "approval");
   }
   if (p.unasked) {
      /* DAWN asked, not the user: only their very next message, in the
       * conversation it was posted in, answers it (taken either way). */
      session_t *ctx = session_get_command_context();
      const int64_t conv = ctx ? session_turn_conversation(ctx) : 0;
      if (origin->turn_number != p.hdr.origin.turn_number + 1 || conv != p.conv) {
         return coding_agent_error("That approval was for the message right after DAWN posted "
                                   "it; ask the user again (approve shows it anew).");
      }
   }
   /* Still the open request the user was shown: same id, kind and value. */
   char *err = NULL;
   struct json_object *ans = fetch_open(user_id, p.job, &err);
   if (ans == NULL) {
      return err;
   }
   bool open = false;
   struct json_object *pending = json_object_object_get(ans, "pending");
   for (size_t i = 0; json_object_is_type(pending, json_type_array) &&
                      i < json_object_array_length(pending) && !open;
        i++) {
      struct json_object *r = json_object_array_get_idx(pending, i);
      open = request_id(r) == p.request_id && strcmp(coding_gateway_jstr(r, "kind"), p.kind) == 0 &&
             strcmp(coding_gateway_jstr(json_object_object_get(r, "payload"), "value"), p.value) ==
                 0;
   }
   json_object_put(ans);
   if (!open) {
      return coding_agent_error("That request isn't open any more; nothing was allowed.");
   }
   char next[CR_NEXT_MAX];
   if (reply(user_id, p.job, p.request_id, for_repo ? "always_repo" : "allow", NULL, next,
             sizeof(next), &err) != CODING_GW_OK) {
      return coding_agent_error_free(err);
   }
   turn_origin_note_answered(session_get_command_context(), p.hdr.origin.turn_number);
   char scope[CODING_AGENT_REPO_MAX + 32];
   scope_text(scope, sizeof(scope), for_repo, p.repo);
   char buf[CONV_TITLE_MAX + CODING_GATEWAY_REQUEST_LINE_MAX + CR_NEXT_MAX + CODING_AGENT_REPO_MAX +
            160];
   snprintf(buf, sizeof(buf), "Allowed: coding job #%lld (%s) may now %s, %s. %s%s",
            (long long)p.job, p.title, p.line, scope, next,
            for_repo ? " It stands until removed (grants lists it; revoke_grant removes it)." : "");
   return strdup(buf);
}

/* Session @p sid's live approval (caller holds s_pending_mutex), or NULL. */
static const cr_pending_t *approval_live_locked(uint32_t sid, time_t now) {
   for (int i = 0; i < CR_PENDING_MAX; i++) {
      const pending_slot_t *h = &s_pending[i].hdr;
      if (h->active && h->origin.session_id == sid && now - h->made_at <= s_pending_slots.ttl_sec) {
         return &s_pending[i];
      }
   }
   return NULL;
}

uint32_t coding_requests_stage_unasked(session_t *session,
                                       int user_id,
                                       int64_t job,
                                       int64_t conv,
                                       const char *title,
                                       const char *repo,
                                       struct json_object *req,
                                       char *line,
                                       size_t line_len,
                                       bool *already_out,
                                       uint32_t *turn_out) {
   *already_out = false;
   *turn_out = 0;
   if (!session || user_id <= 0 || conv <= 0 || !req ||
       !kind_in(coding_gateway_jstr(req, "kind"), k_reach) ||
       !coding_gateway_request_line(req, line, line_len) ||
       coding_delegate_live(session->session_id)) {
      return 0;
   }
   turn_origin_t origin;
   if (!turn_origin_unasked(session, &origin)) {
      return 0; /* busy, or a preview the user may still be answering */
   }
   *turn_out = origin.turn_number;
   const time_t now = pending_slots_now();
   uint32_t id = 0;
   pthread_mutex_lock(&s_pending_mutex);
   const cr_pending_t *live = approval_live_locked(session->session_id, now);
   /* This very request was asked already (a repeated event): nothing new. */
   *already_out = live != NULL && live->unasked && live->job == job &&
                  live->request_id == request_id(req);
   if (live == NULL) {
      pending_stage_rc_t rc = PENDING_STAGED;
      cr_pending_t *p = (cr_pending_t *)pending_slots_stage(&s_pending_slots, &origin, user_id,
                                                            CR_PENDING_APPROVE, now, &rc);
      if (p != NULL) {
         id = p->hdr.item_id;
         p->job = job;
         p->request_id = request_id(req);
         safe_strscpy(p->kind, coding_gateway_jstr(req, "kind"));
         safe_strscpy(p->value,
                      coding_gateway_jstr(json_object_object_get(req, "payload"), "value"));
         safe_strscpy(p->line, line);
         safe_strscpy(p->title, title ? title : "");
         copy_repo(p->repo, sizeof(p->repo), repo ? repo : "");
         p->unasked = true;
         p->conv = conv;
      }
   }
   pthread_mutex_unlock(&s_pending_mutex);
   return id;
}

void coding_requests_drop_unasked(uint32_t id) {
   pthread_mutex_lock(&s_pending_mutex);
   for (int i = 0; i < CR_PENDING_MAX; i++) {
      if (s_pending[i].hdr.active && s_pending[i].hdr.item_id == id && s_pending[i].unasked) {
         memset(&s_pending[i], 0, sizeof(s_pending[i]));
      }
   }
   pthread_mutex_unlock(&s_pending_mutex);
}

/* What confirm_approve, waiting for the user's reply code, allows. */
int coding_requests_describe(const char *value, char *out, size_t out_len, int *valid_for_sec) {
   struct json_object *details = tool_parse_details(value, true);
   const uint32_t id = coding_agent_details_id(details);
   turn_origin_t origin;
   int n = -1;
   snprintf(out, out_len, "that approval isn't waiting any more; ask for it again");
   if (id == 0 || !turn_origin_capture(&origin)) {
      json_object_put(details);
      return FAILURE;
   }
   pthread_mutex_lock(&s_pending_mutex);
   pending_slot_t *slot = NULL;
   if (pending_slots_find(&s_pending_slots, &origin, tool_get_current_user_id(), CR_PENDING_APPROVE,
                          id, pending_slots_now(), &slot) == PENDING_FOUND) {
      const cr_pending_t *p = (const cr_pending_t *)slot;
      bool for_repo = false;
      char *bad = confirm_scope(p, details, &for_repo);
      if (bad == NULL) {
         char scope[CODING_AGENT_REPO_MAX + 32];
         scope_text(scope, sizeof(scope), for_repo, p->repo);
         n = snprintf(out, out_len, "allow coding job #%lld (%s) to %s, %s", (long long)p->job,
                      p->title, p->line, scope);
         *valid_for_sec = pending_slots_valid_for(&s_pending_slots, slot, pending_slots_now());
      }
      free(bad);
   }
   pthread_mutex_unlock(&s_pending_mutex);
   json_object_put(details);
   return (n > 0 && (size_t)n < out_len) ? SUCCESS : FAILURE;
}

/* ========== deny and answer: at once ========== */

char *coding_requests_deny(struct json_object *details, int user_id) {
   const int64_t job = coding_agent_details_job(details);
   job_record_t rec;
   char *bad = coding_agent_check_job(job, user_id, &rec);
   if (bad != NULL) {
      return bad;
   }
   char *err = NULL;
   struct json_object *ans = fetch_open(user_id, job, &err);
   if (ans == NULL) {
      return err;
   }
   struct json_object *req = pick(ans, details, k_reach, job, &err);
   if (req == NULL) {
      json_object_put(ans);
      return err;
   }
   char line[CODING_GATEWAY_REQUEST_LINE_MAX];
   if (!coding_gateway_request_line(req, line, sizeof(line))) {
      snprintf(line, sizeof(line), "do what it asked");
   }
   const int64_t rid = request_id(req);
   json_object_put(ans);
   char next[CR_NEXT_MAX];
   if (reply(user_id, job, rid, "deny", NULL, next, sizeof(next), &err) != CODING_GW_OK) {
      return coding_agent_error_free(err);
   }
   char buf[CONV_TITLE_MAX + CODING_GATEWAY_REQUEST_LINE_MAX + CR_NEXT_MAX + 96];
   snprintf(buf, sizeof(buf), "Declined: coding job #%lld (%s) won't %s. %s", (long long)job,
            rec.title, line, next);
   return strdup(buf);
}

static const char *const k_question[] = { "question", NULL };

char *coding_requests_answer(struct json_object *details, int user_id) {
   const int64_t job = coding_agent_details_job(details);
   job_record_t rec;
   char *bad = coding_agent_check_job(job, user_id, &rec);
   if (bad != NULL) {
      return bad;
   }
   const char *text = coding_gateway_jstr(details, "answer");
   if (text[0] == '\0' || strlen(text) > CR_ANSWER_MAX) {
      return coding_agent_error("answer needs {\"job\": N, \"answer\": \"<the user's answer, at "
                                "most 2000 characters>\"}.");
   }
   char *err = NULL;
   struct json_object *ans = fetch_open(user_id, job, &err);
   if (ans == NULL) {
      return err;
   }
   struct json_object *req = pick(ans, details, k_question, job, &err);
   if (req == NULL) {
      json_object_put(ans);
      return err;
   }
   const int64_t rid = request_id(req);
   json_object_put(ans);
   char next[CR_NEXT_MAX];
   if (reply(user_id, job, rid, NULL, text, next, sizeof(next), &err) != CODING_GW_OK) {
      return coding_agent_error_free(err);
   }
   strbuf_t sb;
   strbuf_init(&sb, 256);
   strbuf_appendf(&sb, "Sent the answer to coding job #%lld (%s): %s\n%s", (long long)job,
                  rec.title, text, next);
   return strbuf_steal(&sb);
}

/* ========== standing approvals: list and remove ========== */

char *coding_requests_grants(int user_id) {
   struct json_object *ans = NULL;
   char *err = NULL;
   if (coding_gateway_call(user_id, "grants_list", json_object_new_object(), &ans, &err) !=
       CODING_GW_OK) {
      return coding_agent_error_free(err);
   }
   struct json_object *grants = json_object_object_get(ans, "grants");
   const size_t n = json_object_is_type(grants, json_type_array) ? json_object_array_length(grants)
                                                                 : 0;
   strbuf_t sb;
   strbuf_init(&sb, 512);
   strbuf_append(&sb, n == 0 ? "No standing approvals: coding tasks ask for any host or tool "
                               "beyond the defaults."
                             : "Standing approvals (revoke_grant {grant: N} removes one):\n");
   for (size_t i = 0; i < n; i++) {
      struct json_object *g = json_object_array_get_idx(grants, i);
      /* Shown as a request is: a checked host or tool name, or nothing. */
      struct json_object *req = json_object_new_object();
      struct json_object *payload = json_object_new_object();
      json_object_object_add(payload, "value",
                             json_object_new_string(coding_gateway_jstr(g, "value")));
      json_object_object_add(req, "kind", json_object_new_string(coding_gateway_jstr(g, "kind")));
      json_object_object_add(req, "payload", payload);
      char line[CODING_GATEWAY_REQUEST_LINE_MAX];
      if (!coding_gateway_request_line(req, line, sizeof(line))) {
         snprintf(line, sizeof(line), "something DAWN can't show");
      }
      json_object_put(req);
      const char *repo = coding_gateway_jstr(g, "repo");
      const bool named = coding_agent_repo_name_ok(repo);
      strbuf_appendf(&sb, "- grant %lld: %s, for %s%s\n", (long long)request_id(g), line,
                     repo[0] == '\0' ? "every repo"
                     : named         ? "every task on "
                                     : "a repo DAWN can't show",
                     named ? repo : "");
   }
   json_object_put(ans);
   return strbuf_steal(&sb);
}

char *coding_requests_revoke(struct json_object *details, int user_id) {
   struct json_object *jg = NULL;
   const int64_t grant = json_object_object_get_ex(details, "grant", &jg)
                             ? json_object_get_int64(jg)
                             : 0;
   if (grant <= 0) {
      return coding_agent_error("revoke_grant needs {\"grant\": N} (grants lists them).");
   }
   struct json_object *args = json_object_new_object();
   json_object_object_add(args, "grant_id", json_object_new_int64(grant));
   struct json_object *ans = NULL;
   char *err = NULL;
   const int rc = coding_gateway_call(user_id, "grant_revoke", args, &ans, &err);
   json_object_put(ans);
   if (rc != CODING_GW_OK) {
      return coding_agent_error_free(err);
   }
   free(err);
   char buf[128];
   snprintf(buf, sizeof(buf), "Removed standing approval %lld (grants shows what still stands).",
            (long long)grant);
   return strdup(buf);
}
