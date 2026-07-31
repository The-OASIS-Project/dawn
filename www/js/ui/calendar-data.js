/*
 * DAWN WebUI — Calendar data layer (DawnCalendarData).
 *
 * The source-agnostic state + logic behind the Calendar pane: window math,
 * the CalDAV→normalized adapter, the calendar id→{name,color} map, and the
 * response-ordering guard.  No DOM — DawnCalendar (calendar.js) owns rendering
 * and WS I/O and delegates all state here.  Keeping this separate keeps a future
 * second event source (scheduler events → combined timeline) out of the render
 * file: it becomes another adapter here, nothing in calendar.js changes.
 *
 * Ordering guard: window-echo equality.  We always request an explicit
 * {start,end}, and a success response echoes that same {start,end}, so the
 * response for the currently-requested window is identified by matching it —
 * a stale response (from a window superseded by a refetch / reopen) simply
 * won't match and is dropped.  This is stateless, so unlike a request counter
 * it cannot desync if a request ever goes unanswered (e.g. the socket drops
 * after send).  Error frames carry no window, so they surface the error
 * optimistically; a later matching success clears it (self-healing).
 */
(function () {
   'use strict';

   /* List-mode window: a rolling N days forward from now.  Always sent as an
    * explicit {start,end} (not {days}) so the cap is uniform (the {days} path
    * clamps 1..90, explicit allows 366) and the window is client-predictable. */
   const LIST_WINDOW_DAYS = 30;

   /* Local 'YYYY-MM-DD' for a Date, built from local field getters (DST-safe —
    * never epoch arithmetic). */
   function keyFromDate(dt) {
      const y = dt.getFullYear();
      const m = String(dt.getMonth() + 1).padStart(2, '0');
      const d = String(dt.getDate()).padStart(2, '0');
      return y + '-' + m + '-' + d;
   }

   const _now = new Date();

   const state = {
      active: false /* true while the Calendar tab is the visible pane */,
      mode: 'month' /* 'month' (default) | 'list' */,
      anchor: {
         year: _now.getFullYear(),
         month: _now.getMonth(),
      } /* displayed month (month mode) */,
      selectedKey: keyFromDate(_now) /* selected day 'YYYY-MM-DD' (month mode) */,
      eventsLoaded: false,
      calendarsLoaded: false,
      dirty: false /* a calendar_events_changed arrived while inactive */,
      error: null /* null | 'load' */,
      /* null-proto maps so a pathological calendar id like "__proto__" is an
       * ordinary key, never a prototype write. */
      calendars: Object.create(null) /* calendarKey -> { name, color } */,
      calendarOrder: [] /* stable key order for the legend */,
      filterOff: Object.create(null) /* calendarKey -> true (hidden client-side) */,
      events: [] /* normalized, sorted by start */,
      truncated: false,
      window: { start: 0, end: 0 } /* the currently-requested/displayed window (epoch s) */,
   };

   function nowSec() {
      return Math.floor(Date.now() / 1000);
   }

   /* First day of the week for the user's locale, as a JS getDay() value
    * (0=Sun..6=Sat).  Intl.weekInfo.firstDay is 1=Mon..7=Sun; feature-detect it
    * (not universal) and fall back to Sunday. */
   function getWeekStart() {
      try {
         const loc = new Intl.Locale(navigator.language || 'en-US');
         const wi = loc.weekInfo || (loc.getWeekInfo && loc.getWeekInfo());
         if (wi && typeof wi.firstDay === 'number') {
            return wi.firstDay === 7 ? 0 : wi.firstDay;
         }
      } catch (e) {
         /* older browser — fall through */
      }
      return 0;
   }

   /* The 42-cell (6-week) month grid window for state.anchor, aligned to the
    * locale week start.  All Date construction (never epoch ± 86400) so the
    * cell days and window edges are DST-correct. */
   function computeMonthWindow() {
      const ws = getWeekStart();
      const first = new Date(state.anchor.year, state.anchor.month, 1);
      const offset = (first.getDay() - ws + 7) % 7; /* days from week start to the 1st */
      const firstCell = new Date(state.anchor.year, state.anchor.month, 1 - offset);
      const lastCell = new Date(
         firstCell.getFullYear(),
         firstCell.getMonth(),
         firstCell.getDate() + 41,
         23,
         59,
         59
      );
      return {
         start: Math.floor(firstCell.getTime() / 1000) /* local midnight of the first cell */,
         end: Math.floor(lastCell.getTime() / 1000) /* end of the last cell's day */,
      };
   }

   /* The window for the current mode: Month = the 42-cell grid range; List =
    * [now, now + N days].  Always an explicit {start,end} (see the ordering
    * guard). */
   function computeWindow() {
      if (state.mode === 'month') return computeMonthWindow();
      const start = nowSec();
      return { start: start, end: start + LIST_WINDOW_DAYS * 86400 };
   }

   /* CalDAV occurrence frame -> normalized, source-agnostic occurrence.
    * Drops cancelled occurrences (the backend does not filter is_cancelled, and
    * unfiltered they render as ghost meetings) and omits is_override (no render
    * use).  Returns null for a dropped occurrence. */
   function fromCalDav(e) {
      if (!e || e.cancelled) return null;
      return {
         id: e.id,
         source: 'caldav',
         calendarKey: String(e.calendar_id),
         title: e.summary || '',
         start: e.start,
         end: e.end,
         allDay: !!e.all_day,
         startDate: e.start_date || '',
         endDate: e.end_date || '',
         location: e.location || '',
      };
   }

   /* Mark a fresh events request: remember the window we're asking for.  Caller
    * then sends the WS request for state.window. */
   function beginEventsFetch() {
      state.window = computeWindow();
      return state.window;
   }

   /* Fold a calendar_upcoming_events_response into state.  Returns true if this
    * response is the current one (and thus the caller should re-render), false
    * if stale.  Handles success:false (a handler error — distinct from an empty
    * success window) by entering the 'load' error state. */
   function ingestEvents(payload) {
      if (!payload || payload.success !== true) {
         /* Error frames carry no window to correlate against; surface the error
          * optimistically — a later response for the current window clears it. */
         state.eventsLoaded = true;
         state.error = 'load';
         return true;
      }
      /* Window-echo guard: drop a response whose echoed window is not the one we
       * currently want (a superseded fetch).  Stateless — cannot desync. */
      if (payload.start !== state.window.start || payload.end !== state.window.end) {
         return false;
      }
      state.eventsLoaded = true;
      state.error = null;
      state.truncated = !!payload.truncated;
      const raw = Array.isArray(payload.events) ? payload.events : [];
      const norm = [];
      for (let i = 0; i < raw.length; i++) {
         const n = fromCalDav(raw[i]);
         if (n) norm.push(n);
      }
      norm.sort(function (a, b) {
         return a.start - b.start || a.id - b.id;
      });
      state.events = norm;
      return true;
   }

   /* Fold a calendar_list_my_calendars_response into the id->{name,color} map.
    * Last-writer-wins (the list changes rarely; no ordering guard needed). */
   function ingestCalendars(payload) {
      state.calendarsLoaded = true;
      if (!payload || payload.success !== true || !Array.isArray(payload.calendars)) {
         return;
      }
      const map = Object.create(null);
      const order = [];
      for (let i = 0; i < payload.calendars.length; i++) {
         const c = payload.calendars[i];
         const key = String(c.id);
         map[key] = { name: c.name || '', color: c.color || '' };
         order.push(key);
      }
      state.calendars = map;
      state.calendarOrder = order;
   }

   function colorFor(calendarKey) {
      const c = state.calendars[calendarKey];
      return c && c.color ? c.color : '';
   }

   function nameFor(calendarKey) {
      const c = state.calendars[calendarKey];
      return c && c.name ? c.name : '';
   }

   function isFilteredOff(calendarKey) {
      return state.filterOff[calendarKey] === true;
   }

   function toggleFilter(calendarKey) {
      if (state.filterOff[calendarKey]) delete state.filterOff[calendarKey];
      else state.filterOff[calendarKey] = true;
   }

   /* Lifecycle-state setters — so every transition of the pane's own state
    * (active / dirty / error) funnels through the data layer, matching how the
    * ingest state (events / calendars) is already owned here. */
   function setActive(v) {
      state.active = !!v;
   }
   function setDirty(v) {
      state.dirty = !!v;
   }
   function setError(v) {
      state.error = v || null;
   }
   function setMode(m) {
      state.mode = m === 'list' ? 'list' : 'month';
   }
   function setAnchorMonth(year, month) {
      /* new Date normalizes month overflow/underflow (e.g. month -1 or 12). */
      const d = new Date(year, month, 1);
      state.anchor = { year: d.getFullYear(), month: d.getMonth() };
   }
   function shiftMonth(delta) {
      setAnchorMonth(state.anchor.year, state.anchor.month + delta);
   }
   function setSelectedKey(k) {
      if (k) state.selectedKey = k;
   }

   window.DawnCalendarData = {
      state: state,
      computeWindow: computeWindow,
      getWeekStart: getWeekStart,
      keyFromDate: keyFromDate,
      fromCalDav: fromCalDav,
      beginEventsFetch: beginEventsFetch,
      ingestEvents: ingestEvents,
      ingestCalendars: ingestCalendars,
      colorFor: colorFor,
      nameFor: nameFor,
      isFilteredOff: isFilteredOff,
      toggleFilter: toggleFilter,
      setActive: setActive,
      setDirty: setDirty,
      setError: setError,
      setMode: setMode,
      setAnchorMonth: setAnchorMonth,
      shiftMonth: shiftMonth,
      setSelectedKey: setSelectedKey,
   };
})();
