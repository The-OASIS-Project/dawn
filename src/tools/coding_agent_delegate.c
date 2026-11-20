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
 * Starting a coding task: delegate stages the task and returns a preview,
 * and the user's yes (confirm, naming its id) creates the coding job and starts
 * its task at the gateway.
 */

#define _GNU_SOURCE /* strcasestr */

#include <json-c/json.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "core/conv_event.h"
#include "core/event_payload.h"
#include "core/job_dispatch.h"
#include "core/job_manager.h"
#include "core/pending_slots.h"
#include "core/session_manager.h"
#include "core/strbuf.h"
#include "core/tool_call_challenge.h"
#include "core/turn_origin.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/coding_agent_gateway.h"
#include "tools/coding_agent_internal.h"
#include "tools/tool_pending.h"
#include "tools/tool_registry.h"
#include "utils/string_utils.h"

#define CA_PENDING_DELEGATE 1
#define CA_PENDING_REVISE 2
#define CA_PENDING_TTL_SEC 600
#define CA_CONFIRM_TURNS 3 /* turns after the preview that may confirm it */
#define CA_PENDING_MAX 8
#define CA_REPO_MAX 64
#define CA_TASK_MAX 4000 /* the task text sent (the gateway caps what it stores too) */
#define CA_BRANCH_MAX 160
#define CA_NAME_MAX 60      /* a job's short name, as the user hears it */
#define CA_PR_URL_MAX 200   /* a pull request's address (the gateway's cap) */
#define CA_FEEDBACK_MAX 512 /* what the revise result says of the feedback sent */

typedef struct {
   pending_slot_t hdr;
   char repo[CA_REPO_MAX];
   char task[CA_TASK_MAX];
   char name[CONV_TITLE_MAX]; /* the job's title: the model's short name, else from the task */
   double budget_usd;         /* 0 = the gateway's default */
   int64_t parent_conv;
   int64_t job;              /* a revise: the finished job that gets the work */
   bool merge_base;          /* a revise: the gateway merges the base in first */
   char base[CA_BRANCH_MAX]; /* a revise: its base branch, as shown ("" unknown) */
} ca_pending_t;

PENDING_ITEM_CHECK(ca_pending_t);
/* A delegate the user may still answer holds DAWN's unasked items back. */
_Static_assert(CA_CONFIRM_TURNS <= TURN_ORIGIN_UNASKED_QUIET_TURNS,
               "the quiet window must cover the confirm window");
static ca_pending_t s_pending[CA_PENDING_MAX];
static pthread_mutex_t s_pending_mutex = PTHREAD_MUTEX_INITIALIZER;
PENDING_ARRAY_CHECK(s_pending);
/* One of the next few turns may confirm: the user may ask about the task
 * before agreeing.  The id binds the confirm to the task they were shown. */
static const pending_slots_t s_pending_slots = {
   .base = s_pending,
   .stride = sizeof(s_pending[0]),
   .count = CA_PENDING_MAX,
   .ttl_sec = CA_PENDING_TTL_SEC,
   .later_turns = CA_CONFIRM_TURNS,
};

bool coding_delegate_live(uint32_t session_id) {
   pthread_mutex_lock(&s_pending_mutex);
   const bool live = pending_slots_live(&s_pending_slots, session_id, -1, pending_slots_now()) !=
                     NULL;
   pthread_mutex_unlock(&s_pending_mutex);
   return live;
}

ca_confirm_kind_t coding_delegate_staged(uint32_t id, const turn_origin_t *origin, int user_id) {
   int kind = 0;
   pthread_mutex_lock(&s_pending_mutex);
   const bool found = pending_slots_kind_of(&s_pending_slots, origin, user_id, id,
                                            pending_slots_now(), &kind);
   pthread_mutex_unlock(&s_pending_mutex);
   if (!found) {
      return CA_CONFIRM_NONE;
   }
   return kind == CA_PENDING_REVISE ? CA_CONFIRM_REVISE : CA_CONFIRM_DELEGATE;
}

/* ========== delegate: a preview, then the user's yes ========== */

/* Whether @p name is a short one-line name: no control characters, no double
 * quote (previews quote it), and none of the invisible or direction-changing
 * characters that would make it show differently than it reads (C1 controls,
 * U+200B-200F, U+2028-202E, U+2060-2069, U+FEFF). */
static bool name_ok(const char *name) {
   const size_t n = strlen(name);
   if (n > CA_NAME_MAX) {
      return false;
   }
   for (size_t i = 0; i < n; i++) {
      const unsigned char c = (unsigned char)name[i];
      if (c < 0x20 || c == 0x7f || c == '"' ||
          (c == 0xc2 && i + 1 < n && (unsigned char)name[i + 1] < 0xa0)) {
         return false;
      }
      if (c == 0xe2 && i + 2 < n) {
         const unsigned char c1 = (unsigned char)name[i + 1], c2 = (unsigned char)name[i + 2];
         if ((c1 == 0x80 && ((c2 >= 0x8b && c2 <= 0x8f) || (c2 >= 0xa8 && c2 <= 0xae))) ||
             (c1 == 0x81 && c2 >= 0xa0 && c2 <= 0xa9)) {
            return false;
         }
      }
      if (c == 0xef && i + 2 < n && (unsigned char)name[i + 1] == 0xbb &&
          (unsigned char)name[i + 2] == 0xbf) {
         return false;
      }
   }
   return true;
}

