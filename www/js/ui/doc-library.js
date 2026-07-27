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
      // Tabbed note detail: one surface for Read (markdown) + Edit (form).
      detailId: null, // null = create mode; else the open item's id
      detailIsNote: false, // documents open here too, but Read-only (no tabs)
      activePane: 'read', // 'read' | 'edit'
      editorDirty: false, // unsaved edits in the textarea (guards close/switch)
      detailTrigger: null, // element to restore focus to when the detail closes
      // Monotonic guard: a save/update round-trip only applies to the detail if
      // no open/close happened meanwhile (else a late response would stamp the
      // new id/label onto whatever note is now open — silent corruption).
      submitSeq: 0,
      pendingSubmitSeq: null,
   };

   let searchTimer = null;

   // DawnTablist bindings (bound once in init(); persistent DOM)
   let scopeTablist = null;
   let detailTablist = null;

   let callbacks = {
      trapFocus: null,
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
      // v63: tabbed note detail (Read markdown | Edit form)
      el.detail = document.getElementById('doc-library-note-detail');
      el.detailTabs = document.getElementById('doc-library-note-detail-tabs');
      el.tabRead = document.getElementById('doc-library-note-tab-read');
      el.tabEdit = document.getElementById('doc-library-note-tab-edit');
      el.detailTabEls = el.detailTabs ? el.detailTabs.querySelectorAll('.note-detail-tab') : [];
      el.readPane = document.getElementById('doc-library-note-read');
      el.editPane = document.getElementById('doc-library-note-edit');
      el.detailClose = document.getElementById('doc-library-note-detail-close');
      // Read pane — markdown body (notes) / summary (documents)
      el.viewerLabel = document.getElementById('doc-library-note-viewer-label');
      el.viewerText = document.getElementById('doc-library-note-viewer-text');
      el.viewerHistory = document.getElementById('doc-library-note-viewer-history');
      el.viewerDelete = document.getElementById('doc-library-note-viewer-delete');
      el.versions = document.getElementById('doc-library-note-versions');
      // Edit pane — form
      el.noteForm = document.getElementById('doc-library-note-form');
      el.noteLabel = document.getElementById('doc-library-note-label');
      el.noteText = document.getElementById('doc-library-note-text');
      el.noteCount = document.getElementById('doc-library-note-count');
      el.noteError = document.getElementById('doc-library-note-error');
      el.noteLabelHint = document.getElementById('doc-library-note-label-hint');
      el.noteCancel = document.getElementById('doc-library-note-cancel');
      el.deleted =
         document.getElementById('doc-library-note-deleted') ||
         document.getElementById('doc-library-deleted');
      el.deletedToggle = document.getElementById('doc-library-deleted-toggle');

      if (!el.btn || !el.popover) return;

      el.btn.addEventListener('click', toggle);
      el.closeBtn.addEventListener('click', close);

      // Scope tabs (Documents | Notes) — shared roving-tabindex helper
      if (el.tabs && el.tabs.length && typeof DawnTablist !== 'undefined') {
         scopeTablist = DawnTablist.bind({
            tabs: el.tabs,
            getActive: () => state.scope,
            onActivate: setScope,
            attr: 'scope',
         });
      }
      // Note-detail tabs (Read | Edit) — same helper
      if (el.detailTabEls && el.detailTabEls.length && typeof DawnTablist !== 'undefined') {
         detailTablist = DawnTablist.bind({
            tabs: el.detailTabEls,
            getActive: () => state.activePane,
            onActivate: switchPane,
            attr: 'pane',
         });
      }

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

      // Note detail — create / edit form
      if (el.newNoteBtn) el.newNoteBtn.addEventListener('click', () => openDetail(null, 'edit'));
      if (el.noteCancel) el.noteCancel.addEventListener('click', onCancelEdit);
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

      // Note detail — Read pane actions + close
      if (el.detailClose) el.detailClose.addEventListener('click', () => requestCloseDetail());
      if (el.viewerDelete) {
         el.viewerDelete.addEventListener('click', () => {
            if (state.detailId != null) confirmDelete(state.detailId);
         });
      }
      if (el.viewerHistory) {
         el.viewerHistory.addEventListener('click', () => {
            if (state.detailId != null) requestVersionList(state.detailId);
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
         if (el.detail && !el.detail.classList.contains('hidden')) {
            requestCloseDetail();
         } else {
            close();
         }
      });
   }

   /* =============================================================================
    * Tabs + search
    * ============================================================================= */

   async function setScope(scope) {
      if (scope !== 'documents' && scope !== 'notes') return;
      // Guard an unsaved editor before navigating away (consistent with Cancel/ESC)
      if (state.editorDirty) {
         if (
            await DawnDialog.confirm('Discard changes to this note?', {
               title: 'Discard note',
               okText: 'Discard',
            })
         ) {
            applyScope(scope);
         }
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
      closeDetail(false);
      // Roving tabindex + aria via the shared helper (falls back to a manual
      // loop if DawnTablist is unavailable for any reason)
      if (scopeTablist) {
         scopeTablist.sync();
      } else {
         el.tabs.forEach((tab) => {
            const active = tab.dataset.scope === scope;
            tab.setAttribute('aria-selected', active ? 'true' : 'false');
            tab.tabIndex = active ? 0 : -1;
         });
      }
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
      if (typeof DawnJobs !== 'undefined') DawnJobs.close();

      triggerElement = document.activeElement;
      el.popover.classList.remove('hidden');
      el.btn.classList.add('active');
      state.isOpen = true;

      // Set up focus trap
      if (callbacks.trapFocus) {
         focusTrapCleanup = callbacks.trapFocus(el.popover);
      }

      // Reset to a clean state: restore the last-used tab, no search, detail closed
      closeDetail(false);
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
      // If the deleted item was open in the detail (either pane), close it
      if (state.detailId === payload.id) closeDetail(false);
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
    * Tabbed note detail — Read (markdown) + Edit (form) on one surface
    * ============================================================================= */

   /* Open the detail for an item on the given pane.  item === null → create mode
    * (Edit pane, empty form).  The textarea is the SINGLE source of truth for the
    * note text; the Read pane renders markdown from it.  Documents open here too,
    * but Read-only (tab strip hidden, summary sentence instead of markdown). */
   function openDetail(item, pane) {
      if (!el.detail) return;
      state.submitSeq++; // invalidate any in-flight save/update for the prior detail
      // Remember what to return focus to when the detail closes (WCAG 2.4.3)
      state.detailTrigger = document.activeElement;
      state.detailId = item ? item.id : null;
      state.detailIsNote = item ? !!(item.is_note || item.filetype === 'note') : true;
      state.editorDirty = false;
      const isNote = state.detailIsNote;
      const isCreate = state.detailId == null;

      el.viewerLabel.textContent = item ? item.filename : 'New note';
      // Seed the form (single text source).  Only notes are inline-editable.
      el.noteLabel.value = isNote && !isCreate ? item.filename : '';
      el.noteText.value = isNote && !isCreate ? item.text || '' : '';
      el.noteLabelHint.textContent = isCreate
         ? 'Friday can read this back to you by label.'
         : 'Changing the label changes how you’ll refer to this.';
      hideNoteError();
      updateNoteCount();

      // Documents: no Edit tab, Read pane is a plain region (no orphan tabpanel).
      if (el.detailTabs) el.detailTabs.classList.toggle('hidden', !isNote);
      if (el.readPane) {
         if (isNote) {
            el.readPane.setAttribute('role', 'tabpanel');
            el.readPane.setAttribute('aria-labelledby', 'doc-library-note-tab-read');
            el.readPane.removeAttribute('aria-label');
         } else {
            el.readPane.setAttribute('role', 'region');
            el.readPane.setAttribute('aria-label', 'Document');
            el.readPane.removeAttribute('aria-labelledby');
         }
      }
      updateReadActions();

      hideVersions();
      if (el.notesActions) el.notesActions.classList.add('hidden');
      el.detail.classList.remove('hidden');

      // Documents force Read; notes honor the requested pane.
      switchPane(isNote && pane === 'edit' ? 'edit' : 'read');
      // Initial focus per pane (switchPane already focuses the label for Edit)
      if (state.activePane === 'read') {
         const target = isNote ? el.tabRead : el.readPane;
         if (target && target.focus) target.focus();
      }
   }

   // Guard an unsaved edit before opening a different item (name click / pencil)
   async function requestOpenDetail(item, pane) {
      if (state.editorDirty) {
         if (
            await DawnDialog.confirm('Discard changes to this note?', {
               title: 'Discard note',
               okText: 'Discard',
            })
         ) {
            openDetail(item, pane);
         }
         return;
      }
      openDetail(item, pane);
   }

   // restoreFocus: return focus to the opener (Close/ESC); NOT on scope reset.
   function closeDetail(restoreFocus) {
      if (!el.detail) return;
      state.submitSeq++; // invalidate any in-flight save/update for this detail
      const wasOpen = !el.detail.classList.contains('hidden');
      el.detail.classList.add('hidden');
      hideVersions();
      hideNoteError();
      state.detailId = null;
      state.detailIsNote = false;
      state.editorDirty = false;
      // Restore the "+ New note" affordance on the Notes tab
      if (el.notesActions) el.notesActions.classList.toggle('hidden', state.scope !== 'notes');
      if (restoreFocus && wasOpen && state.detailTrigger && state.detailTrigger.focus) {
         state.detailTrigger.focus();
      }
      state.detailTrigger = null;
   }

   // Close button / ESC: confirm if there are unsaved edits
   async function requestCloseDetail() {
      if (state.editorDirty) {
         if (
            await DawnDialog.confirm('Discard changes to this note?', {
               title: 'Discard note',
               okText: 'Discard',
            })
         ) {
            closeDetail(true);
         }
      } else {
         closeDetail(true);
      }
   }

   // Switch between Read and Edit panes.  Read is a pure (non-destructive) render
   // of the live textarea, so Edit→Read needs no discard guard and does NOT clear
   // state.editorDirty — a later Close/ESC/scope-switch still guards.
   function switchPane(name) {
      if (name !== 'read' && name !== 'edit') return;
      if (!state.detailIsNote) name = 'read'; // documents have no Edit pane
      state.activePane = name;
      if (el.readPane) el.readPane.classList.toggle('hidden', name !== 'read');
      if (el.editPane) el.editPane.classList.toggle('hidden', name !== 'edit');
      if (detailTablist) detailTablist.sync();
      if (name === 'read') {
         renderReadPane();
      } else if (el.noteLabel && el.noteLabel.focus) {
         el.noteLabel.focus();
      }
   }

   // Render the Read pane: markdown for notes (from the live textarea), or a
   // summary sentence for documents (their body lives server-side).
   function renderReadPane() {
      if (!el.viewerText) return;
      if (!state.detailIsNote) {
         const item = state.documents.find((d) => d.id === state.detailId);
         const n = item ? item.num_chunks || 0 : 0;
         el.viewerText.textContent = `Document · ${n} part${n === 1 ? '' : 's'}. Ask Friday to read or edit its content; version history and delete are below.`;
         return;
      }
      const text = el.noteText ? el.noteText.value || '' : '';
      if (!text.trim()) {
         el.viewerText.textContent = 'Nothing to preview yet.';
         return;
      }
      if (typeof DawnFormat !== 'undefined' && DawnFormat.markdown) {
         el.viewerText.innerHTML = DawnFormat.markdown(text);
      } else {
         el.viewerText.textContent = text; // fallback if the formatter is unavailable
      }
   }

   // History + Delete apply to existing items only; hide them in create mode.
   function updateReadActions() {
      const existing = state.detailId != null;
      if (el.viewerHistory) el.viewerHistory.style.display = existing ? '' : 'none';
      if (el.viewerDelete) el.viewerDelete.style.display = existing ? '' : 'none';
   }

   // Cancel in the Edit form: existing note → back to Read (discard-guarded);
   // create mode → close the detail.
   async function onCancelEdit() {
      const revert = () => {
         if (state.detailId != null) {
            const item = state.documents.find((d) => d.id === state.detailId);
            el.noteLabel.value = item ? item.filename : el.noteLabel.value;
            el.noteText.value = item ? item.text || '' : '';
            state.editorDirty = false;
            hideNoteError();
            updateNoteCount();
            switchPane('read');
            if (el.tabRead && el.tabRead.focus) el.tabRead.focus();
         } else {
            closeDetail(true);
         }
      };
      if (state.editorDirty) {
         if (
            await DawnDialog.confirm('Discard changes to this note?', {
               title: 'Discard note',
               okText: 'Discard',
            })
         ) {
            revert();
         }
      } else {
         revert();
      }
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
      // Only show history for the item currently open in the detail.
      if (payload.id !== state.detailId) return;
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
         btn.addEventListener('click', async () => {
            const vid = parseInt(btn.dataset.versionId, 10);
            if (
               await DawnDialog.confirm(
                  'Restore this version? The current text is saved to history first, so you can undo.',
                  { title: 'Restore version', okText: 'Restore' }
               )
            ) {
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
      closeDetail(false);
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
      // Tag this round-trip so a late response can't apply to a re-targeted detail.
      state.pendingSubmitSeq = ++state.submitSeq;
      if (state.detailId != null) {
         requestNoteUpdate(state.detailId, label, text);
      } else {
         requestNoteSave(label, text);
      }
   }

   // True if the in-flight save/update still targets the currently-open detail.
   // If the user closed the detail or opened another note meanwhile, the response
   // must NOT stamp its id/label onto the new target (silent corruption).
   function submitStillCurrent() {
      const current =
         state.pendingSubmitSeq != null &&
         state.pendingSubmitSeq === state.submitSeq &&
         el.detail &&
         !el.detail.classList.contains('hidden');
      state.pendingSubmitSeq = null;
      return current;
   }

   // Save (create) success: stay open on Read with the just-saved text; adopt the
   // new id so a follow-up edit issues an update (not a duplicate create).
   function handleNoteSaveResponse(payload) {
      if (!payload?.success) {
         // Show the error only if the user is still in this editor (else it's stale).
         if (submitStillCurrent()) showNoteError(payload?.error || 'Could not save the note.');
         return;
      }
      if (typeof DawnToast !== 'undefined') DawnToast.show('Note saved', 'success');
      if (!submitStillCurrent()) {
         refreshList(); // detail moved on — just pick up the new row
         return;
      }
      if (payload.id != null) state.detailId = payload.id;
      state.detailIsNote = true;
      state.editorDirty = false;
      if (payload.label != null && el.viewerLabel) el.viewerLabel.textContent = payload.label;
      updateReadActions();
      hideVersions();
      switchPane('read');
      if (el.tabRead && el.tabRead.focus) el.tabRead.focus();
      refreshList();
   }

   // Update success: stay open on Read, re-rendered from the saved textarea text.
   function handleNoteUpdateResponse(payload) {
      if (!payload?.success) {
         if (submitStillCurrent()) showNoteError(payload?.error || 'Could not update the note.');
         return;
      }
      if (typeof DawnToast !== 'undefined') DawnToast.show('Note updated', 'success');
      if (!submitStillCurrent()) {
         refreshList();
         return;
      }
      state.editorDirty = false;
      const label =
         payload.label != null ? payload.label : el.noteLabel ? el.noteLabel.value.trim() : '';
      if (label && el.viewerLabel) el.viewerLabel.textContent = label;
      hideVersions();
      switchPane('read');
      if (el.tabRead && el.tabRead.focus) el.tabRead.focus();
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
         if (doc) requestOpenDetail(doc, 'edit');
      });
   }

   async function confirmDelete(id) {
      const doc = state.documents.find((d) => d.id === id);
      const isNote = doc && (doc.is_note || doc.filetype === 'note');
      const name = doc ? doc.filename : isNote ? 'this note' : 'this document';
      const noun = isNote ? 'note' : 'document';
      if (
         await DawnDialog.confirm(
            `Delete "${name}"? You can restore it from "Recently deleted" for a short window.`,
            {
               title: `Delete ${noun.charAt(0).toUpperCase() + noun.slice(1)}`,
               okText: 'Delete',
               danger: true,
            }
         )
      ) {
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
         if (doc) requestOpenDetail(doc, 'read');
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
