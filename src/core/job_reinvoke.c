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
 * Background-job result reinjection (reinvoke_parent).  See job_reinvoke.h.
 *
 * Two delivery paths, chosen per re-engagement:
 *   - LIVE (preferred): the owner is viewing the parent conversation in a
 *     connected WebUI session — ENQUEUE the re-engagement onto that session's
 *     turn queue (TURN_SOURCE_BACKGROUND) so it streams INTO the session (warm
 *     prompt cache, native token stream, no reload) but SERIALIZED behind any
 *     user turn.  The turn queue is the sole gate — a reinvoke never races a user
 *     turn on the same session.  See reinvoke_turn_entry() + webui_find_reinvoke_viewer().
 *   - DETACHED (fallback): no connected viewer (or the live queue rejected us) —
 *     run on a throwaway job-pool session bound to the parent conv, persist the
 *     reply server-side, and nudge any open tab to refresh.  The proven job_worker
 *     pattern.
 *
 * LOCKING: s_inflight_mutex is a LEAF (guards only the in-flight parent set +
 * its count).  It is never held across a job_manager_* call, a conv_db_* call,
 * or the LLM dispatch.
 */

#include "core/job_reinvoke.h"

#include <json-c/json.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/conv_event.h"
#include "core/job_dispatch.h"
#include "core/job_manager.h"
#include "core/memory_filter.h"
#include "core/session_manager.h"
#include "core/text_input_dispatch.h"
#include "core/turn_queue.h"
#include "dawn_error.h"
#include "llm/llm_interface.h"
#include "logging.h"
#include "memory/memory_history_loader.h"
#include "webui/webui_server.h" /* webui_tool_iteration_cb — bubble-seal hook (job_reinvoke is ENABLE_WEBUI-only) */

/* Generous per-result head cap so a pathological job output can't blow the
 * reinjected prompt.  Decision #3 is "full result" — this is a safety ceiling,
 * not a summarizer. */
#define JOB_REINVOKE_RESULT_CAP 12000

/* In-flight parent conversations — one re-engagement worker per parent at a time
 * (single-writer). Sized comfortably above max_concurrent_reinvokes. */
#define JOB_REINVOKE_MAX_INFLIGHT 64

/* A coalesced group can hold at most one monitor batch of jobs, so the group /
 * fired-id buffers are sized to JOB_MONITOR_MAX_PER_TICK (job_manager.h).  Pin
 * that cross-file invariant at compile time. */
_Static_assert(JOB_MONITOR_MAX_PER_TICK <= JOB_REINVOKE_MAX_INFLIGHT,
               "reinvoke in-flight set must cover a full monitor batch");
static int64_t s_inflight[JOB_REINVOKE_MAX_INFLIGHT];
static int s_inflight_n = 0;
static pthread_mutex_t s_inflight_mutex = PTHREAD_MUTEX_INITIALIZER;

/* How long a parent may keep deferring its re-engagement before its finished
 * jobs are delivered as plain notifications instead.
 *
 * Deferral itself is correct and common — a parent mid-turn should wait.  What
 * is not survivable is deferring FOREVER: conv_db_job_list_pending_followups is
 * global (no user filter), ordered finished_at ASC and limited to a few rows, so
 * a parent whose turn-in-flight flag never clears pins the oldest rows at the
 * head of that queue for EVERY user on the daemon, and keeps the dirty-gated
 * monitor scanning every second.  Ten minutes is far longer than any real turn
 * and short enough that a wedge degrades rather than silences. */
#define JOB_REINVOKE_MAX_DEFER_SEC 600
#define JOB_REINVOKE_DEFER_SLOTS 16

/* A parent that is genuinely still deferring is re-noted on every drain, so
 * anything not seen for this long has moved on (its rows were fired elsewhere,
 * or it recovered) and its slot is reclaimed.  Without the sweep the table fills
 * with dead parents, and a full table degrades on sight — which would turn the
 * ordinary case, a parent briefly mid-turn, into an instant demotion. */
#define JOB_REINVOKE_DEFER_STALE_SEC 60

typedef struct {
   int64_t parent_conv;
   time_t first_deferred_at;
   time_t last_seen;
} reinvoke_defer_t;

static reinvoke_defer_t s_defer[JOB_REINVOKE_DEFER_SLOTS];
static int s_defer_n = 0; /* monitor thread only — no lock needed */

/* Record a deferral for @parent_conv; true when it has been deferring longer
 * than the bound and its rows should be degraded to notifications.  A full table
 * answers true, which fails toward delivering rather than toward silence. */