/* Whether job @p job's work was closed at the gateway (its branch and clone are
 * gone): no revise can reach it.  False when the gateway can't tell.  With
 * @p pr_url, the pull request it published goes there ("" for none); with
 * @p base, its base branch ("" when the gateway can't tell). */
static bool task_dropped_pr(int user_id,
                            int64_t job,
                            char *pr_url,
                            size_t pr_len,
                            char *base,
                            size_t base_len) {
   struct json_object *ans = NULL;
   char *err = NULL;
   bool dropped = false;
   if (pr_url != NULL) {
      pr_url[0] = '\0';
   }
   if (base != NULL) {
      base[0] = '\0';
   }
   if (coding_gateway_call(user_id, "task_list", coding_agent_job_args(job), &ans, &err) ==
       CODING_GW_OK) {
      struct json_object *task = json_object_object_get(ans, "task");
      dropped = coding_gateway_jstr(task, "closed")[0] != '\0';
      const char *url = coding_gateway_jstr(json_object_object_get(task, "published"), "pr_url");
      if (pr_url != NULL && coding_publish_pr_url_ok(url)) {
         snprintf(pr_url, pr_len, "%s", url);
      }
      const char *b = coding_gateway_jstr(task, "base");
      if (base != NULL && strlen(b) < base_len && b[strcspn(b, "\r\n\"")] == '\0') {
         snprintf(base, base_len, "%s", b);
      }
   }
   json_object_put(ans);
   free(err);
   return dropped;
}

static bool task_dropped(int user_id, int64_t job) {
   return task_dropped_pr(user_id, job, NULL, 0, NULL, 0);
}

/* A finished coding job of @p user_id the task names ("job 1977", "job #1977"),
 * or 0: continuing one is revise, since a new task can't see its branch. */
static int64_t names_finished_job(const char *task, int user_id) {
   for (const char *p = task; (p = strcasestr(p, "job")) != NULL; p += 3) {
      const char *q = p + 3;
      q += strspn(q, " #");
      if (q == p + 3 || *q < '1' || *q > '9') {
         continue;
      }
      const int64_t job = strtoll(q, NULL, 10);
      job_record_t rec;
      if (conv_db_job_get(job, user_id, &rec) == AUTH_DB_SUCCESS &&
          strcmp(rec.job_kind, JOB_KIND_CODING) == 0 &&
          (strcmp(rec.job_status, "done") == 0 || strcmp(rec.job_status, "failed") == 0) &&
          !task_dropped(user_id, job)) { /* a closed one takes no more work */
         return job;
      }
   }
   return 0;
}

