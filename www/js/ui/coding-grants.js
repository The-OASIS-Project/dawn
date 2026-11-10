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
 * The coding agent's standing approvals, at the foot of the jobs panel: the
 * hosts and tools allowed for every task on a repo (or every repo), each with
 * Revoke.  Fetched when the section is opened; everything rendered as text.
 */
(function (global) {
   'use strict';

   let box = null;
   let list = null;
   let status = null;
   let last = null; /* the last answer, shown when the section opens */
   let focusIdx = null; /* the revoked row's place: focus goes back near it */
   let revoking = 0; /* the grant this tab asked to remove (every tab gets each answer) */

   function elem(tag, cls, text) {
      const e = document.createElement(tag);
      if (cls) e.className = cls;
      if (text != null) e.textContent = String(text);
      return e;
   }

   function wsReady() {
      return typeof DawnWS !== 'undefined' && DawnWS.isConnected();
   }

   function ask(type, payload) {
      if (!wsReady()) return false;
      DawnWS.send({ type: type, payload: payload || {} });
      return true;
   }

   function say(text) {
      if (status) status.textContent = text;
   }

   function note(text) {
      if (list) list.replaceChildren(elem('div', 'coding-grants-note', text));
   }

   function row(g, idx) {
      const li = elem('li', 'coding-grant');
      const what = elem('span', 'coding-grant-what');
      const kind = g.kind === 'tool' ? 'Tool' : 'Host';
      what.appendChild(elem('span', 'coding-grant-kind', kind));
      const value = elem('span', 'coding-grant-value', g.value || '');
      value.setAttribute('dir', 'auto');
      what.appendChild(value);
      if (!g.checked) {
         what.appendChild(elem('span', 'coding-grant-odd', 'unrecognized value'));
      }
      const scope = g.repo ? 'every task on ' + g.repo : 'every repo';
      what.appendChild(elem('span', 'coding-grant-scope', scope));
      li.appendChild(what);
      const mine = g.id === revoking; /* this tab's revoke, still on its way */
      const btn = elem('button', 'coding-card-btn secondary', mine ? 'Revoking…' : 'Revoke');
      if (mine) btn.setAttribute('aria-disabled', 'true');
      btn.type = 'button';
      btn.setAttribute('aria-label', 'Revoke ' + kind + ' ' + (g.value || '') + ', ' + scope);
      btn.addEventListener('click', function () {
         if (btn.getAttribute('aria-disabled') === 'true') return;
         if (!ask('coding_grant_revoke', { id: g.id })) {
            say('Not connected.');
            return;
         }
         btn.setAttribute('aria-disabled', 'true');
         btn.textContent = 'Revoking…';
         focusIdx = idx;
         revoking = g.id;
      });
      li.appendChild(btn);
      return li;
   }

   function render(own) {
      if (!list || !last) return;
      if (!Array.isArray(last.grants)) {
         note(last.error || 'The coding agent didn’t answer.');
         return;
      }
      if (last.grants.length === 0) {
         note('None: each task asks for any host or tool beyond the defaults.');
      } else {
         const ul = elem('ul', 'coding-grants-list');
         last.grants.forEach(function (g, i) {
            ul.appendChild(row(g, i));
         });
         list.replaceChildren(ul);
      }
      if (focusIdx !== null && own) {
         const btns = list.querySelectorAll('button');
         const target = btns.length ? btns[Math.min(focusIdx, btns.length - 1)] : null;
         (target || box.querySelector('summary')).focus();
         focusIdx = null;
      }
   }

   function handleResponse(payload) {
      if (!payload) return;
      last = payload;
      const own = !!payload.revoked && payload.revoked === revoking;
      if (own || (payload.error && revoking)) {
         revoking = 0; /* answered, or the request never reached the gateway */
      }
      if (own) {
         say(payload.ok ? 'Approval revoked.' : payload.message || 'It couldn’t be removed.');
         if (!payload.ok && typeof DawnToast !== 'undefined') {
            DawnToast.show(payload.message || 'The approval couldn’t be removed.', 'error');
         }
      }
      /* Offered once the gateway answers: approvals outlive the jobs that made them. */
      if (box && Array.isArray(payload.grants)) box.classList.remove('hidden');
      if (box && box.open) render(own || !!payload.error);
   }

   /* The jobs panel opened: what stands now (a grant may have come from chat). */
   function refresh() {
      ask('coding_grants');
   }

   function init() {
      box = document.getElementById('coding-grants');
      list = document.getElementById('coding-grants-list');
      status = document.getElementById('coding-grants-status');
      if (!box || !list) return;
      box.addEventListener('toggle', function () {
         if (!box.open) return;
         if (last) {
            render(false);
         } else {
            note('Loading…');
         }
         if (!ask('coding_grants')) note('Not connected.');
      });
   }

   if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', init);
   } else {
      init();
   }

   global.DawnCodingGrants = {
      handleResponse: handleResponse,
      refresh: refresh,
   };
})(window);
