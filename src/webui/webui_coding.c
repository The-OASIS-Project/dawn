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
 * The coding-job card in the WebUI: what a coding job waits on
 * (coding_pending), and the user's click answering it (coding_reply).  Both ask
 * the agent gateway, so they run on its short-lived threads, never the lws
 * thread; answers go to every tab of the user (a tab may close meanwhile).  A
 * click is checked against the live request in coding_requests_card_reply.
 */

#include <inttypes.h>
#include <json-c/json.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/coding_agent_card.h"
#include "tools/coding_agent_gateway.h"
#include "tools/tool_registry.h"
#include "utils/string_utils.h"
#include "webui/webui_internal.h"

#define WC_JOBS_MAX 16     /* jobs one coding_pending asks about */
#define WC_USERS_MAX 8     /* users with a coding_pending lookup running at once */
#define WC_INFLIGHT_MAX 16 /* card replies being sent at once (one per user and job) */
#define WC_LINE_MAX CODING_GATEWAY_REQUEST_LINE_MAX
#define WC_ANSWER_MAX 2048 /* the server's cap is 2000 bytes; longer is refused there */
#define WC_NONCE_MAX 40    /* the clicking tab's own reference, echoed back */
#define WC_HEAD_MAX (CODING_HEAD_LEN + 1)
#define WC_SHA_MAX (CODING_SHA256_LEN + 1)

static void send_to_user(int user_id, const char *type, struct json_object *payload) {
   struct json_object *frame = json_object_new_object();
   json_object_object_add(frame, "type", json_object_new_string(type));
   json_object_object_add(frame, "payload", payload);
   webui_broadcast_json_to_user(user_id, frame, true);
}

/* Whether @p job is @p user_id's coding job (a database read, no gateway). */
static bool users_coding_job(int user_id, int64_t job) {
   job_record_t rec;
   return job > 0 && conv_db_job_get(job, user_id, &rec) == AUTH_DB_SUCCESS &&
          strcmp(rec.job_kind, JOB_KIND_CODING) == 0;
}

/* =============================================================================
 * coding_pending {jobs: [N, ...]}
 *
 * One lookup per user at a time: asks that come while one runs are merged into
 * the next round, so many tabs (or events) cost one gateway pass, in order.
 * ============================================================================= */

static struct {
   int user_id; /* 0: free */
   int n;       /* jobs waiting for the next round */
   int64_t jobs[WC_JOBS_MAX];
} s_users[WC_USERS_MAX];
static pthread_mutex_t s_users_mutex = PTHREAD_MUTEX_INITIALIZER;

static void merge_jobs(int slot, const int64_t *jobs, int n) {
   for (int i = 0; i < n; i++) {
      bool seen = false;
      for (int j = 0; j < s_users[slot].n && !seen; j++) {
         seen = s_users[slot].jobs[j] == jobs[i];
      }
      if (!seen && s_users[slot].n < WC_JOBS_MAX) {
         s_users[slot].jobs[s_users[slot].n++] = jobs[i];
      }
   }
}

static void pending_run(void *arg) {
   const int slot = (int)(intptr_t)arg;
   for (;;) {
      int64_t jobs[WC_JOBS_MAX];
      pthread_mutex_lock(&s_users_mutex);
      const int user_id = s_users[slot].user_id;
      const int n = s_users[slot].n;
      memcpy(jobs, s_users[slot].jobs, sizeof(jobs[0]) * (size_t)n);
      s_users[slot].n = 0;
      if (n == 0) {
         s_users[slot].user_id = 0; /* nothing more asked: the slot is free */
      }
      pthread_mutex_unlock(&s_users_mutex);
      if (n == 0) {
         return;
      }
      struct json_object *list = json_object_new_array();
      for (int i = 0; i < n && !coding_gateway_stopping(); i++) {
         char *err = NULL;
         struct json_object *open = coding_requests_open(user_id, jobs[i], &err);
         free(err); /* the gateway is away: nothing shown for it */
         if (open == NULL) {
            continue;
         }
         json_object_object_add(open, "job", json_object_new_int64(jobs[i]));
         json_object_array_add(list, open);
      }
      if (coding_gateway_stopping()) {
         json_object_put(list); /* the WebUI may be going away */
         continue;
      }
      struct json_object *payload = json_object_new_object();
      json_object_object_add(payload, "jobs", list);
      send_to_user(user_id, "coding_pending_response", payload);
   }
}