char *coding_delegate_propose(struct json_object *details,
                              int user_id,
                              int64_t parent_conv,
                              const turn_origin_t *origin) {
   const char *repo = coding_gateway_jstr(details, "repo");
   const char *task = coding_gateway_jstr(details, "task");
   const char *name = coding_gateway_jstr(details, "name");
   struct json_object *jbudget = NULL;
   const double budget = json_object_object_get_ex(details, "budget_usd", &jbudget)
                             ? json_object_get_double(jbudget)
                             : 0.0;
   if (repo[0] == '\0' || strlen(repo) >= CA_REPO_MAX || task[0] == '\0') {
      return coding_agent_error(
          "delegate needs {\"repo\": <a repo from list>, \"task\": <what to do>}.");
   }
   if (strlen(task) >= CA_TASK_MAX) {
      return coding_agent_error("That task is too long to send; say it more briefly.");
   }
   if (!name_ok(name)) {
      return coding_agent_error("name is a short one-line name for the job, at most 60 "
                                "bytes, without double quotes.");
   }
   if (budget < 0.0) {
      return coding_agent_error("budget_usd can't be negative (0 or none: the gateway's default).");
   }
   const int64_t named = names_finished_job(task, user_id);
   if (named > 0) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "This continues coding job #%lld: use revise {\"job\": %lld, \"instructions\": "
               "...} instead. A new task starts from the repo's base and can't see that job's "
               "branch or work.",
               (long long)named, (long long)named);
      return coding_agent_error(msg);
   }
   /* A proposal may follow reading the agent's reports: it starts nothing, and
    * its confirm needs the user's yes.  But in a turn that read the agent's
    * words it may not replace one the user has already seen: their yes would
    * land on the new one. */
   if (coding_agent_turn_read(origin) && coding_delegate_live(origin->session_id)) {
      return coding_agent_error("A task is already waiting for the user's answer; it can't be "
                                "replaced in a turn that read the coding agent's report. Ask "
                                "the user about the waiting one first.");
   }
   /* The repo must be one the gateway works in. */
   struct json_object *agents = NULL;
   char *err = NULL;
   if (coding_gateway_call(0, "agents_list", json_object_new_object(), &agents, &err) !=
       CODING_GW_OK) {
      return coding_agent_error_free(err);
   }
   bool known = false;
   struct json_object *repos = NULL;
   if (json_object_object_get_ex(agents, "repos", &repos) &&
       json_object_is_type(repos, json_type_array)) {
      for (size_t i = 0; i < json_object_array_length(repos) && !known; i++) {
         known = strcmp(coding_gateway_jstr(json_object_array_get_idx(repos, i), "name"), repo) ==
                 0;
      }
   }
   json_object_put(agents);
   if (!known) {
      return coding_agent_error(
          "The coding agent doesn't work in that repo; list shows the repos it "
          "does.");
   }

   pending_stage_rc_t rc = PENDING_STAGED;
   pthread_mutex_lock(&s_pending_mutex);
   ca_pending_t *p = (ca_pending_t *)pending_slots_stage(&s_pending_slots, origin, user_id,
                                                         CA_PENDING_DELEGATE, pending_slots_now(),
                                                         &rc);
   const uint32_t id = p ? p->hdr.item_id : 0;
   const uint32_t replaced = p ? p->hdr.replaced_turn : 0;
   char shown[CONV_TITLE_MAX] = "";
   if (p != NULL) {
      safe_strscpy(p->repo, repo);
      safe_strscpy(p->task, task);
      if (coding_agent_has_text(name)) {
         safe_strscpy(p->name, name);
      } else {
         conv_generate_title(task, p->name, sizeof(p->name));
      }
      safe_strscpy(shown, p->name);
      p->budget_usd = budget;
      p->parent_conv = parent_conv;
   }
   pthread_mutex_unlock(&s_pending_mutex);
   coding_agent_preview_replaced(replaced);
   if (id == 0) {
      return tool_pending_stage_refusal(rc, "coding task");
   }
   strbuf_t sb;
   strbuf_init(&sb, CA_TASK_MAX);
   strbuf_appendf(&sb, "About to give the coding agent a task named \"%s\" in repo '%s'", shown,
                  repo);
   if (budget > 0.0) {
      strbuf_appendf(&sb, " (budget $%.2f)", budget);
   }
   strbuf_appendf(&sb,
                  ":\n%s\nRead it to the user and ask them to confirm; if they say yes in their "
                  "reply, call coding_agent with action 'confirm' and arguments "
                  "{\"ids\": [%u]}.",
                  task, (unsigned)id);
   return strbuf_steal(&sb);
}

/* Start the staged task for job @p conv_id: once more with the same name when
 * the first answer was lost (the gateway returns the task it started).  The
 * branch goes to @p branch; *@p queued: every slot was busy, so the gateway
 * starts it when a running task finishes.  CODING_GW_*. */
static int start_task(int user_id,
                      int64_t conv_id,
                      const ca_pending_t *p,
                      char *branch,
                      size_t branch_len,
                      bool *queued,
                      char **err) {
   char ref[CODING_AGENT_REF_MAX];
   if (!coding_gateway_ref(conv_id, ref, sizeof(ref))) {
      *err = strdup("Couldn't read the coding job.");
      return CODING_GW_FAILED;
   }
   int rc = CODING_GW_FAILED;
   for (int attempt = 0; attempt < 2 && rc == CODING_GW_FAILED; attempt++) {
      free(*err);
      *err = NULL;
      struct json_object *args = json_object_new_object();
      json_object_object_add(args, "repo", json_object_new_string(p->repo));
      json_object_object_add(args, "task", json_object_new_string(p->task));
      json_object_object_add(args, "client_ref", json_object_new_string(ref));
      if (p->budget_usd > 0.0) {
         json_object_object_add(args, "budget_usd", json_object_new_double(p->budget_usd));
      }
      struct json_object *ans = NULL;
      rc = coding_gateway_call(user_id, "task_start", args, &ans, err);
      if (rc == CODING_GW_OK) {
         snprintf(branch, branch_len, "%s", coding_gateway_jstr(ans, "branch"));
         *queued = strcmp(coding_gateway_jstr(ans, "status"), "queued") == 0;
      }
      json_object_put(ans);
   }
   return rc;
}