static bool defer_note(int64_t parent_conv, time_t now) {
   for (int i = 0; i < s_defer_n;) { /* sweep the dead before looking for a slot */
      if (s_defer[i].parent_conv != parent_conv &&
          (now - s_defer[i].last_seen) > JOB_REINVOKE_DEFER_STALE_SEC) {
         s_defer[i] = s_defer[s_defer_n - 1];
         s_defer_n--;
         continue; /* re-test the row swapped into i */
      }
      if (s_defer[i].parent_conv == parent_conv) {
         s_defer[i].last_seen = now;
         return (now - s_defer[i].first_deferred_at) > JOB_REINVOKE_MAX_DEFER_SEC;
      }
      i++;
   }
   if (s_defer_n >= JOB_REINVOKE_DEFER_SLOTS) {
      return true;
   }
   s_defer[s_defer_n].parent_conv = parent_conv;
   s_defer[s_defer_n].first_deferred_at = now;
   s_defer[s_defer_n].last_seen = now;
   s_defer_n++;
   return false;
}

/* Forget a parent's deferral history — it got its worker, so the clock restarts
 * if it ever backs up again. */
static void defer_forget(int64_t parent_conv) {
   for (int i = 0; i < s_defer_n; i++) {
      if (s_defer[i].parent_conv == parent_conv) {
         s_defer[i] = s_defer[s_defer_n - 1];
         s_defer_n--;
         return;
      }
   }
}

/* Work handed to the re-engagement worker thread (reinvoke_thread).  No job list:
 * the coalesced set is RE-QUERIED at dispatch time
 * (conv_db_job_pending_reinvoke_for_parent) so a re-engagement that waited behind
 * an active turn picks up sibling jobs that finished meanwhile (true coalescing). */
typedef struct {
   int64_t parent_conv;
   int user_id;
} reinvoke_work_t;

/* Marker enqueued onto a LIVE viewer session's turn queue (TURN_SOURCE_BACKGROUND).
 * Carries the retained viewer + the per-parent s_inflight claim; the turn queue
 * owns all three from a successful enqueue until the closure (run path) or the
 * free closure (purge/reject path) runs the exactly-once release tail. */
typedef struct {
   int64_t parent_conv;
   int user_id;
   session_t *live; /* retained */
} reinvoke_queued_t;

/* =============================================================================
 * In-flight parent set (leaf-locked)
 * ============================================================================= */

typedef enum {
   CLAIM_OK,
   CLAIM_AT_CAP,
   CLAIM_ALREADY_INFLIGHT
} claim_result_t;

/* Reserve a parent for re-engagement iff under the global concurrency cap and
 * not already in flight.  Atomic check-and-insert under the leaf lock. */
static claim_result_t inflight_try_claim(int64_t parent_conv, int cap) {
   claim_result_t rc = CLAIM_OK;
   pthread_mutex_lock(&s_inflight_mutex);
   if (s_inflight_n >= cap || s_inflight_n >= JOB_REINVOKE_MAX_INFLIGHT) {
      rc = CLAIM_AT_CAP;
   } else {
      for (int i = 0; i < s_inflight_n; i++) {
         if (s_inflight[i] == parent_conv) {
            rc = CLAIM_ALREADY_INFLIGHT;
            break;
         }
      }
      if (rc == CLAIM_OK) {
         s_inflight[s_inflight_n++] = parent_conv;
      }
   }
   pthread_mutex_unlock(&s_inflight_mutex);
   return rc;
}

static void inflight_release(int64_t parent_conv) {
   pthread_mutex_lock(&s_inflight_mutex);
   for (int i = 0; i < s_inflight_n; i++) {
      if (s_inflight[i] == parent_conv) {
         s_inflight[i] = s_inflight[s_inflight_n - 1];
         s_inflight_n--;
         break;
      }
   }
   pthread_mutex_unlock(&s_inflight_mutex);
}

/* =============================================================================
 * Helpers
 * ============================================================================= */

/* Append a bounded, NUL-safe copy of @src to the growable buffer at *buf.
 * Overflow-guarded (returns false rather than wrap) though the result cap keeps
 * every real call far from SIZE_MAX. */
static bool sb_append(char **buf, size_t *len, size_t *cap, const char *src, size_t src_len) {
   if (src_len > SIZE_MAX - 1 - *len) {
      return false; /* size overflow */
   }
   size_t need = *len + src_len + 1;
   if (need > *cap) {
      size_t newcap = (*cap == 0) ? 1024 : *cap;
      while (newcap < need) {
         if (newcap > SIZE_MAX / 2) {
            newcap = need;
            break;
         }
         newcap *= 2;
      }
      char *nb = realloc(*buf, newcap);
      if (nb == NULL) {
         return false;
      }
      *buf = nb;
      *cap = newcap;
   }
   memcpy(*buf + *len, src, src_len);
   *len += src_len;
   (*buf)[*len] = '\0';
   return true;
}

