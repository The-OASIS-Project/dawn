/*
 * DAWN WebUI — Calendar pane (DawnCalendar).
 *
 * A browsable CalDAV calendar as a tab inside the Scheduler & Watches popover.
 * Phase 1 ships List mode (a day-grouped agenda of the next 30 days); the month
 * grid is Phase 2.  Rendering + WS I/O live here; all state, window math, and the
 * source-agnostic occurrence model live in DawnCalendarData (calendar-data.js).
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

   /* Trailing-edge debounce for calendar_events_changed: a multi-account sync
    * fires a burst, and a change during an in-flight fetch must refetch AFTER it
    * (the in-flight response may predate the change; the window-echo guard drops
    * a superseded response, and this schedules the fresh read). */
   const EVENTS_CHANGED_DEBOUNCE_MS = 300;

   const els = { pane: null, legend: null, body: null };
   let changeTimer = null;

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

   function keyFromDate(dt) {
      const y = dt.getFullYear();
      const m = String(dt.getMonth() + 1).padStart(2, '0');
      const d = String(dt.getDate()).padStart(2, '0');
      return y + '-' + m + '-' + d;
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
      return (
         '<div class="cal-row" data-cal-key="' +
         escapeAttr(ev.calendarKey) +
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

   /* Group visible (filter-applied) events by local day, ascending. All-day
    * events key on their startDate string; timed on the local date of start.
    * Within a day the global start-order is preserved, so all-day (midnight)
    * sorts above timed. */
   function groupByDay(events) {
      const groups = [];
      const index = {};
      for (let i = 0; i < events.length; i++) {
         const ev = events[i];
         if (Data.isFilteredOff(ev.calendarKey)) continue;
         const key = ev.allDay && ev.startDate ? ev.startDate : localDateKey(ev.start);
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

   /* Render gate: only touch the DOM while the pane is the visible tab in an
    * open popover (state.active). A late response after close updates state and
    * returns here early — no paint into a hidden pane. */
   function render() {
      if (!Data.state.active) return;
      renderLegend();
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
      if (s.events.length === 0) {
         if (s.calendarsLoaded && s.calendarOrder.length === 0) {
            els.body.innerHTML = simpleStateHTML(
               'No calendars',
               'Add a CalDAV account to see your events here.',
               { id: 'add-account', label: 'Add a calendar account' }
            );
         } else {
            els.body.innerHTML = simpleStateHTML(
               'Nothing scheduled',
               'No events in the next 30 days.',
               null
            );
            announce('No events in the next 30 days');
         }
         return;
      }

      /* Events exist; apply the client-side per-calendar filter. */
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

   /* ---- events ----------------------------------------------------------- */

   function onBodyClick(e) {
      const action = e.target.closest ? e.target.closest('[data-cal-action]') : null;
      if (!action) return;
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
   }

   /* ---- lifecycle / public API ------------------------------------------ */

   function activate() {
      const s = Data.state;
      Data.setActive(true);
      render(); /* show cached state (or the loading placeholder) immediately */
      if (!s.eventsLoaded || s.dirty || s.error) {
         Data.setDirty(false);
         load();
      }
   }

   function deactivate() {
      Data.setActive(false);
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
      if (!els.pane || !els.body) return;
      els.body.addEventListener('click', onBodyClick);
      if (els.legend) els.legend.addEventListener('click', onLegendClick);
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
