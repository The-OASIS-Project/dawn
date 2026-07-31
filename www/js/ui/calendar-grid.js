/*
 * DAWN WebUI — Calendar month grid (DawnCalendarGrid).
 *
 * Pure render + keyboard for the 6×7 month grid.  Given an anchor month, a
 * day→events bucket map, and callbacks, it paints a role="grid" of density-dot
 * cells and owns roving-tabindex arrow-key navigation.  It holds no app state —
 * DawnCalendar (calendar.js) owns the data and re-renders on selection / month
 * change.  Coloring is CSSOM-only (density dots), consistent with the agenda.
 *
 * render(container, opts) where opts = {
 *   year, month,        // anchor month (month is 0-based)
 *   weekStart,          // 0=Sun..6=Sat
 *   todayKey,           // 'YYYY-MM-DD'
 *   selectedKey,        // 'YYYY-MM-DD' (must fall within the anchor month)
 *   buckets,            // { 'YYYY-MM-DD': [normalizedEvent, ...] }  (filter-applied)
 *   colorFor,           // (calendarKey) -> css color string
 *   dotCap,             // max dots per cell before "+N"
 *   onSelect,           // (dateKey) => void   user picked an in-month day
 *   onPageToDate,       // (dateKey) => void   picked/stepped to a day in another month
 * }
 */