/* Runaway/injection-filtered force-fire fallback: the job is done and marked
 * fired, but we won't re-engage the LLM with it — surface a passive notify so
 * the user still learns the result is ready (decision #5 + injection safety). */
static void reinvoke_notify_fallback(int user_id, const char *title, int64_t job_id) {
   char text[CONV_TITLE_MAX + 96];
   snprintf(text, sizeof(text), "Background job \"%s\" finished — ask me for the result.", title);
   /* job_notify_user, NOT a bare broadcast: every caller below marks the row
    * fired, and each of these paths is reached precisely when a re-engagement is
    * impossible — which correlates with the browser being gone, so a bare
    * broadcast would fire the row into an empty room.  The funnel queues it
    * durably when nobody is listening. */
   job_notify_user(user_id, text, job_id, job_manager_running_count());
}

/* Build the coalesced user-role automated-event envelope for a group of jobs.
 * Returns a malloc'd string (caller frees), or NULL on OOM / no results.
 * Jobs whose reinvoke attempt count exceeds the runaway ceiling are force-fired
 * and excluded; @fired_ids/@n_fired receive the surviving (to-be-fired) ids. */
/* Build the reinjected prompt by RE-QUERYING the parent's still-pending reinvoke
 * jobs at dispatch time (C4).  Carries attempt-time side effects — the runaway
 * ceiling bump, the injection-command check, and force-fire+notify — which is why
 * it must run at dispatch, not at enqueue (a turn later purged must not have
 * bumped counts or fired notifications).  Returns the malloc'd envelope + the
 * fired_ids to mark on success, or NULL if nothing is left to re-engage. */