void webui_coding_handle_pending(ws_connection_t *conn, json_object *payload) {
   if (!conn || !conn->authenticated || conn->is_satellite || conn->auth_user_id <= 0) {
      return;
   }
   struct json_object *arr = NULL;
   if (!json_object_object_get_ex(payload, "jobs", &arr) ||
       !json_object_is_type(arr, json_type_array)) {
      return;
   }
   const int user_id = conn->auth_user_id;
   /* Only the user's coding jobs reach the gateway (others cost nothing). */
   int64_t jobs[WC_JOBS_MAX];
   int n = 0;
   for (size_t i = 0; i < json_object_array_length(arr) && i < 2 * WC_JOBS_MAX && n < WC_JOBS_MAX;
        i++) {
      const int64_t job = json_object_get_int64(json_object_array_get_idx(arr, i));
      if (users_coding_job(user_id, job)) {
         jobs[n++] = job;
      }
   }
   if (n == 0) {
      return;
   }
   int slot = -1, free_slot = -1;
   bool running = false;
   pthread_mutex_lock(&s_users_mutex);
   for (int i = 0; i < WC_USERS_MAX && slot < 0; i++) {
      if (s_users[i].user_id == user_id) {
         slot = i;
         running = true;
      } else if (s_users[i].user_id == 0 && free_slot < 0) {
         free_slot = i;
      }
   }
   if (slot < 0 && free_slot >= 0) {
      slot = free_slot;
      s_users[slot].user_id = user_id;
      s_users[slot].n = 0;
   }
   if (slot >= 0) {
      merge_jobs(slot, jobs, n);
   }
   pthread_mutex_unlock(&s_users_mutex);
   if (slot < 0 || running) {
      return; /* the running round takes it (or too many users at once) */
   }
   if (coding_gateway_run_async(pending_run, (void *)(intptr_t)slot) != SUCCESS) {
      pthread_mutex_lock(&s_users_mutex);
      s_users[slot].user_id = 0;
      s_users[slot].n = 0;
      pthread_mutex_unlock(&s_users_mutex);
   }
}

/* =============================================================================
 * coding_reply {job, request_id, decision: allow|allow_repo|deny|answer, answer?,
 *               line?, nonce?}
 * ============================================================================= */

typedef struct {
   int user_id;
   int64_t job;
   int64_t request_id;
   coding_card_decision_t decision;
   char answer[WC_ANSWER_MAX];
   char line[WC_LINE_MAX];
   char nonce[WC_NONCE_MAX];
} wc_reply_t;

/* Card replies on their way, by user and job: one at a time per job, so a
 * double click (or a scripted burst) doesn't stack threads. */
static struct {
   int user_id;
   int64_t job;
} s_inflight[WC_INFLIGHT_MAX];
static pthread_mutex_t s_inflight_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef enum {
   WC_CLAIMED,
   WC_ALREADY,
   WC_FULL
} wc_claim_t;

static wc_claim_t inflight_claim(int user_id, int64_t job) {
   wc_claim_t rc = WC_FULL;
   int free_slot = -1;
   pthread_mutex_lock(&s_inflight_mutex);
   for (int i = 0; i < WC_INFLIGHT_MAX; i++) {
      if (s_inflight[i].job == job && s_inflight[i].user_id == user_id) {
         rc = WC_ALREADY;
         free_slot = -1;
         break;
      }
      if (s_inflight[i].job == 0 && free_slot < 0) {
         free_slot = i;
      }
   }
   if (rc != WC_ALREADY && free_slot >= 0) {
      s_inflight[free_slot].user_id = user_id;
      s_inflight[free_slot].job = job;
      rc = WC_CLAIMED;
   }
   pthread_mutex_unlock(&s_inflight_mutex);
   return rc;
}

static void inflight_release(int user_id, int64_t job) {
   pthread_mutex_lock(&s_inflight_mutex);
   for (int i = 0; i < WC_INFLIGHT_MAX; i++) {
      if (s_inflight[i].job == job && s_inflight[i].user_id == user_id) {
         s_inflight[i].job = 0;
         s_inflight[i].user_id = 0;
      }
   }
   pthread_mutex_unlock(&s_inflight_mutex);
}

static void send_reply_result(int user_id,
                              int64_t job,
                              int64_t request_id,
                              const char *nonce,
                              bool ok,
                              const char *message) {
   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "job", json_object_new_int64(job));
   json_object_object_add(payload, "request_id", json_object_new_int64(request_id));
   json_object_object_add(payload, "nonce", json_object_new_string(nonce ? nonce : ""));
   json_object_object_add(payload, "ok", json_object_new_boolean(ok));
   json_object_object_add(payload, "message", json_object_new_string(message ? message : ""));
   send_to_user(user_id, "coding_reply_response", payload);
}

static const char *decision_name(coding_card_decision_t d) {
   switch (d) {
      case CODING_CARD_ALLOW:
         return "allow";
      case CODING_CARD_ALLOW_REPO:
         return "allow_repo";
      case CODING_CARD_DENY:
         return "deny";
      default:
         return "answer";
   }
}

