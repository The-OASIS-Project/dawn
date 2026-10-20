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
 * Silent-Observe surface (Phase 0 of Dynamic Context Injection).
 *
 * Tracks today's running count of background observations the daemon emitted,
 * surfaces them as a peek popup over the transcript area, and shows a
 * persistent warning chip (outside the peek, separately styled) when any
 * observation in the last hour was a filter rejection.
 */

(function () {
   'use strict';

   const MAX_PEEK_ITEMS = 10;
   const FILTER_MATCH_LOOKBACK_MS = 60 * 60 * 1000;

   // Today's items (capped to MAX_PEEK_ITEMS).  Newer items appended to head.
   const items = [];
   let todayCount = 0;
   let lastBucketDay = todayBucket();
   let lastFilterMatchTs = 0;
   // Tracks the lastFilterMatchTs the user dismissed against — if a NEW
   // filter-match arrives, auto-expand fires again; otherwise the user's
   // dismissal sticks for the rest of the lookback window.
   let userDismissedAt = 0;
   // Chip dismissal is separate from peek dismissal.  Phase 0 stopgap:
   // clicking the chip = user has reviewed.  Chip stays hidden until a
   // NEWER filter-match arrives (lastFilterMatchTs > chipDismissedAt).
   // Phase 2 replaces this with proper review-tracking via the viewer page.
   let chipDismissedAt = 0;

   function todayBucket() {
      return DawnTime.dayKey(DawnTime.nowSec());
   }

   function rolloverIfNeeded() {
      const bucket = todayBucket();
      if (bucket !== lastBucketDay) {
         lastBucketDay = bucket;
         todayCount = 0;
         // Reset dismissal state at day-rollover so yesterday's dismissed
         // chip doesn't suppress today's first filter-match.
         chipDismissedAt = 0;
      }
   }

   function el(id) {
      return document.getElementById(id);
   }

   function fmtTime(ts) {
      if (!ts) return '';
      const p = DawnTime.parts(ts);
      return `${String(p.hour).padStart(2, '0')}:${String(p.minute).padStart(2, '0')}`;
   }

   function sanitizeNote(note) {
      if (typeof note !== 'string') return '';
      // Belt-and-suspenders.  We then assign via textContent which is itself
      // safe; DOMPurify with empty allowlists strips any HTML the LLM might
      // have included regardless.
      try {
         return window.DOMPurify
            ? window.DOMPurify.sanitize(note, { ALLOWED_TAGS: [], ALLOWED_ATTR: [] })
            : note.replace(/[<>&]/g, (c) => ({ '<': '&lt;', '>': '&gt;', '&': '&amp;' })[c]);
      } catch (e) {
         return '';
      }
   }

   function findRecentFilterMatch() {
      const cutoff = Date.now() - FILTER_MATCH_LOOKBACK_MS;
      return items.find((it) => it.filter_match && it.ts * 1000 >= cutoff);
   }

   function renderBadge() {
      rolloverIfNeeded();
      const rail = el('silent-observe-rail');
      const badge = el('silent-observe-badge');
      if (!rail || !badge) return;
      if (todayCount > 0) {
         rail.classList.remove('hidden');
         rail.setAttribute('aria-label', `DAWN background observations, ${todayCount} today`);
         badge.classList.remove('hidden');
         badge.textContent = String(todayCount);
      } else {
         badge.classList.add('hidden');
         rail.setAttribute('aria-label', 'DAWN background observations');
      }
   }

   function renderWarningChip() {
      const chip = el('silent-observe-warning-chip');
      if (!chip) return;
      const recentMatch = findRecentFilterMatch();
      // Chip shows iff: there's a recent filter-match AND its timestamp is
      // newer than the user's last chip dismissal.  Persistence promise
      // (security review): a newer rejection always re-arms regardless of
      // prior dismissal.
      if (recentMatch && lastFilterMatchTs > chipDismissedAt) {
         const cat = recentMatch.category || 'unknown';
         chip.textContent = `Filtered: blocked-pattern match in ${cat} (click to dismiss)`;
         chip.classList.remove('hidden');
         chip.setAttribute('role', 'button');
         chip.setAttribute('tabindex', '0');
         chip.style.cursor = 'pointer';
      } else {
         chip.classList.add('hidden');
      }
   }

   function renderPeekList() {
      const list = el('silent-observe-peek-list');
      if (!list) return;
      // Re-render from scratch.  N is small (≤10).
      while (list.firstChild) list.removeChild(list.firstChild);
      if (items.length === 0) {
         const p = document.createElement('p');
         p.className = 'silent-observe-empty';
         p.textContent = 'No background observations yet.';
         list.appendChild(p);
         return;
      }
      for (const it of items) {
         const row = document.createElement('div');
         row.className = 'silent-observe-row';
         if (it.filter_match) row.classList.add('filter-match');

         const meta = document.createElement('div');
         meta.className = 'silent-observe-row-meta';

         const cat = document.createElement('span');
         cat.className = 'silent-observe-category';
         cat.textContent = it.category || 'system';
         meta.appendChild(cat);

         const time = document.createElement('span');
         time.className = 'silent-observe-time';
         time.textContent = fmtTime(it.ts);
         meta.appendChild(time);

         const note = document.createElement('div');
         note.className = 'silent-observe-note';
         note.textContent = sanitizeNote(it.note);

         row.appendChild(meta);
         row.appendChild(note);
         list.appendChild(row);
      }
   }

   let peekEscToken = null; // DawnEscStack registration while the peek is visible

   function setPeekVisible(visible) {
      const peek = el('silent-observe-peek');
      const rail = el('silent-observe-rail');
      if (!peek) return;
      if (visible) {
         peek.classList.remove('hidden');
         if (peekEscToken === null) {
            peekEscToken = DawnEscStack.register(() => {
               dismissPeek();
               return true;
            });
         }
         if (rail) rail.setAttribute('aria-expanded', 'true');
         renderPeekList();
         renderWarningChip();
         // Move focus into the peek's close button so keyboard users can
         // dismiss without hunting for the affordance.
         const closeBtn = el('silent-observe-peek-close');
         if (closeBtn) closeBtn.focus();
      } else {
         peek.classList.add('hidden');
         if (peekEscToken !== null) {
            DawnEscStack.unregister(peekEscToken);
            peekEscToken = null;
         }
         if (rail) {
            rail.setAttribute('aria-expanded', 'false');
            // Restore focus to the rail button so the focus ring doesn't fall
            // off into the document body.
            rail.focus();
         }
      }
   }

   function isPeekVisible() {
      const peek = el('silent-observe-peek');
      return peek && !peek.classList.contains('hidden');
   }

   function handleEvent(payload) {
      if (!payload || typeof payload !== 'object') return;

      rolloverIfNeeded();

      // Defensive type checks — payload comes off the wire.
      const ts = typeof payload.ts === 'number' ? payload.ts : Math.floor(Date.now() / 1000);
      const category = typeof payload.category === 'string' ? payload.category : 'system';
      const note = typeof payload.note === 'string' ? payload.note : '';
      const filter_match = !!payload.filter_match;

      todayCount++;
      items.unshift({ ts, category, note, filter_match });
      while (items.length > MAX_PEEK_ITEMS) items.pop();

      const previousFilterMatchTs = lastFilterMatchTs;
      if (filter_match) {
         lastFilterMatchTs = Date.now();
      }

      renderBadge();
      renderWarningChip();

      // Auto-expand peek on a NEW filter-match the user hasn't already
      // dismissed.  Re-arms whenever a fresh filter rejection arrives —
      // dismissed state applies to a single match, not the whole hour.
      const newFilterMatch = filter_match && lastFilterMatchTs > userDismissedAt;
      if (newFilterMatch && !isPeekVisible()) {
         setPeekVisible(true);
      } else if (isPeekVisible()) {
         renderPeekList();
      }
      // Suppress unused warning — kept for diagnostic logging if revived.
      void previousFilterMatchTs;
   }

   function dismissPeek() {
      // Record the dismissal against the current filter-match timestamp so
      // a subsequent filter-match in the same hour still re-arms auto-expand.
      userDismissedAt = lastFilterMatchTs;
      setPeekVisible(false);
   }

   function init() {
      const rail = el('silent-observe-rail');
      const closeBtn = el('silent-observe-peek-close');
      if (rail) {
         rail.addEventListener('click', (ev) => {
            ev.preventDefault();
            if (isPeekVisible()) dismissPeek();
            else setPeekVisible(true);
         });
      }
      if (closeBtn) {
         closeBtn.addEventListener('click', dismissPeek);
      }
      // Phase 0 chip dismiss — click or Enter/Space dismisses.  Re-arms
      // automatically when a NEWER filter-match arrives (see
      // renderWarningChip).  Phase 2 will replace this with review-flow
      // via the viewer page.
      const chip = el('silent-observe-warning-chip');
      if (chip) {
         const dismissChip = (ev) => {
            ev.preventDefault();
            ev.stopPropagation();
            chipDismissedAt = lastFilterMatchTs;
            renderWarningChip();
         };
         chip.addEventListener('click', dismissChip);
         chip.addEventListener('keydown', (ev) => {
            if (ev.key === 'Enter' || ev.key === ' ') dismissChip(ev);
         });
      }
      // Esc dismiss is handled via DawnEscStack (register-on-show in
      // setPeekVisible / unregister on hide).
      // Click outside the peek to dismiss (but ignore clicks on the rail
      // button itself — that toggle is already handled).
      document.addEventListener('click', (ev) => {
         if (!isPeekVisible()) return;
         const peek = el('silent-observe-peek');
         if (!peek) return;
         if (peek.contains(ev.target)) return;
         if (rail && rail.contains(ev.target)) return;
         dismissPeek();
      });
   }

   if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', init);
   } else {
      init();
   }

   // Public surface — invoked by dawn.js's WebSocket dispatch.
   window.DawnSilentObserve = {
      handleEvent,
      reset() {
         items.length = 0;
         todayCount = 0;
         lastFilterMatchTs = 0;
         userDismissedAt = 0;
         renderBadge();
         renderPeekList();
         renderWarningChip();
      },
   };
})();
