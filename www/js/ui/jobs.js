/*
 * Background-jobs PANEL — the observe surface for spawned jobs.
 *
 * Reads two disjoint sets, deliberately rendered as two sections, because they
 * arrive by different contracts (see src/webui/webui_jobs.c — "the lifetime
 * split"):
 *
 *   ACTIVE   the complete running/queued set, held whole by DawnJobsActivity.
 *            Structurally bounded, so it is grouped by parent conversation —
 *            the fan-out case ("this chat spawned four jobs") is exactly what
 *            grouping is for, and the set arriving whole is what makes a group
 *            count trustworthy.
 *   HISTORY  terminal jobs, keyset-paginated via list_jobs. NOT grouped:
 *            a group whose members straddle a page boundary would render as
 *            two groups with wrong counts, so history stays a flat list.
 *
 * This module owns no job state of its own. The active set is read from
 * DawnJobsActivity on each tick (its rows are that store's own objects — read
 * only, never mutate), and history is the accumulation of pages this panel
 * requested. Nothing here derives a count that the server could have sent.
 *
 * XSS: every string on a job row is server-supplied and `title` is LLM-authored
 * from the spawn goal, so it is prompt-injectable. The server UTF-8-sanitizes
 * but deliberately does NOT HTML-escape (escaping is the renderer's job). This
 * file therefore builds rows with createElement/textContent throughout and
 * never assigns innerHTML — structurally XSS-proof rather than dependent on
 * remembering to escape at each of a dozen interpolation sites. (§8.7 mandates
 * DawnFormat.escapeHtml; textContent is the stronger form of the same
 * obligation, so the deviation is deliberate.)
 *
 * NOT here, deliberately: `handleConversationEvent` / `handleConversationEvents`.
 * dawn.js already routes both frames to this namespace and they fall through as
 * no-ops until CP4b implements the transcript-side `.agent-event.*` rendering
 * and switches the browser from load_conversation to attach_conversation. The
 * absence is a checkpoint boundary, not an oversight.
 */