static void reply_run(void *arg) {
   wc_reply_t *w = arg;
   char *result = coding_requests_card_reply(w->user_id, w->job, w->request_id, w->decision,
                                             w->decision == CODING_CARD_ANSWER ? w->answer : NULL,
                                             w->line);
   const bool failed = result == NULL || tool_result_is_error(result);
   const char *text = result ? result : "The reply couldn't be sent.";
   if (failed && result != NULL) {
      text += strlen(TOOL_RESULT_ERROR_MARK);
   }
   /* The decision by name only; an answer's words stay out of the log. */
   OLOG_INFO("coding_agent: card %s on job %" PRId64 " request %" PRId64 " by user %d: %s",
             decision_name(w->decision), w->job, w->request_id, w->user_id,
             failed ? "refused" : "sent");
   if (!coding_gateway_stopping()) {
      send_reply_result(w->user_id, w->job, w->request_id, w->nonce, !failed, text);
   }
   free(result);
   inflight_release(w->user_id, w->job);
   free(w);
}

void webui_coding_handle_reply(ws_connection_t *conn, json_object *payload) {
   if (!conn || !conn->authenticated || conn->is_satellite || conn->auth_user_id <= 0) {
      return;
   }
   const int user_id = conn->auth_user_id;
   struct json_object *o = NULL;
   const int64_t job = json_object_object_get_ex(payload, "job", &o) ? json_object_get_int64(o) : 0;
   const int64_t rid = json_object_object_get_ex(payload, "request_id", &o)
                           ? json_object_get_int64(o)
                           : 0;
   const char *nonce = json_object_object_get_ex(payload, "nonce", &o) ? json_object_get_string(o)
                                                                       : "";
   if (nonce == NULL || strlen(nonce) >= WC_NONCE_MAX) {
      nonce = "";
   }
   const char *choice = json_object_object_get_ex(payload, "decision", &o)
                            ? json_object_get_string(o)
                            : "";
   /* The client's word is matched, never passed on: the gateway's "always"
    * (every repo) can't be reached from here. */
   coding_card_decision_t decision;
   if (choice && strcmp(choice, "allow") == 0) {
      decision = CODING_CARD_ALLOW;
   } else if (choice && strcmp(choice, "allow_repo") == 0) {
      decision = CODING_CARD_ALLOW_REPO;
   } else if (choice && strcmp(choice, "deny") == 0) {
      decision = CODING_CARD_DENY;
   } else if (choice && strcmp(choice, "answer") == 0) {
      decision = CODING_CARD_ANSWER;
   } else {
      send_reply_result(user_id, job, rid, nonce, false, "Malformed reply.");
      return;
   }
   const char *answer = json_object_object_get_ex(payload, "answer", &o) ? json_object_get_string(o)
                                                                         : "";
   const char *line = json_object_object_get_ex(payload, "line", &o) ? json_object_get_string(o)
                                                                     : "";
   if (job <= 0 || rid <= 0 || (answer && strlen(answer) >= WC_ANSWER_MAX) ||
       (line && strlen(line) >= WC_LINE_MAX)) {
      send_reply_result(user_id, job, rid, nonce, false, "Malformed reply.");
      return;
   }
   /* The user's own coding job before anything is held for it. */
   if (!users_coding_job(user_id, job)) {
      send_reply_result(user_id, job, rid, nonce, false, "No coding job with that number.");
      return;
   }
   const wc_claim_t claim = inflight_claim(user_id, job);
   if (claim != WC_CLAIMED) {
      send_reply_result(user_id, job, rid, nonce, false,
                        claim == WC_ALREADY ? "A reply for this job is already on its way."
                                            : "Too many replies at once; try again in a moment.");
      return;
   }
   wc_reply_t *w = calloc(1, sizeof(*w));
   if (w != NULL) {
      w->user_id = user_id;
      w->job = job;
      w->request_id = rid;
      w->decision = decision;
      snprintf(w->answer, sizeof(w->answer), "%s", answer ? answer : "");
      snprintf(w->line, sizeof(w->line), "%s", line ? line : "");
      snprintf(w->nonce, sizeof(w->nonce), "%s", nonce);
   }
   if (w == NULL || coding_gateway_run_async(reply_run, w) != SUCCESS) {
      free(w);
      inflight_release(user_id, job);
      send_reply_result(user_id, job, rid, nonce, false, "The reply couldn't be sent now.");
   }
}

