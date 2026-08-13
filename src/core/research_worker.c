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
 * Deep-research worker (DEEP_RESEARCH_DESIGN.md §4/§15 Step 6).  See research_worker.h.
 *
 * Layer 2: reuses the background-job pool (job_manager_*) for the slot/caps/
 * teardown, but runs the research CONTROLLER (research_run_execute) rather than
 * the generic tool loop — so it is a sibling of job_worker.c, not a caller of it.
 * A research job's answer is not a chat message; the report is materialized from
 * research_claims into a report revision by research_run_execute, so nothing is
 * persisted to the job conversation here (final_message_id is always 0).  Step 8
 * turns that revision into a delivered notes doc.
 */

#include "core/research_worker.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "core/job_dispatch.h" /* job_provider_from_default */
#include "core/job_manager.h"
#include "core/session_manager.h"
#include "logging.h"
#include "tools/research_run.h"

/* Strong def in webui_broadcasts.c (this worker is ENABLE_WEBUI-only, so it always
 * links).  Declared here rather than pulling in the heavy webui_server.h — the
 * job_worker.c → webui_server.h include is tracked debt we don't want to grow.
 * Signals an open WebUI chat that a conversation has new messages → refetch+render. */
void webui_broadcast_conversation_messages_appended(int user_id, int64_t conv_id);

/* Work item handed to the detached worker thread. */
typedef struct {
   int user_id;
   int64_t conv_id;
} research_work_t;

/* Best-effort: mark the run's own header terminal too, but only if it exists and
 * has not already finished.  The job row is the source of truth for the pool; the
 * research row mirrors it for the `status`/report surfaces.  Called on every exit
 * path so a run never lingers at 'planning'/'researching' after its job ended. */
static void research_mark_run_terminal(int64_t conv_id,
                                       const char *status,
                                       const char *stop_reason,
                                       time_t now) {
   research_run_t r;
   if (research_db_run_get_by_conversation(conv_id, &r) == AUTH_DB_SUCCESS && r.finished_at == 0) {
      research_db_run_set_terminal(r.id, status, stop_reason, now);
   }
}

/* Land the completion in the ORIGINATING conversation (the chat the run was
 * launched from), so the user sees it in-thread rather than only as a transient
 * toast.  This is NOT reinvoke_parent (§11): it persists a FIXED template — the
 * user's own brief + a finding count + a pointer to the notes report, NEVER the
 * web-derived report body — and does NOT re-engage the LLM, so it opens no
 * injection path.  Best-effort: any failure here never affects the terminal state.
 * A rootless / voice run (parent_id <= 0) has no chat thread, so the toast covers
 * it. */
static void research_deliver_to_parent(int64_t job_conv, int user_id) {
   job_record_t rec;
   if (conv_db_job_get(job_conv, user_id, &rec) != AUTH_DB_SUCCESS || rec.parent_id <= 0) {
      return;
   }
   research_run_t run;
   if (research_db_run_get_by_conversation(job_conv, &run) != AUTH_DB_SUCCESS) {
      return;
   }
   int claim_count = 0;
   research_db_claim_count(run.id, &claim_count);

   char msg[RESEARCH_BRIEF_MAX + 256];
   if (claim_count > 0) {
      snprintf(msg, sizeof(msg),
               "🔍 Deep research complete — \"%s\".\n\nI gathered %d finding%s and saved a cited "
               "report to your notes. Ask me about it, or open it in your documents.",
               run.brief, claim_count, claim_count == 1 ? "" : "s");
   } else {
      snprintf(msg, sizeof(msg),
               "🔍 Deep research on \"%s\" finished, but I couldn't find enough to report. Try "
               "rephrasing or narrowing the question.",
               run.brief);
   }

   int64_t msg_id = 0;
   if (conv_db_add_message_with_tools(rec.parent_id, user_id, "assistant", msg, NULL, NULL, NULL,
                                      &msg_id) != AUTH_DB_SUCCESS) {
      OLOG_WARNING("research_worker: failed to post completion to parent conv %lld",
                   (long long)rec.parent_id);
      return;
   }
   webui_broadcast_conversation_messages_appended(user_id, rec.parent_id);
   OLOG_INFO("research_worker: posted completion to parent conv %lld (msg %lld)",
             (long long)rec.parent_id, (long long)msg_id);
}