static char *build_envelope(int64_t parent_conv, int user_id, int64_t *fired_ids, int *n_fired) {
   *n_fired = 0;
   int cap = g_config.jobs.max_reinvokes_per_tree;

   job_record_t rows[JOB_MONITOR_MAX_PER_TICK];
   int nrows = 0;
   if (conv_db_job_pending_reinvoke_for_parent(parent_conv, user_id, rows, JOB_MONITOR_MAX_PER_TICK,
                                               &nrows) != AUTH_DB_SUCCESS ||
       nrows == 0) {
      return NULL; /* siblings already fired/claimed elsewhere, or none left */
   }

   char *buf = NULL;
   size_t len = 0, bcap = 0;
   const char *head =
       "[automated background-job update]\n"
       "The background task(s) you started earlier have finished. Their full results are below. "
       "Tell me about them now, and take any obvious next step I'd want.\n";
   if (!sb_append(&buf, &len, &bcap, head, strlen(head))) {
      return NULL;
   }

   int included = 0;
   for (int i = 0; i < nrows; i++) {
      int64_t job_id = rows[i].id;
      const char *title = rows[i].title;
      int newcount = 0;
      bool runaway = (conv_db_job_bump_reinvoke(job_id, &newcount) == AUTH_DB_SUCCESS &&
                      newcount > cap);

      char *result = NULL;
      if (conv_db_job_last_assistant_text(job_id, user_id, &result) != AUTH_DB_SUCCESS) {
         OLOG_WARNING("job_reinvoke: could not read result for job %lld", (long long)job_id);
      }

      /* Don't re-engage the LLM with an untrusted result that trips the injection
       * filter — the result came from an LLM/tool run over external content.  Use
       * the NARROWED command-subset check (not the full blocklist): a research
       * result legitimately contains "bearer", "api key", "system prompt",
       * markdown links, etc., so the full filter false-positives heavily here;
       * the command subset still catches "ignore your instructions"-class attacks.
       * Force-fire + notify on a hit instead of re-engaging. */
      bool injected = (result != NULL && memory_filter_check_injection_commands(result));

      if (runaway || injected) {
         conv_db_job_mark_fired(job_id);
         reinvoke_notify_fallback(user_id, title, job_id);
         OLOG_WARNING("job_reinvoke: job %lld force-fired (%s) — notifying instead of re-engaging",
                      (long long)job_id, runaway ? "reinvoke ceiling" : "injection filter");
         free(result);
         continue;
      }

      char hdr[CONV_TITLE_MAX + 16];
      snprintf(hdr, sizeof(hdr), "\n— \"%s\" —\n", title);
      if (!sb_append(&buf, &len, &bcap, hdr, strlen(hdr))) {
         free(result);
         free(buf);
         return NULL;
      }

      /* A runtime-reaped job has no assistant text, so say WHY rather than
       * letting the model report an empty result as "produced no output" —
       * "timed out" is the difference between a useless answer and an
       * actionable one (design §5: the timeout follow-up must be honest). */
      const char *empty_reason = job_record_timed_out(&rows[i])
                                     ? "(the job timed out before producing a result)"
                                 : (strcmp(rows[i].job_status, "interrupted") == 0)
                                     ? "(the job was interrupted before producing a result)"
                                 : (strcmp(rows[i].job_status, "failed") == 0)
                                     ? "(the job failed before producing a result)"
                                     : "(the job produced no output)";
      /* Only a 'done' job HAS a result.  conv_db_job_last_assistant_text returns
       * the last assistant row whatever it is, and for a job that never reached a
       * final answer that row is the tool-call preamble — "I'll dive into X, let
       * me search…".  Handing that over as the result is worse than saying
       * nothing: it reads like an answer, so the model either reports it as one or
       * silently redoes the whole task inline.  Live-verified on conv 1041, whose
       * preamble was presented as research and prompted the parent to run the
       * searches over again in its own turn — correct output, invisible cost. */
      const bool has_result = (strcmp(rows[i].job_status, "done") == 0);
      const char *body = (has_result && result && result[0] != '\0') ? result : empty_reason;
      size_t body_len = strlen(body);
      bool truncated = false;
      if (body_len > JOB_REINVOKE_RESULT_CAP) {
         body_len = JOB_REINVOKE_RESULT_CAP;
         /* Back off to a UTF-8 code-point boundary so JSON encoding of the
          * request can't 400 on a split multibyte sequence. */
         while (body_len > 0 && ((unsigned char)body[body_len] & 0xC0) == 0x80) {
            body_len--;
         }
         truncated = true;
      }
      if (!sb_append(&buf, &len, &bcap, body, body_len) ||
          (truncated &&
           !sb_append(&buf, &len, &bcap, "\n…[result truncated]",
                      strlen("\n…[result truncated]")))) { /* byte len, not char count (… = 3B) */
         free(result);
         free(buf);
         return NULL;
      }
      free(result);

      fired_ids[(*n_fired)++] = job_id;
      included++;
   }

   if (included == 0) {
      free(buf);
      return NULL; /* everything was a runaway; nothing to re-engage */
   }
   return buf;
}

/* =============================================================================
 * Re-engagement worker (live-stream path + detached fallback)
 * ============================================================================= */

/* Common transient-envelope dispatch opts: the synthetic [automated event]
 * message is NOT echoed as a user bubble (on_user_msg_added NULL) and NOT
 * persisted (conversation_id 0) — only the assistant reply lands in the conv. */
static text_input_dispatch_opts_t reinvoke_dispatch_opts(int user_id) {
   text_input_dispatch_opts_t opts = {
      .conversation_id = 0,
      .auth_user_id = user_id,
      .sentence_cb = NULL,
      .sentence_userdata = NULL,
      .on_user_msg_added = NULL,
      .user_msg_added_ctx = NULL,
      .channel_hint = NULL,
      .is_background_turn = true, /* reinvoke: emits observe `status` (§6.2) */
   };
   return opts;
}

/* ---------------------------------------------------------------------------
 * LIVE path: a re-engagement turn enqueued onto the viewer session's turn queue
 * (TURN_SOURCE_BACKGROUND).  The turn queue serializes it behind any user turn —
 * the sole gate, so no reinvoke races a user turn on the same session.  A marker
 * (reinvoke_queued_t) owns the retained viewer + the per-parent s_inflight claim
 * from a successful enqueue; EXACTLY ONE of the run tail (reinvoke_turn_entry) or
 * the free closure (reinvoke_turn_free) performs the release tail.
 * ------------------------------------------------------------------------- */

/* free_work closure: drop a queued reinvoke WITHOUT running it (bound-reject,
 * purge, being_destroyed, or spawn failure).  Releases the viewer retain + the
 * s_inflight claim and re-arms the monitor so the re-engagement retries. */
static void reinvoke_turn_free(void *work) {
   reinvoke_queued_t *m = (reinvoke_queued_t *)work;
   if (m == NULL) {
      return;
   }
   if (m->live != NULL) {
      session_release(m->live);
   }
   inflight_release(m->parent_conv);
   job_manager_mark_dirty(); /* C2: re-arm so a dropped re-engagement retries */
   free(m);
}