/* =============================================================================
 * coding_grants {} and coding_grant_revoke {id}: the standing approvals list
 *
 * Answered to every tab of the user (an open list there stays current).  The
 * handshake's origin check covers a forged revoke; the gateway checks owner.
 * One at a time per user: a list asked for meanwhile is the running one's
 * answer; a second revoke is asked to wait a moment.
 * ============================================================================= */

typedef struct {
   int user_id;
   int64_t revoke; /* 0: just the list */
} wc_grants_t;

static int s_granting[WC_USERS_MAX]; /* users with a grants request running (0: free) */
static pthread_mutex_t s_granting_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Claim a slot for @p user_id: true when none of theirs runs and one is free. */
static bool granting_claim(int user_id, bool *busy) {
   bool claimed = false;
   *busy = false;
   pthread_mutex_lock(&s_granting_mutex);
   int free_slot = -1;
   for (int i = 0; i < WC_USERS_MAX; i++) {
      *busy = *busy || s_granting[i] == user_id;
      if (s_granting[i] == 0 && free_slot < 0) {
         free_slot = i;
      }
   }
   if (!*busy && free_slot >= 0) {
      s_granting[free_slot] = user_id;
      claimed = true;
   }
   pthread_mutex_unlock(&s_granting_mutex);
   return claimed;
}

static void granting_release(int user_id) {
   pthread_mutex_lock(&s_granting_mutex);
   for (int i = 0; i < WC_USERS_MAX; i++) {
      if (s_granting[i] == user_id) {
         s_granting[i] = 0;
         break;
      }
   }
   pthread_mutex_unlock(&s_granting_mutex);
}

static void grants_run(void *arg) {
   wc_grants_t *w = arg;
   struct json_object *payload = json_object_new_object();
   if (w->revoke > 0) {
      char *result = coding_requests_grant_revoke(w->user_id, w->revoke);
      const bool failed = result == NULL || tool_result_is_error(result);
      const char *text = result ? result : "The approval couldn't be removed.";
      if (failed && result != NULL) {
         text += strlen(TOOL_RESULT_ERROR_MARK);
      }
      OLOG_INFO("coding_agent: grant %" PRId64 " revoke by user %d: %s", w->revoke, w->user_id,
                failed ? "refused" : "done");
      json_object_object_add(payload, "revoked", json_object_new_int64(w->revoke));
      json_object_object_add(payload, "ok", json_object_new_boolean(!failed));
      json_object_object_add(payload, "message", json_object_new_string(text));
      free(result);
   }
   char *err = NULL;
   struct json_object *list = coding_requests_grant_list(w->user_id, &err);
   if (list != NULL) {
      json_object_object_add(payload, "grants", list);
   } else {
      json_object_object_add(payload, "error",
                             json_object_new_string("The coding-agent gateway didn't answer."));
   }
   free(err);
   if (!coding_gateway_stopping()) {
      send_to_user(w->user_id, "coding_grants_response", payload);
   } else {
      json_object_put(payload);
   }
   granting_release(w->user_id);
   free(w);
}

static void grants_start(ws_connection_t *conn, int64_t revoke) {
   if (!conn || !conn->authenticated || conn->is_satellite || conn->auth_user_id <= 0) {
      return;
   }
   bool busy = false;
   if (!granting_claim(conn->auth_user_id, &busy)) {
      if (busy && revoke == 0) {
         return; /* the one running answers every tab */
      }
      struct json_object *payload = json_object_new_object();
      json_object_object_add(payload, "error", json_object_new_string("Try again in a moment."));
      if (revoke > 0) {
         json_object_object_add(payload, "revoked", json_object_new_int64(revoke));
         json_object_object_add(payload, "ok", json_object_new_boolean(false));
         json_object_object_add(payload, "message",
                                json_object_new_string("Try again in a moment."));
      }
      send_to_user(conn->auth_user_id, "coding_grants_response", payload);
      return;
   }
   wc_grants_t *w = calloc(1, sizeof(*w));
   if (w != NULL) {
      w->user_id = conn->auth_user_id;
      w->revoke = revoke;
   }
   if (w == NULL || coding_gateway_run_async(grants_run, w) != SUCCESS) {
      free(w);
      granting_release(conn->auth_user_id);
      struct json_object *payload = json_object_new_object();
      json_object_object_add(payload, "error", json_object_new_string("Try again in a moment."));
      send_to_user(conn->auth_user_id, "coding_grants_response", payload);
   }
}

void webui_coding_handle_grants(ws_connection_t *conn, json_object *payload) {
   (void)payload;
   grants_start(conn, 0);
}

void webui_coding_handle_grant_revoke(ws_connection_t *conn, json_object *payload) {
   struct json_object *o = NULL;
   const int64_t id = json_object_object_get_ex(payload, "id", &o) ? json_object_get_int64(o) : 0;
   if (id <= 0) {
      return;
   }
   grants_start(conn, id);
}

