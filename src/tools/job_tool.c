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
 * Background job tool (background-jobs Phase 1).
 *
 * The conversational surface for background jobs (JARVIS principle — job control
 * is a tool + WebUI, never config): spawn a fire-and-forget job, list a user's
 * jobs, read a job's status/result, cancel one.  A job runs in the separate
 * job-session pool (job_manager) via job_worker; this tool creates the job
 * conversation, enforces the injection filter + running caps, and reads results
 * back.  Single-user by invariant: the child inherits the caller's user_id
 * (never accepts a caller-supplied one).  Layer 3.
 */

#include <json-c/json.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "core/conv_event.h"
#include "core/event_payload.h"
#include "core/job_dispatch.h"
#include "core/job_manager.h"
#include "core/job_worker.h"
#include "core/memory_filter.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "llm/llm_interface.h"
#include "logging.h"
#include "tools/tool_registry.h"

/* Cap on rows returned by 'list'. */
#define JOB_TOOL_LIST_MAX 32

/* --- status: capture the job's last assistant message (its result) --------- */

typedef struct {
   char *last_assistant; /* strdup'd; caller frees */
} status_fetch_t;

static int status_msg_cb(const conversation_message_t *msg, void *ctx) {
   status_fetch_t *f = (status_fetch_t *)ctx;
   if (msg != NULL && msg->content != NULL && strcmp(msg->role, "assistant") == 0) {
      char *dup = strdup(msg->content); /* content is borrowed — copy it */
      if (dup != NULL) {                /* on OOM keep the prior capture */
         free(f->last_assistant);
         f->last_assistant = dup;
      }
   }
   return 0; /* continue */
}

/* --- spawn ----------------------------------------------------------------- */