char *coding_delegate_confirm(uint32_t id, int user_id, const turn_origin_t *origin) {
   ca_pending_t p;
   turn_origin_rc_t orc = TURN_ORIGIN_OK;
   pthread_mutex_lock(&s_pending_mutex);
   const pending_find_rc_t rc = pending_slots_take(&s_pending_slots, origin, user_id,
                                                   CA_PENDING_DELEGATE, id, pending_slots_now(), &p,
                                                   sizeof(p), &orc);
   pthread_mutex_unlock(&s_pending_mutex);
   if (rc != PENDING_FOUND) {
      return tool_pending_take_refusal(rc, orc, "coding task");
   }
   /* Answered: the job's requests may be asked about on DAWN's own. */
   turn_origin_note_answered(session_get_command_context(), p.hdr.origin.turn_number);

   /* The job row first: the gateway's task is named after it.  Its report turn
    * runs on the LLM of the turn that confirmed it. */
   const char *title = p.name;
   session_llm_config_t llm;
   job_selection_of_spawner(&llm);
   conv_llm_columns_t llm_cols;
   job_selection_columns(&llm, &llm_cols);
   int64_t conv_id = 0;
   if (conv_db_create_job_ex(user_id, title, p.parent_conv, "detached", "notify", NULL, 1, p.task,
                             job_spawn_origin_string(), &llm_cols, &conv_id) != AUTH_DB_SUCCESS) {
      return coding_agent_error("Couldn't create the coding job.");
   }
   if (conv_db_job_set_kind(conv_id, JOB_KIND_CODING) != AUTH_DB_SUCCESS) {
      coding_agent_end_quietly(conv_id, user_id, "failed", "couldn't mark the job");
      return coding_agent_error("Couldn't set up the coding job.");
   }
   job_update_emit(conv_id, user_id);
   if (p.parent_conv > 0) {
      conv_event_emit(p.parent_conv, user_id, CONV_EVENT_SPAWN,
                      event_payload_spawn(conv_id, title));
   }

   char branch[CA_BRANCH_MAX] = "";
   char *err = NULL;
   bool queued = false;
   if (start_task(user_id, conv_id, &p, branch, sizeof(branch), &queued, &err) != CODING_GW_OK) {
      /* Refused, or no answer: one whose answer was lost may exist, so stop it. */
      coding_agent_end_quietly(conv_id, user_id, "failed", "the gateway didn't start it");
      coding_gateway_cancel(user_id, conv_id);
      return coding_agent_error_free(err);
   }

   if (queued) {
      /* The gateway holds it until a slot frees; the job stays queued and its
       * "started" event (the watcher) makes it running.  A cancel since the row
       * was made reached no task, so it is stopped here. */
      job_record_t rec;
      if (conv_db_job_get(conv_id, user_id, &rec) == AUTH_DB_SUCCESS &&
          strcmp(rec.job_status, "cancelled") == 0) {
         coding_gateway_cancel(user_id, conv_id);
         return coding_agent_error(
             "The job was cancelled while it started; the agent was stopped.");
      }
   } else if (conv_db_job_set_running(conv_id, time(NULL)) != AUTH_DB_SUCCESS) {
      /* queued -> running is a claim: a cancel since the row was made wins, and
       * the task it started is stopped. */
      job_record_t rec;
      const bool read = conv_db_job_get(conv_id, user_id, &rec) == AUTH_DB_SUCCESS;
      /* "running": the watcher saw its "started" event first, so it runs. */
      if (!read || strcmp(rec.job_status, "running") != 0) {
         if (read && strcmp(rec.job_status, "queued") != 0 &&
             strcmp(rec.job_status, "cancelled") != 0) {
            /* The agent already finished (its end came in first): the job says how. */
            char buf[2 * CONV_TITLE_MAX + 64];
            snprintf(buf, sizeof(buf), "Started coding job #%lld (%s); it has already ended (%s).",
                     (long long)conv_id, title, rec.job_status);
            return strdup(buf);
         }
         coding_gateway_cancel(user_id, conv_id);
         if (read && strcmp(rec.job_status, "cancelled") == 0) {
            return coding_agent_error(
                "The job was cancelled while it started; the agent was stopped.");
         }
         coding_agent_end_quietly(conv_id, user_id, "failed", "couldn't record the start");
         return coding_agent_error("Couldn't record the job's start, so the agent was stopped.");
      }
   }
   job_update_emit(conv_id, user_id);

   strbuf_t sb;
   strbuf_init(&sb, 256);
   strbuf_appendf(&sb,
                  "%s coding job #%lld (\"%s\") in %s on branch %s. %s the user hears when it "
                  "needs them or finishes, and can check on it with status. Call it by its name "
                  "when you talk about it.",
                  queued ? "Queued" : "Started", (long long)conv_id, title, p.repo, branch,
                  queued ? "Every slot at the agent is busy, so it starts by itself when a running "
                           "task finishes; nothing to do until then. Then"
                         : "It runs in the background;");
   return strbuf_steal(&sb);
}

/* What a confirm of task @p id, waiting for the user's reply code, starts: the
 * whole task, or a refusal when it can't be shown whole in the text asking for
 * the code. */