/* =============================================================================
 * coding_view {job, what: changes|file, path?, view?, scope?, nonce}: the changes viewer
 *
 * A job's work from the gateway's mirror, answered to the asking tab only (a
 * file reply can be megabytes once serialized).  One request per user at a
 * time; a newer one replaces the one still waiting, so clicking through files
 * never gets a refusal.  Logged by job and kind only: paths are the agent's.
 * ============================================================================= */

#define WC_VIEW_PATH_MAX 2100 /* a rename's two paths and its separator */
#define WC_VIEW_SCOPE_MAX 41  /* a full commit hash */

typedef struct {
   int64_t job;
   bool file;
   bool diff; /* else new */
   uint32_t session_id;
   char path[WC_VIEW_PATH_MAX];
   char scope[WC_VIEW_SCOPE_MAX]; /* "" (all), "all", "since_push" or a commit's hex */
   char nonce[WC_NONCE_MAX];
} wc_view_req_t;

/* A viewer scope the gateway may be asked for: all, since_push, or 4 to 40 lowercase
 * hex digits (one of the job's commits; the gateway checks it is). */
static bool view_scope_ok(const char *scope) {
   const size_t n = strlen(scope);
   if (n == 0 || strcmp(scope, "all") == 0 || strcmp(scope, "since_push") == 0) {
      return true;
   }
   if (n < 4 || n >= WC_VIEW_SCOPE_MAX) {
      return false;
   }
   return strspn(scope, "0123456789abcdef") == n;
}

static struct {
   int user_id; /* 0: free */
   bool waiting;
   wc_view_req_t next;
} s_viewers[WC_USERS_MAX];
static pthread_mutex_t s_viewers_mutex = PTHREAD_MUTEX_INITIALIZER;

/* @p json to session @p session_id while it is still @p user_id's tab. */
static void send_to_session(uint32_t session_id, int user_id, struct json_object *frame) {
   const char *text = json_object_to_json_string_ext(frame, JSON_C_TO_STRING_PLAIN);
   char *json = text ? strdup(text) : NULL;
   json_object_put(frame);
   session_t *session = json ? session_get(session_id) : NULL; /* NULL once gone */
   if (session == NULL) {
      free(json);
      return;
   }
   pthread_mutex_lock(&session->metrics_mutex);
   const int owner = session->metrics.user_id;
   pthread_mutex_unlock(&session->metrics_mutex);
   if (owner == user_id) {
      webui_send_session_json_take(session, json);
   } else {
      free(json); /* that tab logged in as someone else since */
   }
   session_release(session);
}

static void view_reply(int user_id,
                       const wc_view_req_t *r,
                       struct json_object *result,
                       const char *error) {
   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "job", json_object_new_int64(r->job));
   json_object_object_add(payload, "what", json_object_new_string(r->file ? "file" : "changes"));
   json_object_object_add(payload, "nonce", json_object_new_string(r->nonce));
   json_object_object_add(payload, "ok", json_object_new_boolean(result != NULL));
   if (result != NULL) {
      json_object_object_add(payload, "result", result);
   } else {
      json_object_object_add(payload, "error", json_object_new_string(error));
   }
   struct json_object *frame = json_object_new_object();
   json_object_object_add(frame, "type", json_object_new_string("coding_view_response"));
   json_object_object_add(frame, "payload", payload);
   send_to_session(r->session_id, user_id, frame);
}

static void view_run(void *arg) {
   const int slot = (int)(intptr_t)arg;
   for (;;) {
      pthread_mutex_lock(&s_viewers_mutex);
      const int user_id = s_viewers[slot].user_id;
      const bool waiting = s_viewers[slot].waiting;
      const wc_view_req_t r = s_viewers[slot].next;
      s_viewers[slot].waiting = false;
      if (!waiting) {
         s_viewers[slot].user_id = 0; /* nothing more asked: the slot is free */
      }
      pthread_mutex_unlock(&s_viewers_mutex);
      if (!waiting || coding_gateway_stopping()) {
         if (waiting) {
            pthread_mutex_lock(&s_viewers_mutex);
            s_viewers[slot].user_id = 0;
            pthread_mutex_unlock(&s_viewers_mutex);
         }
         return;
      }
      char *err = NULL;
      struct json_object *result = coding_view_fetch(user_id, r.job, r.file, r.path,
                                                     r.diff ? "diff" : "new", r.scope, &err);
      const char *text = err ? err : "The coding-agent gateway didn't answer.";
      if (err != NULL && tool_result_is_error(err)) {
         text += strlen(TOOL_RESULT_ERROR_MARK);
      }
      OLOG_INFO("coding_agent: viewer %s for job %" PRId64 " by user %d: %s",
                r.file ? "file" : "changes", r.job, user_id, result ? "sent" : "refused");
      if (!coding_gateway_stopping()) {
         view_reply(user_id, &r, result, text);
      } else {
         json_object_put(result);
      }
      free(err);
   }
}

