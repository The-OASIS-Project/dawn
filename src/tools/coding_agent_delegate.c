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
 * and the user's yes (confirm_delegate) creates the coding job and starts
 * its task at the gateway.
 */

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
#include "core/strbuf.h"
#include "core/tool_call_challenge.h"
#include "core/turn_origin.h"
#include "dawn_error.h"
#include "tools/coding_agent_gateway.h"
#include "tools/coding_agent_internal.h"
#include "tools/tool_pending.h"
#include "tools/tool_registry.h"
#include "utils/string_utils.h"

#define CA_PENDING_DELEGATE 1
#define CA_PENDING_TTL_SEC 600
#define CA_CONFIRM_TURNS 3 /* turns after the preview that may confirm it */
#define CA_PENDING_MAX 8
#define CA_REPO_MAX 64
#define CA_TASK_MAX 4000 /* the task text sent (the gateway caps what it stores too) */
#define CA_BRANCH_MAX 160
#define CA_NAME_MAX 60 /* a job's short name, as the user hears it */

typedef struct {
   pending_slot_t hdr;
   char repo[CA_REPO_MAX];
   char task[CA_TASK_MAX];
   char name[CONV_TITLE_MAX]; /* the job's title: the model's short name, else from the task */
   double budget_usd;         /* 0 = the gateway's default */
   int64_t parent_conv;
} ca_pending_t;

PENDING_ITEM_CHECK(ca_pending_t);
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

/* Whether @p s has anything but spaces. */
static bool has_text(const char *s) {
   return s[strspn(s, " ")] != '\0';
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
   /* Not in the turn that read the agent's report: a task the model composed
    * from it would be proposed before the user has seen it. */
   if (coding_agent_turn_read(origin)) {
      return coding_agent_error(
          "This turn read the coding agent's report, so a new task can't be "
          "proposed in it. Tell the user what you would ask the agent to do, and "
          "let them ask for it.");
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
   char shown[CONV_TITLE_MAX] = "";
   if (p != NULL) {
      safe_strscpy(p->repo, repo);
      safe_strscpy(p->task, task);
      if (has_text(name)) {
         safe_strscpy(p->name, name);
      } else {
         conv_generate_title(task, p->name, sizeof(p->name));
      }
      safe_strscpy(shown, p->name);
      p->budget_usd = budget;
      p->parent_conv = parent_conv;
   }
   pthread_mutex_unlock(&s_pending_mutex);
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
                  "reply, call coding_agent with action 'confirm_delegate' and arguments "
                  "{\"id\": %u} (this one only).",
                  task, (unsigned)id);
   return strbuf_steal(&sb);
}

/* Start the staged task for job @p conv_id: once more with the same name when
 * the first answer was lost (the gateway returns the task it started).  The
 * branch goes to @p branch; CODING_GW_*. */
static int start_task(int user_id,
                      int64_t conv_id,
                      const ca_pending_t *p,
                      char *branch,
                      size_t branch_len,
                      char **err) {
   char ref[CODING_AGENT_REF_MAX];
   coding_gateway_ref(conv_id, ref, sizeof(ref));
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
      }
      json_object_put(ans);
   }
   return rc;
}

char *coding_delegate_confirm(struct json_object *details,
                              int user_id,
                              const turn_origin_t *origin) {
   const uint32_t id = coding_agent_details_id(details);
   if (id == 0) {
      return tool_pending_missing_id("coding task");
   }
   /* Not in a turn that read the agent's report: a yes there could be its words. */
   if (coding_agent_turn_read(origin)) {
      return coding_agent_error("This turn read the coding agent's report, so the task can't be "
                                "confirmed in it. Ask the user again.");
   }
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

   /* The job row first: the gateway's task is named after it. */
   const char *title = p.name;
   int64_t conv_id = 0;
   if (conv_db_create_job_ex(user_id, title, p.parent_conv, "detached", "notify", NULL, 1, p.task,
                             job_spawn_origin_string(), &conv_id) != AUTH_DB_SUCCESS) {
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
   if (start_task(user_id, conv_id, &p, branch, sizeof(branch), &err) != CODING_GW_OK) {
      /* Refused, or still no answer: it may have started anyway, so stop it. */
      coding_agent_end_quietly(conv_id, user_id, "failed", "the gateway didn't start it");
      coding_gateway_cancel(user_id, conv_id);
      return coding_agent_error_free(err);
   }

   /* queued -> running is a claim, and only this path makes it: a cancel since
    * the row was made wins, and the task it started is stopped. */
   if (conv_db_job_set_running(conv_id, time(NULL)) != AUTH_DB_SUCCESS) {
      job_record_t rec;
      const bool read = conv_db_job_get(conv_id, user_id, &rec) == AUTH_DB_SUCCESS;
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
   job_update_emit(conv_id, user_id);

   strbuf_t sb;
   strbuf_init(&sb, 256);
   strbuf_appendf(&sb,
                  "Started coding job #%lld (\"%s\") in %s on branch %s. It runs in the "
                  "background; the user hears when it needs them or finishes, and can check on it "
                  "with status. Call it by its name when you talk about it.",
                  (long long)conv_id, title, p.repo, branch);
   return strbuf_steal(&sb);
}

/* What confirm_delegate, waiting for the user's reply code, starts: the whole
 * task, or a refusal when it can't be shown whole in the text asking for the
 * code. */
int coding_delegate_describe(const char *value, char *out, size_t out_len, int *valid_for_sec) {
   struct json_object *details = tool_parse_details(value, true);
   const uint32_t id = coding_agent_details_id(details);
   json_object_put(details);
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
   if (n > 0 && (size_t)n > TOOL_CALL_CHALLENGE_DESC_MAX - 48) {
      snprintf(out, out_len,
               "the task is too long to approve by reply code; the user can ask for it "
               "again in the WebUI");
      return FAILURE;
   }
   return (n > 0 && (size_t)n < out_len) ? SUCCESS : FAILURE;
}