(function (global) {
   'use strict';

   /* Terminal jobs fetched per page. The server clamps its own maximum; this is
    * just what a click of "Load more" is worth. */
   const HISTORY_PAGE = 25;

   /* Re-render cadence for elapsed times while the panel is open. */
   const TICK_MS = 1000;

   /* How long to wait for a list_jobs_response before giving up on the page.
    * Not paranoia: webui_jobs_send_history() logs and returns WITHOUT sending a
    * frame when its DB read fails, so a client that assumed an answer always
    * arrives would sit on "Loading…" forever AND refuse every later request,
    * including the refresh after a cancel or resume. */
   const HISTORY_TIMEOUT_MS = 15000;

   /* Same reasoning for job_action. `webui_jobs_handle_action` promises that
    * every exit answers, but its entry guard (unauthenticated / satellite /
    * no user id) returns silently, and queue_response drops the oldest frame
    * under pressure. Without this, one lost response leaves that job's Cancel
    * and Resume disabled for the life of the page — buildActionButton re-reads
    * pendingActions on every rebuild, so even reopening the panel won't clear
    * it. */
   const ACTION_TIMEOUT_MS = 15000;

   let el = {};
   let isOpen = false;
   let triggerElement = null;
   let focusTrapCleanup = null;
   let escToken = null;
   let tickTimer = null;

   /* Signature of the active set as last rendered. A full re-render every
    * second would destroy button focus and defeat :hover, so the tick only
    * rebuilds when set membership or a status actually changed; otherwise it
    * patches the elapsed text nodes in place. */
   let activeSig = '';

   /* Accumulated terminal rows + the keyset cursor for the next page. */
   let historyRows = [];
   let historyCursor = null;
   let historyHasMore = false;
   let historyLoading = false;
   let historyTimer = null;
   let historyError = '';

   /* conversation_id -> { action, timer } for an in-flight action, so a button
    * can't be double-fired while its job_action_response is outstanding. */
   let pendingActions = Object.create(null);

   /* Last auth state seen. Gates open() itself rather than only the header
    * button, because the activity pill is a SECOND trigger whose visibility is
    * owned by jobs-activity.js and is a pure function of the job count — never
    * of auth. One gate at the entry point covers both. */
   let authOk = false;

   const callbacks = { trapFocus: null };

   /* ---- status vocabulary ------------------------------------------------ */

   /* The server's live states. Anything else is terminal — matching
    * DawnJobsActivity, so an unrecognized future status can't be rendered as
    * though it were still running. */
   function isActiveStatus(status) {
      return status === 'running' || status === 'queued';
   }

   /* Mirrors conv_db_job_reset_for_resume's WHERE clause for the BY_USER case —
    * this panel's Resume goes through `job_action`, which is the human path, so
    * 'cancelled' is resumable here even though the `job` tool refuses it. Keep
    * in sync: a button offered for a status the server rejects just earns a
    * refusal toast. */
   function isResumable(status) {
      return status === 'interrupted' || status === 'failed' || status === 'cancelled';
   }

   /* The narrower set — a job that stopped AGAINST the user's wishes. Cancelling
    * also leaves the follow-up unfired, but that was the point, so flagging it
    * there would be noise. */
   function stoppedUnintentionally(status) {
      return status === 'interrupted' || status === 'failed';
   }

   /* Maps to the shared .dawn-status-dot / .dawn-badge tone classes. */
   function statusTone(status) {
      switch (status) {
         case 'done':
            return 'success';
         case 'running':
            return 'warning';
         case 'failed':
         case 'interrupted':
            return 'error';
         default:
            /* queued, cancelled, and anything unrecognized */
            return 'muted';
      }
   }

   /* ---- time ------------------------------------------------------------- */

   function formatDuration(totalSec) {
      const s = Math.max(0, Math.floor(totalSec));
      if (s < 60) {
         return s + 's';
      }
      const m = Math.floor(s / 60);
      if (m < 60) {
         return m + 'm ' + (s % 60) + 's';
      }
      const h = Math.floor(m / 60);
      return h + 'h ' + (m % 60) + 'm';
   }

   /* A FINISHED job's duration is pure server arithmetic (finished_at -
    * started_at) and so is immune to browser clock skew. Only a RUNNING job
    * needs "now", where a skewed client clock shows a skewed elapsed; it is
    * cosmetic and self-corrects the moment the job finishes. Clamped at 0 so a
    * slightly-behind clock reads "0s" rather than a negative duration. */
   function jobElapsedText(job) {
      if (!job.started_at) {
         return job.status === 'queued' ? 'waiting' : '';
      }
      if (job.finished_at && job.finished_at >= job.started_at) {
         return formatDuration(job.finished_at - job.started_at);
      }
      if (isActiveStatus(job.status)) {
         return formatDuration(Date.now() / 1000 - job.started_at);
      }
      return '';
   }

   /* ---- small DOM helpers ------------------------------------------------ */

   function elem(tag, className, text) {
      const n = document.createElement(tag);
      if (className) {
         n.className = className;
      }
      if (text != null) {
         n.textContent = String(text);
      }
      return n;
   }

   function toast(message, kind) {
      if (typeof DawnToast !== 'undefined') {
         DawnToast.show(message, kind || 'info');
      }
   }

   function wsReady() {
      return typeof DawnWS !== 'undefined' && DawnWS.isConnected();
   }

   /* ---- row construction ------------------------------------------------- */

   /* Opens a conversation and dismisses the panel. Used for both a job's own
    * conversation and its parent — the only difference is which id. */
   function buildOpenButton(convId, label, ariaLabel, attach) {
      const btn = elem('button', 'jobs-open-btn', label);
      btn.type = 'button';
      btn.setAttribute('aria-label', ariaLabel);
      /* Keyed so focus survives an active-list rebuild (captureFocus/restoreFocus),
       * same as the Cancel/Resume buttons — else a keyboard user on View/Open loses
       * focus to <body> when a job transitions mid-interaction. */
      btn.dataset.jobId = String(convId);
      btn.dataset.action = attach ? 'view' : 'open';
      btn.addEventListener('click', function () {
         if (typeof DawnHistory !== 'undefined' && DawnHistory.loadConversation) {
            // A job's own conversation attaches (durable events → completion
            // marker); the parent is a normal conversation.
            DawnHistory.loadConversation(convId, attach ? { attach: true } : undefined);
            close();
         }
      });
      return btn;
   }

   function buildActionButton(job, action, label) {
      const btn = elem('button', 'jobs-action-btn ' + action, label);
      btn.type = 'button';
      /* The accessible name has to carry WHICH job, since a list of identical
       * "Cancel" buttons is unusable by screen reader. */
      btn.setAttribute('aria-label', label + ' background job: ' + (job.title || 'untitled'));
      btn.dataset.jobId = String(job.conversation_id);
      btn.dataset.action = action;
      if (pendingActions[String(job.conversation_id)]) {
         btn.disabled = true;
      }
      btn.addEventListener('click', function () {
         /* Cancel discards an LLM run that may be many minutes deep and cannot
          * be undone (Resume restarts from the goal, not from where it died),
          * so it takes the house confirm the other destructive actions use. */
         if (action !== 'cancel') {
            sendAction(job.conversation_id, action);
            return;
         }
         if (typeof DawnDialog === 'undefined' || !DawnDialog.confirm) {
            sendAction(job.conversation_id, action);
            return;
         }
         /* The goal goes in `detail`, which the modal renders via textContent —
          * the same non-parsing sink this panel uses everywhere else. */
         DawnDialog.confirm('Stop this background job?', {
            title: 'Cancel background job',
            detail: job.title || 'Untitled job',
            okText: 'Stop job',
            cancelText: 'Keep running',
            danger: true,
         }).then(function (ok) {
            if (ok) {
               sendAction(job.conversation_id, action);
            }
         });
      });
      return btn;
   }

   /* Deliberately NOT indented by spawn_depth. Every job is depth 1 today
    * (job->job spawning stays blocked until the Phase-3 caps are enforced), and
    * an indent alone would not produce a tree even once depth arrives: rows are
    * ordered created_at DESC, so a child — created after its parent — sorts
    * ABOVE it, and indenting that renders a child hanging over its own parent.
    * Trees need an ordering change, not a padding rule; Phase 3 owes both. */
   function buildRow(job) {
      const row = elem('div', 'jobs-row');
      row.dataset.jobId = String(job.conversation_id);

      row.appendChild(elem('span', 'dawn-status-dot ' + statusTone(job.status)));

      const main = elem('div', 'jobs-row-main');
      const title = elem('div', 'jobs-row-title', job.title || 'Untitled job');
      /* The title is LLM-authored and can carry bidi overrides (U+202E), which
       * survive the server's UTF-8 sanitize. dir="auto" isolates it so it can
       * only reorder itself, not the surrounding row. */
      title.setAttribute('dir', 'auto');
      main.appendChild(title);

      const meta = elem('div', 'jobs-row-meta');
      meta.appendChild(elem('span', 'dawn-badge ' + statusTone(job.status), job.status));

      const elapsed = elem('span', 'jobs-elapsed', jobElapsedText(job));
      meta.appendChild(elapsed);

      if (job.deliver_to) {
         meta.appendChild(elem('span', 'dawn-badge muted', 'to ' + job.deliver_to));
      }
      /* Says WHY Resume is worth clicking. Scoped to the resumable statuses:
       * a cancelled job also has no follow-up, but that is the outcome the user
       * asked for, so flagging it there would be noise. */
      if (
         stoppedUnintentionally(job.status) &&
         job.on_complete === 'reinvoke_parent' &&
         !job.on_complete_fired
      ) {
         meta.appendChild(elem('span', 'dawn-badge warning soft', 'no follow-up'));
      }
      main.appendChild(meta);

      if (job.error) {
         const err = elem('div', 'jobs-row-error', job.error);
         err.setAttribute('dir', 'auto');
         main.appendChild(err);
      }
      row.appendChild(main);

      const actions = elem('div', 'jobs-row-actions');
      /* A job IS a conversation, and job conversations are hidden from the
       * sidebar — so this panel is the only surface that knows the id, and
       * without this button the job's own transcript and final answer are
       * unreachable from the UI. */
      actions.appendChild(
         buildOpenButton(
            job.conversation_id,
            'View',
            'View background job: ' + (job.title || 'untitled'),
            true /* attach — this is the job's own conversation */
         )
      );
      if (isActiveStatus(job.status)) {
         actions.appendChild(buildActionButton(job, 'cancel', 'Cancel'));
      } else if (isResumable(job.status)) {
         actions.appendChild(buildActionButton(job, 'resume', 'Resume'));
      }
      row.appendChild(actions);
      return row;
   }

   /* ---- active section --------------------------------------------------- */

   function getActiveJobs() {
      if (typeof DawnJobsActivity === 'undefined') {
         return [];
      }
      /* The array is a copy but the ROWS are the store's own objects. Sorting
       * the copy is fine; mutating a row would desync the store's derived
       * counts from what this panel draws. */
      return DawnJobsActivity.getActive()
         .slice()
         .sort(function (a, b) {
            return (b.created_at || 0) - (a.created_at || 0);
         });
   }

   /* Membership + status + the fields a row renders. Anything that changes the
    * DOM structure must appear here or the tick will not notice it. */
   function activeSignature(jobs) {
      return jobs
         .map(function (j) {
            /* Every field buildRow renders, not just the volatile ones. All of
             * these are in practice fixed for the life of an active job, so
             * this changes nothing today — it makes the invariant above TRUE,
             * so a field added to buildRow later cannot silently fail to
             * repaint. ~40 bytes per job, once per second, over <=256 rows. */
            return [
               j.conversation_id,
               j.status,
               j.started_at || 0,
               j.parent_id || 0,
               j.title || '',
               j.deliver_to || '',
               j.error || '',
               j.on_complete || '',
               j.on_complete_fired ? 1 : 0,
            ].join(':');
         })
         .join('|');
   }

   function buildGroupHeader(parentId, count) {
      const head = elem('div', 'jobs-group-head');
      if (parentId) {
         head.appendChild(elem('span', 'jobs-group-label', 'Conversation #' + parentId));
         const open = buildOpenButton(parentId, 'Open', 'Open parent conversation ' + parentId);
         open.className = 'jobs-group-open';
         head.appendChild(open);
      } else {
         /* parent_id 0 — spawned with no parent conversation (a scheduled or
          * tool-initiated job). Not an error; it just has nothing to open. */
         head.appendChild(elem('span', 'jobs-group-label', 'No parent conversation'));
      }
      head.appendChild(elem('span', 'jobs-group-count', count + (count === 1 ? ' job' : ' jobs')));
      return head;
   }

   /* A tick can rebuild the active list under a keyboard user's cursor when a
    * job transitions. Re-find the same control afterwards so focus lands where
    * it was instead of collapsing to <body> mid-interaction. */
   function captureFocus() {
      const a = document.activeElement;
      if (!a || !a.dataset || !a.dataset.jobId || !a.dataset.action) {
         return null;
      }
      return { jobId: a.dataset.jobId, action: a.dataset.action };
   }

   function restoreFocus(mark) {
      if (!mark || !el.activeList) {
         return;
      }
      const nodes = el.activeList.querySelectorAll('.jobs-action-btn, .jobs-open-btn');
      for (let i = 0; i < nodes.length; i++) {
         if (nodes[i].dataset.jobId === mark.jobId && nodes[i].dataset.action === mark.action) {
            nodes[i].focus();
            return;
         }
      }
   }

   function renderActive(preFetched, preComputedSig) {
      if (!el.activeList) {
         return;
      }
      const jobs = preFetched || getActiveJobs();
      const focusMark = captureFocus();
      activeSig = preComputedSig || activeSignature(jobs);
      el.activeList.textContent = '';

      if (el.activeCount) {
         el.activeCount.textContent = String(jobs.length);
      }
      if (jobs.length === 0) {
         el.activeList.appendChild(elem('div', 'jobs-empty', 'No background jobs running.'));
         return;
      }

      /* Group by parent, preserving the created_at ordering of first appearance
       * so the most recently active conversation leads. */
      const order = [];
      const groups = Object.create(null);
      jobs.forEach(function (job) {
         const key = String(job.parent_id || 0);
         if (!groups[key]) {
            groups[key] = [];
            order.push(key);
         }
         groups[key].push(job);
      });

      order.forEach(function (key) {
         const rows = groups[key];
         const group = elem('div', 'jobs-group');
         group.appendChild(buildGroupHeader(Number(key), rows.length));
         rows.forEach(function (job) {
            group.appendChild(buildRow(job));
         });
         el.activeList.appendChild(group);
      });
      restoreFocus(focusMark);
   }

   /* Patch elapsed text without rebuilding rows, so focus and hover survive. */
   function patchElapsed(jobs) {
      const byId = Object.create(null);
      jobs.forEach(function (j) {
         byId[String(j.conversation_id)] = j;
      });
      const rows = el.activeList ? el.activeList.querySelectorAll('.jobs-row') : [];
      for (let i = 0; i < rows.length; i++) {
         const job = byId[rows[i].dataset.jobId];
         if (!job) {
            continue;
         }
         const node = rows[i].querySelector('.jobs-elapsed');
         if (node) {
            node.textContent = jobElapsedText(job);
         }
      }
   }

   function tick() {
      if (!isOpen) {
         return;
      }
      /* Read the set once and hand it (and its signature) to whichever path
       * runs — re-deriving either would repeat the work every second for no
       * gain. */
      const jobs = getActiveJobs();
      const sig = activeSignature(jobs);
      if (sig !== activeSig) {
         renderActive(jobs, sig);
      } else {
         patchElapsed(jobs);
      }
   }

   function startTicker() {
      if (tickTimer === null) {
         tickTimer = global.setInterval(tick, TICK_MS);
      }
   }

   function stopTicker() {
      if (tickTimer !== null) {
         global.clearInterval(tickTimer);
         tickTimer = null;
      }
   }

   /* ---- history section -------------------------------------------------- */

   function renderHistory() {
      if (!el.historyList) {
         return;
      }
      el.historyList.textContent = '';
      if (historyRows.length === 0 && historyLoading) {
         el.historyList.appendChild(elem('div', 'jobs-empty', 'Loading…'));
      } else if (historyRows.length === 0 && !historyError) {
         el.historyList.appendChild(elem('div', 'jobs-empty', 'No finished jobs yet.'));
      } else {
         historyRows.forEach(function (job) {
            el.historyList.appendChild(buildRow(job));
         });
      }
      /* An error gets its own colour and a Retry, so a failed load can never be
       * mistaken for an empty list — they render in the same slot otherwise. */
      if (historyError && !historyLoading) {
         const box = elem('div', 'jobs-empty is-error', historyError);
         const retry = elem('button', 'jobs-retry', 'Retry');
         retry.type = 'button';
         retry.addEventListener('click', function () {
            requestHistory(historyRows.length === 0);
         });
         box.appendChild(retry);
         el.historyList.appendChild(box);
      }
      if (el.historyCount) {
         /* "N shown", not "N total": history is paginated with no total, and a
          * bare count next to a "Load more" would read as the whole set. */
         el.historyCount.textContent = historyRows.length
            ? historyRows.length + ' shown' + (historyHasMore ? '+' : '')
            : '';
      }
      if (el.moreBtn) {
         el.moreBtn.classList.toggle('hidden', !historyHasMore);
         el.moreBtn.disabled = historyLoading;
      }
   }

   function clearHistoryTimer() {
      if (historyTimer !== null) {
         global.clearTimeout(historyTimer);
         historyTimer = null;
      }
   }

   function requestHistory(reset) {
      if (historyLoading) {
         return;
      }
      /* The reset happens BEFORE the connectivity check: bailing out early
       * would leave the previous session's rows on screen with nothing marking
       * them stale — the failure the `truncated` flag and the error string
       * exist to prevent. */
      if (reset) {
         historyRows = [];
         historyCursor = null;
         historyHasMore = false;
         historyError = '';
      }
      if (!wsReady()) {
         historyError = 'Not connected.';
         renderHistory();
         return;
      }
      historyLoading = true;
      historyError = '';
      const payload = { limit: HISTORY_PAGE };
      if (historyCursor) {
         payload.before_created_at = historyCursor.before_created_at;
         payload.before_id = historyCursor.before_id;
      }
      DawnWS.send({ type: 'list_jobs', payload: payload });
      clearHistoryTimer();
      historyTimer = global.setTimeout(function () {
         historyTimer = null;
         if (!historyLoading) {
            return;
         }
         historyLoading = false;
         /* Say the list is incomplete rather than letting a partial or empty
          * list read as authoritative — the same reason the snapshot carries a
          * `truncated` flag. */
         historyError = "Couldn't load finished jobs. Reopen to retry.";
         renderHistory();
      }, HISTORY_TIMEOUT_MS);
      renderHistory();
   }

   function handleHistoryPage(payload) {
      /* Only one history request is ever in flight (requestHistory guards on
       * historyLoading), so a frame arriving when nothing is loading is a late
       * answer to a request that already timed out or was abandoned at close.
       * Applying it would cancel the CURRENT request's watchdog and overwrite
       * the cursor with a stale one. Frames carry no request id, so this is as
       * precisely as the client can correlate them. */
      if (!historyLoading) {
         return;
      }
      clearHistoryTimer();
      historyLoading = false;
      historyError = '';
      if (!payload) {
         renderHistory();
         return;
      }
      const rows = Array.isArray(payload.jobs) ? payload.jobs : [];
      /* Keyset pagination cannot repeat a row, but a reset racing an in-flight
       * page can, so dedupe on conversation_id rather than trusting arrival
       * order. */
      const seen = Object.create(null);
      historyRows.forEach(function (j) {
         seen[String(j.conversation_id)] = true;
      });
      rows.forEach(function (j) {
         if (j && j.conversation_id != null && !seen[String(j.conversation_id)]) {
            seen[String(j.conversation_id)] = true;
            historyRows.push(j);
         }
      });
      historyHasMore = !!payload.has_more;
      historyCursor =
         payload.next_before_id != null
            ? {
                 before_created_at: payload.next_before_created_at || 0,
                 before_id: payload.next_before_id,
              }
            : null;
      /* has_more with no cursor would loop the "Load more" button forever on
       * the same page; treat a missing cursor as the end of the list. */
      if (!historyCursor) {
         historyHasMore = false;
      }
      renderHistory();
   }

   /* ---- actions ---------------------------------------------------------- */

   /* Compares dataset values rather than building an attribute selector, so a
    * job id is never interpolated into a CSS selector string. */
   function setRowButtonsDisabled(convId, disabled) {
      if (!el.popover) {
         return;
      }
      const key = String(convId);
      const nodes = el.popover.querySelectorAll('.jobs-action-btn');
      for (let i = 0; i < nodes.length; i++) {
         if (nodes[i].dataset.jobId === key) {
            nodes[i].disabled = disabled;
         }
      }
   }

   function clearPending(key) {
      const p = pendingActions[key];
      if (!p) {
         return;
      }
      if (p.timer !== null) {
         global.clearTimeout(p.timer);
      }
      delete pendingActions[key];
   }

   function clearAllPending() {
      Object.keys(pendingActions).forEach(clearPending);
   }

   function sendAction(convId, action) {
      if (!wsReady()) {
         toast('Not connected.', 'error');
         return;
      }
      const key = String(convId);
      if (pendingActions[key]) {
         return;
      }
      pendingActions[key] = {
         action: action,
         timer: global.setTimeout(function () {
            if (!pendingActions[key]) {
               return;
            }
            pendingActions[key].timer = null;
            clearPending(key);
            setRowButtonsDisabled(convId, false);
            toast('No answer for that ' + action + '. Try again.', 'error');
         }, ACTION_TIMEOUT_MS),
      };
      setRowButtonsDisabled(convId, true);
      DawnWS.send({
         type: 'job_action',
         payload: { action: action, conversation_id: convId },
      });
   }

   function handleActionResult(payload) {
      if (!payload) {
         return;
      }
      const key = String(payload.conversation_id);
      clearPending(key);
      setRowButtonsDisabled(payload.conversation_id, false);
      toast(
         payload.message || (payload.ok ? 'Done.' : 'That did not work.'),
         payload.ok ? 'success' : 'error'
      );
      if (!payload.ok) {
         return;
      }
      /* Both actions move a job ACROSS the active/history partition — cancel
       * pushes it out of the active set, resume pulls it back in — and only the
       * active side self-heals (its job_update arrives on its own). Refetch the
       * first history page so the row is not shown on both sides or neither. */
      requestHistory(true);
   }

   /* ---- open / close ----------------------------------------------------- */

   function open() {
      if (!el.popover || isOpen) {
         /* Re-entrancy matters: open() is exported, and a second call would
          * overwrite escToken and focusTrapCleanup, orphaning both. An orphaned
          * DawnEscStack handler is worse than a plain leak — escstack.js notes
          * that a stale handler returning true still CONSUMES the Escape and
          * blocks every layer beneath it. */
         return;
      }
      /* Both triggers land here, so this is the one place the auth gate has to
       * hold (the activity pill's visibility tracks the job count, not auth). */
      if (!authOk) {
         return;
      }
      /* Captured BEFORE the sibling popovers close: each of them restores focus
       * to its own trigger on the way out, so reading activeElement afterwards
       * would capture THEIR button and close() would return focus to the wrong
       * control. */
      triggerElement = document.activeElement;

      /* Close the competing popovers that share this header slot. */
      if (typeof DawnMemory !== 'undefined' && DawnMemory.close) DawnMemory.close();
      if (typeof DawnSchedulerQueue !== 'undefined' && DawnSchedulerQueue.close)
         DawnSchedulerQueue.close();
      if (typeof DawnDocLibrary !== 'undefined' && DawnDocLibrary.close) DawnDocLibrary.close();
      if (typeof DawnCodeProjects !== 'undefined' && DawnCodeProjects.close)
         DawnCodeProjects.close();

      el.popover.classList.remove('hidden');
      setExpanded(true);
      isOpen = true;
      if (typeof DawnEscStack !== 'undefined') {
         escToken = DawnEscStack.register(function () {
            close();
            return true;
         });
      }
      if (callbacks.trapFocus) {
         focusTrapCleanup = callbacks.trapFocus(el.popover);
      }
      renderActive();
      requestHistory(true);
      startTicker();
   }

   function close() {
      if (!el.popover) {
         return;
      }
      el.popover.classList.add('hidden');
      setExpanded(false);
      isOpen = false;
      stopTicker();
      /* The history request dies with the panel: leaving historyLoading set
       * would make the NEXT open() early-return without refreshing, and the
       * orphaned timer would then stamp "couldn't load" over a healthy panel. */
      clearHistoryTimer();
      historyLoading = false;
      if (escToken !== null && typeof DawnEscStack !== 'undefined') {
         DawnEscStack.unregister(escToken);
      }
      escToken = null;
      if (focusTrapCleanup) {
         focusTrapCleanup();
         focusTrapCleanup = null;
      }
      restoreTriggerFocus();
   }

   /* Every trigger advertises the panel's state — the header button plus each
    * pill (global and per-conversation). */
   function setExpanded(expanded) {
      const v = expanded ? 'true' : 'false';
      if (el.btn) {
         el.btn.classList.toggle('active', expanded);
         el.btn.setAttribute('aria-expanded', v);
      }
      /* Re-queried rather than cached: the per-conversation pill is rendered
       * into every .conv-jobs-pill slot, and how many of those exist depends on
       * which chrome is mounted. */
      document.querySelectorAll('.jobs-pill').forEach(function (p) {
         p.setAttribute('aria-expanded', v);
      });
   }

   /* The pill is the common way in — and it vanishes the moment the running
    * count hits zero, which is exactly what the user opened the panel to watch.
    * Focusing a display:none element is a silent no-op that drops the caret to
    * <body>, so fall back to the header button, which is always present while
    * authenticated. */
   function restoreTriggerFocus() {
      const target = pickFocusTarget();
      triggerElement = null;
      if (target && typeof target.focus === 'function') {
         target.focus();
      }
   }

   function pickFocusTarget() {
      /* Only move focus if it is currently INSIDE the panel. close() also runs
       * when a sibling popover opens over us — at which point focus belongs to
       * the control the user just clicked, and yanking it back to our own
       * trigger would fight them. Escape and the × both leave focus inside, so
       * the cases that need a restore still get one. */
      if (!el.popover || !el.popover.contains(document.activeElement)) {
         return null;
      }
      const t = triggerElement;
      /* The pill is the common way in, and it vanishes the moment the running
       * count hits zero — which is exactly what the user opened the panel to
       * watch. Focusing a display:none element is a silent no-op that drops the
       * caret to <body>, so fall back to the header button. */
      if (!t || typeof t.focus !== 'function' || !t.isConnected) {
         return el.btn;
      }
      if (t.getClientRects && t.getClientRects().length === 0) {
         return el.btn;
      }
      return t;
   }

   function toggle() {
      if (isOpen) {
         close();
      } else {
         open();
      }
   }

   /* The socket dropped and came back. Any job_action_response and any
    * list_jobs_response in flight died with it, so clear the state that was
    * waiting on them — otherwise the buttons of a job whose response was lost
    * stay disabled for the life of the page, and historyLoading blocks every
    * later request. DawnJobsActivity refreshes the ACTIVE set on its own. */
   function handleReconnect() {
      clearHistoryTimer();
      historyLoading = false;
      clearAllPending();
      if (isOpen) {
         renderActive();
         requestHistory(true);
      }
   }

   function updateVisibility(authState) {
      /* Falls back to the shared state like code-projects.js, so init() can
       * call this with no argument to self-heal if an auth event landed before
       * this module was wired. */
      const state = authState || (typeof DawnState !== 'undefined' ? DawnState.authState : null);
      authOk = !!(state && state.authenticated);
      if (el.btn) {
         el.btn.classList.toggle('hidden', !authOk);
      }
      if (!authOk) {
         if (isOpen) {
            close();
         }
         /* Drop the previous user's rows. Otherwise a de-auth that doesn't
          * navigate away (a reconnect whose token was revoked) leaves their job
          * titles cached in memory and rendered in the closed panel. */
         historyRows = [];
         historyCursor = null;
         historyHasMore = false;
         historyError = '';
         clearAllPending();
         renderHistory();
      }
   }

   function setCallbacks(cbs) {
      if (cbs && typeof cbs.trapFocus === 'function') {
         callbacks.trapFocus = cbs.trapFocus;
      }
   }

   /* ---- init ------------------------------------------------------------- */

   function init() {
      el.btn = document.getElementById('jobs-btn');
      el.popover = document.getElementById('jobs-popover');
      el.closeBtn = document.getElementById('jobs-close');
      el.activeList = document.getElementById('jobs-active-list');
      el.historyList = document.getElementById('jobs-history-list');
      el.activeCount = document.getElementById('jobs-active-count');
      el.historyCount = document.getElementById('jobs-history-count');
      el.moreBtn = document.getElementById('jobs-load-more');

      if (el.btn) {
         el.btn.addEventListener('click', toggle);
      }
      if (el.closeBtn) {
         el.closeBtn.addEventListener('click', close);
      }
      if (el.moreBtn) {
         el.moreBtn.addEventListener('click', function () {
            requestHistory(false);
         });
      }

      /* An activity pill already means "N jobs running", so clicking one to see
       * them is the gesture users reach for first — that applies to the
       * per-conversation pill above the composer as much as the header one, so
       * both are wired. Both are real <button>s in the markup, so role,
       * focusability and Space-on-keyup come from the platform;
       * jobs-activity.js still owns their text and aria-label, this module only
       * owns what the click does.
       *
       * NOT wired: the sidebar's per-conversation job badge. It sits inside a
       * history row that is itself a click target, and a nested button there
       * would both break that row's hit area and offer a worse action than the
       * row already does (open the conversation). */
      /* Delegated, not bound per element: jobs-activity.js discovers pill slots
       * at RENDER time (it renders into every .conv-jobs-pill, and its own
       * comment anticipates a second slot in the collapsed mini-status bar),
       * while this runs once at init. Binding here would leave any later slot
       * looking live — styled, counted, aria-expanded — and doing nothing. */
      document.addEventListener('click', function (e) {
         const pill = e.target && e.target.closest ? e.target.closest('.jobs-pill') : null;
         if (pill) {
            toggle();
         }
      });

      /* Self-heal: an auth event can land before this module is wired (audio
       * init between the two can block on a mic-permission prompt), and
       * updateVisibility would then not run again until the next auth change.
       * Reads the shared state via its own fallback. */
      updateVisibility(null);
   }

   global.DawnJobs = {
      init: init,
      setCallbacks: setCallbacks,
      open: open,
      close: close,
      toggle: toggle,
      updateVisibility: updateVisibility,
      handleReconnect: handleReconnect,
      // jobs_invalidate: re-render the active set and reload history if open —
      // identical to the reconnect resync, so a removed job leaves both lists.
      refresh: handleReconnect,
      handleHistoryPage: handleHistoryPage,
      handleActionResult: handleActionResult,
   };
})(window);