int coding_delegate_describe(uint32_t id, char *out, size_t out_len, int *valid_for_sec) {
   turn_origin_t origin;
   int n = -1;
   snprintf(out, out_len, "that coding task isn't waiting any more; propose it again");
   if (id == 0 || !turn_origin_capture(&origin)) {
      return FAILURE;
   }
   pthread_mutex_lock(&s_pending_mutex);
   pending_slot_t *slot = NULL;
   if (pending_slots_find(&s_pending_slots, &origin, tool_get_current_user_id(),
                          CA_PENDING_DELEGATE, id, pending_slots_now(), &slot) == PENDING_FOUND) {
      const ca_pending_t *p = (const ca_pending_t *)slot;
      char budget[32] = "";
      if (p->budget_usd > 0.0) {
         snprintf(budget, sizeof(budget), " (budget $%.2f)", p->budget_usd);
      }
      n = snprintf(out, out_len, "give the coding agent a task named \"%s\", in repo '%s'%s: %s",
                   p->name, p->repo, budget, p->task);
      *valid_for_sec = pending_slots_valid_for(&s_pending_slots, slot, pending_slots_now());
   }
   pthread_mutex_unlock(&s_pending_mutex);
   if (n > 0 && (size_t)n > CA_DESCRIBE_ROOM) {
      snprintf(out, out_len,
               "the task is too long to approve by reply code; the user can ask for it "
               "again in the WebUI");
      return FAILURE;
   }
   return (n > 0 && (size_t)n < out_len) ? SUCCESS : FAILURE;
}

/* ========== revise: more work for a finished job ========== */

#define CA_FOLLOW_UP "\n\nFollow-up: "
#define CA_REVISE_REF_MAX 48
/* A revise that merges the base waits for the gateway's mirror refresh (45 s at
 * most there), the pull request's feedback (15 s) and the merge itself. */
#define CA_REVISE_MERGE_TIMEOUT_MS 120000L
#define CA_MERGE_SHOWN 8 /* conflicted files the result names */

char *coding_revise_propose(struct json_object *details,
                            int user_id,
                            int64_t parent_conv,
                            const turn_origin_t *origin) {
   const int64_t job = coding_agent_details_job(details);
   job_record_t rec;
   char *bad = coding_agent_check_job(job, user_id, &rec);
   if (bad != NULL) {
      return bad;
   }
   const char *text = coding_gateway_jstr(details, "instructions");
   struct json_object *jbudget = NULL;
   const double budget = json_object_object_get_ex(details, "budget_usd", &jbudget)
                             ? json_object_get_double(jbudget)
                             : 0.0;
   if (!coding_agent_has_text(text)) {
      return coding_agent_error("revise needs {\"job\": N, \"instructions\": <the more work>}.");
   }
   if (strlen(text) >= CA_TASK_MAX) {
      return coding_agent_error("Those instructions are too long to send; say them more briefly.");
   }
   if (budget < 0.0) {
      return coding_agent_error("budget_usd can't be negative (0 or none: the gateway's default).");
   }
   bool merge_base = false;
   if (!coding_agent_yes_no(details, "merge_base", false, &merge_base)) {
      return coding_agent_error("merge_base is true or false.");
   }
   if (strcmp(rec.job_status, "done") != 0 && strcmp(rec.job_status, "failed") != 0) {
      char msg[128];
      snprintf(msg, sizeof(msg),
               "Coding job #%lld is %s; only a finished (done or failed) job takes more work.",
               (long long)job, rec.job_status);
      return coding_agent_error(msg);
   }
   char pr_url[CA_PR_URL_MAX];
   char base[CA_BRANCH_MAX];
   if (task_dropped_pr(user_id, job, pr_url, sizeof(pr_url), base, sizeof(base))) {
      return coding_agent_error("That job was closed: its branch is gone, so it takes no more "
                                "work. Start a new task (delegate) instead.");
   }
   /* As delegate: not replacing a draft the user has seen in a turn that read the agent. */
   if (coding_agent_turn_read(origin) && coding_delegate_live(origin->session_id)) {
      return coding_agent_error("A coding task is already waiting for the user's answer; it "
                                "can't be replaced in a turn that read the coding agent's report. "
                                "Ask the user about the waiting one first.");
   }
   const bool moves = parent_conv > 0 && parent_conv != rec.parent_id;
   pending_stage_rc_t rc = PENDING_STAGED;
   pthread_mutex_lock(&s_pending_mutex);
   ca_pending_t *p = (ca_pending_t *)pending_slots_stage(&s_pending_slots, origin, user_id,
                                                         CA_PENDING_REVISE, pending_slots_now(),
                                                         &rc);
   const uint32_t id = p ? p->hdr.item_id : 0;
   const uint32_t replaced = p ? p->hdr.replaced_turn : 0;
   if (p != NULL) {
      safe_strscpy(p->task, text);
      safe_strscpy(p->name, rec.title);
      p->budget_usd = budget;
      p->job = job;
      p->merge_base = merge_base;
      safe_strscpy(p->base, base);
      /* Set only when asked from another chat: the job moves there on the yes. */
      p->parent_conv = moves ? parent_conv : 0;
   }
   pthread_mutex_unlock(&s_pending_mutex);
   coding_agent_preview_replaced(replaced);
   if (id == 0) {
      return tool_pending_stage_refusal(rc, "follow-up");
   }
   strbuf_t sb;
   strbuf_init(&sb, CA_TASK_MAX);
   strbuf_appendf(&sb, "About to send coding job #%lld (\"%s\") more work, on its branch",
                  (long long)job, rec.title);
   if (merge_base) {
      strbuf_appendf(&sb, " (first merges %s into its branch)", base[0] ? base : "its base");
   }
   if (budget > 0.0) {
      strbuf_appendf(&sb, ", adding up to $%.2f to its budget", budget);
   } else {
      strbuf_append(&sb, ", adding the gateway's usual task budget");
   }
   strbuf_appendf(&sb, ":\n%s\n", text);
   if (moves) {
      strbuf_append(&sb, "Its report will come to this chat.\n");
   }
   if (pr_url[0] != '\0') {
      strbuf_appendf(&sb,
                     "If its pull request (%s) is still open, its open feedback (review threads, "
                     "failing required checks) goes to the agent with this.\n",
                     pr_url);
   }
   strbuf_appendf(&sb,
                  "Read it to the user and ask them to confirm; if they say yes in their "
                  "reply, call coding_agent with action 'confirm' and arguments "
                  "{\"ids\": [%u]}.",
                  (unsigned)id);
   return strbuf_steal(&sb);
}

