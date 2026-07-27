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
 * Background-job worker (background-jobs Phase 1).  See job_worker.h.
 *
 * Layer-2 only: persists directly through conv_db (no WebUI callback), so the
 * whole dispatch/worker stays inside Layer 2 (arch-MED-4).  Reuses the shared
 * Layer-1 dispatch (core_text_input_dispatch) — the same path the WebUI and
 * messaging workers use — so a job runs an identical add-user-msg -> persist ->
 * focus-inject -> tool-loop sequence, just with no audio and a job-scoped
 * persist hook.
 */

#include "core/job_worker.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "core/conv_event.h"
#include "core/event_payload.h"
#include "core/job_dispatch.h"
#include "core/job_manager.h"
#include "core/session_manager.h"
#include "core/text_input_dispatch.h"
#include "dawn_error.h"
#include "llm/llm_interface.h"
#include "logging.h"
#include "memory/memory_history_loader.h"

/* Work item handed to the detached worker thread. */
typedef struct {
   int user_id;
   int64_t conv_id;
   char *goal;  /* owned; freed by the worker */
   bool resume; /* re-dispatch of an interrupted/failed job (history hydrated) */
} job_work_t;

/* What a resumed job is told.  A fresh job is dispatched with the user's goal on
 * an empty session; a resume runs on the SAME conversation with its prior
 * messages hydrated, so re-sending the original goal would read as a duplicate
 * request and invite the model to start over.  This says what happened and what
 * to do about it, and leaves the work already in the transcript to speak for
 * itself. */
#define JOB_RESUME_DIRECTIVE                                                            \
   "Your previous attempt at this task did not finish — it was interrupted by a "     \
   "daemon restart or ended in an error before producing a final answer. The messages " \
   "above are your own work so far. Review them, continue from where you left off "     \
   "without repeating work that is already complete, and produce the final answer."