/* Worker entry for a dequeued reinvoke turn.  Claims the turn AT DEQUEUE (the
 * queue is the gate — no request_generation bump), builds the envelope by
 * re-querying the parent's pending jobs (coalesces late arrivals), dispatches,
 * persists server-side when the client won't save it (C3), then runs the tail. */
static void *reinvoke_turn_entry(void *arg) {
   reinvoke_queued_t *m = (reinvoke_queued_t *)arg;
   session_t *live = m->live;
   uint32_t sid = live->session_id;
   int64_t parent = m->parent_conv;
   int user_id = m->user_id;

   /* Session began teardown after this marker was popped-to-spawn: don't start a
    * turn on it — clean up and let the queue drain. */
   if (atomic_load(&live->being_destroyed)) {
      reinvoke_turn_free(m);
      turn_queue_turn_done(sid);
      return NULL;
   }

   atomic_store(&live->stream_conversation_id, parent);
   session_begin_turn_flags(live);
   atomic_fetch_add(&live->turn_in_flight, 1);

   int64_t fired_ids[JOB_MONITOR_MAX_PER_TICK];
   int n_fired = 0;
   char *envelope = build_envelope(parent, user_id, fired_ids, &n_fired);

   /* build_envelope returns non-NULL only when it included ≥1 job, so n_fired > 0
    * is implied here — the explicit clause is belt-and-suspenders. */
   if (envelope != NULL && n_fired > 0) {
      job_persist_ctx_t pctx = { parent, user_id };
      session_set_tool_persist_hook(live, job_dispatch_tool_persist_cb, &pctx);
      /* Same tool-iteration seal job_worker/WebUI install: without it a multi-
       * iteration re-engagement on the viewer's session never flips
       * llm_streaming_active false between iterations, so every iteration's text
       * (incl. the final answer) collapses into the first bubble and the tool
       * entries pile below it.  A live viewer is a WEBUI session, so this renders. */
      session_set_tool_iteration_hook(live, webui_tool_iteration_cb, NULL);
      text_input_dispatch_opts_t opts = reinvoke_dispatch_opts(user_id);
      char *response = core_text_input_dispatch(live, envelope, NULL, NULL, NULL, 0, &opts);
      session_set_tool_iteration_hook(live, NULL, NULL);
      session_set_tool_persist_hook(live, NULL, NULL);

      /* C3: mark fired ONLY if the reply is actually saved somewhere.  A
       * foreground, connected viewer's client saves the streamed reply; if the
       * viewer switched conversations or the client vanished mid-turn, persist
       * server-side FIRST — never fire a re-engagement that exists nowhere. */
      bool cancelled = atomic_load(&live->cancel_requested);
      if (cancelled || response == NULL || response[0] == '\0') {
         OLOG_INFO("job_reinvoke: live re-engage of parent %lld empty/cancelled; will retry",
                   (long long)parent);
      } else {
         bool client_gone = atomic_load(&live->disconnected);
         bool backgrounded = (webui_session_active_conversation(live) != parent);
         bool persisted_ok = true;
         if (client_gone || backgrounded) {
            int64_t appended_id = 0;
            persisted_ok = (conv_db_add_message_with_tools(parent, user_id, "assistant", response,
                                                           NULL, NULL, NULL,
                                                           &appended_id) == AUTH_DB_SUCCESS);
            if (persisted_ok) {
               job_reinvoke_notify_conv_appended(user_id, parent);
               conv_event_notify_message_appended(parent, user_id, appended_id, "assistant",
                                                  response);
            }
         }
         if (persisted_ok) {
            /* LOAD-BEARING CONTRACT: in the "client-saved" case (foreground +
             * connected, so we did NOT persist server-side above) we mark the jobs
             * fired trusting the browser to save the foreground-streamed reply via
             * its conversation-tagged background save path.  If a future client
             * refactor breaks that save, a fired-but-unsaved re-engagement is lost
             * and never retries.  Keep the client save tied to this decision. */
            conv_db_job_mark_fired_many(fired_ids, n_fired);
            OLOG_INFO(
                "job_reinvoke: streamed re-engagement into live parent %lld (%d result(s), %s)",
                (long long)parent, n_fired,
                (client_gone || backgrounded) ? "persisted server-side" : "client-saved");
         } else {
            OLOG_WARNING(
                "job_reinvoke: re-engage of parent %lld persisted nowhere; leaving unfired",
                (long long)parent);
         }
      }
      free(response);
   }
   free(envelope);

   /* Exactly-once tail (run path). */
   atomic_fetch_sub(&live->turn_in_flight, 1);
   session_release(live);
   inflight_release(parent);
   job_manager_mark_dirty();
   free(m);
   turn_queue_turn_done(sid);
   return NULL;
}