(function () {
   'use strict';

   function keyFromDate(dt) {
      const y = dt.getFullYear();
      const m = String(dt.getMonth() + 1).padStart(2, '0');
      const d = String(dt.getDate()).padStart(2, '0');
      return y + '-' + m + '-' + d;
   }

   function escapeAttr(str) {
      return String(str == null ? '' : str)
         .replace(/&/g, '&amp;')
         .replace(/</g, '&lt;')
         .replace(/>/g, '&gt;')
         .replace(/"/g, '&quot;')
         .replace(/'/g, '&#39;');
   }

   /* Short weekday names indexed by getDay() (0=Sun..6=Sat). 2023-01-01 is a
    * Sunday, so offset i from it gives day i. Locale-formatted, no user input. */
   const WEEKDAY_SHORT = (function () {
      const arr = [];
      for (let i = 0; i < 7; i++) {
         arr.push(new Date(2023, 0, 1 + i).toLocaleDateString([], { weekday: 'short' }));
      }
      return arr;
   })();

   function cellLabel(d, count) {
      const ds = d.toLocaleDateString([], { weekday: 'long', month: 'long', day: 'numeric' });
      const ev = count === 0 ? 'no events' : count === 1 ? '1 event' : count + ' events';
      return escapeAttr(ds + ', ' + ev);
   }

   function dotsHTML(events, cap) {
      if (!events || events.length === 0) return '';
      let html = '<span class="cal-dots" aria-hidden="true">';
      const n = Math.min(events.length, cap);
      for (let i = 0; i < n; i++) {
         html +=
            '<span class="cal-dot" data-cal-key="' +
            escapeAttr(events[i].calendarKey) +
            '"></span>';
      }
      if (events.length > cap) {
         html += '<span class="cal-dot-more">+' + (events.length - cap) + '</span>';
      }
      html += '</span>';
      return html;
   }

   function buildHTML(opts) {
      const ws = opts.weekStart;
      let html = '<div class="cal-grid-weekdays" role="row">';
      for (let i = 0; i < 7; i++) {
         html +=
            '<div class="cal-weekday" role="columnheader">' +
            WEEKDAY_SHORT[(ws + i) % 7] +
            '</div>';
      }
      html += '</div>';

      const first = new Date(opts.year, opts.month, 1);
      const offset = (first.getDay() - ws + 7) % 7;
      const firstCell = new Date(opts.year, opts.month, 1 - offset);
      let idx = 0;
      for (let r = 0; r < 6; r++) {
         html += '<div class="cal-week" role="row">';
         for (let c = 0; c < 7; c++) {
            /* Date construction (never epoch ± 86400) keeps cells DST-correct. */
            const d = new Date(
               firstCell.getFullYear(),
               firstCell.getMonth(),
               firstCell.getDate() + idx
            );
            const key = keyFromDate(d);
            const inMonth = d.getMonth() === opts.month;
            const isToday = key === opts.todayKey;
            const isSel = key === opts.selectedKey;
            const events = (opts.buckets && opts.buckets[key]) || [];
            let cls = 'cal-cell';
            if (!inMonth) cls += ' adjacent';
            if (isToday) cls += ' today';
            if (isSel) cls += ' selected';
            html +=
               '<div class="' +
               cls +
               '" role="gridcell" data-idx="' +
               idx +
               '" data-date="' +
               key +
               '" tabindex="' +
               (isSel ? '0' : '-1') +
               '" aria-selected="' +
               (isSel ? 'true' : 'false') +
               '"' +
               (isToday ? ' aria-current="date"' : '') +
               ' aria-label="' +
               cellLabel(d, events.length) +
               '"><span class="cal-cell-num">' +
               d.getDate() +
               '</span>' +
               dotsHTML(events, opts.dotCap) +
               '</div>';
            idx++;
         }
         html += '</div>';
      }
      return html;
   }

   function colorizeDots(container, opts) {
      const dots = container.querySelectorAll('.cal-dot[data-cal-key]');
      dots.forEach(function (dot) {
         const color = opts.colorFor ? opts.colorFor(dot.getAttribute('data-cal-key')) : '';
         dot.style.backgroundColor = color || '';
      });
   }

   /* Move the roving tabindex + the selected visual to cell `idx`, focus it.
    * Focus-follows-selection: the caller (onSelect) updates the agenda. */
   function selectCell(container, idx) {
      const opts = container._calGridOpts;
      const cells = container.querySelectorAll('.cal-cell');
      cells.forEach(function (c) {
         c.setAttribute('tabindex', '-1');
         c.classList.remove('selected');
         c.setAttribute('aria-selected', 'false');
      });
      const target = container.querySelector('.cal-cell[data-idx="' + idx + '"]');
      if (!target) return;
      target.setAttribute('tabindex', '0');
      target.classList.add('selected');
      target.setAttribute('aria-selected', 'true');
      target.focus();
      const key = target.getAttribute('data-date');
      if (key && opts && opts.onSelect) opts.onSelect(key);
   }

   /* The date at grid index idx (valid for idx < 0 or > 41 too — used to step
    * contiguously off an edge into the adjacent month). */
   function dateAtIndex(opts, idx) {
      const first = new Date(opts.year, opts.month, 1);
      const offset = (first.getDay() - opts.weekStart + 7) % 7;
      const fc = new Date(opts.year, opts.month, 1 - offset);
      return new Date(fc.getFullYear(), fc.getMonth(), fc.getDate() + idx);
   }

   /* Choose a cell: an in-month cell selects in place; a spillover (adjacent-
    * month) cell pages to its month with that day selected — so a selected/
    * focused day is never left dimmed in the overflow, and month navigation is
    * date-contiguous. */
   function pickCell(container, cell) {
      const opts = container._calGridOpts;
      if (cell.classList.contains('adjacent')) {
         if (opts && opts.onPageToDate) opts.onPageToDate(cell.getAttribute('data-date'));
         return;
      }
      const idx = parseInt(cell.getAttribute('data-idx'), 10);
      if (!isNaN(idx)) selectCell(container, idx);
   }

   function onClick(e) {
      const container = e.currentTarget;
      const cell = e.target.closest ? e.target.closest('.cal-cell') : null;
      if (cell) pickCell(container, cell);
   }

   function onKeydown(e) {
      const container = e.currentTarget;
      const opts = container._calGridOpts;
      const cell = e.target.closest ? e.target.closest('.cal-cell') : null;
      if (!cell) return;
      const idx = parseInt(cell.getAttribute('data-idx'), 10);
      if (isNaN(idx)) return;

      let next = -1;
      switch (e.key) {
         case 'ArrowRight':
            next = idx + 1;
            break;
         case 'ArrowLeft':
            next = idx - 1;
            break;
         case 'ArrowDown':
            next = idx + 7;
            break;
         case 'ArrowUp':
            next = idx - 7;
            break;
         case 'Home':
            next = idx - (idx % 7);
            break;
         case 'End':
            next = idx - (idx % 7) + 6;
            break;
         case 'PageUp':
         case 'PageDown': {
            /* Same day-of-month, previous/next month (clamped by Date normalize). */
            e.preventDefault();
            const cur = dateAtIndex(opts, idx);
            const tgt = new Date(
               cur.getFullYear(),
               cur.getMonth() + (e.key === 'PageUp' ? -1 : 1),
               cur.getDate()
            );
            if (opts && opts.onPageToDate) opts.onPageToDate(keyFromDate(tgt));
            return;
         }
         case 'Enter':
         case ' ':
            e.preventDefault();
            pickCell(container, cell);
            return;
         default:
            return;
      }
      e.preventDefault();
      /* Off an edge of the 42-cell grid → step to the contiguous date in the
       * adjacent month. */
      if (next < 0 || next > 41) {
         if (opts && opts.onPageToDate) opts.onPageToDate(keyFromDate(dateAtIndex(opts, next)));
         return;
      }
      const target = container.querySelector('.cal-cell[data-idx="' + next + '"]');
      if (target) pickCell(container, target);
   }

   function render(container, opts) {
      if (!container) return;
      container._calGridOpts = opts;
      if (!container._calGridBound) {
         container.addEventListener('click', onClick);
         container.addEventListener('keydown', onKeydown);
         container._calGridBound = true;
      }
      container.innerHTML = buildHTML(opts);
      colorizeDots(container, opts);
      /* Safety: guarantee exactly one tab stop even if selectedKey wasn't in the
       * grid (shouldn't happen — calendar.js keeps it in the anchor month). */
      if (!container.querySelector('.cal-cell[tabindex="0"]')) {
         const firstIn = container.querySelector('.cal-cell:not(.adjacent)');
         if (firstIn) firstIn.setAttribute('tabindex', '0');
      }
   }

   window.DawnCalendarGrid = { render: render };
})();