static char *handle_spawn(struct json_object *details,
                          int user_id,
                          int64_t parent_conv,
                          bool parent_is_messaging) {
   struct json_object *jgoal = NULL, *joc = NULL, *jdt = NULL;
   const char *goal = (json_object_object_get_ex(details, "goal", &jgoal) && jgoal)
                          ? json_object_get_string(jgoal)
                          : NULL;
   if (goal == NULL || goal[0] == '\0') {
      return strdup("Error: 'goal' is required to start a background job.");
   }
   if (memory_filter_check(goal)) {
      return strdup("Error: that job goal was rejected by the safety filter.");
   }

   /* Default: reinvoke_parent — bring the result back into THIS conversation and
    * react to it when the job finishes (the result flows into the chat). */
   const char *on_complete = (json_object_object_get_ex(details, "on_complete", &joc) && joc)
                                 ? json_object_get_string(joc)
                                 : "reinvoke_parent";
   if (strcmp(on_complete, "notify") != 0 && strcmp(on_complete, "none") != 0 &&
       strcmp(on_complete, "reinvoke_parent") != 0) {
      return strdup("Error: on_complete must be 'notify', 'none', or 'reinvoke_parent'.");
   }
   /* reinvoke_parent re-engages the parent conversation on completion; it only
    * makes sense with a WebUI-style parent to return to.  Fall back to notify (a)
    * at the root (no parent), and (b) for a messaging channel: a messaging
    * forever-conversation has no live WebUI viewer, so a reinvoke would persist a
    * reply the channel user never receives AND desync the engine's in-memory
    * history.  The notify path DOES reach messaging channels (delivery thread). */
   if (strcmp(on_complete, "reinvoke_parent") == 0 && (parent_conv <= 0 || parent_is_messaging)) {
      on_complete = "notify";
   }

   const char *deliver_to = (json_object_object_get_ex(details, "deliver_to", &jdt) && jdt)
                                ? json_object_get_string(jdt)
                                : NULL;
   if (deliver_to != NULL && (deliver_to[0] == '\0' || memory_filter_check(deliver_to))) {
      deliver_to = NULL; /* ignore empty / suspicious delivery target */
   }

   /* Refuse cleanly past a running cap before creating any row. */
   job_provider_class_t provider = job_provider_from_default();
   int cap = job_manager_capacity(user_id, provider);
   if (cap != JOB_MGR_OK) {
      const char *why = (cap == JOB_MGR_CAP_GLOBAL) ? "the system is at its background-job limit"
                        : (cap == JOB_MGR_CAP_PROVIDER)
                            ? "too many jobs are already running on this model"
                        : (cap == JOB_MGR_CAP_USER)
                            ? "you already have the maximum number of jobs running"
                            : "background jobs are unavailable";
      char buf[256];
      snprintf(buf, sizeof(buf),
               "Can't start that background job right now — %s. Try again once one finishes.", why);
      return strdup(buf);
   }

   char title[CONV_TITLE_MAX];
   conv_generate_title(goal, title, sizeof(title));

   int64_t conv_id = 0;
   if (conv_db_create_job(user_id, title, parent_conv, "detached", on_complete, deliver_to, 1, goal,
                          &conv_id) != AUTH_DB_SUCCESS) {
      return strdup("Error: failed to create the background job.");
   }

   /* The job has entered the active set — push its row so watchers see it appear.
    * BEFORE job_worker_spawn(), not after: the worker is detached and can reach
    * set_running or even a terminal disposition immediately, so emitting after
    * the spawn lets this thread read a stale 'queued' row and enqueue it BEHIND
    * the worker's terminal frame.  A client would then drop the terminal (for a
    * job it has not seen yet) and insert the stale active row — pinning a phantom
    * job in its set until the next reconnect.  Emitting here, the row is
    * definitively 'queued' and no worker exists to race.  Its exit is emitted by
    * job_manager_set_terminal(), including on the spawn-failure path below. */
   job_update_emit(conv_id, user_id);

   if (job_worker_spawn(user_id, conv_id, goal) != SUCCESS) {
      job_manager_set_terminal(conv_id, user_id, "failed", "worker spawn failed", time(NULL), 0);
      job_manager_mark_dirty(); /* terminal + unfired — wake the monitor to notify */
      return strdup("Error: failed to start the background job worker.");
   }

   /* Record the spawn on the PARENT's event stream, not the child's: it is the
    * parent a user is watching when a job is launched, and it is what gives the
    * jobs panel its tree edge (§6.2 spawn carries the child conv_id + title).
    * A rootless job has no parent stream to write to. */
   if (parent_conv > 0) {
      conv_event_emit(parent_conv, user_id, CONV_EVENT_SPAWN, event_payload_spawn(conv_id, title));
   }

   char buf[CONV_TITLE_MAX + 160];
   if (strcmp(on_complete, "reinvoke_parent") == 0) {
      snprintf(buf, sizeof(buf),
               "Started background job #%lld: \"%s\". I'll bring the results back into this "
               "conversation when it's done.",
               (long long)conv_id, title);
   } else {
      snprintf(
          buf, sizeof(buf),
          "Started background job #%lld: \"%s\". I'll let you know when it's done — ask me for "
          "its status anytime.",
          (long long)conv_id, title);
   }
   return strdup(buf);
}

/* --- list ------------------------------------------------------------------ */

static char *handle_list(int user_id) {
   job_record_t jobs[JOB_TOOL_LIST_MAX];
   int n = 0;
   if (conv_db_job_list_by_user(user_id, jobs, JOB_TOOL_LIST_MAX, &n) != AUTH_DB_SUCCESS) {
      return strdup("Error: failed to list background jobs.");
   }
   if (n == 0) {
      return strdup("You have no background jobs.");
   }
   char out[4096];
   size_t off = 0;
   int written = snprintf(out, sizeof(out), "Background jobs (%d):\n", n);
   off = (written > 0) ? (size_t)written : 0;
   for (int i = 0; i < n && off < sizeof(out); i++) {
      written = snprintf(out + off, sizeof(out) - off, "  #%lld [%s] %s\n", (long long)jobs[i].id,
                         jobs[i].job_status, jobs[i].title);
      if (written <= 0) {
         break;
      }
      off += (size_t)written;
   }
   return strdup(out);
}

/* --- status ---------------------------------------------------------------- */