/* spawn closure: start the turn worker for a dequeued reinvoke turn. */
static void reinvoke_turn_spawn(void *work) {
   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   int prc = pthread_create(&thread, &attr, reinvoke_turn_entry, work);
   pthread_attr_destroy(&attr);
   if (prc != 0) {
      /* Spawn failed: free this turn + advance the queue so it can't wedge. */
      reinvoke_queued_t *m = (reinvoke_queued_t *)work;
      uint32_t sid = (m != NULL && m->live != NULL) ? m->live->session_id : 0;
      OLOG_ERROR("job_reinvoke: failed to spawn queued reinvoke turn; dropping");
      reinvoke_turn_free(m);
      turn_queue_turn_done(sid);
   }
}

/* DETACHED path: no live viewer — run on a throwaway job-pool session bound to
 * the parent conv, persist the reply server-side, and nudge any open (not
 * currently-viewing) tab to refresh. */
static void reinvoke_run_detached(reinvoke_work_t *w,
                                  session_t *s,
                                  char *envelope,
                                  const int64_t *fired_ids,
                                  int n_fired) {
   /* Hydrate the parent conversation's history so the model reacts in context. */
   size_t restored = 0;
   struct json_object *hist = memory_history_load_from_db(w->parent_conv, w->user_id, &restored);
   if (hist != NULL) {
      pthread_mutex_lock(&s->history_mutex);
      if (s->conversation_history) {
         json_object_put(s->conversation_history);
      }
      s->conversation_history = hist;
      pthread_mutex_unlock(&s->history_mutex);
   }

   job_persist_ctx_t pctx = { w->parent_conv, w->user_id };
   session_set_tool_persist_hook(s, job_dispatch_tool_persist_cb, &pctx);
   /* Iteration seal (see live path): a no-op with no viewer, but correct if the
    * parent conv is opened mid-turn and gets live events, and consistent with
    * job_worker. */
   session_set_tool_iteration_hook(s, webui_tool_iteration_cb, NULL);
   text_input_dispatch_opts_t opts = reinvoke_dispatch_opts(w->user_id);
   char *response = core_text_input_dispatch(s, envelope, NULL, NULL, NULL, 0, &opts);
   session_set_tool_iteration_hook(s, NULL, NULL);
   session_set_tool_persist_hook(s, NULL, NULL);

   bool cancelled = atomic_load(&s->cancel_requested);
   if (!cancelled && response != NULL && response[0] != '\0') {
      /* Mark fired ONLY once the reply is durably stored (invariant C3).  This
       * path is detached: there is no client to client-save the reply, so this
       * insert is the only durable store.  Firing on a failed persist would
       * permanently suppress the follow-up AND lose the output — the exact
       * silent-loss shape the invariant exists to prevent.  Leaving the rows
       * unfired lets the monitor retry on a later tick; retries stay bounded
       * because build_envelope() bumps each job's reinvoke count per attempt and
       * force-fires past max_reinvokes_per_tree. */
      int64_t appended_id = 0;
      if (conv_db_add_message_with_tools(w->parent_conv, w->user_id, "assistant", response, NULL,
                                         NULL, NULL, &appended_id) == AUTH_DB_SUCCESS) {
         conv_db_job_mark_fired_many(fired_ids, n_fired);
         job_reinvoke_notify_conv_appended(w->user_id, w->parent_conv);
         conv_event_notify_message_appended(w->parent_conv, w->user_id, appended_id, "assistant",
                                            response);
         OLOG_INFO("job_reinvoke: re-engaged parent %lld (detached) with %d job result(s)",
                   (long long)w->parent_conv, n_fired);
      } else {
         OLOG_ERROR("job_reinvoke: failed to persist re-engagement reply to parent %lld; "
                    "leaving %d job(s) unfired so the monitor retries",
                    (long long)w->parent_conv, n_fired);
      }
   } else {
      OLOG_WARNING("job_reinvoke: detached re-engage of parent %lld empty/cancelled; retrying",
                   (long long)w->parent_conv);
   }
   free(response);
   job_manager_end(s);
}

