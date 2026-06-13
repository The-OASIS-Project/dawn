/*
 * Code Projects popover (coding harness).
 *
 * WebSocket-driven panel to list / import / refresh / delete code projects.
 * Mirrors doc-library.js: auth-gated header button, popover toggle, live
 * refresh on the code_project_status_changed broadcast. All server-supplied
 * strings are HTML-escaped before insertion (stored-XSS sink otherwise).
 */
(function () {
   'use strict';

   let el = {};
   let isOpen = false;
   let triggerElement = null;
   let focusTrapCleanup = null;
   const callbacks = { trapFocus: null, showConfirmModal: null };

   /* ---- escaping -------------------------------------------------------- */
   function escapeHtml(str) {
      return String(str == null ? '' : str)
         .replace(/&/g, '&amp;')
         .replace(/</g, '&lt;')
         .replace(/>/g, '&gt;')
         .replace(/"/g, '&quot;')
         .replace(/'/g, '&#39;');
   }

   function toast(msg, kind) {
      if (typeof DawnToast !== 'undefined') {
         DawnToast.show(msg, kind || 'info');
      }
   }

   function setStatus(msg) {
      if (el.status) {
         el.status.textContent = msg || '';
      }
   }

   /* ---- WS helpers ------------------------------------------------------ */
   function wsReady() {
      return typeof DawnWS !== 'undefined' && DawnWS.isConnected();
   }

   function requestList() {
      if (!wsReady()) return;
      DawnWS.send({ type: 'code_projects_list' });
   }

   /* ---- open / close --------------------------------------------------- */
   function open() {
      if (!el.popover) return;
      // Close competing header popovers that share this top-right slot.
      if (typeof DawnMemory !== 'undefined') DawnMemory.close();
      if (typeof DawnSchedulerQueue !== 'undefined') DawnSchedulerQueue.close();
      if (typeof DawnDocLibrary !== 'undefined') DawnDocLibrary.close();

      triggerElement = document.activeElement;
      el.popover.classList.remove('hidden');
      if (el.btn) {
         el.btn.classList.add('active');
         el.btn.setAttribute('aria-expanded', 'true');
      }
      isOpen = true;
      if (callbacks.trapFocus) {
         focusTrapCleanup = callbacks.trapFocus(el.popover);
      }
      requestList();
   }

   function close() {
      if (!el.popover) return;
      el.popover.classList.add('hidden');
      if (el.btn) {
         el.btn.classList.remove('active');
         el.btn.setAttribute('aria-expanded', 'false');
      }
      isOpen = false;
      if (focusTrapCleanup) {
         focusTrapCleanup();
         focusTrapCleanup = null;
      }
      if (triggerElement && typeof triggerElement.focus === 'function') {
         triggerElement.focus();
         triggerElement = null;
      }
   }

   function toggle() {
      if (isOpen) {
         close();
      } else {
         open();
      }
   }

   /* ---- actions -------------------------------------------------------- */
   function submitImport(e) {
      if (e) e.preventDefault();
      if (!wsReady()) return;
      const url = el.urlInput ? el.urlInput.value.trim() : '';
      const name = el.nameInput ? el.nameInput.value.trim() : '';
      const global = el.globalCheck ? !!el.globalCheck.checked : false;
      if (el.importError) el.importError.textContent = '';
      if (!url) {
         if (el.importError) el.importError.textContent = 'A repository URL is required.';
         return;
      }
      const payload = { url: url };
      if (name) payload.name = name;
      if (global) payload.global = true;
      DawnWS.send({ type: 'code_projects_import', payload: payload });
      setStatus('Importing…');
      /* Inputs are cleared on success in handleImportResponse so a failed import
       * keeps the user's typed URL. */
   }

   function refreshProject(name) {
      if (!wsReady() || !name) return;
      DawnWS.send({ type: 'code_projects_refresh', payload: { name: name } });
   }

   function deleteProject(name) {
      if (!wsReady() || !name) return;
      const go = () => DawnWS.send({ type: 'code_projects_delete', payload: { name: name } });
      const message = 'Delete project "' + name + '"? This removes the clone and its index.';
      /* Project names are charset-limited ([a-z0-9_-]) so the textContent modal
       * renders them verbatim. */
      if (callbacks.showConfirmModal) {
         callbacks.showConfirmModal(message, go, { title: 'Delete project', okText: 'Delete' });
      } else if (window.confirm(message)) {
         go();
      }
   }

   /* ---- rendering ------------------------------------------------------ */
   function statusBadge(status) {
      const s = escapeHtml(status);
      const cls = status === 'ready' ? 'ready' : status === 'error' ? 'error' : 'pending';
      return '<span class="code-project-badge ' + cls + '">' + s + '</span>';
   }

   function renderList(projects) {
      if (!el.list) return;
      if (!projects || projects.length === 0) {
         el.list.innerHTML = '<div class="code-projects-empty">No code projects yet.</div>';
         return;
      }
      let html = '';
      projects.forEach((p) => {
         const name = escapeHtml(p.name);
         const src = escapeHtml(p.source_url);
         const msg = p.status_msg ? escapeHtml(p.status_msg) : '';
         const owned = !!p.owned;
         html +=
            '<div class="code-project-row">' +
            '<div class="code-project-main">' +
            '<div class="code-project-name">' +
            name +
            (p.is_global ? ' <span class="code-project-global">global</span>' : '') +
            '</div>' +
            '<div class="code-project-meta">' +
            statusBadge(p.status) +
            (msg ? ' <span class="code-project-msg">' + msg + '</span>' : '') +
            '</div>' +
            '<div class="code-project-src">' +
            src +
            '</div>' +
            '</div>' +
            '<div class="code-project-actions">' +
            (owned
               ? '<button class="btn-link code-project-refresh" data-name="' +
                 name +
                 '" title="Re-index" aria-label="Re-index ' +
                 name +
                 '">&#x21bb;</button>' +
                 '<button class="btn-link code-project-delete" data-name="' +
                 name +
                 '" title="Delete" aria-label="Delete ' +
                 name +
                 '">&times;</button>'
               : '') +
            '</div>' +
            '</div>';
      });
      el.list.innerHTML = html;
      el.list.querySelectorAll('.code-project-refresh').forEach((b) => {
         b.addEventListener('click', () => refreshProject(b.getAttribute('data-name')));
      });
      el.list.querySelectorAll('.code-project-delete').forEach((b) => {
         b.addEventListener('click', () => deleteProject(b.getAttribute('data-name')));
      });
   }

   /* ---- server message handlers --------------------------------------- */
   function handleListResponse(payload) {
      if (!payload || !payload.ok) {
         setStatus('Could not load projects.');
         return;
      }
      setStatus('');
      renderList(payload.projects || []);
   }

   function handleImportResponse(payload) {
      if (!payload) return;
      setStatus('');
      toast(payload.message || (payload.ok ? 'Import started' : 'Import failed'),
            payload.ok ? 'success' : 'error');
      if (payload.ok) {
         if (el.urlInput) el.urlInput.value = '';
         if (el.nameInput) el.nameInput.value = '';
         if (el.globalCheck) el.globalCheck.checked = false;
         requestList();
      }
   }

   function handleActionResponse(payload) {
      if (!payload) return;
      toast(payload.message || (payload.ok ? 'Done' : 'Failed'), payload.ok ? 'success' : 'error');
      if (payload.ok) requestList();
   }

   /* Broadcast: a project's status changed (clone/index progress). Re-fetch the
    * (access-scoped) list while the popover is open. */
   function handleStatusChanged() {
      if (isOpen) requestList();
   }

   /* ---- visibility (auth-gated) --------------------------------------- */
   function updateVisibility(authState) {
      if (!el.btn) return;
      const st = authState || (typeof DawnState !== 'undefined' ? DawnState.authState : null);
      const authed = !!(st && st.authenticated);
      el.btn.classList.toggle('hidden', !authed);
      if (!authed) close();
      if (el.globalLabel) {
         el.globalLabel.classList.toggle('hidden', !(st && st.isAdmin));
      }
   }

   /* ---- init ----------------------------------------------------------- */
   function init(options) {
      if (options) {
         if (options.trapFocus) callbacks.trapFocus = options.trapFocus;
         if (options.showConfirmModal) callbacks.showConfirmModal = options.showConfirmModal;
      }
      el.btn = document.getElementById('coding-btn');
      el.popover = document.getElementById('code-projects-popover');
      el.closeBtn = document.getElementById('code-projects-close');
      el.list = document.getElementById('code-projects-list');
      el.status = document.getElementById('code-projects-status');
      el.form = document.getElementById('code-projects-import-form');
      el.urlInput = document.getElementById('code-projects-url');
      el.nameInput = document.getElementById('code-projects-name');
      el.globalCheck = document.getElementById('code-projects-global-check');
      el.globalLabel = document.getElementById('code-projects-global-label');
      el.importError = document.getElementById('code-projects-import-error');

      if (!el.btn || !el.popover) return;

      el.btn.addEventListener('click', toggle);
      if (el.closeBtn) el.closeBtn.addEventListener('click', close);
      if (el.form) el.form.addEventListener('submit', submitImport);
      document.addEventListener('keydown', (e) => {
         if (e.key === 'Escape' && isOpen) close();
      });

      updateVisibility();
   }

   window.DawnCodeProjects = {
      init: init,
      open: open,
      close: close,
      toggle: toggle,
      updateVisibility: updateVisibility,
      handleListResponse: handleListResponse,
      handleImportResponse: handleImportResponse,
      handleActionResponse: handleActionResponse,
      handleStatusChanged: handleStatusChanged,
   };
})();
