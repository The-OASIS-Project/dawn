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
 * Login expiry warning: the daemon sends the login's expires_at (epoch
 * seconds) on the session frame and every pong; a toast warns shortly before
 * it ends.  Use moves the expiry on, so in practice this shows only at the
 * 30-day cap or on a tab left alone for days.
 */
(function (global) {
   'use strict';

   const WARN_BEFORE_MS = 10 * 60 * 1000;
   /* Longer delays are re-checked: setTimeout can't wait 30 days */
   const MAX_WAIT_MS = 24 * 60 * 60 * 1000;

   let expiresAtMs = 0;
   let timer = null;
   let warnedFor = 0;
   let toast = null;

   function warn() {
      timer = null;
      const left = expiresAtMs - Date.now();
      if (left > WARN_BEFORE_MS) {
         schedule();
         return;
      }
      if (left <= 0 || warnedFor === expiresAtMs) {
         return;
      }
      warnedFor = expiresAtMs;
      const at = new Date(expiresAtMs).toLocaleTimeString([], {
         hour: 'numeric',
         minute: '2-digit',
      });
      if (global.DawnToast) {
         toast = DawnToast.show(
            'Your login ends at ' + at + '. You will need to sign in again then.',
            'warning',
            0
         );
      }
   }

   function schedule() {
      if (timer) {
         clearTimeout(timer);
         timer = null;
      }
      if (expiresAtMs <= 0) {
         return;
      }
      const wait = Math.min(Math.max(expiresAtMs - WARN_BEFORE_MS - Date.now(), 0), MAX_WAIT_MS);
      timer = setTimeout(warn, wait);
   }

   /**
    * Note the login's expiry from a server frame.
    * @param {number} expiresAt - epoch seconds; absent, 0 or not a number is ignored
    */
   function note(expiresAt) {
      if (typeof expiresAt !== 'number' || !(expiresAt > 0)) {
         return;
      }
      const ms = expiresAt * 1000;
      if (ms === expiresAtMs) {
         return;
      }
      expiresAtMs = ms;
      /* Used since the warning: the old end time no longer holds */
      if (toast && typeof toast.dismiss === 'function') {
         toast.dismiss();
      }
      toast = null;
      schedule();
   }

   global.DawnLoginExpiry = { note: note };
})(window);