/* The job's goal for its next end: its first goal and this follow-up (only the
 * latest, so revising often doesn't grow it).  Caller frees. */
static char *revised_goal(int64_t job, int user_id, const char *text) {
   char *goal = NULL;
   if (conv_db_job_get_goal(job, user_id, &goal) != AUTH_DB_SUCCESS) {
      free(goal);
      return NULL; /* not losing its first goal */
   }
   strbuf_t sb;
   strbuf_init(&sb, 512);
   if (goal != NULL) {
      char *cut = strstr(goal, CA_FOLLOW_UP);
      if (cut != NULL) {
         *cut = '\0';
      }
      strbuf_append(&sb, goal);
      free(goal);
   }
   strbuf_appendf(&sb, CA_FOLLOW_UP "%s", text);
   return strbuf_steal(&sb);
}

/* What the gateway sent the agent of its pull request's open feedback, as a
 * sentence for the result (" ..."), or "" when it has no open pull request. */
static void feedback_line(struct json_object *ans, char *out, size_t len) {
   out[0] = '\0';
   struct json_object *fb = json_object_object_get(ans, "feedback");
   const char *why = coding_gateway_jstr(ans, "feedback_error");
   if (json_object_is_type(fb, json_type_object)) {
      const int threads = json_object_get_int(json_object_object_get(fb, "threads"));
      const int checks = json_object_get_int(json_object_object_get(fb, "checks"));
      const char *url = coding_gateway_jstr(fb, "pr_url");
      if (threads == 0 && checks == 0) {
         snprintf(out, len, " Its pull request has no open feedback.");
      } else {
         snprintf(out, len,
                  " With it went %d review thread%s and %d failing required check%s from %s; "
                  "the agent reports which commit addresses each thread.",
                  threads, threads == 1 ? "" : "s", checks, checks == 1 ? "" : "s",
                  coding_publish_pr_url_ok(url) ? url : "its pull request");
      }
   } else if (why[0] != '\0') {
      snprintf(out, len,
               " The gateway couldn't read its pull request's feedback (%s), so the agent works "
               "from the instructions alone.",
               why);
   }
}

/* What the gateway's merge of the base did, as a sentence for the result
 * (" ..."), or "" when none was asked. */
static void merge_line(struct json_object *ans, const char *base, char *out, size_t len) {
   out[0] = '\0';
   struct json_object *m = json_object_object_get(ans, "merge");
   if (!json_object_is_type(m, json_type_object)) {
      return;
   }
   const char *b = base[0] ? base : "the base branch";
   struct json_object *files = json_object_object_get(m, "conflicts");
   if (json_object_is_type(files, json_type_array)) {
      strbuf_t sb;
      strbuf_init(&sb, 256);
      const size_t n = json_object_array_length(files);
      const size_t shown = n < CA_MERGE_SHOWN ? n : CA_MERGE_SHOWN;
      const int total = json_object_get_int(json_object_object_get(m, "conflicts_count"));
      for (size_t i = 0; i < shown; i++) {
         strbuf_appendf(&sb, "%s%s", i ? ", " : "",
                        json_object_get_string(json_object_array_get_idx(files, i)));
      }
      const int more = (total > (int)n ? total : (int)n) - (int)shown;
      if (more > 0) {
         strbuf_appendf(&sb, " and %d more", more);
      }
      char *list = strbuf_steal(&sb);
      snprintf(out, len,
               " Merging %s conflicted in %s; the agent resolves them and concludes the merge.", b,
               list ? list : "some files");
      free(list);
      return;
   }
   const char *merged = coding_gateway_jstr(m, "merged");
   if (strcmp(merged, "up to date") == 0) {
      snprintf(out, len, " Its branch already had %s; nothing to merge.", b);
   } else if (merged[0] != '\0') {
      snprintf(out, len, " The gateway merged %s into its branch first (now at %.12s).", b, merged);
   }
}

/* Send the follow-up to job @p job's task: once more with the same reference
 * when the first answer was lost (the gateway adds nothing for a repeat).  What
 * went with it of its pull request's feedback goes to @p fb, and what its merge
 * of the base did to @p mg (sentences). */