static void research_worker_run(research_work_t *work) {
   session_t *s = NULL;
   int rc = job_manager_begin(work->user_id, work->conv_id, job_provider_from_default(), &s);
   if (rc != JOB_MGR_OK) {
      /* The tool checks caps before creating the row, but guard the race. */
      const char *err = (rc == JOB_MGR_CAP_GLOBAL || rc == JOB_MGR_CAP_PROVIDER ||
                         rc == JOB_MGR_CAP_USER)
                            ? "job capacity reached"
                            : "failed to start research job";
      time_t now = time(NULL);
      /* Research row first, then the job row — uniform ordering on every exit so
       * "the research header is written before the job terminal" is an invariant
       * a monitor poll can rely on (never done-job/researching-run). */
      research_mark_run_terminal(work->conv_id, "failed", "failed", now);
      job_manager_set_terminal(work->conv_id, work->user_id, "failed", err, now, 0);
      job_manager_mark_dirty(); /* let the monitor notify this failure */
      OLOG_WARNING("research_worker: could not start research job conv %lld (rc=%d)",
                   (long long)work->conv_id, rc);
      return;
   }

   /* Claim the row.  Losing means it left 'queued' while this detached thread was
    * waiting to be scheduled — a cancel retired it, so the run must not start.
    * Release the slot and leave the terminal job row as the canceller wrote it;
    * only mirror the cancel onto the research header (the canceller retires the
    * job row but knows nothing about research_runs). */
   if (conv_db_job_set_running(work->conv_id, time(NULL)) != AUTH_DB_SUCCESS) {
      OLOG_INFO("research_worker: conv %lld left 'queued' before start (cancelled) — standing down",
                (long long)work->conv_id);
      research_mark_run_terminal(work->conv_id, "cancelled", "cancelled", time(NULL));
      job_manager_end(s);
      return;
   }
   /* queued -> running is a state change WITHIN the active set (no count change),
    * but without it a watcher's row reads 'queued' with started_at 0 for the whole
    * runtime and an elapsed timer renders 1970.  Single-owner from here on. */
   job_update_emit(work->conv_id, work->user_id);

   research_run_t run;
   if (research_db_run_get_by_conversation(work->conv_id, &run) != AUTH_DB_SUCCESS) {
      /* Step 7 creates the run and the job atomically before spawning, so a
       * missing run here is a real integrity fault, not a race.  Still try to
       * mirror the failure onto the research header: if the get failed only
       * transiently (SQLite lock/IO) the row is really there and this recovers
       * the mirror; if the row is genuinely gone it is a finished_at-guarded
       * no-op.  Without it a transient fault could strand the run at 'planning'
       * forever while its job is 'failed'. */
      time_t now = time(NULL);
      research_mark_run_terminal(work->conv_id, "failed", "failed", now);
      job_manager_set_terminal(work->conv_id, work->user_id, "failed", "research run row missing",
                               now, 0);
      job_manager_mark_dirty();
      OLOG_ERROR("research_worker: no research run for conv %lld — failing job",
                 (long long)work->conv_id);
      job_manager_end(s);
      return;
   }

   OLOG_INFO("research_worker: running research job conv %lld (run %lld, session %u, user %d)",
             (long long)work->conv_id, (long long)run.id, s->session_id, work->user_id);

   research_budgets_t b;
   research_budgets_load(&b); /* compile-time defaults overlaid with [research] config */

   /* Drive the round loop + synthesis.  Returns a static stop_reason literal:
    * coverage | budget | token_budget | cancelled | failed. */
   const char *stop = research_run_execute(s, &run, &b);

   time_t now = time(NULL);

   /* Disposition — same authoritative signals job_worker uses, because the loop's
    * "cancelled" is ambiguous: a user Cancel, a runtime reap, and daemon shutdown
    * all arrive as the session cancel flag.  job_manager_claim_reaped() resolves
    * reap-vs-user-cancel under the pool lock (and stops the reap clock), and
    * is_shutting_down() isolates the daemon-pulled-the-rug case. */
   bool user_cancelled = false;
   bool reaped = job_manager_claim_reaped(s, &user_cancelled);
   const bool shutdown_stop = !reaped && !user_cancelled && job_manager_is_shutting_down();

   const char *job_status;
   const char *job_err = NULL;
   const char *research_status;
   const char *research_stop;

   if (shutdown_stop) {
      /* Honest "the daemon stopped it": 'interrupted' rather than 'cancelled', so
       * it neither suppresses a completion notice as if the user asked for it nor
       * lands where only a human may resume.  P0 has no research resume, so this is
       * purely descriptive.  If the run gathered any findings before the stop, its
       * report was filed to notes on the way out (persist gates on findings, not on
       * the stop reason), so it is retrievable via `status`. */
      job_status = "interrupted";
      job_err = "daemon shutting down";
      research_status = "failed";
      research_stop = "interrupted";
   } else if (user_cancelled) {
      job_status = "cancelled";
      research_status = "cancelled";
      research_stop = "cancelled";
   } else if (reaped) {
      job_status = "failed";
      job_err = JOB_ERR_TIMED_OUT;
      research_status = "failed";
      research_stop = "timeout";
   } else if (stop != NULL && (strcmp(stop, "coverage") == 0 || strcmp(stop, "budget") == 0 ||
                               strcmp(stop, "token_budget") == 0)) {
      /* The run finished on its own terms — the ONLY stop strings that map to a
       * successful 'done'.  Whitelisted explicitly (not an else) so a stop the
       * three authoritative flags didn't claim can never be upgraded to success:
       * research_run_execute emits "cancelled" the instant it sees the session
       * cancel flag, so if any FUTURE cancel source ever raises that flag outside
       * job_manager's three mechanisms, this must fail closed, not report a
       * completed run.  (job_worker.c is immune the same way — its fallthrough is
       * 'failed', never 'done'.) */
      job_status = "done";
      research_status = "done";
      research_stop = stop;
   } else {
      /* "failed" (controller gave up after RESEARCH_MAX_CONSECUTIVE_FAILURES dead
       * provider calls / a NULL-arg fault), a stray unattributed "cancelled", or
       * any unrecognized/NULL stop — never a success.  research_stop records the
       * actual reason for diagnostics. */
      job_status = "failed";
      job_err = "research produced no usable result";
      research_status = "failed";
      research_stop = (stop != NULL) ? stop : "failed";
   }

   /* Record the research header first (the `status` surface reads it), then the
    * job terminal (the pool + completion monitor read it). */
   research_db_run_set_terminal(run.id, research_status, research_stop, now);
   job_manager_set_terminal(work->conv_id, work->user_id, job_status, job_err, now, 0);

   /* A user cancel suppresses the completion notification (they asked for the
    * stop); mark it fired so the monitor skips it, mirroring job_worker. */
   if (strcmp(job_status, "cancelled") == 0) {
      conv_db_job_mark_fired(work->conv_id);
   }

   /* On a clean finish, land a completion message in the originating chat (safe
    * template, not reinvoke — see research_deliver_to_parent).  Failure/interrupt
    * states stay toast+status only, to keep error noise out of the thread. */
   if (strcmp(job_status, "done") == 0) {
      research_deliver_to_parent(work->conv_id, work->user_id);
   }

   /* NB: `run` is the pre-loop snapshot (research_run_execute advances rounds only
    * in the DB), so do not log run.rounds_run here — it would read stale (0).
    * research_run_execute emits the authoritative round count in its own log. */
   OLOG_INFO("research_worker: research job conv %lld finished (job=%s, research=%s/%s)",
             (long long)work->conv_id, job_status, research_status, research_stop);

   /* Wake the completion monitor to fire the follow-up (Step 8 refines delivery to
    * the report link; a 'cancelled' row is already marked fired above). */
   job_manager_mark_dirty();

   /* cancel-then-wait teardown of the job session (frees it). */
   job_manager_end(s);
}

static void *research_worker_thread(void *arg) {
   research_work_t *work = (research_work_t *)arg;
   research_worker_run(work);
   free(work);
   return NULL;
}

int research_worker_spawn(int user_id, int64_t conv_id) {
   if (user_id <= 0 || conv_id <= 0) {
      return FAILURE;
   }
   research_work_t *work = calloc(1, sizeof(research_work_t));
   if (work == NULL) {
      return FAILURE;
   }
   work->user_id = user_id;
   work->conv_id = conv_id;

   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   int prc = pthread_create(&thread, &attr, research_worker_thread, work);
   pthread_attr_destroy(&attr);
   if (prc != 0) {
      OLOG_ERROR("research_worker: pthread_create failed (%d) for conv %lld", prc,
                 (long long)conv_id);
      free(work);
      return FAILURE;
   }
   return SUCCESS;
}