void webui_coding_handle_view(ws_connection_t *conn, json_object *payload) {
   if (!conn || !conn->authenticated || conn->is_satellite || conn->auth_user_id <= 0) {
      return;
   }
   session_t *s = conn_get_session(conn);
   if (s == NULL) {
      return; /* nowhere to answer */
   }
   const int user_id = conn->auth_user_id;
   struct json_object *o = NULL;
   wc_view_req_t r = { .session_id = s->session_id };
   r.job = json_object_object_get_ex(payload, "job", &o) ? json_object_get_int64(o) : 0;
   const char *nonce = json_object_object_get_ex(payload, "nonce", &o) ? json_object_get_string(o)
                                                                       : "";
   snprintf(r.nonce, sizeof(r.nonce), "%s", nonce && strlen(nonce) < WC_NONCE_MAX ? nonce : "");
   /* The client's words are matched, never passed on. */
   const char *what = json_object_object_get_ex(payload, "what", &o) ? json_object_get_string(o)
                                                                     : "";
   const char *view = json_object_object_get_ex(payload, "view", &o) ? json_object_get_string(o)
                                                                     : "diff";
   const char *path = json_object_object_get_ex(payload, "path", &o) ? json_object_get_string(o)
                                                                     : "";
   const char *scope = json_object_object_get_ex(payload, "scope", &o) ? json_object_get_string(o)
                                                                       : "";
   r.file = what && strcmp(what, "file") == 0;
   r.diff = !view || strcmp(view, "new") != 0;
   const bool known = what && (r.file || strcmp(what, "changes") == 0) &&
                      (!view || strcmp(view, "diff") == 0 || strcmp(view, "new") == 0);
   if (!known || r.job <= 0 || !path || strlen(path) >= sizeof(r.path) ||
       (r.file && path[0] == '\0') || !scope || !view_scope_ok(scope)) {
      view_reply(user_id, &r, NULL, "Malformed request.");
      return;
   }
   snprintf(r.path, sizeof(r.path), "%s", path);
   snprintf(r.scope, sizeof(r.scope), "%s", scope);
   if (!users_coding_job(user_id, r.job)) {
      view_reply(user_id, &r, NULL, "No coding job with that number.");
      return;
   }
   pthread_mutex_lock(&s_viewers_mutex);
   int slot = -1, free_slot = -1;
   for (int i = 0; i < WC_USERS_MAX && slot < 0; i++) {
      if (s_viewers[i].user_id == user_id) {
         slot = i;
      } else if (s_viewers[i].user_id == 0 && free_slot < 0) {
         free_slot = i;
      }
   }
   const bool start = slot < 0 && free_slot >= 0;
   if (start) {
      slot = free_slot;
      s_viewers[slot].user_id = user_id;
   }
   wc_view_req_t replaced = { .job = 0 };
   if (slot >= 0) {
      if (s_viewers[slot].waiting && s_viewers[slot].next.session_id != r.session_id) {
         replaced = s_viewers[slot].next; /* another tab's: it hears why, not silence */
      }
      s_viewers[slot].next = r; /* replaces one still waiting: only the newest matters */
      s_viewers[slot].waiting = true;
   }
   pthread_mutex_unlock(&s_viewers_mutex);
   if (replaced.job > 0) {
      view_reply(user_id, &replaced, NULL, "Another tab asked meanwhile; try again.");
   }
   if (slot < 0) {
      view_reply(user_id, &r, NULL, "Too many at once; try again in a moment.");
      return;
   }
   if (start && coding_gateway_run_async(view_run, (void *)(intptr_t)slot) != SUCCESS) {
      pthread_mutex_lock(&s_viewers_mutex);
      s_viewers[slot].user_id = 0;
      s_viewers[slot].waiting = false;
      pthread_mutex_unlock(&s_viewers_mutex);
      view_reply(user_id, &r, NULL, "Try again in a moment.");
   }
}

/* =============================================================================
 * coding_publish_preview {job, nonce}, coding_publish {job, head, pr_body_sha256?,
 * pr_title?, pr_description?, nonce}, coding_close {job, nonce, confirm?, head?}:
 * the viewer's buttons
 *
 * The click is the user's confirm: no model turn in between.  Everything shown
 * and sent to the gateway is built here from its state; the client's head and
 * description hash only bind the push to what was shown.  One at a time per job
 * (with the card's replies); answered to the asking tab.
 * ============================================================================= */