static void reinvoke_run(reinvoke_work_t *w) {
   /* Prefer streaming into the user's live session — but via its TURN QUEUE, which
    * serializes the re-engagement behind any user turn (the sole gate).  The
    * envelope is built at DEQUEUE (in the closure), never here, so a turn later
    * purged never bumps the reinvoke ceiling or fires a notification (arch-H1). */
   session_t *live = webui_find_reinvoke_viewer(w->parent_conv, w->user_id);
   if (live != NULL) {
      reinvoke_queued_t *m = calloc(1, sizeof(*m));
      if (m != NULL) {
         m->parent_conv = w->parent_conv;
         m->user_id = w->user_id;
         m->live = live;
         int qrc = turn_queue_enqueue(live->session_id, TURN_SOURCE_BACKGROUND, m,
                                      reinvoke_turn_spawn, reinvoke_turn_free);
         if (qrc == TURN_QUEUE_OK) {
            /* The queue owns the marker + the retained viewer + the s_inflight
             * claim now; the closure (run or free) performs the exactly-once
             * tail.  Do NOT release/inflight_release/mark_dirty here. */
            return;
         }
         free(m); /* FULL / closing / FAIL: reclaim the marker, fall back to detached */
      }
      session_release(live);
      live = NULL;
   }

   /* Detached path: no connected viewer (or the live queue rejected us) — run on a
    * throwaway job-pool session bound to the parent conv and persist server-side. */
   session_t *det = NULL;
   int rc = job_manager_begin_ex(w->user_id, w->parent_conv, job_provider_from_default(),
                                 /*count_against_user_cap=*/false, &det);
   if (rc != JOB_MGR_OK) {
      OLOG_INFO("job_reinvoke: parent %lld re-engage deferred (rc=%d); will retry",
                (long long)w->parent_conv, rc);
      inflight_release(w->parent_conv);
      job_manager_mark_dirty();
      return;
   }

   int64_t fired_ids[JOB_MONITOR_MAX_PER_TICK];
   int n_fired = 0;
   char *envelope = build_envelope(w->parent_conv, w->user_id, fired_ids, &n_fired);
   if (envelope == NULL || n_fired == 0) {
      /* All jobs force-fired/notified, or nothing left.  Undo the claim and retry. */
      job_manager_end(det);
      free(envelope);
      inflight_release(w->parent_conv);
      job_manager_mark_dirty();
      return;
   }

   reinvoke_run_detached(w, det, envelope, fired_ids, n_fired);
   free(envelope);
   inflight_release(w->parent_conv);
   job_manager_mark_dirty(); /* new completions may have arrived while running */
}

static void *reinvoke_thread(void *arg) {
   reinvoke_work_t *w = (reinvoke_work_t *)arg;
   reinvoke_run(w);
   free(w);
   return NULL;
}

/* Spawn a detached decision worker for one parent.  The worker re-queries the
 * parent's pending jobs at dispatch time, so no coalesced item list is threaded
 * through.  On failure the caller's claim is released and the rows are left
 * unfired for a later tick. */
static bool reinvoke_spawn(int64_t parent_conv, int user_id) {
   reinvoke_work_t *w = calloc(1, sizeof(*w));
   if (w == NULL) {
      return false;
   }
   w->parent_conv = parent_conv;
   w->user_id = user_id;

   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   int prc = pthread_create(&thread, &attr, reinvoke_thread, w);
   pthread_attr_destroy(&attr);
   if (prc != 0) {
      OLOG_ERROR("job_reinvoke: pthread_create failed (%d) for parent %lld", prc,
                 (long long)parent_conv);
      free(w);
      return false;
   }
   return true;
}

/* =============================================================================
 * Processor (called by the completion monitor, main-loop thread)
 * ============================================================================= */

