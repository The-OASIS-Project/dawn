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
 * Time zone choices for a <select>: every zone the browser knows, sorted by
 * its offset now and labeled with it.  The server checks the name on save.
 */

(function () {
   'use strict';

   let zones = null; // [{ value, label, offset }], built once

   /* Minutes east of UTC for @p zone now, from "GMT-05:00" / "GMT"; null when
    * the browser doesn't know the zone. */
   function offsetMinutes(zone) {
      try {
         const part = new Intl.DateTimeFormat('en-US', {
            timeZone: zone,
            timeZoneName: 'longOffset',
         })
            .formatToParts(new Date())
            .find((p) => p.type === 'timeZoneName');
         const m = /GMT([+-])(\d{2}):(\d{2})/.exec(part ? part.value : '');
         return m ? (m[1] === '-' ? -1 : 1) * (Number(m[2]) * 60 + Number(m[3])) : 0;
      } catch (e) {
         return null;
      }
   }

   function label(zone, offset) {
      if (offset === null) return zone;
      const sign = offset < 0 ? '-' : '+';
      const abs = Math.abs(offset);
      const hh = String(Math.floor(abs / 60)).padStart(2, '0');
      const mm = String(abs % 60).padStart(2, '0');
      return `(UTC${sign}${hh}:${mm}) ${zone}`;
   }

   function list() {
      if (zones) return zones;
      const names =
         typeof Intl.supportedValuesOf === 'function' ? Intl.supportedValuesOf('timeZone') : [];
      /* Chrome's list has no UTC */
      const all = names.includes('UTC') ? names : names.concat('UTC');
      zones = all
         .map((zone) => {
            const offset = offsetMinutes(zone);
            return { value: zone, label: label(zone, offset), offset: offset || 0 };
         })
         .sort((a, b) => a.offset - b.offset || a.value.localeCompare(b.value));
      return zones;
   }

   /* @p value as the list spells it (the server stores current names,
    * "Asia/Kolkata", where a browser may list "Asia/Calcutta"). */
   function listed(value) {
      if (!value || list().some((zone) => zone.value === value)) return value;
      try {
         const spelled = new Intl.DateTimeFormat('en-US', { timeZone: value }).resolvedOptions()
            .timeZone;
         if (list().some((zone) => zone.value === spelled)) return spelled;
      } catch (e) {
         /* a zone the browser doesn't know: kept as it is */
      }
      return value;
   }

   /**
    * Fill @p select with @p firstLabel (value "") and every zone, once, and
    * select @p current (added when the list lacks it, so a save can't
    * silently replace it).
    */
   function fill(select, current, firstLabel) {
      if (!select) return;
      if (select.dataset.tzFirst !== firstLabel) {
         select.replaceChildren(new Option(firstLabel, ''));
         for (const zone of list()) select.add(new Option(zone.label, zone.value));
         select.dataset.tzFirst = firstLabel;
      }
      const value = listed(current || '');
      if (value && ![...select.options].some((o) => o.value === value))
         select.add(new Option(label(value, offsetMinutes(value)), value));
      select.value = value;
   }

   window.DawnTimezones = { fill };
})();