static int revise_task(int user_id,
                       int64_t job,
                       const ca_pending_t *p,
                       char *fb,
                       size_t fb_len,
                       char *mg,
                       size_t mg_len,
                       char **err) {
   fb[0] = '\0';
   mg[0] = '\0';
   char ref[CA_REVISE_REF_MAX];
   snprintf(ref, sizeof(ref), "rev:%lld.%lld.%u", (long long)time(NULL), (long long)p->hdr.made_at,
            (unsigned)p->hdr.item_id);
   int rc = CODING_GW_FAILED;
   for (int attempt = 0; attempt < 2 && rc == CODING_GW_FAILED; attempt++) {
      free(*err);
      *err = NULL;
      struct json_object *args = coding_agent_job_args(job);
      if (args == NULL) {
         *err = strdup("Couldn't read the coding job.");
         return CODING_GW_REFUSED; /* nothing was sent */
      }
      json_object_object_add(args, "instructions", json_object_new_string(p->task));
      json_object_object_add(args, "revise_ref", json_object_new_string(ref));
      if (p->budget_usd > 0.0) {
         json_object_object_add(args, "budget_usd", json_object_new_double(p->budget_usd));
      }
      if (p->merge_base) {
         json_object_object_add(args, "merge_base", json_object_new_boolean(true));
      }
      struct json_object *ans = NULL;
      rc = coding_gateway_call_timed(user_id, "task_revise", args,
                                     p->merge_base ? CA_REVISE_MERGE_TIMEOUT_MS : 0, &ans, err);
      if (rc == CODING_GW_OK) {
         feedback_line(ans, fb, fb_len);
         merge_line(ans, p->base, mg, mg_len);
      }
      json_object_put(ans);
   }
   return rc;
}

typedef enum {
   CA_TASK_CLOSED,
   CA_TASK_OPEN,
   CA_TASK_UNKNOWN
} ca_task_state_t;

/* With no answer: whether the task is open at the gateway anyway (the
 * follow-up got through), or that can't be told either. */
static ca_task_state_t task_state(int user_id, int64_t job) {
   struct json_object *ans = NULL;
   char *err = NULL;
   ca_task_state_t state = CA_TASK_UNKNOWN;
   if (coding_gateway_call(user_id, "task_list", coding_agent_job_args(job), &ans, &err) ==
       CODING_GW_OK) {
      const char *status = coding_gateway_jstr(json_object_object_get(ans, "task"), "status");
      state = coding_task_status_open(status) ? CA_TASK_OPEN : CA_TASK_CLOSED;
   }
   json_object_put(ans);
   free(err);
   return state;
}

