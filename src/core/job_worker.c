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
#include "core/job_dispatch.h"
#include "core/job_manager.h"
#include "core/session_manager.h"
#include "core/text_input_dispatch.h"
#include "dawn_error.h"
#include "llm/llm_interface.h"
#include "logging.h"

/* Work item handed to the detached worker thread. */
typedef struct {
   int user_id;
   int64_t conv_id;
   char *goal; /* owned; freed by the worker */
} job_work_t;

static void job_worker_run(job_work_t *work) {
   session_t *s = NULL;
   int rc = job_manager_begin(work->user_id, work->conv_id, job_provider_from_default(), &s);
   if (rc != JOB_MGR_OK) {
      /* The tool checks caps before creating the row, but guard the race. */
      const char *err = (rc == JOB_MGR_CAP_GLOBAL || rc == JOB_MGR_CAP_PROVIDER ||
                         rc == JOB_MGR_CAP_USER)
                            ? "job capacity reached"
                            : "failed to start job";
      conv_db_job_set_terminal(work->conv_id, "failed", err, time(NULL));
      job_manager_mark_dirty(); /* let the monitor notify this failure */
      OLOG_WARNING("job_worker: could not start job conv %lld (rc=%d)", (long long)work->conv_id,
                   rc);
      return;
   }

   conv_db_job_set_running(work->conv_id, time(NULL));
   OLOG_INFO("job_worker: running job conv %lld (session %u, user %d)", (long long)work->conv_id,
             s->session_id, work->user_id);

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
   if (atomic_load(&s->cancel_requested)) {
      /* Cancelled mid-run: mark cancelled + suppress the completion follow-up. */
      conv_db_job_set_terminal(work->conv_id, "cancelled", NULL, now);
      conv_db_job_mark_fired(work->conv_id);
      OLOG_INFO("job_worker: job conv %lld cancelled", (long long)work->conv_id);
   } else if (response != NULL && response[0] != '\0') {
      if (conv_db_add_message_with_tools(work->conv_id, work->user_id, "assistant", response, NULL,
                                         NULL, NULL, NULL) != AUTH_DB_SUCCESS) {
         OLOG_WARNING("job_worker: failed to persist final answer to conv %lld",
                      (long long)work->conv_id);
      }
      conv_db_job_set_terminal(work->conv_id, "done", NULL, now);
      OLOG_INFO("job_worker: job conv %lld done", (long long)work->conv_id);
   } else {
      conv_db_job_set_terminal(work->conv_id, "failed", "no response from model", now);
      OLOG_WARNING("job_worker: job conv %lld failed (empty response)", (long long)work->conv_id);
   }
   free(response);

   /* Refresh the parent conversation's "jobs running" pills (this job left the
    * active set). */
   job_record_t jr;
   if (conv_db_job_get(work->conv_id, work->user_id, &jr) == AUTH_DB_SUCCESS) {
      job_activity_emit(work->user_id, jr.parent_id);
   }

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

int job_worker_spawn(int user_id, int64_t conv_id, const char *goal) {
   if (user_id <= 0 || conv_id <= 0 || goal == NULL) {
      return FAILURE;
   }
   job_work_t *work = calloc(1, sizeof(job_work_t));
   if (work == NULL) {
      return FAILURE;
   }
   work->user_id = user_id;
   work->conv_id = conv_id;
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