static char *handle_status(struct json_object *details, int user_id) {
   struct json_object *jid = NULL;
   int64_t conv_id = (json_object_object_get_ex(details, "job_id", &jid) && jid)
                         ? json_object_get_int64(jid)
                         : 0;
   if (conv_id <= 0) {
      return handle_list(user_id); /* no id → list them */
   }

   job_record_t r;
   int rc = conv_db_job_get(conv_id, user_id, &r);
   if (rc == AUTH_DB_FORBIDDEN) {
      return strdup("That background job belongs to someone else.");
   }
   if (rc != AUTH_DB_SUCCESS) {
      return strdup("No such background job.");
   }

   status_fetch_t f = { NULL };
   conv_db_get_messages(conv_id, user_id, status_msg_cb, &f);

   char *out = NULL;
   if (strcmp(r.job_status, "done") == 0 && f.last_assistant != NULL) {
      /* Header adds the title (up to CONV_TITLE_MAX) + the id + fixed chars; size
       * for all of them so a long title never truncates the result tail. */
      size_t need = strlen(f.last_assistant) + strlen(r.title) + 64;
      out = malloc(need);
      if (out != NULL) {
         snprintf(out, need, "Job #%lld (\"%s\") is done:\n\n%s", (long long)conv_id, r.title,
                  f.last_assistant);
      }
   } else if (strcmp(r.job_status, "failed") == 0) {
      char buf[CONV_TITLE_MAX + JOB_ERROR_MAX + 64];
      snprintf(buf, sizeof(buf), "Job #%lld (\"%s\") failed%s%s.", (long long)conv_id, r.title,
               r.job_error[0] ? ": " : "", r.job_error);
      out = strdup(buf);
   } else {
      char buf[CONV_TITLE_MAX + 64];
      snprintf(buf, sizeof(buf), "Job #%lld (\"%s\") is %s.", (long long)conv_id, r.title,
               r.job_status);
      out = strdup(buf);
   }
   free(f.last_assistant);
   return out ? out : strdup("Error: out of memory reading job status.");
}

/* --- cancel ---------------------------------------------------------------- */

static char *handle_resume(struct json_object *details, int user_id) {
   struct json_object *jid = NULL;
   int64_t conv_id = (json_object_object_get_ex(details, "job_id", &jid) && jid)
                         ? json_object_get_int64(jid)
                         : 0;
   if (conv_id <= 0) {
      return strdup("Error: 'job_id' is required to resume a background job.");
   }

   char buf[192];
   /* Same one mutation path the WebUI uses — including the ownership check, so
    * a job_id belonging to someone else is refused here too.
    *
    * BY_TOOL, deliberately: this callback is reached by the MODEL, including on
    * a reinvoke turn whose content came from untrusted job output, so it must
    * not be able to restart a job the user cancelled.  A person wanting that
    * clicks Resume in the panel. */
   switch (job_worker_resume(conv_id, user_id, JOB_RESUME_ORIGIN_TOOL)) {
      case JOB_RESUME_STARTED:
         snprintf(buf, sizeof(buf),
                  "Resuming background job #%lld — it'll pick up where it left off.",
                  (long long)conv_id);
         return strdup(buf);
      case JOB_RESUME_NOT_RESUMABLE:
         /* Hedged, because this covers two quite different states: a cancelled
          * job (restartable, just not by me) and a done/running one (not
          * restartable at all).  The result code doesn't distinguish them, so
          * the wording must not claim to either. */
         snprintf(buf, sizeof(buf),
                  "I can only restart a job that was interrupted or failed, and #%lld isn't in "
                  "that state. If you cancelled it and want it back, you can resume it yourself "
                  "from the background-jobs panel.",
                  (long long)conv_id);
         return strdup(buf);
      case JOB_RESUME_CAPACITY:
         return strdup("Too many jobs are running right now — try resuming it again shortly.");
      case JOB_RESUME_FAILED:
         snprintf(buf, sizeof(buf), "Couldn't restart job #%lld.", (long long)conv_id);
         return strdup(buf);
      case JOB_RESUME_NOTHING_TO_RUN:
         snprintf(buf, sizeof(buf),
                  "Job #%lld never started and its instruction wasn't recorded, so there's nothing "
                  "to resume — ask me to do it again and I'll start a fresh job.",
                  (long long)conv_id);
         return strdup(buf);
      case JOB_RESUME_FORBIDDEN:
      case JOB_RESUME_NOT_FOUND:
         /* One answer for both.  Job ids are small sequential integers, so a
          * distinct "belongs to someone else" turns this into an enumeration
          * oracle for other users' jobs — the WebUI handler already collapses
          * them and this surface must match. */
         break;
   }
   /* No `default:` above, so -Wswitch makes a new enumerator a build error at
    * every surface rather than a silent mislabel. */
   return strdup("No such background job.");
}