/* A title as typed, before its runs of white space become one space: room for
 * a few times what the checked title may hold. */
#define WC_PR_TITLE_RAW_MAX (CODING_PR_TITLE_MAX * 4)

typedef enum {
   WC_ACT_PREVIEW,
   WC_ACT_PUBLISH,
   WC_ACT_CLOSE,
} wc_act_kind_t;

typedef struct {
   wc_act_kind_t kind;
   int user_id;
   int64_t job;
   uint32_t session_id;
   bool confirm;
   char head[WC_HEAD_MAX];
   char sha[WC_SHA_MAX];
   char nonce[WC_NONCE_MAX];
   bool own_title, own_desc;               /* publish: the user's own text below */
   char pr_title[WC_PR_TITLE_RAW_MAX + 1]; /* as typed: checked where it is used */
   char pr_desc[CODING_PR_DESC_MAX + 1];
} wc_action_t;

static const char *const k_act_types[] = {
   [WC_ACT_PREVIEW] = "coding_publish_preview_response",
   [WC_ACT_PUBLISH] = "coding_publish_response",
   [WC_ACT_CLOSE] = "coding_close_response",
};
static const char *const k_act_names[] = {
   [WC_ACT_PREVIEW] = "publish preview",
   [WC_ACT_PUBLISH] = "publish",
   [WC_ACT_CLOSE] = "close",
};

/* An answer with @p payload's fields (taken) plus the request's job, nonce, ok. */
static void action_reply(const wc_action_t *a, struct json_object *payload, bool ok) {
   json_object_object_add(payload, "job", json_object_new_int64(a->job));
   json_object_object_add(payload, "nonce", json_object_new_string(a->nonce));
   json_object_object_add(payload, "ok", json_object_new_boolean(ok));
   struct json_object *frame = json_object_new_object();
   json_object_object_add(frame, "type", json_object_new_string(k_act_types[a->kind]));
   json_object_object_add(frame, "payload", payload);
   send_to_session(a->session_id, a->user_id, frame);
}

static void action_refuse(const wc_action_t *a, const char *text) {
   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "error", json_object_new_string(text));
   action_reply(a, payload, false);
}

/* A refusal result's words without its mark. */
static const char *refusal_text(const char *err) {
   if (err == NULL) {
      return "The coding-agent gateway didn't answer.";
   }
   return tool_result_is_error(err) ? err + strlen(TOOL_RESULT_ERROR_MARK) : err;
}

static void action_run(void *arg) {
   wc_action_t *a = arg;
   struct json_object *payload = NULL;
   char *err = NULL;
   if (a->kind == WC_ACT_PREVIEW) {
      struct json_object *preview = coding_publish_preview(a->user_id, a->job, &err);
      if (preview != NULL) {
         payload = json_object_new_object();
         json_object_object_add(payload, "preview", preview);
      }
   } else if (a->kind == WC_ACT_PUBLISH) {
      char *pr_url = NULL;
      char *result = coding_publish_click(a->user_id, a->job, a->head, a->sha,
                                          a->own_title ? a->pr_title : NULL,
                                          a->own_desc ? a->pr_desc : NULL, &pr_url);
      if (result != NULL && !tool_result_is_error(result)) {
         payload = json_object_new_object();
         json_object_object_add(payload, "message", json_object_new_string(result));
         if (pr_url != NULL) {
            json_object_object_add(payload, "pr_url", json_object_new_string(pr_url));
         }
         free(result);
      } else {
         err = result;
      }
      free(pr_url);
   } else {
      payload = coding_close_click(a->user_id, a->job, a->confirm, a->head, &err);
   }
   OLOG_INFO("coding_agent: %s of job %" PRId64 " by user %d: %s", k_act_names[a->kind], a->job,
             a->user_id, payload ? "done" : "refused");
   if (coding_gateway_stopping()) {
      json_object_put(payload);
   } else if (payload != NULL) {
      action_reply(a, payload, true);
   } else {
      action_refuse(a, refusal_text(err));
   }
   free(err);
   inflight_release(a->user_id, a->job);
   free(a);
}

