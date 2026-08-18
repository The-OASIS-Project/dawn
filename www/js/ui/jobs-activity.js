/*
 * Background-jobs ACTIVE SET — the client's store of running jobs, plus the two
 * activity pills that project from it.
 *
 * The store is a set of job rows keyed by conversation id, fed by two frames:
 *   jobs_snapshot  the complete active set, on (re)connect
 *   job_update     one row per lifecycle transition (upsert; drop when terminal)
 *
 * Counts are DERIVED here by grouping the set on parent_id — the server no
 * longer sends counts at all. The property that used to be guaranteed by
 * server-side counting is preserved by using set membership rather than +/-1
 * arithmetic: a duplicate, out-of-order or replayed job_update converges to the
 * same answer, so reconnect / multi-tab / rapid switching all stay correct.
 * Deriving is sound because the active set is structurally bounded and arrives
 * whole (see src/webui/webui_jobs.c); job HISTORY is paginated and deliberately
 * never reaches this store.
 *
 * The two pills it renders:
 *   - a PER-CONVERSATION pill next to the state ("thinking") pill, counting
 *     active jobs whose parent is the CURRENTLY VIEWED conversation;
 *   - a GLOBAL pill in the header, counting all of them.
 * Both are pure functions of (set, activeConversationId), so switching
 * conversations is a re-render, never a stateful reset.
 */

