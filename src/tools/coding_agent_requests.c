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
 * task only.  Declining and answering a question reach no further, so they
 * act at once.
 */

#include <json-c/json.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "core/pending_slots.h"
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
} cr_pending_t;

PENDING_ITEM_CHECK(cr_pending_t);
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

char *coding_requests_approve(struct json_object *details,
                              int user_id,
                              const turn_origin_t *origin) {
   /* Not in a turn that read the agent's words (a question it asks can try to
    * steer an approval): the waiting notice already names what it asks. */
   if (coding_agent_turn_read(origin)) {
      return coding_agent_error("This turn read the coding agent's words, so an approval can't be "
                                "proposed in it. Tell the user what the job waits on, and let them "
                                "ask for it.");
   }
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
      json_object_put(ans);
      return coding_agent_error("DAWN can't check what that request asks for, so it can't be "
                                "approved here; decline it, or answer the agent another way.");
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
   }
   pthread_mutex_unlock(&s_pending_mutex);
   json_object_put(ans);
   if (id == 0) {
      return tool_pending_stage_refusal(rc, "approval");
   }
   strbuf_t sb;
   strbuf_init(&sb, 512);
   strbuf_appendf(&sb,
                  "About to allow coding job #%lld (%s) to %s, for this task only. Ask the user "
                  "to confirm; if they say yes in their reply, call coding_agent with action "
                  "'confirm_approve' and arguments {\"id\": %u} (this one only).",
                  (long long)job, rec.title, line, (unsigned)id);
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
   pthread_mutex_lock(&s_pending_mutex);
   const pending_find_rc_t rc = pending_slots_take(&s_pending_slots, origin, user_id,
                                                   CR_PENDING_APPROVE, id, pending_slots_now(), &p,
                                                   sizeof(p), &orc);
   pthread_mutex_unlock(&s_pending_mutex);
   if (rc != PENDING_FOUND) {
      return tool_pending_take_refusal(rc, orc, "approval");
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
   if (reply(user_id, p.job, p.request_id, "allow", NULL, next, sizeof(next), &err) !=
       CODING_GW_OK) {
      return coding_agent_error_free(err);
   }
   char buf[CONV_TITLE_MAX + CODING_GATEWAY_REQUEST_LINE_MAX + CR_NEXT_MAX + 96];
   snprintf(buf, sizeof(buf), "Allowed: coding job #%lld (%s) may now %s, for this task. %s",
            (long long)p.job, p.title, p.line, next);
   return strdup(buf);
}

/* What confirm_approve, waiting for the user's reply code, allows. */
int coding_requests_describe(const char *value, char *out, size_t out_len, int *valid_for_sec) {
   struct json_object *details = tool_parse_details(value, true);
   const uint32_t id = coding_agent_details_id(details);
   json_object_put(details);
   turn_origin_t origin;
   int n = -1;
   snprintf(out, out_len, "that approval isn't waiting any more; ask for it again");
   if (id == 0 || !turn_origin_capture(&origin)) {
      return FAILURE;
   }
   pthread_mutex_lock(&s_pending_mutex);
   pending_slot_t *slot = NULL;
   if (pending_slots_find(&s_pending_slots, &origin, tool_get_current_user_id(), CR_PENDING_APPROVE,
                          id, pending_slots_now(), &slot) == PENDING_FOUND) {
      const cr_pending_t *p = (const cr_pending_t *)slot;
      n = snprintf(out, out_len, "allow coding job #%lld (%s) to %s, for this task only",
                   (long long)p->job, p->title, p->line);
      *valid_for_sec = pending_slots_valid_for(&s_pending_slots, slot, pending_slots_now());
   }
   pthread_mutex_unlock(&s_pending_mutex);
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