static char *handle_cancel(struct json_object *details, int user_id) {
   struct json_object *jid = NULL;
   int64_t conv_id = (json_object_object_get_ex(details, "job_id", &jid) && jid)
                         ? json_object_get_int64(jid)
                         : 0;
   if (conv_id <= 0) {
      return strdup("Error: 'job_id' is required to cancel a background job.");
   }

   /* Confirm it IS a job before cancelling.  The job-session pool also holds
    * reinvoke sessions bound to a plain PARENT conversation, and
    * job_manager_cancel matches on conversation id alone — so without this,
    * cancelling your own conversation id would abort an in-flight re-engagement
    * and report it as a cancelled background job.  The WebUI handler already
    * does this check; matching it here removes the asymmetry. */
   job_record_t probe;
   if (conv_db_job_get(conv_id, user_id, &probe) != AUTH_DB_SUCCESS) {
      return strdup("No such background job.");
   }

   char status[JOB_STATUS_MAX];
   char buf[192];
   switch (job_manager_cancel_or_retire(conv_id, user_id, status, sizeof(status))) {
      case JOB_CANCEL_SIGNALLED:
         snprintf(buf, sizeof(buf), "Cancelling background job #%lld.", (long long)conv_id);
         return strdup(buf);
      case JOB_CANCEL_RETIRED:
         snprintf(buf, sizeof(buf), "Cancelled queued job #%lld.", (long long)conv_id);
         return strdup(buf);
      case JOB_CANCEL_ALREADY_TERMINAL:
         snprintf(buf, sizeof(buf), "Job #%lld already finished (%s).", (long long)conv_id, status);
         return strdup(buf);
      case JOB_CANCEL_FORBIDDEN:
      case JOB_CANCEL_NOT_FOUND:
         break; /* one answer for both — see handle_resume */
   }
   return strdup("No such background job.");
}

/* --- dispatch -------------------------------------------------------------- */

static char *job_tool_callback(const char *action, char *value, int *should_respond) {
   if (should_respond != NULL) {
      *should_respond = 1; /* results feed back to the LLM */
   }
   if (action == NULL || action[0] == '\0') {
      return strdup("Error: action is required (spawn, list, status, cancel).");
   }

   struct json_object *details = NULL;
   if (value != NULL && value[0] != '\0') {
      details = json_tokener_parse(value);
      if (details == NULL) {
         return strdup("Error: invalid JSON in details parameter.");
      }
   } else {
      details = json_object_new_object();
   }

   /* Caller context: user_id is the spawner's (non-overridable); parent conv is
    * the conversation this request came from. */
   int user_id = 1;
   int64_t parent_conv = 0;
   bool parent_is_messaging = false;
   bool caller_is_job = false;
   session_t *ctx = session_get_command_context();
   if (ctx != NULL) {
      if (ctx->metrics.user_id > 0) {
         user_id = ctx->metrics.user_id;
      }
      parent_conv = atomic_load(&ctx->stream_conversation_id);
      parent_is_messaging = (ctx->type == SESSION_TYPE_MESSAGING);
      caller_is_job = (ctx->type == SESSION_TYPE_JOB);
   }

   /* Actions that START WORK: they take a pool slot, a provider counter and an
    * LLM budget, and they act on behalf of a specific user.  Both guards below
    * key off this one predicate so a future action can't be added to the
    * dispatch chain and silently miss them. */
   const bool starts_work = (strcmp(action, "spawn") == 0 || strcmp(action, "resume") == 0);

   /* No caller context means no session to attribute this to.  Every other path
    * (LLM tool call, legacy <command> tag) sets it; the one that does not is an
    * MQTT publish, which is UNAUTHENTICATED on a broker without auth/TLS and
    * would otherwise run as user_id 1 with the fan-out backstop wide open.
    * Reads stay available (they are already user-scoped and harmless); anything
    * that starts work fails closed. */
   if (ctx == NULL && starts_work) {
      json_object_put(details);
      OLOG_WARNING("job_tool: refused '%s' from a caller with no session context", action);
      return strdup("Error: background jobs can only be started from a user session.");
   }

   /* Backstop: a background-job worker runs headless and must never start further
    * jobs (unbounded fan-out).  The `job` tool is already removed from a job
    * session's schema, so the model shouldn't see it; this refuses any non-schema
    * path (e.g. a legacy <command> tag) that reaches it from a job context.
    *
    * `resume` counts: it takes a pool slot and spawns a worker exactly like
    * spawn does, so leaving it out would reopen the fan-out this closes — via a
    * job the model can find with `list`. */
   if (caller_is_job && starts_work) {
      json_object_put(details);
      return strdup("Error: background jobs can't start further background jobs.");
   }

   char *result = NULL;
   if (strcmp(action, "spawn") == 0) {
      result = handle_spawn(details, user_id, parent_conv, parent_is_messaging);
   } else if (strcmp(action, "list") == 0) {
      result = handle_list(user_id);
   } else if (strcmp(action, "status") == 0) {
      result = handle_status(details, user_id);
   } else if (strcmp(action, "cancel") == 0) {
      result = handle_cancel(details, user_id);
   } else if (strcmp(action, "resume") == 0) {
      result = handle_resume(details, user_id);
   } else {
      char buf[160];
      snprintf(buf, sizeof(buf),
               "Error: unknown action '%s'. Valid: spawn, list, status, cancel, resume.", action);
      result = strdup(buf);
   }

   json_object_put(details);
   return result;
}