char *coding_revise_confirm(uint32_t id, int user_id, const turn_origin_t *origin) {
   ca_pending_t p;
   turn_origin_rc_t orc = TURN_ORIGIN_OK;
   pthread_mutex_lock(&s_pending_mutex);
   const pending_find_rc_t rc = pending_slots_take(&s_pending_slots, origin, user_id,
                                                   CA_PENDING_REVISE, id, pending_slots_now(), &p,
                                                   sizeof(p), &orc);
   pthread_mutex_unlock(&s_pending_mutex);
   if (rc != PENDING_FOUND) {
      return tool_pending_take_refusal(rc, orc, "follow-up");
   }
   turn_origin_note_answered(session_get_command_context(), p.hdr.origin.turn_number);

   job_record_t before;
   if (conv_db_job_get(p.job, user_id, &before) != AUTH_DB_SUCCESS) {
      return coding_agent_error("No coding job with that number.");
   }
   /* Reopened BEFORE the gateway is asked: the watcher stops a task that reports
    * while its job isn't open. */
   char *goal = revised_goal(p.job, user_id, p.task);
   const int claimed = goal ? conv_db_job_reopen_coding(p.job, user_id, goal) : AUTH_DB_FAILURE;
   free(goal);
   if (claimed != AUTH_DB_SUCCESS) {
      return coding_agent_error(claimed == AUTH_DB_NOT_FOUND
                                    ? "That job isn't finished any more; nothing was sent."
                                    : "Couldn't reopen the coding job.");
   }
   job_update_emit_ex(p.job, user_id, true);
   conv_event_emit(p.job, user_id, CONV_EVENT_RESUME, strdup("{\"continued\":true}"));

   char *err = NULL;
   char fb[CA_FEEDBACK_MAX];
   char mg[CA_FEEDBACK_MAX];
   const int sent = revise_task(user_id, p.job, &p, fb, sizeof(fb), mg, sizeof(mg), &err);
   /* No answer twice: open only if the gateway says its task is.  Unknown too
    * (it is most likely down, so nothing got through) puts the job back. */
   const ca_task_state_t state = sent == CODING_GW_FAILED ? task_state(user_id, p.job)
                                                          : CA_TASK_OPEN;
   if (sent == CODING_GW_REFUSED || state != CA_TASK_OPEN) {
      /* Back as it was, with no second notice of its old end. */
      coding_agent_end_quietly(p.job, user_id, before.job_status,
                               before.job_error[0] ? before.job_error : NULL);
      return coding_agent_error_free(err);
   }
   free(err);
   /* Asked from another chat: the job moves there, so its report reaches it. */
   if (p.parent_conv > 0 && p.parent_conv != before.parent_id) {
      const int moved = conv_db_job_set_parent(p.job, user_id, p.parent_conv);
      if (moved == AUTH_DB_SUCCESS) {
         conv_event_emit(p.parent_conv, user_id, CONV_EVENT_SPAWN,
                         event_payload_spawn(p.job, before.title));
      } else {
         OLOG_WARNING("coding_agent: job %lld not moved to chat %lld (rc=%d); its report goes "
                      "to its old chat",
                      (long long)p.job, (long long)p.parent_conv, moved);
      }
   }
   const time_t started = before.started_at ? before.started_at : time(NULL);
   job_record_t now;
   const bool running = conv_db_job_set_running(p.job, started) == AUTH_DB_SUCCESS;
   const bool read = !running && conv_db_job_get(p.job, user_id, &now) == AUTH_DB_SUCCESS;
   /* Running: the watcher claimed it on a "started" event, so the follow-up runs. */
   if (!running && !(read && strcmp(now.job_status, "running") == 0)) {
      /* Ended meanwhile: by the new run (a real end, already reported, kept) or
       * from the task's old end (a reconcile racing this confirm, while the
       * follow-up runs: opened again rather than let the agent be stopped). */
      const bool ended = read && (strcmp(now.job_status, "done") == 0 ||
                                  strcmp(now.job_status, "failed") == 0);
      if (ended && task_state(user_id, p.job) != CA_TASK_OPEN) {
         char buf[160];
         snprintf(buf, sizeof(buf),
                  "Sent coding job #%lld the follow-up; it has already ended (%s).",
                  (long long)p.job, now.job_status);
         return strdup(buf);
      }
      char *again = ended ? revised_goal(p.job, user_id, p.task) : NULL;
      const bool reopened = again &&
                            conv_db_job_reopen_coding(p.job, user_id, again) == AUTH_DB_SUCCESS &&
                            conv_db_job_set_running(p.job, started) == AUTH_DB_SUCCESS;
      free(again);
      if (!reopened) {
         coding_gateway_cancel(user_id, p.job);
         char buf[160];
         snprintf(buf, sizeof(buf),
                  "Coding job #%lld changed (%s) while the follow-up was sent, so the agent was "
                  "stopped; ask again.",
                  (long long)p.job, read ? now.job_status : "unknown");
         return coding_agent_error(buf);
      }
   }
   job_update_emit_ex(p.job, user_id, true);

   strbuf_t sb;
   strbuf_init(&sb, 256);
   strbuf_appendf(&sb,
                  "Sent coding job #%lld (\"%s\") the follow-up; its agent picks up on the same "
                  "branch.%s%s The user hears when it needs them or finishes.",
                  (long long)p.job, p.name, mg, fb);
   return strbuf_steal(&sb);
}

/* What a confirm of follow-up @p id, waiting for the user's reply code, sends. */
int coding_revise_describe(uint32_t id, char *out, size_t out_len, int *valid_for_sec) {
   turn_origin_t origin;
   int n = -1;
   snprintf(out, out_len, "that follow-up isn't waiting any more; propose it again");
   if (id == 0 || !turn_origin_capture(&origin)) {
      return FAILURE;
   }
   pthread_mutex_lock(&s_pending_mutex);
   pending_slot_t *slot = NULL;
   if (pending_slots_find(&s_pending_slots, &origin, tool_get_current_user_id(), CA_PENDING_REVISE,
                          id, pending_slots_now(), &slot) == PENDING_FOUND) {
      const ca_pending_t *p = (const ca_pending_t *)slot;
      char budget[48] = "the usual task budget";
      if (p->budget_usd > 0.0) {
         snprintf(budget, sizeof(budget), "up to $%.2f", p->budget_usd);
      }
      char merge[CA_BRANCH_MAX + 48] = "";
      if (p->merge_base) {
         snprintf(merge, sizeof(merge), " (first merges %s into its branch)",
                  p->base[0] ? p->base : "its base");
      }
      n = snprintf(out, out_len, "send coding job #%lld (\"%s\") more work%s, adding %s%s: %s",
                   (long long)p->job, p->name, merge, budget,
                   p->parent_conv > 0 ? "; its report comes to this chat" : "", p->task);
      *valid_for_sec = pending_slots_valid_for(&s_pending_slots, slot, pending_slots_now());
   }
   pthread_mutex_unlock(&s_pending_mutex);
   if (n > 0 && (size_t)n > CA_DESCRIBE_ROOM) {
      snprintf(out, out_len,
               "the follow-up is too long to approve by reply code; the user can ask for it "
               "again in the WebUI");
      return FAILURE;
   }
   return (n > 0 && (size_t)n < out_len) ? SUCCESS : FAILURE;
}
