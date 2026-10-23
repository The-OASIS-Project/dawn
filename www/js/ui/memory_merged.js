/*
 * DAWN Memory: merged and replaced facts (DawnMemoryMerged)
 *
 * A fact merged into another (a duplicate cleanup) or replaced by a newer value
 * leaves recall but is kept for a while.  This dialog lists them, newest first
 * (filterable by text), and restores one.  Its own module because memory.js is
 * over the JS file-size limit; memory.js only calls init().
 */
(function () {
   'use strict';

   const SEARCH_DEBOUNCE_MS = 250;

   let onMemoriesChanged = null;
   let escToken = null;
   let trigger = null;
   let searchTimer = null;
   let lastQuery = '';
   let trapCleanup = null;

   function el(id) {
      return document.getElementById(id);
   }

   function connected() {
      return typeof DawnWS !== 'undefined' && DawnWS.isConnected();
   }

   function request(query) {
      lastQuery = query || '';
      if (!connected()) {
         setStatus('Not connected; try again in a moment', true);
         return;
      }
      DawnWS.send({ type: 'list_memory_superseded', payload: { query: lastQuery } });
   }

   /* The one live region: loading, counts, empty and error text.  @p clear also
    * empties the list. */
   function setStatus(text, clear) {
      const status = el('memory-merged-status');
      if (status) status.textContent = text;
      const body = el('memory-merged-body');
      if (clear && body) body.textContent = '';
   }

   function emptyText() {
      return lastQuery ? 'No duplicates or old values match.' : 'No duplicates or old values.';
   }

   function open() {
      const modal = el('memory-merged-modal');
      if (!modal) return;
      trigger = document.activeElement;
      modal.classList.remove('hidden');
      const M = window.DawnSettingsModals;
      if (trapCleanup) trapCleanup();
      trapCleanup = M && typeof M.trapFocus === 'function' ? M.trapFocus(modal) : null;
      if (escToken === null && typeof DawnEscStack !== 'undefined') {
         escToken = DawnEscStack.register(() => {
            close();
            return true;
         });
      }
      const search = el('memory-merged-search');
      if (search) {
         search.value = '';
         /* On a touch screen focusing the search box opens the keyboard over
          * the list; the trap's first focus (the close button) is enough. */
         if (window.matchMedia && window.matchMedia('(pointer: fine)').matches) search.focus();
      }
      setStatus('Loading…', true);
      request('');
   }

   function close() {
      const modal = el('memory-merged-modal');
      if (modal) modal.classList.add('hidden');
      clearTimeout(searchTimer);
      if (trapCleanup) {
         trapCleanup();
         trapCleanup = null;
      }
      if (escToken !== null && typeof DawnEscStack !== 'undefined') {
         DawnEscStack.unregister(escToken);
         escToken = null;
      }
      if (trigger && typeof trigger.focus === 'function') {
         trigger.focus();
      }
      trigger = null;
   }

   function formatWhen(seconds) {
      if (!seconds) return '';
      try {
         return DawnTime.format(seconds, { month: 'short', day: 'numeric', year: 'numeric' });
      } catch (e) {
         return '';
      }
   }

   function renderRow(f) {
      const item = document.createElement('div');
      item.className = 'memory-item fact';
      item.setAttribute('role', 'listitem');
      item.dataset.factId = String(f.id);

      const header = document.createElement('div');
      header.className = 'memory-item-header';
      const text = document.createElement('div');
      text.className = 'memory-item-text';
      text.textContent = f.text || '';
      const restore = document.createElement('button');
      restore.className = 'btn-link memory-merged-restore';
      restore.dataset.factId = String(f.id);
      restore.textContent = 'Restore';
      const restoreLabel = (f.text || '').length > 60 ? f.text.slice(0, 60) + '…' : f.text || '';
      restore.setAttribute('aria-label', 'Restore: ' + restoreLabel);
      header.appendChild(text);
      header.appendChild(restore);

      const meta = document.createElement('div');
      meta.className = 'memory-item-meta';
      const kept = document.createElement('span');
      kept.className = 'memory-merged-keeper';
      /* The fact it went into, which may itself have changed since. */
      const keeperLabel = f.replaced ? 'Replaced by: ' : 'Duplicate of: ';
      kept.textContent = keeperLabel + (f.keeper_text || '(since deleted)');
      meta.appendChild(kept);
      const when = formatWhen(f.superseded_at);
      if (when) {
         const date = document.createElement('span');
         date.className = 'memory-item-date';
         date.textContent = when;
         meta.appendChild(date);
      }
      item.appendChild(header);
      item.appendChild(meta);
      return item;
   }

   function handleListResponse(payload) {
      const modal = el('memory-merged-modal');
      if (!modal || modal.classList.contains('hidden')) return;
      /* A reply to an older search (typing on, or a reopen) is dropped. */
      if (payload && typeof payload.query === 'string' && payload.query !== lastQuery) return;
      if (!payload || !payload.success) {
         setStatus((payload && payload.error) || "Couldn't load duplicates and old values.", true);
         return;
      }
      const facts = Array.isArray(payload.facts) ? payload.facts : [];
      if (facts.length === 0) {
         setStatus(emptyText(), true);
         return;
      }
      const body = el('memory-merged-body');
      body.textContent = '';
      facts.forEach((f) => body.appendChild(renderRow(f)));
      setStatus(
         payload.truncated
            ? `Showing the ${facts.length} newest; search to find others`
            : facts.length === 1
              ? '1 fact'
              : `${facts.length} facts`,
         false
      );
   }

   function handleRestoreResponse(payload) {
      if (!payload || !payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show((payload && payload.error) || "Couldn't restore the fact", 'error');
         }
         request(lastQuery);
         /* The list is rebuilt: keep the keyboard in the dialog. */
         const search = el('memory-merged-search');
         if (search) search.focus();
         return;
      }
      if (typeof DawnToast !== 'undefined') {
         DawnToast.show(
            payload.both_current
               ? 'Restored. The newer value is still current too; delete whichever is wrong.'
               : 'Restored',
            'success'
         );
      }
      const body = el('memory-merged-body');
      const row = body && body.querySelector(`.memory-item[data-fact-id="${payload.fact_id}"]`);
      if (row) {
         /* If the keyboard was on this row (or nowhere), keep it near: the next
          * row's Restore still usable, else the previous one's, else the
          * search box.  Elsewhere (the user moved on), leave it. */
         const focused = document.activeElement;
         const wasHere = !focused || focused === document.body || row.contains(focused);
         const usable = (r) => {
            const b = r && r.querySelector('.memory-merged-restore');
            return b && !b.disabled ? b : null;
         };
         const target =
            usable(row.nextElementSibling) ||
            usable(row.previousElementSibling) ||
            el('memory-merged-search');
         row.remove();
         if (wasHere && target) target.focus();
      }
      const left = body ? body.querySelectorAll('.memory-item').length : 0;
      setStatus(left === 0 ? emptyText() : left === 1 ? '1 fact' : `${left} facts`, false);
      if (onMemoriesChanged) onMemoriesChanged();
   }

   function init(opts) {
      onMemoriesChanged = opts && opts.onMemoriesChanged;
      const openBtn = el('memory-merged');
      if (openBtn) openBtn.addEventListener('click', open);
      const closeBtn = el('memory-merged-close');
      if (closeBtn) closeBtn.addEventListener('click', close);
      const modal = el('memory-merged-modal');
      if (modal) {
         modal.addEventListener('click', (e) => {
            if (e.target === modal) close();
         });
      }
      const search = el('memory-merged-search');
      if (search) {
         search.addEventListener('input', () => {
            clearTimeout(searchTimer);
            searchTimer = setTimeout(() => request(search.value.trim()), SEARCH_DEBOUNCE_MS);
         });
      }
      const body = el('memory-merged-body');
      if (body) {
         body.addEventListener('click', (e) => {
            const btn = e.target.closest('.memory-merged-restore');
            if (!btn) return;
            const id = Number(btn.dataset.factId);
            if (!Number.isFinite(id) || id <= 0) return;
            if (!connected()) {
               if (typeof DawnToast !== 'undefined') {
                  DawnToast.show('Not connected; try again in a moment', 'error');
               }
               return;
            }
            btn.disabled = true;
            DawnWS.send({ type: 'restore_memory_fact', payload: { fact_id: id } });
         });
      }
   }

   window.DawnMemoryMerged = {
      init,
      open,
      close,
      handleListResponse,
      handleRestoreResponse,
   };
})();
