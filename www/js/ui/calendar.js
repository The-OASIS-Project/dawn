/*
 * DAWN WebUI — Calendar pane (DawnCalendar).
 *
 * A browsable CalDAV calendar as a tab inside the Scheduler & Watches popover.
 * Two views: Month (a mini-grid — DawnCalendarGrid — plus the selected day's
 * agenda) and List (a day-grouped agenda of the next 30 days); the view is
 * forced to List on a narrow viewport.  Rendering + WS I/O live here; all state,
 * window math, and the source-agnostic occurrence model live in DawnCalendarData
 * (calendar-data.js); the grid render + keyboard nav in calendar-grid.js.
 *
 * Lifecycle: the scheduler popover calls activate()/deactivate() as the Calendar
 * tab becomes / stops being the visible pane, so state.active is exactly
 * "visible in an open popover" — it is also the render gate (a late response that
 * arrives after close updates state but paints nothing).  Fetches are lazy (on
 * first activate) + dirty-flagged (a calendar_events_changed while inactive
 * refetches on the next activate); no polling — the push covers changes.
 *
 * Coloring is structural: the calendar color is applied ONLY to the 3px accent
 * bar / legend dot, via CSSOM (el.style.backgroundColor — the browser validates
 * the untrusted CalDAV color; never a string-built style= that could inject CSS),
 * so event text is always theme-foreground on the theme surface (guaranteed AA).
 */
