/**
 * DAWN Time Module
 * Shows times in the user's zone (My Settings, else the server's), the zone Friday
 * uses, not the browser's.  The server sends it on connect (`session`) and when it
 * changes (`timezone_changed`); until then the browser's zone is used.
 *
 * Instants are epoch seconds.  A "civil date" is a local Date at midnight whose
 * year/month/day are a calendar date in the user's zone: calendar arithmetic
 * (next day, month grid) works on it with the usual Date getters, and it is
 * turned back into an instant only through startOf().
 *
 * Usage:
 *   DawnTime.format(epochSec, { hour: 'numeric', minute: '2-digit' })
 *   DawnTime.dayKey(epochSec)          // 'YYYY-MM-DD' in the user's zone
 *   DawnTime.startOf(DawnTime.today()) // epoch seconds of the user's midnight
 *   DawnEvents.on('timezone', fn)      // the zone changed: redraw
 */
(function (global) {
   'use strict';

   let zone; /* IANA name, or undefined for the browser's own */
   const formatters = new Map();

   const PARTS_OPTS = {
      year: 'numeric',
      month: 'numeric',
      day: 'numeric',
      hour: 'numeric',
      minute: 'numeric',
      second: 'numeric',
      weekday: 'short',
      hourCycle: 'h23',
   };
   const WEEKDAYS = { Sun: 0, Mon: 1, Tue: 2, Wed: 3, Thu: 4, Fri: 5, Sat: 6 };

   function formatter(locale, opts) {
      const key = JSON.stringify([locale, opts]);
      let f = formatters.get(key);
      if (!f) {
         f = new Intl.DateTimeFormat(locale, Object.assign({}, opts, { timeZone: zone }));
         formatters.set(key, f);
      }
      return f;
   }

   /** Use @p name for every time shown; '' or a name the browser doesn't know
    *  means the browser's zone. */
   function setZone(name) {
      let z;
      if (typeof name === 'string' && name) {
         try {
            new Intl.DateTimeFormat('en-US', { timeZone: name });
            z = name;
         } catch (e) {
            console.warn('DawnTime: unknown time zone', name);
         }
      }
      if (z === zone) return;
      zone = z;
      formatters.clear();
      if (typeof DawnEvents !== 'undefined') DawnEvents.emit('timezone', zone);
   }

   function getZone() {
      return zone;
   }

   /** @p epochSec formatted like Date.toLocale*String(locale, opts), in the zone. */
   function format(epochSec, opts, locale) {
      return formatter(locale || [], opts || {}).format(new Date(epochSec * 1000));
   }

   /** Date and time, as toLocaleString() with no options shows them. */
   function formatDateTime(epochSec) {
      return format(epochSec, {
         year: 'numeric',
         month: 'numeric',
         day: 'numeric',
         hour: 'numeric',
         minute: '2-digit',
         second: '2-digit',
      });
   }

   /** The date, as toLocaleDateString() with no options shows it. */
   function formatDate(epochSec) {
      return format(epochSec, { year: 'numeric', month: 'numeric', day: 'numeric' });
   }

   /** The zone's clock at @p epochSec: {year, month (1-12), day, hour, minute,
    *  second, weekday (0=Sun)}. */
   function parts(epochSec) {
      const out = {};
      const list = formatter('en-US', PARTS_OPTS).formatToParts(new Date(epochSec * 1000));
      for (let i = 0; i < list.length; i++) {
         const p = list[i];
         if (p.type === 'weekday') out.weekday = WEEKDAYS[p.value];
         else if (p.type !== 'literal') out[p.type] = Number(p.value);
      }
      out.hour %= 24; /* some engines print midnight as 24 */
      return out;
   }

   /** The civil date (see above) @p epochSec falls on. */
   function dateOf(epochSec) {
      const p = parts(epochSec);
      return new Date(p.year, p.month - 1, p.day);
   }

   function nowSec() {
      return Math.floor(Date.now() / 1000);
   }

   /** Today's civil date. */
   function today() {
      return dateOf(nowSec());
   }

   function keyOf(civil) {
      const m = String(civil.getMonth() + 1).padStart(2, '0');
      const d = String(civil.getDate()).padStart(2, '0');
      return civil.getFullYear() + '-' + m + '-' + d;
   }

   /** 'YYYY-MM-DD' of the date @p epochSec falls on. */
   function dayKey(epochSec) {
      return keyOf(dateOf(epochSec));
   }

   /* Seconds the zone's clock is ahead of UTC at @p epochSec. */
   function offsetAt(epochSec) {
      const p = parts(epochSec);
      const wall = Date.UTC(p.year, p.month - 1, p.day, p.hour, p.minute, p.second) / 1000;
      return wall - Math.floor(epochSec);
   }

   /** The first instant of civil date @p civil in the zone (epoch seconds). */
   function startOf(civil) {
      const y = civil.getFullYear();
      const m = civil.getMonth();
      const d = civil.getDate();
      const wall = Date.UTC(y, m, d) / 1000;
      let t = wall - offsetAt(wall);
      t = wall - offsetAt(t); /* again, in case the offset changed in between */
      /* Midnight skipped by a clock change: t reads on the day before; the day
       * starts when the clock jumps, which is where that reading reaches 24:00. */
      const p = parts(t);
      if (p.day !== d || p.month !== m + 1 || p.year !== y) {
         t += 86400 - (p.hour * 3600 + p.minute * 60 + p.second);
      }
      return t;
   }

   global.DawnTime = {
      setZone: setZone,
      getZone: getZone,
      format: format,
      formatDateTime: formatDateTime,
      formatDate: formatDate,
      parts: parts,
      dateOf: dateOf,
      today: today,
      keyOf: keyOf,
      dayKey: dayKey,
      startOf: startOf,
      nowSec: nowSec,
   };
})(window);