window.DawnJobsActivity = (function () {
   'use strict';

   // conversation_id (as string key) -> job row, for ACTIVE jobs only.
   var jobs = Object.create(null);

   // Derived: parent conversation_id (as string key) -> active job count (> 0).
   var byParent = Object.create(null);

   // A job leaves the active set the moment it reports a terminal status. The
   // server only sends these two as live states, so anything else is terminal —
   // which keeps an unrecognized future status from pinning a row forever.
   function isActive(status) {
      return status === 'running' || status === 'queued';
   }

   // Jobs seen terminal since the last jobs_request was sent.
   //
   // This exists for ONE race, and only for snapshots: the server reads the
   // active set and enqueues the frame as two separate steps, so a job finishing
   // in between enqueues its terminal job_update FIRST (dropped here as "never
   // tracked"), and the snapshot then re-inserts the row it read a moment
   // earlier — pinning a finished job until the next reconnect.
   //
   // It deliberately does NOT gate live job_update frames. Those are now
   // ordered at the source: every active-status emit either precedes the
   // worker's existence (spawn/resume) or is claim-gated on 'queued'
   // (conv_db_job_set_running), so a stale active frame cannot follow a terminal
   // one. Gating them too would mean a single dropped frame — queue_response
   // discards the oldest under pressure — could hide a running job for its whole
   // runtime with no self-heal.
   //
   // Cleared on every jobs_request (see handleReconnect), so it only ever holds
   // the handful of terminals from one in-flight round trip. The cap is a
   // backstop against a snapshot request that never gets answered.
   var TERMINAL_MEMORY_MAX = 128;
   var terminalSeen = Object.create(null);
   var terminalOrder = [];

   function markTerminal(key) {
      if (terminalSeen[key]) {
         return;
      }
      terminalSeen[key] = true;
      terminalOrder.push(key);
      if (terminalOrder.length > TERMINAL_MEMORY_MAX) {
         delete terminalSeen[terminalOrder.shift()];
      }
   }

   function unmarkTerminal(key) {
      if (!terminalSeen[key]) {
         return;
      }
      delete terminalSeen[key];
      // Drop the ordering entry too: leaving it would let a later re-termination
      // push a duplicate key, and the eventual eviction of the stale copy would
      // then release a mark that is still live.
      var i = terminalOrder.indexOf(key);
      if (i >= 0) {
         terminalOrder.splice(i, 1);
      }
   }

   function recount() {
      byParent = Object.create(null);
      for (var id in jobs) {
         var p = jobs[id].parent_id;
         if (p) {
            var key = String(p);
            byParent[key] = (byParent[key] || 0) + 1;
         }
      }
   }

   // Resolver for the currently-viewed conversation id (wired in init()).
   var activeConvIdFn = function () {
      return null;
   };

   function total() {
      var t = 0;
      for (var k in byParent) {
         t += byParent[k];
      }
      return t;
   }

   // Authoritative active-job count for a conversation (0 if none) — read by the
   // sidebar to show a per-conversation "jobs running" badge on the history row.
   function getCount(convId) {
      if (convId == null) {
         return 0;
      }
      return byParent[String(convId)] || 0;
   }

   function plural(n) {
      return n + ' background task' + (n === 1 ? '' : 's') + ' running';
   }

   var SVG_NS = 'http://www.w3.org/2000/svg';

   /* Build the OASIS arc-reactor badge once per pill (idempotent): the same
    * circle + inscribed inverted triangle as the web icon (favicon.svg), plus a
    * pulsing glow ring, with the count in the core.  Strokes use currentColor so
    * the amber colour + glow come from CSS (.jobs-pill), matching the composer
    * "working" reactor — no per-instance SVG gradient/filter ids to collide.
    * The whole SVG is aria-hidden decorative; the button's aria-label carries the
    * screen-reader text.  DOM-built (never innerHTML). */
   function ensureReactor(el) {
      var svg = el.querySelector('svg.jobs-reactor');
      if (svg) {
         return svg;
      }
      svg = document.createElementNS(SVG_NS, 'svg');
      svg.setAttribute('class', 'jobs-reactor');
      svg.setAttribute('viewBox', '0 0 64 64');
      svg.setAttribute('aria-hidden', 'true');
      svg.setAttribute('focusable', 'false');

      function circle(cls, w) {
         var c = document.createElementNS(SVG_NS, 'circle');
         c.setAttribute('class', cls);
         c.setAttribute('cx', '32');
         c.setAttribute('cy', '32');
         c.setAttribute('r', '26');
         c.setAttribute('fill', 'none');
         c.setAttribute('stroke', 'currentColor');
         c.setAttribute('stroke-width', w);
         return c;
      }
      svg.appendChild(circle('reactor-glow', '5'));
      svg.appendChild(circle('reactor-ring', '2.5'));

      /* Inverted equilateral triangle inscribed in the r=26 circle — identical
       * geometry to favicon.svg. */
      var tri = document.createElementNS(SVG_NS, 'polygon');
      tri.setAttribute('class', 'reactor-tri');
      tri.setAttribute('points', '32,58 9.48,19 54.52,19');
      tri.setAttribute('fill', 'none');
      tri.setAttribute('stroke', 'currentColor');
      tri.setAttribute('stroke-width', '2');
      tri.setAttribute('stroke-linejoin', 'round');
      svg.appendChild(tri);

      var txt = document.createElementNS(SVG_NS, 'text');
      txt.setAttribute('class', 'reactor-count');
      txt.setAttribute('x', '32');
      /* y=29, not the geometric centre 32: the inverted triangle is widest at the
       * top and pinches toward its bottom point, so the count sits in the wide
       * upper band — more horizontal room for two digits, and optically centred
       * against the triangle's top-heavy mass. */
      txt.setAttribute('y', '29');
      txt.setAttribute('text-anchor', 'middle');
      txt.setAttribute('dominant-baseline', 'central');
      svg.appendChild(txt);

      el.appendChild(svg);
      return svg;
   }

   function renderPill(el, n, tooltip) {
      if (!el) {
         return;
      }
      if (n > 0) {
         /* The reactor + count are aria-hidden decoration; the button's
          * aria-label carries the descriptive text for AT. */
         var svg = ensureReactor(el);
         /* Clamp the DISPLAY to a short token so a pathological count can't overflow
          * the core: two digits fit inside the triangle; the effectively-unreachable
          * 99+ case (jobs concurrency is capped well below 100) sits within the ring.
          * The true n stays in the tooltip/aria-label. */
         svg.querySelector('.reactor-count').textContent = n > 99 ? '99+' : String(n);
         el.title = tooltip;
         el.setAttribute('aria-label', tooltip);
         el.classList.remove('hidden');
      } else {
         el.removeAttribute('title');
         el.removeAttribute('aria-label');
         el.classList.add('hidden'); /* reactor SVG stays parked, hidden */
      }
   }

   /* Last per-conversation count announced (and which conversation), so the sr-only
    * region speaks only on a 0<->n transition rather than on every increment. */
   var lastAnnouncedConvCount = 0;
   var lastAnnouncedConvId = null;

   function announceConvCount(n) {
      var convId = activeConvIdFn ? activeConvIdFn() : null;
      if (String(convId) !== String(lastAnnouncedConvId)) {
         /* Switched conversations — adopt the new view's baseline silently.  A 0
          * here means "this conversation has no jobs," NOT "jobs finished," so it
          * must never trigger the "finished" announcement (PR #24 review). */
         lastAnnouncedConvId = convId;
         lastAnnouncedConvCount = n;
         return;
      }
      if (n > 0 === lastAnnouncedConvCount > 0) {
         return; /* still running / still idle — nothing worth interrupting for */
      }
      lastAnnouncedConvCount = n;
      var el = document.getElementById('jobs-activity-announcer');
      if (!el) {
         return;
      }
      /* Plain text, not aria-hidden, in an element that is always rendered —
       * the two properties the pill itself could not satisfy. */
      el.textContent = n > 0 ? plural(n) + ' in this conversation' : 'Background tasks finished';
   }

   function renderConvPill() {
      var id = activeConvIdFn();
      var n = id != null && byParent[String(id)] ? byParent[String(id)] : 0;
      announceConvCount(n);
      // Rendered into every .conv-jobs-pill slot: the full-visualizer status row
      // AND the collapsed mini-status bar, so it's visible in either state.
      var els = document.querySelectorAll('.conv-jobs-pill');
      for (var i = 0; i < els.length; i++) {
         renderPill(els[i], n, plural(n) + ' in this conversation');
      }
   }

   function renderGlobalPill() {
      var n = total();
      renderPill(document.getElementById('global-jobs-pill'), n, plural(n));
   }

   function renderAll() {
      renderConvPill();
      renderGlobalPill();
   }

   // --- server-driven updates ------------------------------------------------

   // job_update: one job's current row. Upsert while active, drop when terminal.
   function upsertJob(job) {
      if (!job || job.conversation_id == null) {
         return;
      }
      var key = String(job.conversation_id);
      // A resume is the one transition that legitimately moves a job backwards
      // out of a terminal state, so it clears any mark an in-flight snapshot
      // would otherwise apply. The server flags it explicitly rather than us
      // inferring it from the status, because a tab that did not initiate the
      // resume sees only this frame.
      if (job.resumed) {
         unmarkTerminal(key);
      }
      var trackedWentTerminal = false;
      if (!isActive(job.status)) {
         markTerminal(key);
         if (!(key in jobs)) {
            return; // terminal row for a job we never tracked — nothing changed
         }
         delete jobs[key];
         trackedWentTerminal = true;
      } else {
         // Applied unconditionally: live frames are ordered at the source.
         jobs[key] = job;
      }
      recount();
      renderAll();
      // A tracked job just crossed the active→terminal partition. The active side
      // self-heals above, but the panel's HISTORY page is separately paginated and
      // stays stale unless updated — and handleActionResult only refetches for
      // PANEL-initiated actions, so a terminal transition arriving any other way
      // (conversation Stop, voice, another tab, or self-completion) would leave the
      // row in neither list until a manual refresh. Hand the panel the full row so
      // it can move it into History in place (no reset/refetch).
      if (trackedWentTerminal && typeof DawnJobs !== 'undefined' && DawnJobs.onJobTerminal) {
         DawnJobs.onJobTerminal(job);
      }
      // Patch just the affected conversation's sidebar badge (no list re-render).
      // A job's parent never changes, so the row carries the right parent whether
      // it just joined the set or just left it. A rootless job (parent_id 0) has
      // no conversation badge to patch.
      if (
         job.parent_id &&
         typeof DawnHistory !== 'undefined' &&
         DawnHistory.updateConversationJobsBadge
      ) {
         DawnHistory.updateConversationJobsBadge(String(job.parent_id));
      }
   }

   // jobs_snapshot: replace the whole active set (connect/reconnect).
   function applySnapshot(list, truncated) {
      // The server could not fit the whole active set in one frame, so every
      // count derived below is a lower bound. Shouldn't happen under any valid
      // config; say so rather than render a wrong number as if it were right.
      if (truncated) {
         console.warn('DawnJobsActivity: active-job snapshot truncated — counts are lower bounds');
      }
      jobs = Object.create(null);
      if (Array.isArray(list)) {
         list.forEach(function (job) {
            if (!job || job.conversation_id == null || !isActive(job.status)) {
               return;
            }
            var key = String(job.conversation_id);
            if (terminalSeen[key]) {
               return; // finished between the server's read and this frame
            }
            jobs[key] = job;
         });
      }
      recount();
      renderAll();
      // The whole set was replaced — reconcile every rendered sidebar row so
      // conversations that dropped to zero lose their badge too.
      if (typeof DawnHistory !== 'undefined' && DawnHistory.refreshAllJobsBadges) {
         DawnHistory.refreshAllJobsBadges();
      }
   }

   // Every active job row, for surfaces that need more than a count (the jobs
   // panel). The ARRAY is a copy, but the rows are the store's own objects —
   // treat them as read-only. Mutating a row would silently desync the derived
   // counts from what the panel renders.
   function getActive() {
      return Object.keys(jobs).map(function (k) {
         return jobs[k];
      });
   }

   // --- lifecycle hooks ------------------------------------------------------

   function handleReconnect() {
      if (typeof DawnWS !== 'undefined') {
         // A snapshot request opens a fresh reconciliation window, so drop the
         // terminal memory here. It exists only to suppress rows that finished
         // between the server's read and the frame's arrival — marks older than
         // this request are stale, and keeping them would make the snapshot
         // refuse a job resumed while this tab was disconnected (whose `resumed`
         // frame it never received).
         terminalSeen = Object.create(null);
         terminalOrder = [];
         DawnWS.send({ type: 'jobs_request' });
      }
   }

   // Called when the viewed conversation changes — just re-scopes the per-conv
   // pill from the existing map (no reset).
   function onConversationSwitch() {
      renderConvPill();
   }

   function init(getActiveConvId) {
      if (typeof getActiveConvId === 'function') {
         activeConvIdFn = getActiveConvId;
      }
      renderAll();
   }

   return {
      init: init,
      upsertJob: upsertJob,
      applySnapshot: applySnapshot,
      handleReconnect: handleReconnect,
      // A jobs_invalidate nudge re-syncs exactly like a reconnect: re-request the
      // snapshot so the pill drops a removed (e.g. cascade-deleted) job.
      refresh: handleReconnect,
      onConversationSwitch: onConversationSwitch,
      getCount: getCount,
      getActive: getActive,
   };
})();