(function () {
   'use strict';

   const Data = window.DawnCalendarData;
   const Grid = window.DawnCalendarGrid;

   /* Max density dots per month-grid cell before a "+N" overflow. */
   const DOT_CAP = 3;

   /* Below this width the popover is full-width and a month grid doesn't fit, so
    * the view is forced to List (presentation only — the stored pref is kept, so
    * widening restores Month). */
   const NARROW_MQ = window.matchMedia ? window.matchMedia('(max-width: 600px)') : null;

   /* Trailing-edge debounce for calendar_events_changed: a multi-account sync
    * fires a burst, and a change during an in-flight fetch must refetch AFTER it
    * (the in-flight response may predate the change; the window-echo guard drops
    * a superseded response, and this schedules the fresh read). */
   const EVENTS_CHANGED_DEBOUNCE_MS = 300;

   const els = {
      pane: null,
      legend: null,
      body: null,
      grid: null,
      prevBtn: null,
      nextBtn: null,
      period: null,
      viewMonthBtn: null,
      viewListBtn: null,
      upcomingToggle: null,
      detail: null,
      detailTitle: null,
      detailMeta: null,
      detailClose: null,
   };
   let changeTimer = null;
   let detailEscToken = null;
   let detailReturnFocus = null;
   /* Set once the user navigates/selects a day, so we stop auto-anchoring to
    * "today's month" on activate (they've chosen where to look). */
   let userTouched = false;

   /* ---- helpers ---------------------------------------------------------- */

   function escapeHtml(str) {
      if (typeof DawnFormat !== 'undefined' && DawnFormat.escapeHtml) {
         return DawnFormat.escapeHtml(str == null ? '' : String(str));
      }
      if (str == null) return '';
      return String(str)
         .replace(/&/g, '&amp;')
         .replace(/</g, '&lt;')
         .replace(/>/g, '&gt;')
         .replace(/"/g, '&quot;');
   }

   /* NOTE: the escapeHtml fallback above does not escape ' — every attribute in
    * this file is double-quoted (escapeAttr adds ' for attribute contexts). Keep
    * attributes double-quoted so the fallback stays safe. */
   function escapeAttr(str) {
      if (typeof DawnFormat !== 'undefined' && DawnFormat.escapeAttr) {
         return DawnFormat.escapeAttr(str == null ? '' : String(str));
      }
      return escapeHtml(str).replace(/'/g, '&#39;');
   }

   function isConnected() {
      return typeof DawnWS !== 'undefined' && DawnWS.isConnected && DawnWS.isConnected();
   }

   /* Polite live region scoped to the calendar pane (the shared scheduler
    * announcer is aria-live=assertive, too loud for an informational count). */
   function announce(msg) {
      const el = document.getElementById('calendar-sr');
      if (el) el.textContent = msg;
   }

   /* Delegate to the data layer's DST-safe key builder (single source of truth;
    * calendar-grid.js keeps a deliberate standalone copy to stay dependency-free
    * — it must remain byte-identical). */
   function keyFromDate(dt) {
      return Data.keyFromDate(dt);
   }

   function localDateKey(epochSec) {
      return keyFromDate(new Date(epochSec * 1000));
   }

   function formatTime(epochSec) {
      return new Date(epochSec * 1000).toLocaleTimeString([], {
         hour: '2-digit',
         minute: '2-digit',
      });
   }

   /* 'YYYY-MM-DD' -> "Today · Thu Jul 31" / "Tomorrow · …" / "Mon Aug 4".
    * Built with new Date(y, m-1, d) (local, DST-safe) — never epoch arithmetic. */
   function dayLabel(dateKey) {
      const parts = dateKey.split('-');
      const date = new Date(Number(parts[0]), Number(parts[1]) - 1, Number(parts[2]));
      const today = new Date();
      const tomorrow = new Date();
      tomorrow.setDate(tomorrow.getDate() + 1);
      const weekday = date.toLocaleDateString([], { weekday: 'short' });
      const md = date.toLocaleDateString([], { month: 'short', day: 'numeric' });
      let prefix = '';
      if (dateKey === keyFromDate(today)) prefix = 'Today · ';
      else if (dateKey === keyFromDate(tomorrow)) prefix = 'Tomorrow · ';
      return prefix + weekday + ' ' + md;
   }

   function parseDateKey(key) {
      const p = key.split('-');
      return new Date(Number(p[0]), Number(p[1]) - 1, Number(p[2]));
   }

   function addDays(dt, n) {
      return new Date(dt.getFullYear(), dt.getMonth(), dt.getDate() + n);
   }

   /* Bucket filter-applied events by local day for the MONTH grid + selected-day
    * agenda: timed events on the local date of their start; all-day events across
    * [startDate, endDate) — endDate is the iCal-exclusive DTEND, so a 1-day
    * all-day event (empty/next-day endDate) hits exactly one cell.  The all-day
    * span is clamped to the visible window (Data.state.window) so a multi-year
    * all-day event still lands on its in-window days and the loop stays ≤42
    * iterations.  (List mode uses groupByDay instead, which keys all-day events
    * on startDate only — one row at the start — because a list shouldn't repeat a
    * multi-day event on every day; the two bucketers differ deliberately.) */
   function buildBuckets(events) {
      const buckets = Object.create(null);
      const win = Data.state.window;
      const winStart = win && win.start ? startOfLocalDay(new Date(win.start * 1000)) : null;
      const winEnd = win && win.end ? startOfLocalDay(new Date(win.end * 1000)) : null;
      const upcoming = isUpcomingOnly();
      const todayKey = keyFromDate(new Date());
      for (let i = 0; i < events.length; i++) {
         const ev = events[i];
         if (Data.isFilteredOff(ev.calendarKey)) continue;
         if (ev.allDay && ev.startDate) {
            const endExcl = ev.endDate
               ? parseDateKey(ev.endDate)
               : addDays(parseDateKey(ev.startDate), 1);
            let d = parseDateKey(ev.startDate);
            if (winStart && d.getTime() < winStart.getTime()) d = new Date(winStart);
            let guard = 0;
            while (
               d.getTime() < endExcl.getTime() &&
               (!winEnd || d.getTime() <= winEnd.getTime()) &&
               guard < 60
            ) {
               const k = keyFromDate(d);
               if (!upcoming || k >= todayKey) (buckets[k] || (buckets[k] = [])).push(ev);
               d = addDays(d, 1);
               guard++;
            }
         } else {
            const k = localDateKey(ev.start);
            if (!upcoming || k >= todayKey) (buckets[k] || (buckets[k] = [])).push(ev);
         }
      }
      return buckets;
   }

   function startOfLocalDay(dt) {
      return new Date(dt.getFullYear(), dt.getMonth(), dt.getDate());
   }

   /* ---- mode / viewport -------------------------------------------------- */

   function isNarrow() {
      return !!(NARROW_MQ && NARROW_MQ.matches);
   }

   function prefMode() {
      const v = window.DawnStore ? DawnStore.get(DawnStore.KEYS.CALENDAR_VIEW, 'month') : 'month';
      return v === 'list' ? 'list' : 'month';
   }

   /* "Upcoming only" (default on): hide events dated before today, so the current
    * month isn't cluttered with past/finished events.  Client-side (the data is
    * already fetched), so toggling just re-renders.  List mode is inherently
    * forward already (its window starts at now). */
   function isUpcomingOnly() {
      return window.DawnStore
         ? DawnStore.getBool(DawnStore.KEYS.CALENDAR_UPCOMING_ONLY, true)
         : true;
   }

   /* The mode actually rendered: the stored pref, forced to List on a narrow
    * viewport.  Returns true if it differs from the data layer's current mode
    * (so the caller must refetch — the window differs between modes). */
   function syncMode() {
      const eff = isNarrow() ? 'list' : prefMode();
      if (Data.state.mode !== eff) {
         Data.setMode(eff);
         return true;
      }
      return false;
   }

   /* ---- WS I/O ----------------------------------------------------------- */

   function sendCalendars() {
      if (typeof DawnWS !== 'undefined') DawnWS.send({ type: 'calendar_list_my_calendars' });
   }

   function sendEvents() {
      const win = Data.beginEventsFetch();
      if (typeof DawnWS !== 'undefined') {
         DawnWS.send({
            type: 'calendar_upcoming_events',
            payload: { start: win.start, end: win.end },
         });
      }
   }

   function load() {
      if (!isConnected()) {
         Data.setError('load');
         render();
         return;
      }
      Data.setError(null);
      sendCalendars();
      sendEvents();
      render(); /* shows the loading placeholder until the responses land */
   }

   function scheduleRefetch() {
      if (changeTimer) clearTimeout(changeTimer);
      changeTimer = setTimeout(function () {
         changeTimer = null;
         if (Data.state.active) load();
      }, EVENTS_CHANGED_DEBOUNCE_MS);
   }

   /* ---- render ----------------------------------------------------------- */

   function simpleStateHTML(headline, hint, action) {
      let html = '<div class="sched-popover-empty">';
      html += '<div class="sched-popover-empty-headline">' + escapeHtml(headline) + '</div>';
      if (hint) html += '<div class="sched-popover-empty-hint">' + escapeHtml(hint) + '</div>';
      if (action) {
         html +=
            '<button type="button" class="cal-empty-action" data-cal-action="' +
            escapeAttr(action.id) +
            '">' +
            escapeHtml(action.label) +
            '</button>';
      }
      html += '</div>';
      return html;
   }

   function rowHTML(ev) {
      const time = ev.allDay ? 'all-day' : formatTime(ev.start);
      const cal = Data.nameFor(ev.calendarKey);
      const label = (ev.title || '(no title)') + ', ' + time + (cal ? ', ' + cal : '');
      return (
         '<div class="cal-row" role="button" tabindex="0" data-event-id="' +
         escapeAttr(String(ev.id)) +
         '" data-cal-key="' +
         escapeAttr(ev.calendarKey) +
         '" aria-label="' +
         escapeAttr(label) +
         '">' +
         '<span class="cal-row-accent" aria-hidden="true"></span>' +
         '<span class="cal-row-time' +
         (ev.allDay ? ' allday' : '') +
         '">' +
         escapeHtml(time) +
         '</span>' +
         '<span class="cal-row-title">' +
         escapeHtml(ev.title || '(no title)') +
         '</span>' +
         '<span class="cal-row-cal">' +
         escapeHtml(cal) +
         '</span>' +
         '</div>'
      );
   }

   /* LIST-mode grouping: visible (filter-applied) events by local day, ascending.
    * All-day events key on their startDate ONLY (one row at the start — a list
    * shouldn't repeat a multi-day event on every covered day; contrast
    * buildBuckets, which spans them across grid cells for the month view).
    * Within a day the global start-order is preserved, so all-day (midnight)
    * sorts above timed. */
   function groupByDay(events) {
      const groups = [];
      const index = {};
      const upcoming = isUpcomingOnly();
      const todayKey = keyFromDate(new Date());
      for (let i = 0; i < events.length; i++) {
         const ev = events[i];
         if (Data.isFilteredOff(ev.calendarKey)) continue;
         const key = ev.allDay && ev.startDate ? ev.startDate : localDateKey(ev.start);
         if (upcoming && key < todayKey) continue;
         let g = index[key];
         if (!g) {
            g = { key: key, label: dayLabel(key), items: [] };
            index[key] = g;
            groups.push(g);
         }
         g.items.push(ev);
      }
      groups.sort(function (a, b) {
         return a.key < b.key ? -1 : a.key > b.key ? 1 : 0;
      });
      return groups;
   }

   function agendaHTML(groups) {
      let html = '';
      for (let i = 0; i < groups.length; i++) {
         const g = groups[i];
         html += '<div class="cal-day">';
         html += '<div class="cal-day-header">' + escapeHtml(g.label) + '</div>';
         for (let j = 0; j < g.items.length; j++) {
            html += rowHTML(g.items[j]);
         }
         html += '</div>';
      }
      if (Data.state.truncated) {
         html +=
            '<div class="cal-truncated"><span class="dawn-badge muted">' +
            'Some events beyond this range are hidden.</span></div>';
      }
      return html;
   }

   /* Apply the (untrusted) CalDAV color to accent bars via CSSOM only. */
   function colorizeAccents() {
      if (!els.body) return;
      const rows = els.body.querySelectorAll('.cal-row');
      rows.forEach(function (row) {
         const key = row.getAttribute('data-cal-key');
         const accent = row.querySelector('.cal-row-accent');
         if (accent) accent.style.backgroundColor = Data.colorFor(key) || '';
      });
   }

   function renderLegend() {
      const legend = els.legend;
      if (!legend) return;
      const s = Data.state;
      if (!s.calendarsLoaded || s.calendarOrder.length === 0) {
         legend.classList.add('hidden');
         legend.innerHTML = '';
         return;
      }
      let html = '';
      for (let i = 0; i < s.calendarOrder.length; i++) {
         const key = s.calendarOrder[i];
         const off = Data.isFilteredOff(key);
         html +=
            '<button type="button" class="cal-legend-chip' +
            (off ? ' off' : '') +
            '" data-cal-key="' +
            escapeAttr(key) +
            '" aria-pressed="' +
            (off ? 'false' : 'true') +
            '">' +
            '<span class="cal-legend-dot" aria-hidden="true"></span>' +
            '<span class="cal-legend-name">' +
            escapeHtml(Data.nameFor(key)) +
            '</span>' +
            '</button>';
      }
      legend.innerHTML = html;
      legend.classList.remove('hidden');
      const chips = legend.querySelectorAll('.cal-legend-chip');
      chips.forEach(function (chip) {
         const key = chip.getAttribute('data-cal-key');
         const dot = chip.querySelector('.cal-legend-dot');
         if (dot) dot.style.backgroundColor = Data.colorFor(key) || '';
      });
   }

   function monthLabel(anchor) {
      return new Date(anchor.year, anchor.month, 1).toLocaleDateString([], {
         month: 'long',
         year: 'numeric',
      });
   }

   function setToggleActive(mode) {
      if (els.viewMonthBtn) {
         const on = mode === 'month';
         els.viewMonthBtn.classList.toggle('active', on);
         els.viewMonthBtn.setAttribute('aria-pressed', on ? 'true' : 'false');
      }
      if (els.viewListBtn) {
         const on = mode === 'list';
         els.viewListBtn.classList.toggle('active', on);
         els.viewListBtn.setAttribute('aria-pressed', on ? 'true' : 'false');
      }
   }

   /* Update the header in place (don't rebuild — keeps focus on a nav button
    * across a month page). */
   function renderHeader() {
      const s = Data.state;
      const month = s.mode === 'month';
      if (els.prevBtn) els.prevBtn.classList.toggle('hidden', !month);
      if (els.nextBtn) els.nextBtn.classList.toggle('hidden', !month);
      if (els.period) els.period.textContent = month ? monthLabel(s.anchor) : 'Next 30 days';
      if (els.upcomingToggle) els.upcomingToggle.checked = isUpcomingOnly();
      setToggleActive(s.mode);
   }

   function renderGrid() {
      if (!els.grid) return;
      const s = Data.state;
      const show = s.mode === 'month' && !s.error && s.eventsLoaded && Grid;
      if (!show) {
         els.grid.classList.add('hidden');
         return;
      }
      /* Grid.render replaces the grid's innerHTML; if the user was keyboard-
       * navigating it when a background refetch triggered this render, restore
       * focus to the (still-selected) cell so nav isn't interrupted. */
      els.grid.setAttribute('aria-label', 'Calendar, ' + monthLabel(s.anchor));
      const hadFocus = els.grid.contains(document.activeElement);
      Grid.render(els.grid, {
         year: s.anchor.year,
         month: s.anchor.month,
         weekStart: Data.getWeekStart(),
         todayKey: keyFromDate(new Date()),
         selectedKey: s.selectedKey,
         buckets: buildBuckets(s.events),
         colorFor: Data.colorFor,
         dotCap: DOT_CAP,
         onSelect: onGridSelect,
         onPageToDate: onGridPageToDate,
      });
      els.grid.classList.remove('hidden');
      if (hadFocus) focusSelectedCell();
   }

   /* Render gate: only touch the DOM while the pane is the visible tab in an
    * open popover (state.active). A late response after close updates state and
    * returns here early — no paint into a hidden pane. */
   function render() {
      if (!Data.state.active) return;
      renderHeader();
      renderLegend();
      renderGrid();
      renderBody();
   }

   /* Paint the agenda body (also called on a legend toggle, which re-filters
    * without rebuilding the legend so keyboard focus stays on the chip). */
   function renderBody() {
      if (!Data.state.active || !els.body) return;
      const s = Data.state;
      els.body.removeAttribute('aria-busy');

      if (s.error) {
         els.body.innerHTML = simpleStateHTML(
            'Couldn’t load calendar',
            'Check your connection and try again.',
            { id: 'retry', label: 'Retry' }
         );
         return;
      }
      if (!s.eventsLoaded) {
         els.body.setAttribute('aria-busy', 'true');
         els.body.innerHTML = simpleStateHTML('Loading…', null, null);
         return;
      }
      /* No calendars configured — deep-link to settings, in either mode. */
      if (s.calendarsLoaded && s.calendarOrder.length === 0) {
         els.body.innerHTML = simpleStateHTML(
            'No calendars',
            'Add a CalDAV account to see your events here.',
            { id: 'add-account', label: 'Add a calendar account' }
         );
         return;
      }

      /* Month mode: the grid is the browse surface; the body shows the selected
       * day's events (even when the whole month is empty). */
      if (s.mode === 'month') {
         renderSelectedDay();
         return;
      }

      /* List mode. */
      if (s.events.length === 0) {
         els.body.innerHTML = simpleStateHTML(
            'Nothing scheduled',
            'No events in the next 30 days.',
            null
         );
         announce('No events in the next 30 days');
         return;
      }
      const groups = groupByDay(s.events);
      if (groups.length === 0) {
         els.body.innerHTML = simpleStateHTML(
            'Nothing to show',
            'No events for the selected calendars.',
            null
         );
         announce('No events for the selected calendars');
         return;
      }
      let visible = 0;
      for (let i = 0; i < groups.length; i++) visible += groups[i].items.length;
      els.body.innerHTML = agendaHTML(groups);
      colorizeAccents();
      announce(
         visible === 1 ? '1 event in the next 30 days' : visible + ' events in the next 30 days'
      );
   }

   /* A "some events hidden" note when the 256 cap trimmed the window (the
    * recursive window-bisect is a deferred follow-up; the note keeps the view
    * honest rather than silently sparse). */
   function truncatedNoteHTML() {
      return Data.state.truncated
         ? '<div class="cal-truncated"><span class="dawn-badge muted">' +
              'Some events this month are hidden (too many to show).</span></div>'
         : '';
   }

   /* Month mode: agenda for the selected day only. */
   function renderSelectedDay() {
      const s = Data.state;
      const label = dayLabel(s.selectedKey);
      const dayEvents = buildBuckets(s.events)[s.selectedKey] || [];
      let html = truncatedNoteHTML();
      html += '<div class="cal-day"><div class="cal-day-header">' + escapeHtml(label) + '</div>';
      if (dayEvents.length === 0) {
         html += '<div class="sched-popover-empty-compact">Nothing scheduled</div>';
      } else {
         for (let i = 0; i < dayEvents.length; i++) html += rowHTML(dayEvents[i]);
      }
      html += '</div>';
      els.body.innerHTML = html;
      colorizeAccents();
      /* No SR announce here: the focused gridcell's aria-label already conveys
       * the day + its event count, and this render also fires on passive
       * refetches where an announcement would be spurious. */
   }

   /* ---- read-only detail popover ---------------------------------------- */

   function findEventById(id) {
      const evs = Data.state.events;
      for (let i = 0; i < evs.length; i++) {
         if (String(evs[i].id) === String(id)) return evs[i];
      }
      return null;
   }

   function fmtDayShort(dt) {
      return dt.toLocaleDateString([], { weekday: 'short', month: 'short', day: 'numeric' });
   }

   function formatDetailWhen(ev) {
      if (ev.allDay) {
         const start = ev.startDate ? parseDateKey(ev.startDate) : new Date(ev.start * 1000);
         /* endDate is the iCal-exclusive DTEND; the inclusive last day is
          * endDate - 1. Show a range only for genuinely multi-day events. */
         let lastDay = null;
         if (ev.endDate) {
            const inclusive = addDays(parseDateKey(ev.endDate), -1);
            if (inclusive.getTime() > start.getTime()) lastDay = inclusive;
         }
         return 'All day · ' + fmtDayShort(start) + (lastDay ? ' – ' + fmtDayShort(lastDay) : '');
      }
      const start = new Date(ev.start * 1000);
      let s = fmtDayShort(start) + ' · ' + formatTime(ev.start);
      if (ev.end) {
         const end = new Date(ev.end * 1000);
         /* Prefix the end time with its date when the event crosses midnight. */
         const crossesDay = keyFromDate(start) !== keyFromDate(end);
         s += ' – ' + (crossesDay ? fmtDayShort(end) + ' ' : '') + formatTime(ev.end);
      }
      return s;
   }

   function detailMetaHTML(ev) {
      let html = '<div class="calendar-detail-when">' + escapeHtml(formatDetailWhen(ev)) + '</div>';
      if (ev.location) {
         html += '<div class="calendar-detail-loc">' + escapeHtml(ev.location) + '</div>';
      }
      const cal = Data.nameFor(ev.calendarKey);
      if (cal) {
         html +=
            '<div class="calendar-detail-cal"><span class="cal-legend-dot" aria-hidden="true" data-cal-key="' +
            escapeAttr(ev.calendarKey) +
            '"></span>' +
            escapeHtml(cal) +
            '</div>';
      }
      return html;
   }

   /* Own Tab-trap so the detail composes with the scheduler popover's focus
    * trap: keep Tab within the dialog AND stopPropagation so the parent trap
    * (a keydown listener on the outer popover) doesn't also act. */
   function detailKeydown(e) {
      if (e.key !== 'Tab') return;
      e.stopPropagation();
      const nodes = els.detail.querySelectorAll(
         'button, [href], input, select, textarea, [tabindex]:not([tabindex="-1"])'
      );
      const list = [];
      for (let i = 0; i < nodes.length; i++) {
         const el = nodes[i];
         if (el.disabled || el.getAttribute('aria-hidden') === 'true') continue;
         if (el.getClientRects().length > 0) list.push(el);
      }
      if (list.length === 0) return;
      const first = list[0];
      const last = list[list.length - 1];
      const active = document.activeElement;
      if (e.shiftKey) {
         if (active === first || !els.detail.contains(active)) {
            e.preventDefault();
            last.focus();
         }
      } else if (active === last || !els.detail.contains(active)) {
         e.preventDefault();
         first.focus();
      }
   }

   function openDetail(ev) {
      if (!ev || !els.detail) return;
      detailReturnFocus = document.activeElement;
      if (els.detailTitle) els.detailTitle.textContent = ev.title || '(no title)';
      if (els.detailMeta) {
         els.detailMeta.innerHTML = detailMetaHTML(ev);
         const dot = els.detailMeta.querySelector('.cal-legend-dot[data-cal-key]');
         if (dot) dot.style.backgroundColor = Data.colorFor(dot.getAttribute('data-cal-key')) || '';
      }
      els.detail.classList.remove('hidden');
      if (detailEscToken) DawnEscStack.unregister(detailEscToken);
      detailEscToken = DawnEscStack.register(function () {
         closeDetail();
         return true;
      });
      if (els.detailClose) els.detailClose.focus();
   }

   function closeDetail(returnFocus) {
      if (!els.detail || els.detail.classList.contains('hidden')) return;
      els.detail.classList.add('hidden');
      if (detailEscToken) {
         DawnEscStack.unregister(detailEscToken);
         detailEscToken = null;
      }
      /* returnFocus === false on the deactivate path (the row is about to be
       * hidden and focus belongs to whatever triggered the deactivate). */
      if (returnFocus !== false && detailReturnFocus && document.contains(detailReturnFocus)) {
         detailReturnFocus.focus();
      }
      detailReturnFocus = null;
   }

   /* ---- events ----------------------------------------------------------- */

   function onBodyClick(e) {
      const action = e.target.closest ? e.target.closest('[data-cal-action]') : null;
      if (action) {
         const a = action.getAttribute('data-cal-action');
         if (a === 'retry') {
            load();
         } else if (a === 'add-account') {
            if (window.DawnSettings && DawnSettings.openSection) {
               if (window.DawnSchedulerQueue) DawnSchedulerQueue.close();
               DawnSettings.openSection('calendar-accounts-section');
            } else {
               console.warn('DawnCalendar: settings deep-link unavailable (DawnSettings missing)');
            }
         }
         return;
      }
      const row = e.target.closest ? e.target.closest('.cal-row[data-event-id]') : null;
      if (row) {
         const ev = findEventById(row.getAttribute('data-event-id'));
         if (ev) openDetail(ev);
      }
   }

   function onBodyKeydown(e) {
      if (e.key !== 'Enter' && e.key !== ' ') return;
      const row = e.target.closest ? e.target.closest('.cal-row[data-event-id]') : null;
      if (!row) return;
      e.preventDefault();
      const ev = findEventById(row.getAttribute('data-event-id'));
      if (ev) openDetail(ev);
   }

   function onLegendClick(e) {
      const chip = e.target.closest ? e.target.closest('.cal-legend-chip') : null;
      if (!chip) return;
      const key = chip.getAttribute('data-cal-key');
      if (!key) return;
      Data.toggleFilter(key);
      /* Mutate the clicked chip in place — do NOT rebuild the legend, or the
       * button that has keyboard focus is replaced and focus falls to <body>.
       * Only the agenda body needs to re-filter. */
      const off = Data.isFilteredOff(key);
      chip.classList.toggle('off', off);
      chip.setAttribute('aria-pressed', off ? 'false' : 'true');
      renderBody();
      renderGrid(); /* month mode: re-filter the grid dots too (focus stays on the chip) */
   }

   /* When the anchor month changes, keep the selection inside it: today if the
    * new month contains today, else its 1st. */
   function reselectForAnchor() {
      const s = Data.state;
      const now = new Date();
      if (now.getFullYear() === s.anchor.year && now.getMonth() === s.anchor.month) {
         Data.setSelectedKey(keyFromDate(now));
      } else {
         Data.setSelectedKey(keyFromDate(new Date(s.anchor.year, s.anchor.month, 1)));
      }
   }

   /* Don't focus a hidden grid (e.g. after a disconnect paged into the error
    * state) — that would drop focus to <body>. */
   function focusSelectedCell() {
      if (!els.grid || els.grid.classList.contains('hidden')) return;
      const cell = els.grid.querySelector('.cal-cell[tabindex="0"]');
      if (cell) cell.focus();
   }

   function monthOfKey(key) {
      const p = key.split('-');
      return { year: Number(p[0]), month: Number(p[1]) - 1 };
   }

   /* Month-button navigation: jump a month, select today (if landed on) or the
    * 1st — a "next month" gesture carries no specific target day. */
   function pageMonth(delta, focusGrid) {
      userTouched = true;
      Data.shiftMonth(delta);
      reselectForAnchor();
      load(); /* new window; render() paints the new month (dots fill on response) */
      if (focusGrid) focusSelectedCell();
   }

   /* Page to a specific date in another month (a spillover-cell pick or a
    * keyboard step off the grid edge) — keeps navigation date-contiguous. */
   function pageToDate(key, focusGrid) {
      userTouched = true;
      const km = monthOfKey(key);
      Data.setAnchorMonth(km.year, km.month);
      Data.setSelectedKey(key);
      load();
      if (focusGrid) focusSelectedCell();
   }

   /* Grid callbacks. */
   function onGridSelect(key) {
      userTouched = true;
      Data.setSelectedKey(key);
      renderBody(); /* the grid moved its own selection visual + focus */
   }
   function onGridPageToDate(key) {
      pageToDate(key, true);
   }

   function onToggleView(view) {
      if (window.DawnStore) DawnStore.set(DawnStore.KEYS.CALENDAR_VIEW, view);
      if (syncMode()) load();
      else render();
   }

   function onToggleUpcoming() {
      const on = els.upcomingToggle ? els.upcomingToggle.checked : true;
      if (window.DawnStore) DawnStore.setBool(DawnStore.KEYS.CALENDAR_UPCOMING_ONLY, on);
      render(); /* client-side filter — no refetch, just re-render */
   }

   function onViewportChange() {
      if (!Data.state.active) return; /* resync on next activate() */
      if (syncMode()) load();
      else render();
   }

   /* ---- lifecycle / public API ------------------------------------------ */

   function activate() {
      const s = Data.state;
      Data.setActive(true);
      /* Re-anchor an untouched calendar to today's month — covers the page
       * staying open across a month boundary (anchor was seeded at script load).
       * If that moves the anchor, the cached events are for the old month, so
       * force a refetch. */
      let anchorMoved = false;
      if (!userTouched) {
         const now = new Date();
         if (s.anchor.year !== now.getFullYear() || s.anchor.month !== now.getMonth()) {
            Data.setAnchorMonth(now.getFullYear(), now.getMonth());
            anchorMoved = true;
         }
         Data.setSelectedKey(keyFromDate(now));
      }
      const modeChanged = syncMode(); /* reflect the pref + viewport before painting */
      render(); /* show cached state (or the loading placeholder) immediately */
      if (!s.eventsLoaded || s.dirty || s.error || modeChanged || anchorMoved) {
         Data.setDirty(false);
         load();
      }
   }

   function deactivate() {
      Data.setActive(false);
      closeDetail(false); /* don't leave the detail overlay up when the pane is hidden */
      if (changeTimer) {
         clearTimeout(changeTimer);
         changeTimer = null;
      }
   }

   function handleCalendarsResponse(payload) {
      Data.ingestCalendars(payload);
      render(); /* gated on active; refreshes legend + colors */
   }

   function handleEventsResponse(payload) {
      const current = Data.ingestEvents(payload);
      if (current) {
         Data.setDirty(false);
         render();
      }
   }

   function onEventsChanged() {
      /* Always mark dirty (the single "unserviced change" flag): if active, a
       * debounced refetch services + clears it; if that refetch is cancelled by
       * a tab switch before it fires, dirty survives so the next activate()
       * refetches.  If inactive, next activate() picks it up. */
      Data.setDirty(true);
      if (Data.state.active) scheduleRefetch();
   }

   function init() {
      els.pane = document.getElementById('calendar-pane');
      els.legend = document.getElementById('calendar-legend');
      els.body = document.getElementById('calendar-body');
      els.grid = document.getElementById('calendar-grid');
      els.prevBtn = document.getElementById('calendar-prev');
      els.nextBtn = document.getElementById('calendar-next');
      els.period = document.getElementById('calendar-period');
      els.viewMonthBtn = document.getElementById('calendar-view-month');
      els.viewListBtn = document.getElementById('calendar-view-list');
      els.upcomingToggle = document.getElementById('calendar-upcoming');
      els.detail = document.getElementById('calendar-detail');
      els.detailTitle = document.getElementById('calendar-detail-title');
      els.detailMeta = document.getElementById('calendar-detail-meta');
      els.detailClose = document.getElementById('calendar-detail-close');
      if (!els.pane || !els.body) return;
      if (els.upcomingToggle) els.upcomingToggle.addEventListener('change', onToggleUpcoming);
      els.body.addEventListener('click', onBodyClick);
      els.body.addEventListener('keydown', onBodyKeydown);
      if (els.legend) els.legend.addEventListener('click', onLegendClick);
      if (els.detail) {
         els.detail.addEventListener('keydown', detailKeydown);
         /* Click the backdrop (not the card) to close. */
         els.detail.addEventListener('click', function (e) {
            if (e.target === els.detail) closeDetail();
         });
      }
      if (els.detailClose) {
         els.detailClose.addEventListener('click', function () {
            closeDetail();
         });
      }
      if (els.prevBtn) {
         els.prevBtn.addEventListener('click', function () {
            pageMonth(-1, false);
         });
      }
      if (els.nextBtn) {
         els.nextBtn.addEventListener('click', function () {
            pageMonth(1, false);
         });
      }
      if (els.viewMonthBtn) {
         els.viewMonthBtn.addEventListener('click', function () {
            onToggleView('month');
         });
      }
      if (els.viewListBtn) {
         els.viewListBtn.addEventListener('click', function () {
            onToggleView('list');
         });
      }
      if (NARROW_MQ) {
         if (NARROW_MQ.addEventListener) NARROW_MQ.addEventListener('change', onViewportChange);
         else if (NARROW_MQ.addListener) NARROW_MQ.addListener(onViewportChange); /* older */
      }
   }

   window.DawnCalendar = {
      init: init,
      activate: activate,
      deactivate: deactivate,
      handleCalendarsResponse: handleCalendarsResponse,
      handleEventsResponse: handleEventsResponse,
      onEventsChanged: onEventsChanged,
   };
})();
