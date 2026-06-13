/**
 * DAWN Document Library Panel
 * Upload, index, list, and delete documents for RAG search.
 * Documents are chunked and embedded server-side for semantic search.
 */
(function () {
   'use strict';

   const SUPPORTED_TYPES = ['pdf', 'docx', 'txt', 'md'];
   const MAX_FILE_SIZE = 10 * 1024 * 1024; // 10 MB
   const PAGE_SIZE = 20;

   const NOTE_MAX_LEN = 4000;
   const NOTE_LABEL_MAX = 80;
   const SEARCH_DEBOUNCE_MS = 250;
   const SCOPE_STORAGE_KEY = 'dawn_doc_library_scope'; // remember the active tab across opens

   function loadScope() {
      try {
         const s = localStorage.getItem(SCOPE_STORAGE_KEY);
         return s === 'notes' || s === 'documents' ? s : 'documents';
      } catch (_e) {
         return 'documents';
      }
   }

   let state = {
      documents: [],
      isOpen: false,
      indexing: false,
      indexingFilename: '',
      offset: 0,
      hasMore: false,
      showAll: false,
      scope: 'documents', // 'documents' | 'notes' — restored from localStorage on open()
      query: '',
      editingId: null, // null = create; note id = editing
      editorDirty: false,
      editorTrigger: null, // element to restore focus to on editor close
      viewingId: null, // note id shown in the read-only viewer, else null
      viewerTrigger: null, // element to restore focus to on viewer close
   };

   let searchTimer = null;

   let callbacks = {
      trapFocus: null,
      showConfirmModal: null,
   };

   // Focus management state
   let focusTrapCleanup = null;
   let triggerElement = null;

   let el = {};

   /* =============================================================================
    * Init
    * ============================================================================= */

   function init(options) {
      if (options) {
         if (options.trapFocus) callbacks.trapFocus = options.trapFocus;
         if (options.showConfirmModal) callbacks.showConfirmModal = options.showConfirmModal;
      }
      el.btn = document.getElementById('doc-library-btn');
      el.popover = document.getElementById('doc-library-popover');
      el.closeBtn = document.getElementById('doc-library-close');
      el.list = document.getElementById('doc-library-list');
      el.dropzone = document.getElementById('doc-library-dropzone');
      el.fileInput = document.getElementById('doc-library-file-input');
      el.docCount = document.getElementById('doc-library-count');
      el.chunkCount = document.getElementById('doc-library-chunks');
      el.indexingBar = document.getElementById('doc-library-indexing');
      el.loadMoreBtn = document.getElementById('doc-library-load-more');
      el.showAllLabel = document.getElementById('doc-library-show-all-label');
      el.showAllCheck = document.getElementById('doc-library-show-all');
      el.globalLabel = document.getElementById('doc-library-global-label');
      el.globalCheck = document.getElementById('doc-library-global-check');
      // v61: tabs, search, note editor
      el.searchInput = document.getElementById('doc-library-search-input');
      el.tabs = el.popover ? el.popover.querySelectorAll('.doc-library-tab') : [];
      el.uploadArea = el.popover ? el.popover.querySelector('.doc-library-upload') : null;
      el.notesActions = document.getElementById('doc-library-notes-actions');
      el.newNoteBtn = document.getElementById('doc-library-new-note');
      el.editor = document.getElementById('doc-library-note-editor');
      el.noteForm = document.getElementById('doc-library-note-form');
      el.noteLabel = document.getElementById('doc-library-note-label');
      el.noteText = document.getElementById('doc-library-note-text');
      el.noteCount = document.getElementById('doc-library-note-count');
      el.noteError = document.getElementById('doc-library-note-error');
      el.noteLabelHint = document.getElementById('doc-library-note-label-hint');
      el.noteCancel = document.getElementById('doc-library-note-cancel');
      // v61 follow-up: read-only note viewer
      el.viewer = document.getElementById('doc-library-note-viewer');
      el.viewerLabel = document.getElementById('doc-library-note-viewer-label');
      el.viewerText = document.getElementById('doc-library-note-viewer-text');
      el.viewerClose = document.getElementById('doc-library-note-viewer-close');
      el.viewerHistory = document.getElementById('doc-library-note-viewer-history');
      el.viewerEdit = document.getElementById('doc-library-note-viewer-edit');
      el.viewerDelete = document.getElementById('doc-library-note-viewer-delete');
      el.versions = document.getElementById('doc-library-note-versions');
      el.deleted =
         document.getElementById('doc-library-note-deleted') ||
         document.getElementById('doc-library-deleted');
      el.deletedToggle = document.getElementById('doc-library-deleted-toggle');

      if (!el.btn || !el.popover) return;

      el.btn.addEventListener('click', toggle);
      el.closeBtn.addEventListener('click', close);

      // Tabs (roving tabindex + ←/→/Home/End per WAI-ARIA)
      el.tabs.forEach((tab) => {
         tab.addEventListener('click', () => setScope(tab.dataset.scope));
         tab.addEventListener('keydown', onTabKeydown);
      });

      // Search (debounced live; Enter submits immediately)
      if (el.searchInput) {
         el.searchInput.addEventListener('input', () => {
            clearTimeout(searchTimer);
            searchTimer = setTimeout(runSearch, SEARCH_DEBOUNCE_MS);
         });
         el.searchInput.addEventListener('keydown', (e) => {
            if (e.key === 'Enter') {
               e.preventDefault();
               clearTimeout(searchTimer);
               runSearch();
            }
         });
      }

      // Note editor
      if (el.newNoteBtn) el.newNoteBtn.addEventListener('click', () => openEditor(null));
      if (el.noteCancel) el.noteCancel.addEventListener('click', () => requestCloseEditor());
      if (el.noteForm) el.noteForm.addEventListener('submit', onNoteSubmit);
      if (el.noteText) {
         el.noteText.addEventListener('input', () => {
            state.editorDirty = true;
            updateNoteCount();
         });
      }
      if (el.noteLabel)
         el.noteLabel.addEventListener('input', () => {
            state.editorDirty = true;
         });

      // Read-only note viewer actions
      if (el.viewerClose) el.viewerClose.addEventListener('click', () => closeViewer(true));
      if (el.viewerEdit) {
         el.viewerEdit.addEventListener('click', () => {
            const doc = state.documents.find((d) => d.id === state.viewingId);
            closeViewer(false);
            if (doc) openEditor(doc);
         });
      }
      if (el.viewerDelete) {
         el.viewerDelete.addEventListener('click', () => {
            const doc = state.documents.find((d) => d.id === state.viewingId);
            if (doc) confirmDelete(doc.id);
         });
      }
      if (el.viewerHistory) {
         el.viewerHistory.addEventListener('click', () => {
            if (state.viewingId) requestVersionList(state.viewingId);
         });
      }
      if (el.deletedToggle) {
         el.deletedToggle.addEventListener('click', () => {
            if (el.deleted && !el.deleted.classList.contains('hidden')) {
               el.deleted.classList.add('hidden');
               el.deleted.innerHTML = '';
            } else {
               requestDeletedList();
            }
         });
      }

      // Load More button
      if (el.loadMoreBtn) {
         el.loadMoreBtn.addEventListener('click', () => {
            requestList(state.offset);
         });
      }

      // Dropzone events
      if (el.dropzone) {
         el.dropzone.addEventListener('click', () => el.fileInput?.click());
         el.dropzone.addEventListener('dragover', (e) => {
            e.preventDefault();
            el.dropzone.classList.add('dragover');
         });
         el.dropzone.addEventListener('dragleave', () => {
            el.dropzone.classList.remove('dragover');
         });
         el.dropzone.addEventListener('drop', (e) => {
            e.preventDefault();
            el.dropzone.classList.remove('dragover');
            if (e.dataTransfer?.files?.length > 0) {
               handleFileSelect(e.dataTransfer.files[0]);
            }
         });
      }

      if (el.fileInput) {
         el.fileInput.addEventListener('change', (e) => {
            if (e.target.files?.length > 0) {
               handleFileSelect(e.target.files[0]);
               e.target.value = '';
            }
         });
      }

      // Show All toggle (admin only)
      if (el.showAllCheck) {
         el.showAllCheck.addEventListener('change', () => {
            state.showAll = el.showAllCheck.checked;
            state.documents = [];
            state.offset = 0;
            state.hasMore = false;
            requestList(0);
         });
      }

      // Sticky panel: NO outside-click auto-close (mirrors the music panel).  The
      // panel closes only on the × button, Escape, or a competing header panel
      // opening over it (memory/scheduler call DawnDocLibrary.close() on open).
      // This stops the panel vanishing when an in-panel action spawns the confirm
      // modal (which lives outside the popover and used to read as an outside click).

      // Close on Escape — viewer/editor first (nested), then the popover.  Bail if
      // the confirm modal is open so its OWN Escape handler can close it without us
      // re-spawning the discard dialog underneath.
      document.addEventListener('keydown', (e) => {
         if (e.key !== 'Escape' || !state.isOpen) return;
         const confirmModal = document.getElementById('confirm-modal');
         if (confirmModal && !confirmModal.classList.contains('hidden')) return;
         if (el.editor && !el.editor.classList.contains('hidden')) {
            requestCloseEditor();
         } else if (el.viewer && !el.viewer.classList.contains('hidden')) {
            closeViewer(true);
         } else {
            close();
         }
      });
   }

   /* =============================================================================
    * Tabs + search
    * ============================================================================= */

   function setScope(scope) {
      if (scope !== 'documents' && scope !== 'notes') return;
      // Guard an unsaved editor before navigating away (consistent with Cancel/ESC)
      if (state.editorDirty && callbacks.showConfirmModal) {
         callbacks.showConfirmModal('Discard changes to this note?', () => applyScope(scope), {
            title: 'Discard note',
            okText: 'Discard',
         });
         return;
      }
      applyScope(scope);
   }

   function applyScope(scope) {
      state.scope = scope;
      try {
         localStorage.setItem(SCOPE_STORAGE_KEY, scope);
      } catch (_e) {
         /* private mode / quota — non-fatal, tab just won't persist */
      }
      closeEditor(false);
      closeViewer(false);
      // Roving tabindex + aria
      el.tabs.forEach((tab) => {
         const active = tab.dataset.scope === scope;
         tab.setAttribute('aria-selected', active ? 'true' : 'false');
         tab.tabIndex = active ? 0 : -1;
      });
      // Active tabpanel labelling (a11y)
      if (el.list) {
         el.list.setAttribute('aria-labelledby', `doc-library-tab-${scope}`);
      }
      // Primary action morphs per tab: upload on Documents, "+ New note" on Notes
      if (el.uploadArea) el.uploadArea.classList.toggle('hidden', scope !== 'documents');
      if (el.notesActions) el.notesActions.classList.toggle('hidden', scope !== 'notes');
      // Reset list for the new scope
      state.documents = [];
      state.offset = 0;
      state.hasMore = false;
      requestList(0);
      updateSearchPlaceholder();
   }

   function updateSearchPlaceholder() {
      if (!el.searchInput) return;
      el.searchInput.placeholder = state.scope === 'notes' ? 'Search notes' : 'Search documents';
   }

   function onTabKeydown(e) {
      const tabs = Array.from(el.tabs);
      const idx = tabs.indexOf(e.currentTarget);
      let next = -1;
      if (e.key === 'ArrowRight') next = (idx + 1) % tabs.length;
      else if (e.key === 'ArrowLeft') next = (idx - 1 + tabs.length) % tabs.length;
      else if (e.key === 'Home') next = 0;
      else if (e.key === 'End') next = tabs.length - 1;
      if (next >= 0) {
         e.preventDefault();
         tabs[next].focus();
         setScope(tabs[next].dataset.scope);
      }
   }

   function runSearch() {
      const q = el.searchInput ? el.searchInput.value.trim() : '';
      state.query = q;
      state.documents = [];
      state.offset = 0;
      state.hasMore = false;
      requestList(0);
   }

   /* =============================================================================
    * Open / Close
    * ============================================================================= */

   function toggle() {
      if (state.isOpen) {
         close();
      } else {
         open();
      }
   }

   function open() {
      // Close competing header popovers that share this top-right slot
      if (typeof DawnMemory !== 'undefined') DawnMemory.close();
      if (typeof DawnSchedulerQueue !== 'undefined') DawnSchedulerQueue.close();
      if (typeof DawnCodeProjects !== 'undefined') DawnCodeProjects.close();

      triggerElement = document.activeElement;
      el.popover.classList.remove('hidden');
      el.btn.classList.add('active');
      state.isOpen = true;

      // Set up focus trap
      if (callbacks.trapFocus) {
         focusTrapCleanup = callbacks.trapFocus(el.popover);
      }

      // Reset to a clean state: restore the last-used tab, no search, editor/viewer closed
      closeEditor(false);
      closeViewer(false);
      state.query = '';
      if (el.searchInput) el.searchInput.value = '';
      setScope(loadScope());
   }

   function close() {
      el.popover.classList.add('hidden');
      el.btn.classList.remove('active');
      state.isOpen = false;

      // Clean up focus trap
      if (focusTrapCleanup) {
         focusTrapCleanup();
         focusTrapCleanup = null;
      }

      // Restore focus to trigger element
      if (triggerElement && typeof triggerElement.focus === 'function') {
         triggerElement.focus();
         triggerElement = null;
      }
   }

   /* =============================================================================
    * WebSocket API
    * ============================================================================= */

   function requestList(offset) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      const payload = { limit: PAGE_SIZE, offset: offset || 0, scope: state.scope };
      if (state.showAll) payload.show_all = true;
      if (state.query) payload.query = state.query;
      DawnWS.send({ type: 'doc_library_list', payload });
   }

   function requestNoteSave(label, text) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({ type: 'doc_library_note_save', payload: { label, text } });
   }

   function requestNoteUpdate(id, label, text) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({ type: 'doc_library_note_update', payload: { id, label, text } });
   }

   function requestVersionList(docId) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({ type: 'doc_library_version_list', payload: { id: docId } });
   }

   function requestVersionRestore(versionId) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({ type: 'doc_library_version_restore', payload: { version_id: versionId } });
   }

   function requestDeletedList() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({ type: 'doc_library_deleted_list', payload: {} });
   }

   function requestDelete(docId) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({ type: 'doc_library_delete', payload: { id: docId } });
   }

   function requestToggleGlobal(docId, isGlobal) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({
         type: 'doc_library_toggle_global',
         payload: { id: docId, is_global: isGlobal },
      });
   }

   function requestIndex(filename, filetype, text, isGlobal) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      state.indexing = true;
      state.indexingFilename = filename;
      renderIndexing();
      DawnWS.send({
         type: 'doc_library_index',
         payload: { filename, filetype, text, is_global: isGlobal || false },
      });
   }

   /* =============================================================================
    * WebSocket Response Handlers
    * ============================================================================= */

   function handleListResponse(payload) {
      if (!payload?.success) {
         console.error('doc_library list failed:', payload?.error);
         return;
      }

      const newDocs = payload.documents || [];
      state.hasMore = payload.has_more || false;

      if (state.offset === 0) {
         // First page — full render
         state.documents = newDocs;
         renderList();
      } else if (newDocs.length > 0) {
         // Subsequent page — append
         state.documents = state.documents.concat(newDocs);
         appendItems(newDocs);
      }

      state.offset = state.documents.length;
      updateLoadMore();
      updateStats();
   }

   function handleDeleteResponse(payload) {
      if (!payload?.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload?.error || 'Failed to delete document', 'error');
         }
         return;
      }
      // If the deleted note was open in the viewer, close it
      if (state.viewingId === payload.id) closeViewer(false);
      // Remove from local state
      state.documents = state.documents.filter((d) => d.id !== payload.id);
      state.offset = state.documents.length;
      renderList();
      updateLoadMore();
      updateStats();
      if (typeof DawnToast !== 'undefined') {
         DawnToast.show('Document deleted', 'success');
      }
   }

   function handleIndexResponse(payload) {
      state.indexing = false;
      state.indexingFilename = '';
      renderIndexing();

      if (!payload?.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload?.error || 'Indexing failed', 'error');
         }
         return;
      }

      if (typeof DawnToast !== 'undefined') {
         let msg = `Indexed "${payload.filename}" (${payload.num_chunks} chunks)`;
         if (payload.failed_chunks > 0) {
            msg += ` — ${payload.failed_chunks} chunks failed`;
         }
         DawnToast.show(msg, 'success');
      }

      // Refresh list from the start
      state.documents = [];
      state.offset = 0;
      state.hasMore = false;
      requestList(0);
   }

   function handleToggleGlobalResponse(payload) {
      if (!payload?.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload?.error || 'Failed to update document', 'error');
         }
         return;
      }
      // Update local state
      const doc = state.documents.find((d) => d.id === payload.id);
      if (doc) {
         doc.is_global = payload.is_global;
         renderList();
         updateLoadMore();
      }
      const name = doc ? doc.filename : 'Document';
      if (typeof DawnToast !== 'undefined') {
         DawnToast.show(
            payload.is_global ? `"${name}" shared globally` : `"${name}" set to private`,
            'success'
         );
      }
   }

   /* =============================================================================
    * Note editor (create / edit)
    * ============================================================================= */

   function openEditor(note) {
      if (!el.editor) return;
      // Editor and viewer are mutually-exclusive inline panels
      closeViewer(false);
      // Remember what to return focus to when the editor closes (WCAG 2.4.3)
      state.editorTrigger = document.activeElement;
      state.editingId = note ? note.id : null;
      state.editorDirty = false;
      el.noteLabel.value = note ? note.filename : '';
      el.noteText.value = note ? note.text || '' : '';
      el.noteLabelHint.textContent = note
         ? 'Changing the label changes how you’ll refer to this.'
         : 'Friday can read this back to you by label.';
      hideNoteError();
      updateNoteCount();
      el.editor.classList.remove('hidden');
      if (el.notesActions) el.notesActions.classList.add('hidden');
      el.noteLabel.focus();
   }

   // restoreFocus: return focus to the opener (cancel/save/ESC); NOT on tab-switch.
   function closeEditor(restoreFocus) {
      if (!el.editor) return;
      const wasOpen = !el.editor.classList.contains('hidden');
      el.editor.classList.add('hidden');
      state.editingId = null;
      state.editorDirty = false;
      hideNoteError();
      // Restore the "+ New note" affordance on the Notes tab
      if (el.notesActions) el.notesActions.classList.toggle('hidden', state.scope !== 'notes');
      if (restoreFocus && wasOpen && state.editorTrigger && state.editorTrigger.focus) {
         state.editorTrigger.focus();
      }
      state.editorTrigger = null;
   }

   // Cancel/ESC: confirm if there are unsaved changes
   function requestCloseEditor() {
      if (state.editorDirty && callbacks.showConfirmModal) {
         callbacks.showConfirmModal('Discard changes to this note?', () => closeEditor(true), {
            title: 'Discard note',
            okText: 'Discard',
         });
      } else {
         closeEditor(true);
      }
   }

   /* =============================================================================
    * Read-only note viewer (opened by clicking a note name)
    * ============================================================================= */

   function openViewer(item) {
      if (!el.viewer || !item) return;
      state.viewerTrigger = document.activeElement;
      state.viewingId = item.id;
      const isNote = item.is_note || item.filetype === 'note';
      el.viewerLabel.textContent = item.filename;
      if (isNote) {
         el.viewerText.textContent = item.text || '';
      } else {
         /* Documents are multi-part; the WebUI doesn't inline-edit them (read or
          * edit via Friday / document_read).  Show a summary — version history +
          * delete below still apply. */
         const n = item.num_chunks || 0;
         el.viewerText.textContent = `Document · ${n} part${n === 1 ? '' : 's'}. Ask Friday to read or edit its content; version history and delete are below.`;
      }
      /* Inline Edit is the single-chunk note editor — hide it for documents. */
      if (el.viewerEdit) el.viewerEdit.style.display = isNote ? '' : 'none';
      // Editor and viewer are mutually-exclusive inline panels
      closeEditor(false);
      hideVersions();
      if (el.notesActions) el.notesActions.classList.add('hidden');
      el.viewer.classList.remove('hidden');
      // Hand keyboard users a focusable control inside the viewer (WCAG 2.4.3)
      if (el.viewerClose) el.viewerClose.focus();
   }

   /* =============================================================================
    * Version history (v62) — list + restore from the read-only viewer
    * ============================================================================= */

   function hideVersions() {
      if (el.versions) {
         el.versions.classList.add('hidden');
         el.versions.innerHTML = '';
      }
   }

   function handleVersionListResponse(payload) {
      if (!el.versions) return;
      if (!payload?.success) {
         if (typeof DawnToast !== 'undefined')
            DawnToast.show(payload?.error || 'Could not load history', 'error');
         return;
      }
      // Only show history for the note currently open in the viewer.
      if (payload.id !== state.viewingId) return;
      renderVersions(payload.versions || []);
   }

   function renderVersions(versions) {
      if (!el.versions) return;
      el.versions.classList.remove('hidden');
      if (versions.length === 0) {
         el.versions.innerHTML = '<div class="note-versions-empty">No earlier versions yet.</div>';
         return;
      }
      const rows = versions
         .map((v) => {
            const when = new Date(v.archived_at * 1000).toLocaleString();
            return `
            <div class="note-version-row">
               <div class="note-version-info">
                  <div class="note-version-when">${escapeHtml(when)}</div>
                  <div class="note-version-preview">${escapeHtml(v.preview || '')}</div>
               </div>
               <button type="button" class="btn-link note-version-restore" data-version-id="${v.id}">Restore</button>
            </div>`;
         })
         .join('');
      el.versions.innerHTML = `<div class="note-versions-title">Version history</div>${rows}`;
      el.versions.querySelectorAll('.note-version-restore').forEach((btn) => {
         btn.addEventListener('click', () => {
            const vid = parseInt(btn.dataset.versionId, 10);
            if (callbacks.showConfirmModal) {
               callbacks.showConfirmModal(
                  'Restore this version? The current text is saved to history first, so you can undo.',
                  () => requestVersionRestore(vid),
                  { title: 'Restore version', okText: 'Restore' }
               );
            } else {
               requestVersionRestore(vid);
            }
         });
      });
   }

   function handleVersionRestoreResponse(payload) {
      if (!payload?.success) {
         if (typeof DawnToast !== 'undefined')
            DawnToast.show(payload?.error || 'Restore failed', 'error');
         return;
      }
      if (typeof DawnToast !== 'undefined') DawnToast.show('Version restored', 'success');
      closeViewer(false);
      if (el.deleted) {
         el.deleted.classList.add('hidden');
         el.deleted.innerHTML = '';
      }
      refreshList();
   }

   function handleDeletedListResponse(payload) {
      if (!el.deleted) return;
      if (!payload?.success) {
         if (typeof DawnToast !== 'undefined')
            DawnToast.show(payload?.error || 'Could not load deleted items', 'error');
         return;
      }
      const items = payload.deleted || [];
      el.deleted.classList.remove('hidden');
      if (items.length === 0) {
         el.deleted.innerHTML = '<div class="note-versions-empty">No recently deleted items.</div>';
         return;
      }
      const rows = items
         .map((d) => {
            const when = new Date(d.archived_at * 1000).toLocaleString();
            return `
            <div class="note-version-row">
               <div class="note-version-info">
                  <div class="note-version-when">${escapeHtml(d.filename)} · deleted ${escapeHtml(when)}</div>
                  <div class="note-version-preview">${escapeHtml(d.preview || '')}</div>
               </div>
               <button type="button" class="btn-link note-version-restore" data-version-id="${d.version_id}">Restore</button>
            </div>`;
         })
         .join('');
      el.deleted.innerHTML = `<div class="note-versions-title">Recently deleted</div>${rows}`;
      el.deleted.querySelectorAll('.note-version-restore').forEach((btn) => {
         btn.addEventListener('click', () => {
            requestVersionRestore(parseInt(btn.dataset.versionId, 10));
         });
      });
   }

   // Guard an unsaved editor before replacing it with the viewer
   function requestOpenViewer(note) {
      if (state.editorDirty && callbacks.showConfirmModal) {
         callbacks.showConfirmModal('Discard changes to this note?', () => openViewer(note), {
            title: 'Discard note',
            okText: 'Discard',
         });
         return;
      }
      openViewer(note);
   }

   // restoreFocus: return focus to the opener (close/ESC); NOT on edit/delete handoff.
   function closeViewer(restoreFocus) {
      if (!el.viewer) return;
      const wasOpen = !el.viewer.classList.contains('hidden');
      el.viewer.classList.add('hidden');
      hideVersions();
      state.viewingId = null;
      // Restore the "+ New note" affordance on the Notes tab
      if (el.notesActions) el.notesActions.classList.toggle('hidden', state.scope !== 'notes');
      if (restoreFocus && wasOpen && state.viewerTrigger && state.viewerTrigger.focus) {
         state.viewerTrigger.focus();
      }
      state.viewerTrigger = null;
   }

   function updateNoteCount() {
      if (!el.noteCount || !el.noteText) return;
      const len = el.noteText.value.length;
      el.noteCount.textContent = len;
      el.noteCount.parentElement.classList.toggle('warning', len > NOTE_MAX_LEN - 200);
   }

   function showNoteError(msg) {
      if (!el.noteError) return;
      el.noteError.textContent = msg;
      el.noteError.classList.add('visible');
   }

   function hideNoteError() {
      if (el.noteError) el.noteError.classList.remove('visible');
   }

   function onNoteSubmit(e) {
      e.preventDefault();
      const label = el.noteLabel.value.trim();
      const text = el.noteText.value.trim();
      if (!label) {
         showNoteError('A label is required.');
         el.noteLabel.focus();
         return;
      }
      if (label.length > NOTE_LABEL_MAX) {
         showNoteError(`Label is too long (max ${NOTE_LABEL_MAX}).`);
         return;
      }
      if (!text) {
         showNoteError('The note text is required.');
         el.noteText.focus();
         return;
      }
      hideNoteError();
      if (state.editingId) {
         requestNoteUpdate(state.editingId, label, text);
      } else {
         requestNoteSave(label, text);
      }
   }

   function handleNoteSaveResponse(payload) {
      if (!payload?.success) {
         showNoteError(payload?.error || 'Could not save the note.');
         return;
      }
      closeEditor(true);
      if (typeof DawnToast !== 'undefined') DawnToast.show('Note saved', 'success');
      refreshList();
   }

   function handleNoteUpdateResponse(payload) {
      if (!payload?.success) {
         showNoteError(payload?.error || 'Could not update the note.');
         return;
      }
      closeEditor(true);
      if (typeof DawnToast !== 'undefined') DawnToast.show('Note updated', 'success');
      refreshList();
   }

   function refreshList() {
      state.documents = [];
      state.offset = 0;
      state.hasMore = false;
      requestList(0);
   }

   /* =============================================================================
    * File Upload → Extract → Index
    * ============================================================================= */

   async function handleFileSelect(file) {
      const ext = file.name.split('.').pop()?.toLowerCase();
      if (!SUPPORTED_TYPES.includes(ext)) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(`Unsupported file type: .${ext}`, 'error');
         }
         return;
      }

      if (file.size > MAX_FILE_SIZE) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('File too large (max 10 MB)', 'error');
         }
         return;
      }

      // Upload to existing extraction endpoint
      const formData = new FormData();
      formData.append('document', file, file.name);

      state.indexing = true;
      state.indexingFilename = file.name;
      renderIndexing();

      try {
         const response = await fetch('/api/documents', {
            method: 'POST',
            credentials: 'include',
            body: formData,
         });

         if (!response.ok) {
            throw new Error(`Upload failed: ${response.status}`);
         }

         const result = await response.json();

         if (!result.content || result.content.length === 0) {
            throw new Error('No text could be extracted from file');
         }

         // Now index via WebSocket (chunk + embed + store)
         const isGlobal = el.globalCheck ? el.globalCheck.checked : false;
         requestIndex(result.filename || file.name, result.type || ext, result.content, isGlobal);
      } catch (err) {
         state.indexing = false;
         state.indexingFilename = '';
         renderIndexing();
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(err.message || 'Upload failed', 'error');
         }
      }
   }

   /* =============================================================================
    * Rendering
    * ============================================================================= */

   function canToggleGlobal() {
      const authState = typeof DawnState !== 'undefined' ? DawnState.authState : null;
      return authState && authState.authenticated && authState.isAdmin;
   }

   function renderDocItem(doc) {
      const isNote = doc.is_note || doc.filetype === 'note';
      const date = new Date(doc.created_at * 1000).toLocaleDateString();
      const globalBadge = doc.is_global ? '<span class="global-badge">GLOBAL</span>' : '';
      const ownerBadge =
         state.showAll && doc.owner_name
            ? `<span class="owner-badge">${escapeHtml(doc.owner_name)}</span>`
            : '';
      const safeType = isNote ? 'note' : (doc.filetype || '').replace(/[^a-z0-9]/g, '');
      const iconLabel = isNote ? 'NOTE' : escapeHtml(doc.filetype || '');
      // Notes: body preview + edited date; documents: chunk count + date
      const meta = isNote
         ? `${escapeHtml((doc.text || '').slice(0, 90))}${(doc.text || '').length > 90 ? '…' : ''}`
         : `${doc.num_chunks} chunks &middot; ${date}`;
      const pencilSvg =
         '<svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 20h9"/><path d="M16.5 3.5a2.12 2.12 0 0 1 3 3L7 19l-4 1 1-4Z"/></svg>';
      const editBtn = isNote
         ? `<button type="button" class="doc-library-item-edit" data-id="${doc.id}" title="Edit note" aria-label="Edit note">${pencilSvg}</button>`
         : '';
      const globeSvg =
         '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><line x1="2" y1="12" x2="22" y2="12"/><path d="M12 2a15.3 15.3 0 0 1 4 10 15.3 15.3 0 0 1-4 10 15.3 15.3 0 0 1-4-10 15.3 15.3 0 0 1 4-10z"/></svg>';
      const globalToggle = canToggleGlobal()
         ? `<button type="button" class="doc-library-item-global" data-id="${doc.id}" data-global="${doc.is_global ? '1' : '0'}" title="${doc.is_global ? 'Make private' : 'Share globally'}" aria-label="${doc.is_global ? 'Make private' : 'Share globally'}">${globeSvg}</button>`
         : '';
      // The name doubles as the "open read-only viewer" affordance — for notes
      // (text + edit) and documents (summary + version history), both with restore.
      const nameAttrs = `class="doc-library-item-name note-name-clickable" role="button" tabindex="0" data-view-id="${doc.id}" aria-label="View ${escapeAttr(doc.filename)}"`;
      return `
      <div class="doc-library-item${isNote ? ' note-item' : ''}" data-id="${doc.id}">
         <div class="doc-library-item-icon ${safeType}">${iconLabel}</div>
         <div class="doc-library-item-info">
            <div ${nameAttrs} title="${escapeAttr(doc.filename)}">${escapeHtml(doc.filename)}</div>
            <div class="doc-library-item-meta">${meta}${globalBadge}${ownerBadge}</div>
         </div>
         ${editBtn}
         ${globalToggle}
         <button type="button" class="doc-library-item-delete" data-id="${doc.id}" title="Delete">&times;</button>
      </div>`;
   }

   function handleEditClick(btn) {
      btn.addEventListener('click', (e) => {
         e.stopPropagation();
         const id = parseInt(btn.dataset.id, 10);
         const doc = state.documents.find((d) => d.id === id);
         if (doc) openEditor(doc);
      });
   }

   function confirmDelete(id) {
      const doc = state.documents.find((d) => d.id === id);
      const isNote = doc && (doc.is_note || doc.filetype === 'note');
      const name = doc ? doc.filename : isNote ? 'this note' : 'this document';
      const noun = isNote ? 'note' : 'document';
      if (callbacks.showConfirmModal) {
         callbacks.showConfirmModal(
            `Delete "${name}"? You can restore it from "Recently deleted" for a short window.`,
            () => requestDelete(id),
            { title: `Delete ${noun.charAt(0).toUpperCase() + noun.slice(1)}`, okText: 'Delete' }
         );
      } else {
         requestDelete(id);
      }
   }

   function handleDeleteClick(btn) {
      btn.addEventListener('click', (e) => {
         e.stopPropagation();
         confirmDelete(parseInt(btn.dataset.id, 10));
      });
   }

   function handleGlobalClick(btn) {
      btn.addEventListener('click', (e) => {
         e.stopPropagation();
         const id = parseInt(btn.dataset.id, 10);
         const currentlyGlobal = btn.dataset.global === '1';
         requestToggleGlobal(id, !currentlyGlobal);
      });
   }

   // Clicking (or Enter/Space on) a note name opens the read-only viewer
   function handleNameClick(nameEl) {
      const openIt = () => {
         const id = parseInt(nameEl.dataset.viewId, 10);
         const doc = state.documents.find((d) => d.id === id);
         if (doc) requestOpenViewer(doc);
      };
      nameEl.addEventListener('click', openIt);
      nameEl.addEventListener('keydown', (e) => {
         if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            openIt();
         }
      });
   }

   function bindItemButtons(container) {
      container.querySelectorAll('.doc-library-item-delete').forEach((btn) => {
         handleDeleteClick(btn);
      });
      container.querySelectorAll('.doc-library-item-global').forEach((btn) => {
         handleGlobalClick(btn);
      });
      container.querySelectorAll('.doc-library-item-edit').forEach((btn) => {
         handleEditClick(btn);
      });
      container.querySelectorAll('.note-name-clickable').forEach((nameEl) => {
         handleNameClick(nameEl);
      });
   }

   function renderList() {
      if (!el.list) return;

      if (state.documents.length === 0) {
         let msg;
         if (state.query) msg = `No ${state.scope} match “${escapeHtml(state.query)}”.`;
         else if (state.scope === 'notes') msg = 'No notes yet. Create one with “+ New note”.';
         else msg = 'No documents indexed yet';
         el.list.innerHTML = `<div class="doc-library-empty">${msg}</div>`;
         return;
      }

      el.list.innerHTML = state.documents.map(renderDocItem).join('');
      bindItemButtons(el.list);
   }

   function appendItems(docs) {
      if (!el.list) return;

      // Clear empty message if present
      const empty = el.list.querySelector('.doc-library-empty');
      if (empty) empty.remove();

      const html = docs.map(renderDocItem).join('');
      el.list.insertAdjacentHTML('beforeend', html);

      // Bind only the newly added delete buttons
      const items = el.list.querySelectorAll('.doc-library-item');
      const newItems = Array.from(items).slice(-docs.length);
      newItems.forEach((item) => {
         const delBtn = item.querySelector('.doc-library-item-delete');
         if (delBtn) handleDeleteClick(delBtn);
         const globalBtn = item.querySelector('.doc-library-item-global');
         if (globalBtn) handleGlobalClick(globalBtn);
         const editBtn = item.querySelector('.doc-library-item-edit');
         if (editBtn) handleEditClick(editBtn);
         const nameEl = item.querySelector('.note-name-clickable');
         if (nameEl) handleNameClick(nameEl);
      });
   }

   function updateLoadMore() {
      if (!el.loadMoreBtn) return;
      if (state.hasMore) {
         el.loadMoreBtn.classList.remove('hidden');
         el.loadMoreBtn.disabled = false;
      } else {
         el.loadMoreBtn.classList.add('hidden');
      }
   }

   function renderIndexing() {
      if (!el.indexingBar) return;
      if (state.indexing) {
         el.indexingBar.classList.remove('hidden');
         el.indexingBar.innerHTML = `
            <div class="spinner"></div>
            <span>Indexing ${escapeHtml(state.indexingFilename)}...</span>`;
      } else {
         el.indexingBar.classList.add('hidden');
         el.indexingBar.innerHTML = '';
      }
   }

   function updateStats() {
      if (el.docCount) {
         el.docCount.textContent = state.documents.length;
      }
      if (el.chunkCount) {
         const total = state.documents.reduce((sum, d) => sum + d.num_chunks, 0);
         el.chunkCount.textContent = total;
      }
   }

   function escapeHtml(str) {
      const div = document.createElement('div');
      div.textContent = str;
      return div.innerHTML;
   }

   // escapeHtml() relies on textContent->innerHTML, which does NOT encode quotes.
   // For interpolation into HTML attribute contexts (title=, aria-label=), the
   // double-quote must also be encoded or a value containing " breaks out of the
   // attribute. Labels/filenames are settable by the LLM and by other users
   // (admin show_all), so this is a stored-XSS sink without it.
   function escapeAttr(str) {
      return escapeHtml(str).replace(/"/g, '&quot;');
   }

   function updateVisibility() {
      if (!el.btn) return;
      const authState = typeof DawnState !== 'undefined' ? DawnState.authState : null;
      if (authState && authState.authenticated) {
         el.btn.classList.remove('hidden');
         // Show admin controls
         const isAdmin = authState.isAdmin || false;
         if (el.showAllLabel) {
            el.showAllLabel.classList.toggle('hidden', !isAdmin);
         }
         if (el.globalLabel) {
            el.globalLabel.classList.toggle('hidden', !isAdmin);
         }
      } else {
         el.btn.classList.add('hidden');
         close();
      }
   }

   /* =============================================================================
    * Public API
    * ============================================================================= */

   window.DawnDocLibrary = {
      init,
      open,
      close,
      toggle,
      updateVisibility,
      handleListResponse,
      handleDeleteResponse,
      handleIndexResponse,
      handleToggleGlobalResponse,
      handleNoteSaveResponse,
      handleNoteUpdateResponse,
      handleVersionListResponse,
      handleVersionRestoreResponse,
      handleDeletedListResponse,
   };
})();
