/**
 * DAWN Memory — Import / Export surface
 *
 * Owns the memory Export modal and the Import modal (paste / file /
 * preview / commit), their private state, and the two WebSocket
 * response handlers (handleExportResponse / handleImportResponse).
 * Split out of memory.js on 2026-06-16 because that file exceeded the
 * 1500-line JS soft limit.
 *
 * Loaded BEFORE memory.js in index.html so memory.js's init() can call
 * DawnMemoryImport.init(ctx) once the parent module's state is ready.
 * The DawnMemory.handleExportResponse / handleImportResponse surface
 * is preserved via thin forwarders in memory.js so dawn.js's dispatch
 * switch stays pointed at DawnMemory.*.
 */
(function () {
   'use strict';

   /* =============================================================================
    * Module state — owned here, NOT shared with memory.js's memoryState.
    * ============================================================================= */

   /* Import modal state */
   let importState = {
      source: 'paste', // 'paste' or 'file'
      fileData: null, // Parsed JSON from file, or null
      fileName: null,
      previewData: null, // Server preview response
   };

   /* Import-modal tab strip — bound to the shared DawnTablist helper
    * via importTablist below.  attr='source' because the HTML uses
    * data-source="paste|file" instead of data-tab. */
   let importTablist = null;

   /* Separate focus-trap cleanup for the import modal — it's a child
    * dialog of the memory popover, so it needs its own trap that
    * cycles Tab within the modal instead of the parent popover. */
   let importFocusTrapCleanup = null;

   /* Injected at init() time — references to parent module helpers.
    * Reads route through ctx so memory.js stays the source of truth for
    * shared state (escapeHtml, the import button, the post-import
    * refresh path). */
   let ctx = null;

   /* Local escapeHtml shim — uses the parent's escaper if init() injected one,
    * else a real HTML-entity escaper. The output feeds innerHTML, so the
    * fallback must never return the raw string (that would be an XSS sink if
    * ctx is missing/failed to init). */
   function escapeHtml(s) {
      if (ctx && ctx.escapeHtml) return ctx.escapeHtml(s);
      return String(s == null ? '' : s)
         .replace(/&/g, '&amp;')
         .replace(/</g, '&lt;')
         .replace(/>/g, '&gt;')
         .replace(/"/g, '&quot;')
         .replace(/'/g, '&#39;');
   }

   /* =============================================================================
    * Init
    * ============================================================================= */

   function init(injectedCtx) {
      ctx = injectedCtx;

      // Export/Import button handlers.
      const exportBtn = document.getElementById('memory-export');
      if (exportBtn) {
         exportBtn.addEventListener('click', handleExport);
      }
      const importBtn = document.getElementById('memory-import');
      if (importBtn) {
         importBtn.addEventListener('click', openImportModal);
      }

      // Wire the modals.
      initImportModal();
      initExportModal();
   }

   /* =============================================================================
    * Export Handling
    * ============================================================================= */

   function handleExport() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      const modal = document.getElementById('memory-export-modal');
      if (modal) modal.classList.remove('hidden');
   }

   function closeExportModal() {
      const modal = document.getElementById('memory-export-modal');
      if (modal) modal.classList.add('hidden');
   }

   function doExport(format) {
      closeExportModal();
      DawnWS.send({
         type: 'export_memories',
         payload: { format: format },
      });
   }

   function initExportModal() {
      const modal = document.getElementById('memory-export-modal');
      if (!modal) return;

      const textBtn = document.getElementById('memory-export-text-btn');
      const jsonBtn = document.getElementById('memory-export-json-btn');
      const cancelBtn = document.getElementById('memory-export-cancel-btn');

      if (textBtn) textBtn.addEventListener('click', () => doExport('text'));
      if (jsonBtn) jsonBtn.addEventListener('click', () => doExport('json'));
      if (cancelBtn) cancelBtn.addEventListener('click', closeExportModal);

      modal.addEventListener('click', (e) => {
         if (e.target === modal) closeExportModal();
      });
   }

   function handleExportResponse(payload) {
      if (!payload || !payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload?.error || 'Export failed', 'error');
         }
         return;
      }

      let blob, filename;
      if (payload.format === 'text') {
         blob = new Blob([payload.data], { type: 'text/plain' });
         filename = `dawn-memories-${DawnTime.dayKey(DawnTime.nowSec())}.txt`;
      } else {
         const jsonStr = JSON.stringify(payload.data, null, 2);
         blob = new Blob([jsonStr], { type: 'application/json' });
         filename = `dawn-memories-${DawnTime.dayKey(DawnTime.nowSec())}.json`;
      }

      // Trigger download
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = filename;
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
      URL.revokeObjectURL(url);

      const total = (payload.fact_count || 0) + (payload.pref_count || 0);
      if (typeof DawnToast !== 'undefined') {
         DawnToast.show(`Exported ${total} memories`, 'success');
      }
   }

   /* =============================================================================
    * Import Handling
    * ============================================================================= */

   function openImportModal() {
      const modal = document.getElementById('memory-import-modal');
      if (!modal) return;
      modal.classList.remove('hidden');
      resetImportState();
      /* Focus the textarea for immediate input.  Defer via
       * setTimeout(0) so the .hidden-removed visibility transition
       * settles before focus moves — consistent with the same
       * pattern in music.js and scheduler-queue.js. */
      const textArea = document.getElementById('memory-import-text');
      if (textArea) setTimeout(() => textArea.focus(), 0);
      /* Trap Tab/Shift+Tab within the modal so it doesn't escape to
       * the parent memory popover.  skipInitialFocus because the
       * textarea focus above (deferred via setTimeout) is the
       * intended initial focus target. */
      const M = window.DawnSettingsModals;
      if (M && typeof M.trapFocus === 'function') {
         /* Release a prior trap first — a double-open (button clicked while the
          * modal is already up) would otherwise leak the old cleanup handle and
          * leave two competing traps installed. */
         if (importFocusTrapCleanup) {
            importFocusTrapCleanup();
            importFocusTrapCleanup = null;
         }
         importFocusTrapCleanup = M.trapFocus(modal, { skipInitialFocus: true });
      }
   }

   function closeImportModal() {
      const modal = document.getElementById('memory-import-modal');
      if (modal) modal.classList.add('hidden');
      resetImportState();
      if (importFocusTrapCleanup) {
         importFocusTrapCleanup();
         importFocusTrapCleanup = null;
      }
      // Return focus to trigger element
      const importBtn = document.getElementById('memory-import');
      if (importBtn) importBtn.focus();
   }

   function resetImportState() {
      importState.source = 'paste';
      importState.fileData = null;
      importState.fileName = null;
      importState.previewData = null;

      const textArea = document.getElementById('memory-import-text');
      if (textArea) textArea.value = '';

      const filenameEl = document.getElementById('memory-import-filename');
      if (filenameEl) {
         filenameEl.textContent = '';
         filenameEl.classList.add('hidden');
      }

      const preview = document.getElementById('memory-import-preview');
      if (preview) preview.classList.add('hidden');

      const previewBtn = document.getElementById('memory-import-preview-btn');
      if (previewBtn) {
         previewBtn.disabled = true;
         previewBtn.classList.remove('hidden');
      }

      const commitBtn = document.getElementById('memory-import-commit-btn');
      if (commitBtn) commitBtn.classList.add('hidden');

      // Reset tab state — tablist.sync() reads importState.source
      // (just set to 'paste' above) and re-applies markup.
      if (importTablist) importTablist.sync();
      const pastePanel = document.getElementById('memory-import-paste');
      const filePanel = document.getElementById('memory-import-file');
      const helpPanel = document.getElementById('memory-import-help');
      if (pastePanel) pastePanel.classList.remove('hidden');
      if (filePanel) filePanel.classList.add('hidden');
      if (helpPanel) helpPanel.classList.add('hidden');
   }

   function switchImportSource(source) {
      importState.source = source;
      if (importTablist) importTablist.sync();
      const pastePanel = document.getElementById('memory-import-paste');
      const filePanel = document.getElementById('memory-import-file');
      const helpPanel = document.getElementById('memory-import-help');
      if (pastePanel) pastePanel.classList.toggle('hidden', source !== 'paste');
      if (filePanel) filePanel.classList.toggle('hidden', source !== 'file');
      if (helpPanel) helpPanel.classList.add('hidden');
      updateImportPreviewBtn();
   }

   function updateImportPreviewBtn() {
      const previewBtn = document.getElementById('memory-import-preview-btn');
      if (!previewBtn) return;

      if (importState.source === 'paste') {
         const textArea = document.getElementById('memory-import-text');
         previewBtn.disabled = !textArea || textArea.value.trim().length < 3;
      } else {
         previewBtn.disabled = !importState.fileData;
      }
   }

   function handleImportFileSelect(e) {
      const file = e.target.files[0];
      if (!file) return;

      if (file.size > 256 * 1024) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('File too large (256KB max)', 'error');
         }
         e.target.value = '';
         return;
      }

      const filenameEl = document.getElementById('memory-import-filename');
      const reader = new FileReader();

      reader.onload = function (evt) {
         const content = evt.target.result;

         if (file.name.endsWith('.json')) {
            try {
               importState.fileData = JSON.parse(content);
               importState.fileName = file.name;
               if (filenameEl) {
                  filenameEl.textContent = file.name;
                  filenameEl.classList.remove('hidden');
               }
            } catch {
               if (typeof DawnToast !== 'undefined') {
                  DawnToast.show('Invalid JSON file', 'error');
               }
               return;
            }
         } else {
            // Plain text file - treat as paste
            importState.fileData = content;
            importState.fileName = file.name;
            if (filenameEl) {
               filenameEl.textContent = file.name;
               filenameEl.classList.remove('hidden');
            }
         }
         updateImportPreviewBtn();
      };

      reader.readAsText(file);
   }

   /**
    * Build the import WebSocket message from current input state
    */
   function buildImportMessage(commit) {
      let payload;
      if (importState.source === 'paste') {
         const textArea = document.getElementById('memory-import-text');
         const text = textArea ? textArea.value.trim() : '';
         if (!text) return null;

         // Auto-detect: if it starts with { it's likely JSON
         if (text.startsWith('{')) {
            try {
               const parsed = JSON.parse(text);
               payload = { format: 'json', data: parsed };
            } catch {
               payload = { format: 'text', text: text };
            }
         } else {
            payload = { format: 'text', text: text };
         }
      } else {
         if (!importState.fileData) return null;
         if (typeof importState.fileData === 'string') {
            payload = { format: 'text', text: importState.fileData };
         } else {
            payload = { format: 'json', data: importState.fileData };
         }
      }

      payload.commit = commit;
      return { type: 'import_memories', payload };
   }

   function handleImportPreview() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;

      const msg = buildImportMessage(false);
      if (!msg) return;

      const previewBtn = document.getElementById('memory-import-preview-btn');
      if (previewBtn) {
         previewBtn.disabled = true;
         previewBtn.textContent = 'Analyzing...';
      }

      DawnWS.send(msg);
   }

   function handleImportCommit() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      if (!importState.previewData) return;

      const msg = buildImportMessage(true);
      if (!msg) return;

      const commitBtn = document.getElementById('memory-import-commit-btn');
      if (commitBtn) {
         commitBtn.disabled = true;
         commitBtn.textContent = 'Importing...';
      }

      DawnWS.send(msg);
   }

   /**
    * Reset to preview mode when content changes after a preview
    */
   function onImportContentChanged() {
      updateImportPreviewBtn();
      if (importState.previewData) {
         importState.previewData = null;
         const preview = document.getElementById('memory-import-preview');
         if (preview) preview.classList.add('hidden');
         const previewBtn = document.getElementById('memory-import-preview-btn');
         const commitBtn = document.getElementById('memory-import-commit-btn');
         if (previewBtn) {
            previewBtn.classList.remove('hidden');
            previewBtn.textContent = 'Preview';
         }
         if (commitBtn) commitBtn.classList.add('hidden');
      }
   }

   function handleImportResponse(payload) {
      if (!payload) return;

      if (!payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload.error || 'Import failed', 'error');
         }
         const previewBtn = document.getElementById('memory-import-preview-btn');
         if (previewBtn) {
            previewBtn.disabled = false;
            previewBtn.textContent = 'Preview';
         }
         return;
      }

      if (payload.committed) {
         // Import complete
         const total = (payload.imported_facts || 0) + (payload.imported_prefs || 0);
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(
               `Imported ${total} memories` +
                  (payload.skipped_dupes ? ` (${payload.skipped_dupes} duplicates skipped)` : ''),
               'success'
            );
         }
         closeImportModal();
         // Refresh memory data
         if (ctx && typeof ctx.onMemoriesChanged === 'function') ctx.onMemoriesChanged();
         return;
      }

      // Preview mode - show results
      importState.previewData = payload;
      const preview = document.getElementById('memory-import-preview');
      const summaryEl = preview?.querySelector('.memory-import-summary');
      const listEl = preview?.querySelector('.memory-import-list');

      if (!preview || !summaryEl || !listEl) return;

      const totalNew = (payload.imported_facts || 0) + (payload.imported_prefs || 0);
      summaryEl.innerHTML =
         `<span>New: <span class="count">${totalNew}</span></span>` +
         `<span>Duplicates skipped: <span class="count">${payload.skipped_dupes || 0}</span></span>` +
         (payload.skipped_empty
            ? `<span>Empty skipped: <span class="count">${payload.skipped_empty}</span></span>`
            : '');

      // Render preview items (max 50 in UI)
      listEl.innerHTML = '';
      const items = payload.preview || [];
      const maxShow = Math.min(items.length, 50);
      for (let i = 0; i < maxShow; i++) {
         const item = items[i];
         const div = document.createElement('div');
         div.className = 'preview-item';
         if (item.type === 'preference') {
            div.innerHTML =
               `<span class="preview-type">pref</span>` +
               `${escapeHtml(item.category)}: ${escapeHtml(item.value)}`;
         } else {
            div.innerHTML = `<span class="preview-type">fact</span>${escapeHtml(item.text)}`;
         }
         listEl.appendChild(div);
      }
      if (items.length > maxShow) {
         const more = document.createElement('div');
         more.className = 'preview-item';
         more.style.color = 'var(--text-secondary)';
         more.textContent = `... and ${items.length - maxShow} more`;
         listEl.appendChild(more);
      }

      preview.classList.remove('hidden');

      // Switch buttons: hide preview, show commit
      const previewBtn = document.getElementById('memory-import-preview-btn');
      const commitBtn = document.getElementById('memory-import-commit-btn');
      if (previewBtn) previewBtn.classList.add('hidden');
      if (commitBtn) {
         commitBtn.classList.remove('hidden');
         commitBtn.disabled = totalNew === 0;
         commitBtn.textContent = totalNew > 0 ? `Import ${totalNew} Memories` : 'Nothing to Import';
      }
   }

   function initImportModal() {
      const closeBtn = document.getElementById('memory-import-close');
      const cancelBtn = document.getElementById('memory-import-cancel');
      const previewBtn = document.getElementById('memory-import-preview-btn');
      const commitBtn = document.getElementById('memory-import-commit-btn');
      const fileInput = document.getElementById('memory-import-file-input');
      const textArea = document.getElementById('memory-import-text');

      if (closeBtn) closeBtn.addEventListener('click', closeImportModal);
      if (cancelBtn) cancelBtn.addEventListener('click', closeImportModal);
      if (previewBtn) previewBtn.addEventListener('click', handleImportPreview);
      if (commitBtn) commitBtn.addEventListener('click', handleImportCommit);
      if (fileInput) fileInput.addEventListener('change', handleImportFileSelect);
      if (textArea) textArea.addEventListener('input', onImportContentChanged);

      // Help popup toggle
      const helpBtn = document.getElementById('memory-import-help-btn');
      const helpPanel = document.getElementById('memory-import-help');
      const helpClose = document.getElementById('memory-import-help-close');
      const pastePanel = document.getElementById('memory-import-paste');

      if (helpBtn && helpPanel && pastePanel) {
         helpBtn.addEventListener('click', () => {
            pastePanel.classList.add('hidden');
            helpPanel.classList.remove('hidden');
         });
      }
      if (helpClose && helpPanel && pastePanel) {
         helpClose.addEventListener('click', () => {
            helpPanel.classList.add('hidden');
            pastePanel.classList.remove('hidden');
         });
      }

      // Copy prompt button (SVG icon swap)
      const ICON_COPY =
         '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
         'stroke-width="2" stroke-linecap="round" stroke-linejoin="round">' +
         '<rect x="9" y="9" width="13" height="13" rx="2" ry="2"/>' +
         '<path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg>';
      const ICON_CHECK =
         '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
         'stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">' +
         '<polyline points="20 6 9 17 4 12"/></svg>';

      const copyPromptBtn = document.getElementById('memory-import-copy-prompt');
      if (copyPromptBtn) {
         copyPromptBtn.addEventListener('click', () => {
            const promptText = document.getElementById('memory-import-prompt-text');
            if (!promptText) return;
            navigator.clipboard.writeText(promptText.textContent).then(() => {
               copyPromptBtn.innerHTML = ICON_CHECK;
               setTimeout(() => (copyPromptBtn.innerHTML = ICON_COPY), 2000);
            });
         });
      }

      // Tab switching — shared DawnTablist helper, attr='source'.
      const importTabs = document.querySelectorAll('.memory-import-tab');
      if (window.DawnTablist && importTabs.length > 0) {
         importTablist = window.DawnTablist.bind({
            tabs: importTabs,
            attr: 'source',
            getActive: () => importState.source,
            onActivate: (name) => switchImportSource(name),
         });
         importTablist.sync(); /* initial markup → matches importState.source */
      }

      // Close on overlay click
      const modal = document.getElementById('memory-import-modal');
      if (modal) {
         modal.addEventListener('click', (e) => {
            if (e.target === modal) closeImportModal();
         });
      }
   }

   /* =============================================================================
    * Public surface
    * ============================================================================= */

   window.DawnMemoryImport = {
      init,
      /* WebSocket response handlers (dispatched from dawn.js via the
       * DawnMemory.handle*Response thin-forwarders in memory.js). */
      handleExportResponse,
      handleImportResponse,
      /* Opened from the memory popover's Import button (wired in init). */
      openImportModal,
   };
})();