void job_reinvoke_process_pending(const job_record_t *rows, int n) {
   if (rows == NULL || n <= 0) {
      return;
   }
   int cap = g_config.jobs.max_concurrent_reinvokes;
   const time_t now = time(NULL);
   bool deferred = false;

   if (n > JOB_MONITOR_MAX_PER_TICK) {
      n = JOB_MONITOR_MAX_PER_TICK; /* defensive: buffers below are sized to this */
   }

   /* Reduce rows to DISTINCT parents (n is small, bounded by the per-tick cap):
    * one re-engagement per parent, which re-queries the parent's full pending set
    * at dispatch time — so a parent's several completions coalesce without
    * threading an item list through the worker. */
   bool done[JOB_MONITOR_MAX_PER_TICK];
   memset(done, 0, sizeof(done));

   for (int i = 0; i < n; i++) {
      if (done[i]) {
         continue;
      }
      int64_t parent = rows[i].parent_id;
      int uid = rows[i].user_id;
      done[i] = true;
      if (parent <= 0) {
         /* Rootless.  The spawn path falls back to 'notify' when there is no
          * parent, so this looks unreachable — but `conversations.parent_id` is
          * ON DELETE SET NULL (v72), so DELETING the parent conversation orphans
          * its finished children AFTER the fact.
          *
          * Skipping without firing used to strand the row: the monitor routes
          * every reinvoke_parent row here BEFORE the mark-fired branch, so
          * on_complete_fired stayed 0 forever.  conv_db_job_list_pending_followups
          * is GLOBAL (no user filter) and ordered finished_at ASC, so a handful of
          * orphans sort first for good and consume the whole per-tick budget —
          * permanently starving completion notifications for EVERY user, and
          * pinning the dirty-gated monitor on so it scans the DB every second.
          * Fire it and tell the owner instead; there is no parent left to
          * re-engage, so a notification is the correct degraded delivery. */
         conv_db_job_mark_fired(rows[i].id);
         reinvoke_notify_fallback(uid, rows[i].title, rows[i].id);
         continue;
      }

      /* Mark this parent's other rows done so it is processed once this tick. */
      for (int j = i + 1; j < n; j++) {
         if (!done[j] && rows[j].parent_id == parent && rows[j].user_id == uid) {
            done[j] = true;
         }
      }

      /* Defer if a live interactive session is mid-turn on this parent — a cheap
       * early skip that avoids spawning a worker that would just queue behind the
       * active turn.  No longer the SAFETY mechanism (the turn queue serializes),
       * so a race-missed just-dequeued turn is harmless.  Rows stay unfired →
       * retried when the turn ends.  Same for the concurrency cap below. */
      bool defer_now = session_manager_conv_has_turn_in_flight(parent);
      claim_result_t claim = CLAIM_OK;
      if (!defer_now) {
         claim = inflight_try_claim(parent, cap);
         defer_now = (claim != CLAIM_OK); /* at cap, or a worker already owns it */
      }

      if (defer_now) {
         /* CLAIM_ALREADY_INFLIGHT is a worker actively re-engaging this parent —
          * progress, not a wedge — and its rows stay unfired for the whole turn,
          * so they re-appear on EVERY tick meanwhile.  Advancing the clock on
          * those would trip the bound mid-turn and fire "job finished — ask me
          * for the result" while the worker is delivering those very results:
          * the user gets both. */
         if (claim == CLAIM_ALREADY_INFLIGHT) {
            deferred = true;
            continue;
         }
         if (!defer_note(parent, now)) {
            deferred = true;
            continue;
         }
         /* Deferring past the bound: this parent is not coming back on its own,
          * and its rows are holding the head of a GLOBAL queue.  Deliver them as
          * plain notifications so the user still learns the work finished, and
          * let the queue move.  The result stays retrievable either way. */
         OLOG_WARNING("job_reinvoke: parent %lld deferred > %ds — notifying instead of re-engaging",
                      (long long)parent, JOB_REINVOKE_MAX_DEFER_SEC);
         for (int j = i; j < n; j++) {
            if (rows[j].parent_id == parent && rows[j].user_id == uid) {
               conv_db_job_mark_fired(rows[j].id);
               reinvoke_notify_fallback(uid, rows[j].title, rows[j].id);
            }
         }
         continue;
      }

      if (!reinvoke_spawn(parent, uid)) {
         inflight_release(parent);
         deferred = true;
         defer_note(parent, now);
      } else {
         defer_forget(parent); /* it got its worker */
      }
   }

   if (deferred) {
      job_manager_mark_dirty(); /* re-arm so deferred parents are retried */
   }
}

void job_reinvoke_init(void) {
   job_manager_register_reinvoke_processor(job_reinvoke_process_pending);
   OLOG_INFO("job_reinvoke: reinvoke_parent processor registered");
}

/* Weak default: no-op unless the WebUI provides the strong override. */
__attribute__((weak)) void job_reinvoke_notify_conv_appended(int user_id, int64_t conversation_id) {
   (void)user_id;
   (void)conversation_id;
}

/* Weak default: no live viewer without WebUI → always take the detached path. */
__attribute__((weak)) session_t *webui_find_reinvoke_viewer(int64_t conv_id, int user_id) {
   (void)conv_id;
   (void)user_id;
   return NULL;
}

/* Weak default: 0 (no client view) without WebUI — the reinvoke closure then
 * always persists server-side, which is the safe choice for a headless build. */
__attribute__((weak)) int64_t webui_session_active_conversation(session_t *s) {
   (void)s;
   return 0;
}
