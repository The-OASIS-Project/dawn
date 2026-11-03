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
 * DAWN Memory: what memory learned per batch (DawnMemoryBatches)
 *
 * Each time memory consolidates a conversation it records what it did (a
 * batch).  The Memory panel's Learned tab lists them, newest first, a page at
 * a time; the conversation marker (memory-marker.js) shows one batch where it
 * happened.  Both draw a batch with renderBatch, and every Undo, Keep and
 * Forget button, wherever it is, is handled here.  Shown only when the daemon
 * advertises the memory_batches flag.  Its own module because memory.js is
 * over the JS file-size limit; memory.js only calls showTab and loadMore.
 */
(function () {
   'use strict';

   const PAGE_LIMIT = 20;

   /* What each op says, as a label before its text. */
   const OP_LABELS = {
      added: 'Learned',
      reinforced: 'Confirmed',
      merged: 'Merged',
      replaced: 'Replaced',
      closed: 'No longer true',
      linked: 'Same as',
      pref_set: 'Preference',
      dropped: 'Not kept',
      deferred: 'Set aside',
      suggested_forget: 'Maybe forget',
   };

   /* An op's status when it isn't simply applied: its badge, tone and a
    * description for assistive technology. */
   const STATUS = {
      undone: { label: 'Undone', tone: 'muted', help: 'This was undone.' },
      stale: {
         label: 'Changed since',
         tone: 'warning',
         help: 'Changed since this batch, so it can no longer be undone.',
      },
      kept: { label: 'Kept', tone: 'success', help: 'You kept this.' },
      consumed: {
         label: 'Taken up later',
         tone: 'muted',
         help: 'A later batch decided this.',
      },
      expired: { label: 'Expired', tone: 'muted', help: 'Set aside too long ago.' },
      proposed: { label: 'Proposed', tone: 'muted', help: 'Waiting for your decision.' },
   };

   /* The counterpart, as the target line names it. */
   const TARGET_LABELS = { merged: 'into', replaced: 'by', closed: 'by', linked: 'with' };

   const ACTION_LABELS = { undo: 'Undo', keep: 'Keep', forget: 'Forget' };

   let enabled = false;
   const tab = {
      active: false,
      list: null,
      loadMoreBtn: null,
      nextBefore: null,
      hasMore: false,
      loading: false,
   };
   const pendingForgets = new Set(); /* fact ids a Forget was sent for */
   let liveRegion = null;

   function connected() {
      return typeof DawnWS !== 'undefined' && DawnWS.isConnected();
   }

   function toast(text, type) {
      if (typeof DawnToast !== 'undefined') DawnToast.show(text, type || 'info');
   }

   function plural(n, one, many) {
      return n + ' ' + (n === 1 ? one : many);
   }

   function formatWhen(seconds) {
      if (!seconds) return '';
      try {
         return DawnTime.format(seconds, {
            month: 'short',
            day: 'numeric',
            hour: 'numeric',
            minute: '2-digit',
         });
      } catch (e) {
         return '';
      }
   }

   function badge(text, tone) {
      const b = document.createElement('span');
      b.className = 'dawn-badge ' + (tone || 'muted');
      b.textContent = text;
      return b;
   }

   function srOnly(text) {
      const s = document.createElement('span');
      s.className = 'sr-only';
      s.textContent = text;
      return s;
   }

   function shorten(text) {
      const t = text || '';
      return t.length > 60 ? t.slice(0, 60) + '…' : t;
   }

   /* "3 learned, 2 confirmed, ..." from the batch's counts. */
   function summaryText(batch) {
      const c = batch.counts || {};
      const parts = [];
      const add = (n, one, many) => {
         if (n > 0) parts.push(plural(n, one, many));
      };
      add(c.added, 'learned', 'learned');
      add(c.reinforced, 'confirmed', 'confirmed');
      add((c.merged || 0) + (c.replaced || 0) + (c.closed || 0), 'updated', 'updated');
      add((c.linked || 0) + (c.proposed || 0), 'link', 'links');
      add(c.pref_set, 'preference', 'preferences');
      add((c.dropped || 0) + (c.deferred || 0), 'not kept', 'not kept');
      add(c.suggested_forget, 'to review', 'to review');
      return parts.length ? parts.join(', ') : 'Nothing new';
   }

   /* ---- rendering ---- */

   function actionButton(action, batch, op) {
      const btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'memory-batch-action' + (action === 'forget' ? ' danger' : '');
      btn.dataset.action = action;
      btn.dataset.batchId = String(batch.batch_id);
      if (op) {
         btn.dataset.opId = String(op.op_id);
         if (op.fact_id) btn.dataset.factId = String(op.fact_id);
         btn.textContent = ACTION_LABELS[action];
         btn.setAttribute('aria-label', ACTION_LABELS[action] + ': ' + shorten(op.text));
      } else {
         const when = formatWhen(batch.created_at);
         btn.textContent = 'Undo all';
         btn.setAttribute(
            'aria-label',
            'Undo all of the batch from ' + (when || 'earlier') + ': ' + summaryText(batch)
         );
      }
      return btn;
   }

   function renderOp(op, batch) {
      const item = document.createElement('div');
      item.className = 'memory-item memory-batch-op';
      item.setAttribute('role', 'listitem');
      item.dataset.opId = String(op.op_id);
      item.dataset.op = op.op;
      item.dataset.status = op.status;
      if (op.fact_id) item.dataset.factId = String(op.fact_id);

      const header = document.createElement('div');
      header.className = 'memory-item-header';
      const text = document.createElement('div');
      text.className = 'memory-item-text';
      const label = document.createElement('span');
      label.className = 'memory-batch-op-label';
      label.textContent = (OP_LABELS[op.op] || op.op) + ': ';
      text.appendChild(label);
      /* Every string from the record goes in as text, never markup. */
      const words = document.createElement('span');
      words.className = 'memory-batch-op-text';
      words.textContent = op.text || '(forgotten)';
      text.appendChild(words);
      header.appendChild(text);
      if (op.action && ACTION_LABELS[op.action]) {
         header.appendChild(actionButton(op.action, batch, op));
      }
      item.appendChild(header);

      const meta = document.createElement('div');
      meta.className = 'memory-item-meta';
      const status = STATUS[op.status];
      if (status) {
         const b = badge(status.label, status.tone);
         b.title = status.help;
         meta.appendChild(b);
         meta.appendChild(srOnly(status.help));
      }
      if (op.outside) {
         const b = badge('Outside text', 'warning soft');
         b.title = 'Learned while reading text from outside (an email, a web page)';
         meta.appendChild(b);
      }
      if (op.target_text) {
         const target = document.createElement('span');
         target.className = 'memory-batch-target';
         const since =
            op.target_status === 'gone'
               ? ' (since deleted)'
               : op.target_status === 'superseded'
                 ? ' (since changed)'
                 : '';
         target.textContent = (TARGET_LABELS[op.op] || '→') + ' ' + op.target_text + since;
         meta.appendChild(target);
      }
      if (Array.isArray(op.sources) && op.sources.length > 0) {
         const sources = document.createElement('span');
         sources.className = 'memory-batch-sources';
         sources.textContent =
            'Merged in: ' + op.sources.map((s) => s.text || '(forgotten)').join('; ');
         meta.appendChild(sources);
      }
      if (meta.childNodes.length > 0) item.appendChild(meta);

      if (op.reason) {
         const reason = document.createElement('div');
         reason.className = 'memory-batch-reason';
         const who = document.createElement('span');
         who.className = 'memory-batch-reason-label';
         /* The model's words are shown as its own. */
         who.textContent = op.reason_by === 'model' ? "Model's reason: " : 'Reason: ';
         reason.appendChild(who);
         reason.appendChild(document.createTextNode(op.reason));
         item.appendChild(reason);
      }
      return item;
   }

   /**
    * One batch: a head (when, a summary, its badges, Undo all) and its ops.
    * @param {object} batch The batch object
    * @param {object} [opts] {where: 'tab' | 'marker'}
    */
   function renderBatch(batch, opts) {
      const where = (opts && opts.where) || 'tab';
      const box = document.createElement('div');
      box.className = 'memory-batch';
      box.setAttribute('role', 'group');
      box.dataset.batchId = String(batch.batch_id);
      box.dataset.status = batch.status || 'applied';
      const headId = 'memory-batch-' + batch.batch_id + '-' + where;
      box.setAttribute('aria-labelledby', headId);

      const head = document.createElement('div');
      head.className = 'memory-batch-head';
      const title = document.createElement('div');
      /* In the marker the summary line says it; the title stays as the name. */
      title.className = 'memory-batch-title' + (where === 'marker' ? ' sr-only' : '');
      title.id = headId;
      const when = formatWhen(batch.created_at);
      if (when) {
         const time = document.createElement('time');
         time.className = 'memory-batch-when';
         time.dateTime = new Date(batch.created_at * 1000).toISOString();
         time.textContent = when;
         title.appendChild(time);
      }
      const counts = document.createElement('div');
      counts.className = 'memory-batch-counts';
      counts.textContent = summaryText(batch);
      title.appendChild(counts);
      head.appendChild(title);
      if (batch.status === 'undone') head.appendChild(badge('Undone'));
      else if (batch.status === 'partial') head.appendChild(badge('Partly undone'));
      if (batch.outside && where !== 'marker') {
         const b = badge('Read outside text', 'warning soft');
         b.title = 'This conversation included text from outside (an email, a web page)';
         head.appendChild(b);
      }
      const ops = Array.isArray(batch.ops) ? batch.ops : [];
      if (ops.filter((o) => o.action === 'undo').length > 1) {
         head.appendChild(actionButton('undo', batch, null));
      }
      box.appendChild(head);

      const list = document.createElement('div');
      list.className = 'memory-batch-ops';
      list.setAttribute('role', 'list');
      ops.forEach((op) => list.appendChild(renderOp(op, batch)));
      box.appendChild(list);
      return box;
   }

   /**
    * Put @p fresh where @p old is.  When the keyboard was in @p old, it moves
    * to the same button in @p fresh if that is still there, else to the
    * marker's summary or the batch's title.
    */
   function replaceKeepingFocus(old, fresh) {
      const focused = document.activeElement;
      const was = focused && old.contains(focused) ? focused : null;
      old.replaceWith(fresh);
      if (!was) return;
      let target = null;
      if (was.dataset && was.dataset.action) {
         target = fresh.querySelector(
            `.memory-batch-action[data-action="${was.dataset.action}"]` +
               (was.dataset.opId ? `[data-op-id="${was.dataset.opId}"]` : ':not([data-op-id])')
         );
      }
      if (!target) target = fresh.querySelector('summary');
      if (!target) {
         target = fresh.querySelector('.memory-batch-title');
         if (target) target.setAttribute('tabindex', '-1');
      }
      if (target) target.focus();
   }

   /* ---- the Learned tab ---- */

   function request(before) {
      if (!connected()) {
         showTabError('Not connected; try again in a moment');
         return;
      }
      tab.loading = true;
      const payload = { limit: PAGE_LIMIT };
      if (before) payload.before_batch_id = before;
      DawnWS.send({ type: 'list_memory_batches', payload: payload });
   }

   function setTabStatus(text) {
      if (!tab.list) return;
      tab.list.textContent = '';
      const empty = document.createElement('div');
      empty.className = 'memory-empty';
      empty.textContent = text;
      tab.list.appendChild(empty);
   }

   /* The first page failed: say so, with a way to try again. */
   function showTabError(text) {
      if (!tab.list) return;
      tab.list.textContent = '';
      const box = document.createElement('div');
      box.className = 'memory-empty';
      box.setAttribute('role', 'alert');
      box.appendChild(document.createTextNode(text + ' '));
      const retry = document.createElement('button');
      retry.type = 'button';
      retry.className = 'memory-batch-action';
      retry.textContent = 'Try again';
      retry.addEventListener('click', () => {
         setTabStatus('Loading…');
         request(null);
      });
      box.appendChild(retry);
      tab.list.appendChild(box);
   }

   function syncLoadMore() {
      if (!tab.loadMoreBtn) return;
      tab.loadMoreBtn.classList.toggle('hidden', !tab.hasMore);
      tab.loadMoreBtn.disabled = tab.loading || !tab.hasMore;
   }

   /** The Learned tab is shown in @p list: its first page. */
   function showTab(list, loadMoreBtn) {
      tab.active = true;
      tab.list = list;
      tab.loadMoreBtn = loadMoreBtn;
      tab.nextBefore = null;
      tab.hasMore = false;
      setTabStatus('Loading…');
      syncLoadMore();
      request(null);
   }

   /** Another tab is shown. */
   function hideTab() {
      tab.active = false;
   }

   function loadMore() {
      if (!tab.active || tab.loading || !tab.hasMore) return;
      request(tab.nextBefore);
      syncLoadMore();
   }

   function tabBody() {
      if (!tab.list) return null;
      let body = tab.list.querySelector('.memory-batches');
      if (!body) {
         tab.list.textContent = '';
         body = document.createElement('div');
         body.className = 'memory-batches';
         tab.list.appendChild(body);
      }
      return body;
   }

   function handleTabList(payload) {
      tab.loading = false;
      if (!tab.active) return;
      const first = payload.before_batch_id == null;
      if (payload.error) {
         /* A later page failing keeps what is shown. */
         if (first) showTabError(payload.error);
         else toast(payload.error, 'error');
         syncLoadMore();
         return;
      }
      const batches = Array.isArray(payload.batches) ? payload.batches : [];
      if (first && batches.length === 0) {
         setTabStatus('Nothing learned yet. Batches show here after a conversation ends.');
      } else {
         if (first) tab.list.textContent = '';
         const body = tabBody();
         batches.forEach((b) => body.appendChild(renderBatch(b, { where: 'tab' })));
      }
      tab.hasMore = !!payload.has_more;
      tab.nextBefore = payload.has_more ? payload.next_before_batch_id : null;
      syncLoadMore();
   }

   /* ---- frames ---- */

   function handleListResponse(payload) {
      if (!payload) return;
      if (payload.conversation_id != null) {
         if (window.DawnMemoryMarker) DawnMemoryMarker.handleList(payload);
         return;
      }
      handleTabList(payload);
   }

   function announce(text) {
      if (!liveRegion) {
         liveRegion = document.createElement('div');
         liveRegion.className = 'sr-only';
         liveRegion.setAttribute('role', 'status');
         liveRegion.setAttribute('aria-live', 'polite');
         document.body.appendChild(liveRegion);
      }
      liveRegion.textContent = text;
   }

   /* A batch recorded or changed (memory_batch): redrawn where it is shown. */
   function handleBatch(batch) {
      if (!batch || batch.batch_id == null) return;
      let isNew = true;
      if (tab.active && tab.list) {
         const old = tab.list.querySelector(`.memory-batch[data-batch-id="${batch.batch_id}"]`);
         if (old) {
            isNew = false;
            replaceKeepingFocus(old, renderBatch(batch, { where: 'tab' }));
         } else {
            const body = tabBody();
            const firstBatch = body.querySelector('.memory-batch');
            const newest = !firstBatch || Number(firstBatch.dataset.batchId) < batch.batch_id;
            if (newest) body.insertBefore(renderBatch(batch, { where: 'tab' }), firstBatch);
         }
      }
      if (window.DawnMemoryMarker) {
         isNew = DawnMemoryMarker.handleBatch(batch) && isNew;
      }
      if (isNew && batch.status === 'applied') announce('Memory updated: ' + summaryText(batch));
   }

   function reenable(batchId) {
      document
         .querySelectorAll(`.memory-batch[data-batch-id="${batchId}"] .memory-batch-action`)
         .forEach((b) => {
            b.disabled = false;
         });
   }

   function handleUndoResponse(payload) {
      if (!payload) return;
      reenable(payload.batch_id);
      const undone = (payload.undone || []).length;
      const stale = (payload.stale || []).length;
      if (payload.status === 'ok') {
         toast(undone === 0 ? 'Nothing left to undo' : 'Undone', 'success');
      } else if (payload.status === 'partial') {
         toast(
            `Undid ${undone}; ${plural(stale, 'item', 'items')} changed since and stayed as ${stale === 1 ? 'it is' : 'they are'}.`,
            'warning'
         );
      } else if (payload.status === 'stale') {
         toast(payload.error || 'This memory changed since; nothing was undone.', 'warning');
      } else {
         toast(payload.error || "Couldn't undo this.", 'error');
      }
   }

   function handleKeepResponse(payload) {
      if (!payload) return;
      reenable(payload.batch_id);
      if (payload.status === 'ok') toast('Kept', 'success');
      else toast(payload.error || "Couldn't keep this.", 'error');
   }

   /* delete_memory_fact_response for a Forget sent from here: the suggestion
    * leaves every view (the forget removed its row), the keyboard kept near. */
   function handleFactDeleted(payload) {
      if (!payload || payload.fact_id == null) return;
      const factId = String(payload.fact_id);
      if (!pendingForgets.delete(factId)) return;
      document
         .querySelectorAll(`.memory-batch-op[data-op="suggested_forget"][data-fact-id="${factId}"]`)
         .forEach((row) => {
            if (!payload.success) {
               const btn = row.querySelector('.memory-batch-action');
               if (btn) btn.disabled = false;
               return;
            }
            const focused = row.contains(document.activeElement);
            const next = row.nextElementSibling || row.previousElementSibling;
            const batch = row.closest('.memory-batch');
            row.remove();
            if (focused) {
               const target =
                  (next && next.querySelector('.memory-batch-action')) ||
                  (batch &&
                     batch.closest('details') &&
                     batch.closest('details').querySelector('summary')) ||
                  (batch && batch.querySelector('.memory-batch-title'));
               if (target) {
                  if (!target.matches('button, summary')) target.setAttribute('tabindex', '-1');
                  target.focus();
               }
            }
         });
   }

   /* ---- actions ---- */

   function confirmDialog(title, detail, label) {
      if (typeof DawnDialog === 'undefined') return Promise.resolve(true);
      return DawnDialog.confirm(title, { detail: detail, danger: true, okText: label });
   }

   async function act(btn) {
      const batchId = Number(btn.dataset.batchId);
      const opId = btn.dataset.opId ? Number(btn.dataset.opId) : null;
      if (!Number.isFinite(batchId) || batchId <= 0) return;
      if (!connected()) {
         toast('Not connected; try again in a moment', 'error');
         return;
      }
      const action = btn.dataset.action;
      const row = btn.closest('.memory-batch-op');
      const text = row ? row.querySelector('.memory-batch-op-text').textContent : '';
      if (action === 'forget') {
         const factId = btn.dataset.factId;
         if (!factId) return;
         if (!(await confirmDialog('Forget this memory?', text, 'Forget'))) return;
         btn.disabled = true;
         pendingForgets.add(factId);
         DawnWS.send({ type: 'delete_memory_fact', payload: { fact_id: Number(factId) } });
         return;
      }
      if (action === 'undo' && !opId) {
         const batch = btn.closest('.memory-batch');
         const n = batch ? batch.querySelectorAll('.memory-batch-action[data-op-id]').length : 0;
         const counts = batch ? batch.querySelector('.memory-batch-counts').textContent : '';
         const ok = await confirmDialog(
            'Undo ' + plural(n, 'change', 'changes') + '?',
            counts + '. What changed since stays as it is.',
            'Undo all'
         );
         if (!ok) return;
      }
      btn.disabled = true;
      if (action === 'keep') {
         DawnWS.send({ type: 'keep_memory_op', payload: { batch_id: batchId, op_id: opId } });
      } else {
         const payload = { batch_id: batchId };
         if (opId) payload.op_id = opId;
         DawnWS.send({ type: 'undo_memory_batch', payload: payload });
      }
   }

   /** The daemon's features: shows or hides the Learned tab and the markers. */
   function setEnabled(on) {
      enabled = !!on;
      const tabBtn = document.getElementById('memory-tab-learned');
      if (!tabBtn) return;
      tabBtn.hidden = !enabled;
      /* Shown on a daemon that no longer has it: back to Facts. */
      if (!enabled && tabBtn.getAttribute('aria-selected') === 'true' && window.DawnMemory) {
         DawnMemory.switchTab('facts');
      }
   }

   function isEnabled() {
      return enabled;
   }

   function init() {
      document.addEventListener('click', (e) => {
         const btn = e.target.closest && e.target.closest('.memory-batch-action[data-action]');
         if (btn && !btn.disabled) act(btn);
      });
   }

   window.DawnMemoryBatches = {
      init,
      setEnabled,
      isEnabled,
      renderBatch,
      summaryText,
      replaceKeepingFocus,
      showTab,
      hideTab,
      loadMore,
      handleListResponse,
      handleBatch,
      handleUndoResponse,
      handleKeepResponse,
      handleFactDeleted,
   };

   if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', init);
   } else {
      init();
   }
})();