/* =============================================================================
 * Metadata + registration
 * ============================================================================= */

static const treg_param_t job_params[] = {
   {
       .name = "action",
       .description = "The job action: 'spawn' (start a background job), 'list' (show your jobs), "
                      "'status' (check a job / read its result), 'cancel' (cancel a job), "
                      "'resume' (restart a job that was interrupted or failed).",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "spawn", "list", "status", "cancel", "resume" },
       .enum_count = 5,
   },
   {
       .name = "details",
       .description =
           "JSON object with action-specific fields.\n"
           "spawn: {goal (required — the task to run in the background, e.g. 'research the latest "
           "on X and summarize'), on_complete ('reinvoke_parent' (default — bring the result back "
           "into this conversation and react to it when done), 'notify' (just a heads-up; retrieve "
           "with status), or 'none'), deliver_to (optional messaging channel display_name to send "
           "a "
           "completion notice to)}.\n"
           "status: {job_id (the number from spawn/list; omit to list all)}.\n"
           "cancel: {job_id (required)}.\n"
           "resume: {job_id (required)}.\n"
           "list: {} (no fields).",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

static const tool_metadata_t job_metadata = {
   .name = "job",
   .device_string = "job",
   .topic = "dawn",
   .aliases = { "background_job", "jobs" },
   .alias_count = 2,

   .description =
       "Run a task in the BACKGROUND and get on with the conversation.  Use this when the user "
       "asks you to do something that takes a while and doesn't need an immediate answer — "
       "'research X in the background', 'go dig into Y and tell me when you're done', 'kick off a "
       "deep search on Z'.  The job runs on its own; when it finishes you (and the user) are "
       "notified, and the result is retrievable with the 'status' action.  Do NOT use this for "
       "quick answers the user is waiting on — just answer those directly.\n\n"
       "spawn starts a job (returns its id).  list shows the user's jobs.  status <job_id> reports "
       "a job's state and, when done, its result.  cancel <job_id> stops a running job.\n\n"
       "By default (on_complete='reinvoke_parent') a finished job's result is delivered back INTO "
       "this conversation and you are automatically re-engaged to tell the user about it — you do "
       "NOT need to poll or call status.  You'll receive it as an '[automated background-job "
       "update]' message carrying the full result; react to it and take any obvious next step.",

   .params = job_params,
   .param_count = 2,
   .capabilities = TOOL_CAP_NONE,
   .default_local = true,
   .default_remote = true,
   .callback = job_tool_callback,
};

int job_tool_register(void) {
   return tool_registry_register(&job_metadata);
}
