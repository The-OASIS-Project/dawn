/*
 * Background-jobs activity pills.
 *
 * Two display-only indicators driven by a single authoritative client-side map
 * (conversation_id -> active job count), fed by the server:
 *   - a PER-CONVERSATION pill next to the state ("thinking") pill, showing the
 *     count of active background jobs whose parent is the CURRENTLY VIEWED
 *     conversation;
 *   - a GLOBAL pill in the header, showing the total across all conversations.
 *
 * Both pills are pure functions of (map, activeConversationId), so switching
 * conversations is a re-render — never a stateful reset — which avoids the
 * per-conversation state-leak class of bug. Counts are the server's
 * authoritative values (not client +1/-1 deltas), so reconnect / multi-tab /
 * rapid switching all stay correct. A `jobs_activity_request` on (re)connect
 * rehydrates the whole map.
 */

window.DawnJobsActivity = (function () {
   'use strict';

   // conversation_id (as string key) -> active job count (> 0 only)
   var byParent = Object.create(null);

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

   function renderPill(el, n, tooltip) {
      if (!el) {
         return;
      }
      if (n > 0) {
         // Visible "⚙︎ N" is aria-hidden so a screen reader reads the descriptive
         // aria-label instead of the raw gear glyph (announcement of ⚙︎ is AT-dependent).
         el.textContent = '';
         const glyph = document.createElement('span');
         glyph.setAttribute('aria-hidden', 'true');
         glyph.textContent = '⚙︎ ' + n; // gear (text-presentation VS) + count
         el.appendChild(glyph);
         el.title = tooltip;
         el.setAttribute('aria-label', tooltip);
         el.classList.remove('hidden');
      } else {
         el.textContent = '';
         el.removeAttribute('title');
         el.removeAttribute('aria-label');
         el.classList.add('hidden');
      }
   }

   function renderConvPill() {
      var id = activeConvIdFn();
      var n = id != null && byParent[String(id)] ? byParent[String(id)] : 0;
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

   // job_activity: one parent's authoritative active count.
   function setActive(convId, count) {
      if (convId == null) {
         return;
      }
      var key = String(convId);
      var c = Number(count); // defend total() against a stringy/non-finite count
      if (Number.isFinite(c) && c > 0) {
         byParent[key] = c;
      } else {
         delete byParent[key];
      }
      renderAll();
      // Patch just this conversation's sidebar badge (no full list re-render).
      if (typeof DawnHistory !== 'undefined' && DawnHistory.updateConversationJobsBadge) {
         DawnHistory.updateConversationJobsBadge(key);
      }
   }

   // jobs_activity_snapshot: replace the whole map (connect/reconnect).
   function applySnapshot(list) {
      byParent = Object.create(null);
      if (Array.isArray(list)) {
         list.forEach(function (e) {
            var c = e ? Number(e.active_count) : NaN;
            if (e && e.conversation_id != null && Number.isFinite(c) && c > 0) {
               byParent[String(e.conversation_id)] = c;
            }
         });
      }
      renderAll();
      // Snapshot replaced the whole map — reconcile every rendered sidebar row.
      if (typeof DawnHistory !== 'undefined' && DawnHistory.refreshAllJobsBadges) {
         DawnHistory.refreshAllJobsBadges();
      }
   }

   // --- lifecycle hooks ------------------------------------------------------

   function handleReconnect() {
      if (typeof DawnWS !== 'undefined') {
         DawnWS.send({ type: 'jobs_activity_request' });
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
      setActive: setActive,
      applySnapshot: applySnapshot,
      handleReconnect: handleReconnect,
      onConversationSwitch: onConversationSwitch,
      getCount: getCount,
   };
})();