static void action_start(ws_connection_t *conn, json_object *payload, wc_act_kind_t kind) {
   if (!conn || !conn->authenticated || conn->is_satellite || conn->auth_user_id <= 0) {
      return;
   }
   session_t *s = conn_get_session(conn);
   if (s == NULL) {
      return; /* nowhere to answer */
   }
   wc_action_t req = { .kind = kind, .user_id = conn->auth_user_id, .session_id = s->session_id };
   struct json_object *o = NULL;
   req.job = json_object_object_get_ex(payload, "job", &o) ? json_object_get_int64(o) : 0;
   const char *nonce = json_object_object_get_ex(payload, "nonce", &o) ? json_object_get_string(o)
                                                                       : "";
   safe_strscpy(req.nonce, nonce && strlen(nonce) < WC_NONCE_MAX ? nonce : "");
   /* "Close anyway" says so by the key, not by an empty head (a close with only
    * uncommitted edits binds head ""). */
   req.confirm = kind == WC_ACT_CLOSE && json_object_object_get_ex(payload, "confirm", &o) &&
                 json_object_get_boolean(o);
   const bool wants_head = kind == WC_ACT_PUBLISH || req.confirm;
   const char *head = json_object_object_get_ex(payload, "head", &o) ? json_object_get_string(o)
                                                                     : NULL;
   const char *sha = json_object_object_get_ex(payload, "pr_body_sha256", &o)
                         ? json_object_get_string(o)
                         : NULL;
   /* The user's own title and description: only on a publish, taken as typed
    * (coding_publish_click checks them as the chat's are). */
   const char *pr_title = kind == WC_ACT_PUBLISH &&
                                  json_object_object_get_ex(payload, "pr_title", &o) &&
                                  json_object_is_type(o, json_type_string)
                              ? json_object_get_string(o)
                              : NULL;
   const char *pr_desc = kind == WC_ACT_PUBLISH &&
                                 json_object_object_get_ex(payload, "pr_description", &o) &&
                                 json_object_is_type(o, json_type_string)
                             ? json_object_get_string(o)
                             : NULL;
   if (req.job <= 0 || (wants_head && (head == NULL || strlen(head) >= sizeof(req.head))) ||
       (kind == WC_ACT_PUBLISH && pr_desc == NULL &&
        (sha == NULL || strlen(sha) >= sizeof(req.sha))) ||
       (sha != NULL && strlen(sha) >= sizeof(req.sha))) {
      action_refuse(&req, "Malformed request.");
      return;
   }
   /* A NUL inside would cut the text short, unseen. */
   struct json_object *jt = NULL, *jd = NULL;
   if ((pr_title && json_object_object_get_ex(payload, "pr_title", &jt) &&
        (size_t)json_object_get_string_len(jt) != strlen(pr_title)) ||
       (pr_desc && json_object_object_get_ex(payload, "pr_description", &jd) &&
        (size_t)json_object_get_string_len(jd) != strlen(pr_desc))) {
      action_refuse(&req, "The pull request title or description holds a NUL character.");
      return;
   }
   if ((pr_title && strlen(pr_title) >= sizeof(req.pr_title)) ||
       (pr_desc && strlen(pr_desc) >= sizeof(req.pr_desc))) {
      char msg[96];
      snprintf(msg, sizeof(msg), "The pull request description must be 1 to %d bytes.",
               CODING_PR_DESC_MAX);
      action_refuse(&req, pr_desc && strlen(pr_desc) >= sizeof(req.pr_desc)
                              ? msg
                              : "The pull request title is too long.");
      return;
   }
   req.own_title = pr_title != NULL;
   req.own_desc = pr_desc != NULL;
   /* Lengths were checked above: these never cut. */
   safe_strscpy(req.pr_title, pr_title ? pr_title : "");
   safe_strscpy(req.pr_desc, pr_desc ? pr_desc : "");
   safe_strscpy(req.head, wants_head ? head : "");
   safe_strscpy(req.sha, sha ? sha : "");
   /* The user's own coding job before anything is held for it. */
   if (!users_coding_job(req.user_id, req.job)) {
      action_refuse(&req, "No coding job with that number.");
      return;
   }
   const wc_claim_t claim = inflight_claim(req.user_id, req.job);
   if (claim != WC_CLAIMED) {
      action_refuse(&req, claim == WC_ALREADY
                              ? "Something for this job is already on its way; try again in a "
                                "moment."
                              : "Too many at once; try again in a moment.");
      return;
   }
   wc_action_t *a = malloc(sizeof(*a));
   if (a != NULL) {
      *a = req;
   }
   if (a == NULL || coding_gateway_run_async(action_run, a) != SUCCESS) {
      free(a);
      inflight_release(req.user_id, req.job);
      action_refuse(&req, "Try again in a moment.");
   }
}

void webui_coding_handle_publish_preview(ws_connection_t *conn, json_object *payload) {
   action_start(conn, payload, WC_ACT_PREVIEW);
}

void webui_coding_handle_publish(ws_connection_t *conn, json_object *payload) {
   action_start(conn, payload, WC_ACT_PUBLISH);
}

void webui_coding_handle_close(ws_connection_t *conn, json_object *payload) {
   action_start(conn, payload, WC_ACT_CLOSE);
}
