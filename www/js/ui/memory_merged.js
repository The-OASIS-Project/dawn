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

   function el(id) {
      return document.getElementById(id);
   }

   function request(query) {
      lastQuery = query || '';
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({ type: 'list_memory_superseded', payload: { query: lastQuery } });
      }
   }

   function setStatus(text) {
      const body = el('memory-merged-body');
      if (!body) return;
      body.textContent = '';
      const p = document.createElement('p');
      p.className = 'memory-merged-status';
      p.textContent = text;
      body.appendChild(p);
   }

   function open() {
      const modal = el('memory-merged-modal');
      if (!modal) return;
      trigger = document.activeElement;
      modal.classList.remove('hidden');
      if (escToken === null && typeof DawnEscStack !== 'undefined') {
         escToken = DawnEscStack.register(() => {
            close();
            return true;
         });
      }
      const search = el('memory-merged-search');
      if (search) {
         search.value = '';
         search.focus();
      }
      setStatus('Loading…');
      request('');
   }

   function close() {
      const modal = el('memory-merged-modal');
      if (modal) modal.classList.add('hidden');
      clearTimeout(searchTimer);
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
         return new Date(seconds * 1000).toLocaleDateString([], {
            month: 'short',
            day: 'numeric',
            year: 'numeric',
         });
      } catch (e) {
         return '';
      }
   }

   function renderRow(f) {
      const item = document.createElement('div');
      item.className = 'memory-item fact';
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
      const label = (f.text || '').length > 60 ? f.text.slice(0, 60) + '…' : f.text || '';
      restore.setAttribute('aria-label', 'Restore: ' + label);
      header.appendChild(text);
      header.appendChild(restore);

      const meta = document.createElement('div');
      meta.className = 'memory-item-meta';
      const kept = document.createElement('span');
      kept.className = 'memory-merged-keeper';
      kept.textContent = f.keeper_text ? 'Now: ' + f.keeper_text : 'Now: (the kept memory is gone)';
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
         setStatus((payload && payload.error) || "Couldn't load merged memories.");
         return;
      }
      const facts = Array.isArray(payload.facts) ? payload.facts : [];
      if (facts.length === 0) {
         setStatus(lastQuery ? 'No merged memories match.' : 'No merged or replaced memories.');
         return;
      }
      const body = el('memory-merged-body');
      body.textContent = '';
      facts.forEach((f) => body.appendChild(renderRow(f)));
   }

   function handleRestoreResponse(payload) {
      if (!payload || !payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show((payload && payload.error) || "Couldn't restore the memory", 'error');
         }
         request(lastQuery);
         return;
      }
      if (typeof DawnToast !== 'undefined') {
         DawnToast.show(
            payload.both_current
               ? 'Memory restored; the newer one is current too'
               : 'Memory restored',
            'success'
         );
      }
      const body = el('memory-merged-body');
      const row = body && body.querySelector(`.memory-item[data-fact-id="${payload.fact_id}"]`);
      if (row) row.remove();
      if (body && !body.querySelector('.memory-item')) {
         setStatus(lastQuery ? 'No merged memories match.' : 'No merged or replaced memories.');
      }
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
            if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
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