static void job_worker_run(job_work_t *work) {
   session_t *s = NULL;
   int rc = job_manager_begin(work->user_id, work->conv_id, job_provider_from_default(), &s);
   if (rc != JOB_MGR_OK) {
      /* The tool checks caps before creating the row, but guard the race. */
      const char *err = (rc == JOB_MGR_CAP_GLOBAL || rc == JOB_MGR_CAP_PROVIDER ||
                         rc == JOB_MGR_CAP_USER)
                            ? "job capacity reached"
                            : "failed to start job";
      job_manager_set_terminal(work->conv_id, work->user_id, "failed", err, time(NULL), 0);
      job_manager_mark_dirty(); /* let the monitor notify this failure */
      OLOG_WARNING("job_worker: could not start job conv %lld (rc=%d)", (long long)work->conv_id,
                   rc);
      return;
   }

   /* Claim the row.  Losing means it left 'queued' while this detached thread
    * was waiting to be scheduled — a cancel retired it, so the turn must not
    * run.  Release the slot and leave the terminal row exactly as the canceller
    * wrote it (it already emitted the frame and marked the notice fired). */
   if (conv_db_job_set_running(work->conv_id, time(NULL)) != AUTH_DB_SUCCESS) {
      OLOG_INFO("job_worker: conv %lld left 'queued' before start (cancelled) — standing down",
                (long long)work->conv_id);
      job_manager_end(s);
      return;
   }
   /* queued -> running is a state change WITHIN the active set, so it changes no
    * count — but without it a watcher's row reads 'queued' with started_at 0 for
    * the job's whole runtime, and an elapsed timer computed off that renders
    * 1970.  Single-owner: from here on the worker owns this row's transitions. */
   job_update_emit(work->conv_id, work->user_id);
   OLOG_INFO("job_worker: running job conv %lld (session %u, user %d)", (long long)work->conv_id,
             s->session_id, work->user_id);

   /* A resumed job continues an existing transcript, so hydrate it before
    * dispatching — without this the model sees only JOB_RESUME_DIRECTIVE and
    * "continue from where you left off" refers to nothing.  Same seam
    * reinvoke_run_detached uses.
    *
    * An EMPTY transcript means the job never dispatched at all — it was refused
    * at the capacity gate, or its worker failed to start — so there is nothing
    * to continue: this is a first run, not a resume.  Send the stored goal
    * instead of the directive.  Getting this wrong is not cosmetic: sending
    * "continue where you left off" into an empty conversation produced a
    * confident non-answer, marked the job `done`, and — because `done` is not
    * resumable — destroyed the retry.  Observed live on conv 970, which is why
    * v74 made the goal durable at creation. */
   if (work->resume) {
      size_t restored = 0;
      struct json_object *hist = memory_history_load_from_db(work->conv_id, work->user_id,
                                                             &restored);
      if (hist != NULL && restored > 0) {
         pthread_mutex_lock(&s->history_mutex);
         if (s->conversation_history) {
            json_object_put(s->conversation_history);
         }
         s->conversation_history = hist;
         pthread_mutex_unlock(&s->history_mutex);
         OLOG_INFO("job_worker: resuming conv %lld with %zu bytes of history — continuing",
                   (long long)work->conv_id, restored);
      } else {
         if (hist != NULL) {
            json_object_put(hist);
         }
         /* job_worker_resume() refuses to claim a transcript-less row without a
          * goal, so this swap always has something to send. */
         char *goal = NULL;
         if (conv_db_job_get_goal(work->conv_id, work->user_id, &goal) == AUTH_DB_SUCCESS &&
             goal != NULL) {
            free(work->goal);
            work->goal = goal;
            OLOG_INFO("job_worker: conv %lld never ran — restarting from its goal",
                      (long long)work->conv_id);
         } else {
            OLOG_WARNING("job_worker: resume of conv %lld has neither history nor a goal",
                         (long long)work->conv_id);
         }
      }
   }

   /* Persist tool iterations to the job conversation for the whole (synchronous)
    * dispatch; pctx is stack-scoped and the hook fires on THIS thread. */
   job_persist_ctx_t pctx = { work->conv_id, work->user_id };
   session_set_tool_persist_hook(s, job_dispatch_tool_persist_cb, &pctx);

   text_input_dispatch_opts_t opts = {
      .conversation_id = work->conv_id, /* persist the goal (user msg) to the job conv */
      .auth_user_id = work->user_id,
      .sentence_cb = NULL, /* no TTS — text-only job */
      .sentence_userdata = NULL,
      .on_user_msg_added = NULL,
      .user_msg_added_ctx = NULL,
      .channel_hint = NULL,
   };
   char *response = core_text_input_dispatch(s, work->goal, NULL, NULL, NULL, 0, &opts);
   session_set_tool_persist_hook(s, NULL, NULL);

   time_t now = time(NULL);

   /* Disposition. Order matters, because a runtime reap ALSO raises
    * cancel_requested — the two are told apart only by the reap claim:
    *   - snapshot cancel_requested BEFORE claiming, so a reap landing between
    *     the two reads can't masquerade as a user cancel (which would
    *     mark_fired and silently swallow the timeout's follow-up);
    *   - claim_reaped() also stops the reap clock, so nothing can flag this job
    *     while the terminal writes below are in flight. */
   bool cancelled = atomic_load(&s->cancel_requested);
   bool user_cancelled = false;
   bool reaped = job_manager_claim_reaped(s, &user_cancelled);
   bool have_answer = (response != NULL && response[0] != '\0');

   /* Persist a produced answer no matter how the turn ended. A job that finished
    * microseconds before its deadline, or just as the user hit cancel, did the
    * work — throwing it away is never the right outcome, and it stays reachable
    * via `job status` even when the disposition below suppresses a notification.
    *
    * Capture the row id: the `complete` event carries it as final_message_id so a
    * tailer can correlate the disposition with the answer body it already
    * received via message_appended (§6.2). */
   int64_t final_msg_id = 0;
   if (have_answer &&
       conv_db_add_message_with_tools(work->conv_id, work->user_id, "assistant", response, NULL,
                                      NULL, NULL, &final_msg_id) != AUTH_DB_SUCCESS) {
      OLOG_WARNING("job_worker: failed to persist final answer to conv %lld",
                   (long long)work->conv_id);
      final_msg_id = 0;
   }
   /* §6.3: hand the ANSWER BODY to event-only consumers.  Without this a TUI
    * tailing this job sees every step and then never learns the conclusion. */
   if (have_answer) {
      conv_event_notify_message_appended(work->conv_id, work->user_id, final_msg_id, "assistant",
                                         response);
   }

   /* The daemon pulled the rug, as opposed to a human asking for the stop.
    *
    * Deliberately does NOT require cancel_requested.  On SIGINT the thing that
    * actually stops a job is llm_request_interrupt() — a GLOBAL flag the tool
    * loop polls (llm_tool_loop.c, step 11) — not this session's cancel flag,
    * which job_manager_shutdown() sets up to a second later.  Keying on
    * cancel_requested made this branch unreachable for every real Ctrl+C: the
    * worker returned no answer, saw no cancel, and filed itself as "failed: no
    * response from model" (live-verified on conv 1038).  Shutting down plus no
    * answer IS an interruption, however the stop arrived.
    *
    * The !user_cancelled term stays: a Cancel that lands as the daemon goes down
    * would otherwise be filed 'interrupted', which is tool-resumable, so the LLM
    * could quietly restart work the user just stopped. */
   const bool shutdown_stop = !reaped && !user_cancelled && job_manager_is_shutting_down();

   if (shutdown_stop && !have_answer) {
      /* Recording this as 'cancelled' would be wrong twice over: it suppresses
       * the completion notice on the grounds that the user asked for the stop,
       * and it lands the job in a state only a human may resume.  'interrupted'
       * is what actually happened — the same disposition the boot scan assigns
       * to a worker that never got to write at all — and leaving it UNFIRED is
       * what lets the user be told.  A shutdown that arrives AFTER the answer
       * falls through to 'done' below: the work is finished either way, and
       * filing it as interrupted invites a resume that re-runs it. */
      job_manager_set_terminal(work->conv_id, work->user_id, "interrupted", "daemon shutting down",
                               now, 0);
      OLOG_INFO("job_worker: job conv %lld interrupted by shutdown", (long long)work->conv_id);
   } else if (cancelled && !reaped && !shutdown_stop) {
      /* User cancel wins even if an answer landed: they asked it to stop, so no
       * completion notification. The answer above is still retrievable. */
      job_manager_set_terminal(work->conv_id, work->user_id, "cancelled", NULL, now, 0);
      conv_db_job_mark_fired(work->conv_id);
      OLOG_INFO("job_worker: job conv %lld cancelled%s", (long long)work->conv_id,
                have_answer ? " (answer produced before the cancel landed; kept)" : "");
   } else if (have_answer) {
      /* Includes the beat-the-reaper case: the deadline passed but the work
       * completed, so report it honestly as done rather than as a timeout. */
      job_manager_set_terminal(work->conv_id, work->user_id, "done", NULL, now, final_msg_id);
      OLOG_INFO("job_worker: job conv %lld done%s", (long long)work->conv_id,
                reaped ? " (finished as the runtime reap fired; kept the answer)" : "");
   } else if (reaped) {
      /* Deliberately NOT mark_fired: the monitor still owes this job's
       * on_complete (notify or reinvoke_parent).  A reaped reinvoke_parent row
       * flows through the normal reinvoke path, so conv_db_job_bump_reinvoke
       * already charges it against max_reinvokes_per_tree — a
       * timeout -> re-dispatch -> re-spawn loop cannot run forever. */
      job_manager_set_terminal(work->conv_id, work->user_id, "failed", JOB_ERR_TIMED_OUT, now, 0);
      OLOG_WARNING("job_worker: job conv %lld timed out (reaped)", (long long)work->conv_id);
   } else {
      job_manager_set_terminal(work->conv_id, work->user_id, "failed", "no response from model",
                               now, 0);
      OLOG_WARNING("job_worker: job conv %lld failed (empty response)", (long long)work->conv_id);
   }
   free(response);

   /* This job left the active set; job_manager_set_terminal() already pushed the
    * job_update that tells watchers so, on every disposition above. */

   /* Wake the completion monitor to fire the follow-up (a 'cancelled' row is
    * already marked fired, so the monitor skips it). */
   job_manager_mark_dirty();

   /* cancel-then-wait teardown of the job session (frees it). */
   job_manager_end(s);
}

static void *job_worker_thread(void *arg) {
   job_work_t *work = (job_work_t *)arg;
   job_worker_run(work);
   free(work->goal);
   free(work);
   return NULL;
}

/* Static: the only sanctioned way to reach a resume is job_worker_resume(),
 * which owns the ownership check, capacity gate, atomic claim and broadcast.
 * Exporting these next to it would make the unguarded call the shorter one. */
static int job_worker_spawn_ex(int user_id, int64_t conv_id, const char *goal, bool resume);

static int job_worker_spawn_resume(int user_id, int64_t conv_id) {
   return job_worker_spawn_ex(user_id, conv_id, JOB_RESUME_DIRECTIVE, true);
}

job_resume_result_t job_worker_resume(int64_t conv_id, int user_id, job_resume_origin_t origin) {
   if (conv_id <= 0 || user_id <= 0) {
      return JOB_RESUME_NOT_FOUND;
   }

   /* Ownership first: a job id is a guessable integer, so every surface that can
    * reach this must bind its caller's user_id (§8.2).  conv_db_job_get answers
    * FORBIDDEN for a foreign row and NOT_FOUND for a non-job conversation. */
   job_record_t rec;
   int grc = conv_db_job_get(conv_id, user_id, &rec);
   if (grc == AUTH_DB_FORBIDDEN) {
      return JOB_RESUME_FORBIDDEN;
   }
   if (grc != AUTH_DB_SUCCESS) {
      return JOB_RESUME_NOT_FOUND;
   }

   /* Fail before mutating.  job_manager_begin enforces the caps regardless, but
    * only by resetting the row and then immediately failing it, which reads to
    * the user as "resuming broke the job". */
   if (job_manager_capacity(user_id, job_provider_from_default()) != JOB_MGR_OK) {
      return JOB_RESUME_CAPACITY;
   }

   /* Same reason, for the one cap capacity() cannot see: a cancelled job holds
    * its pool slot until job_manager_end() finishes tearing it down, and Resume
    * on a cancelled job is now one click.  begin() refuses the duplicate anyway,
    * but only AFTER the claim has already reset the row — leaving a job that
    * reads 'queued' with no worker until the failure path retires it. */
   if (job_manager_conv_is_live(conv_id)) {
      return JOB_RESUME_NOT_RESUMABLE;
   }

   /* There must be SOMETHING to run: either a transcript to continue, or the
    * stored goal to start from.  A pre-v74 job that never dispatched has
    * neither — its goal was only ever a `messages` row that was never written,
    * and the v74 backfill had nothing to recover.  Refusing is the whole point:
    * resuming one anyway re-engages the model with no task, and the resulting
    * empty `done` is unresumable, so a bad resume is destructive rather than
    * merely useless. */
   char *goal_probe = NULL;
   bool have_goal = (conv_db_job_get_goal(conv_id, user_id, &goal_probe) == AUTH_DB_SUCCESS);
   free(goal_probe);
   if (!have_goal) {
      size_t restored = 0;
      struct json_object *hist = memory_history_load_from_db(conv_id, user_id, &restored);
      if (hist != NULL) {
         json_object_put(hist);
      }
      if (restored == 0) {
         OLOG_WARNING("job_worker: conv %lld has no goal and no transcript — not resumable",
                      (long long)conv_id);
         return JOB_RESUME_NOTHING_TO_RUN;
      }
   }

   /* The claim.  The resumable-status test lives inside the UPDATE, so exactly
    * one of two concurrent callers (two clicks, or the tool and a browser at
    * once) proceeds to spawn a worker.  The origin widens that test to include
    * 'cancelled' for a human caller only — passed down rather than pre-checked
    * here, so the decision stays inside the same atomic statement as the rest of
    * the predicate and a tool caller cannot win a race a user started. */
   if (conv_db_job_reset_for_resume(conv_id, user_id, origin == JOB_RESUME_ORIGIN_USER) !=
       AUTH_DB_SUCCESS) {
      return JOB_RESUME_NOT_RESUMABLE;
   }

   /* Publish 'queued' before the worker exists — same ordering rule as spawn —
    * flagged so watchers release the terminal mark they hold for this job. */
   job_update_emit_ex(conv_id, user_id, true);

   if (job_worker_spawn_resume(user_id, conv_id) != SUCCESS) {
      job_manager_set_terminal(conv_id, user_id, "failed", "resume failed to start", time(NULL), 0);
      /* The reset just cleared on_complete_fired, so this row is terminal and
       * unfired.  Without waking the dirty-gated monitor it waits for an
       * unrelated job transition — on an idle daemon, forever, and across a
       * restart the boot-time guard marks it fired and suppresses the notice. */
      job_manager_mark_dirty();
      return JOB_RESUME_FAILED;
   }
   OLOG_INFO("job_worker: resumed job conv %lld for user %d", (long long)conv_id, user_id);
   return JOB_RESUME_STARTED;
}

int job_worker_spawn(int user_id, int64_t conv_id, const char *goal) {
   return job_worker_spawn_ex(user_id, conv_id, goal, false);
}

static int job_worker_spawn_ex(int user_id, int64_t conv_id, const char *goal, bool resume) {
   if (user_id <= 0 || conv_id <= 0 || goal == NULL) {
      return FAILURE;
   }
   job_work_t *work = calloc(1, sizeof(job_work_t));
   if (work == NULL) {
      return FAILURE;
   }
   work->user_id = user_id;
   work->conv_id = conv_id;
   work->resume = resume;
   work->goal = strdup(goal);
   if (work->goal == NULL) {
      free(work);
      return FAILURE;
   }

   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   int prc = pthread_create(&thread, &attr, job_worker_thread, work);
   pthread_attr_destroy(&attr);
   if (prc != 0) {
      OLOG_ERROR("job_worker: pthread_create failed (%d) for conv %lld", prc, (long long)conv_id);
      free(work->goal);
      free(work);
      return FAILURE;
   }
   return SUCCESS;
}
