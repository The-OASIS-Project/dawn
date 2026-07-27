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
   let escToken = null; /* DawnEscStack registration while the popover is open */
   let featureEnabled = false; /* [code_projects].enabled from server config */
   let activeTab = 'import'; /* 'import' | 'link' — creation-path tab */
   let tablist = null; /* DawnTablist binding (arrow-key/roving-tabindex a11y) */
   const callbacks = { trapFocus: null };

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
      if (typeof DawnJobs !== 'undefined') DawnJobs.close();

      triggerElement = document.activeElement;
      el.popover.classList.remove('hidden');
      if (el.btn) {
         el.btn.classList.add('active');
         el.btn.setAttribute('aria-expanded', 'true');
      }
      isOpen = true;
      escToken = DawnEscStack.register(() => {
         close();
         return true;
      });
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
      if (escToken !== null) {
         DawnEscStack.unregister(escToken);
         escToken = null;
      }
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

   /* ---- creation-path tabs (Import URL | Link local) ------------------- */
   function selectTab(which) {
      /* The Link tab only exists for admins; ignore a request to switch to it
       * when it's hidden (keeps non-admins on the import path). */
      if (which === 'link' && el.tabLink && el.tabLink.classList.contains('hidden')) {
         which = 'import';
      }
      activeTab = which === 'link' ? 'link' : 'import';
      if (el.form) el.form.classList.toggle('hidden', activeTab !== 'import');
      if (el.linkForm) el.linkForm.classList.toggle('hidden', activeTab !== 'link');
      /* DawnTablist.sync() owns the tab buttons' .active / aria-selected / roving
       * tabindex from getActive(). */
      if (tablist) tablist.sync();
   }

   /* Show or clear an inline form error. `.dawn-form .form-error` is display:none
    * until `.visible` is added (the shared primitive's contract). */
   function setFormError(elError, msg) {
      if (!elError) return;
      elError.textContent = msg || '';
      elError.classList.toggle('visible', !!msg);
   }

   /* ---- actions -------------------------------------------------------- */
   function submitImport(e) {
      if (e) e.preventDefault();
      if (!wsReady()) return;
      const url = el.urlInput ? el.urlInput.value.trim() : '';
      const name = el.nameInput ? el.nameInput.value.trim() : '';
      const branch = el.branchInput ? el.branchInput.value.trim() : '';
      const global = el.globalCheck ? !!el.globalCheck.checked : false;
      setFormError(el.importError, '');
      if (!url) {
         setFormError(el.importError, 'A repository URL is required.');
         return;
      }
      const payload = { url: url };
      if (name) payload.name = name;
      if (branch) payload.branch = branch;
      if (global) payload.global = true;
      DawnWS.send({ type: 'code_projects_import', payload: payload });
      setStatus('Importing…');
      /* Inputs are cleared on success in handleImportResponse so a failed import
       * keeps the user's typed URL. */
   }

   function submitLink(e) {
      if (e) e.preventDefault();
      if (!wsReady()) return;
      const path = el.linkPathInput ? el.linkPathInput.value.trim() : '';
      const name = el.linkNameInput ? el.linkNameInput.value.trim() : '';
      setFormError(el.linkError, '');
      if (!path) {
         setFormError(el.linkError, 'A local path is required.');
         return;
      }
      const payload = { path: path };
      if (name) payload.name = name;
      DawnWS.send({ type: 'code_projects_link', payload: payload });
      setStatus('Linking…');
   }

   function refreshProject(name) {
      if (!wsReady() || !name) return;
      DawnWS.send({ type: 'code_projects_refresh', payload: { name: name } });
   }

   function rebuildProject(name) {
      if (!wsReady() || !name) return;
      DawnWS.send({ type: 'code_projects_rebuild', payload: { name: name } });
   }

   async function setBranchProject(name, currentBranch) {
      if (!wsReady() || !name) return;
      const branch = await DawnDialog.prompt(
         'Branch to track for "' + name + '":',
         currentBranch || '',
         {
            title: 'Set Branch',
         }
      );
      if (branch == null) return; /* cancelled */
      const trimmed = branch.trim();
      if (!trimmed) return;
      DawnWS.send({ type: 'code_projects_set_branch', payload: { name: name, branch: trimmed } });
   }

   async function deleteProject(name) {
      if (!wsReady() || !name) return;
      const message = 'Delete project "' + name + '"? This removes the clone and its index.';
      /* Project names are charset-limited ([a-z0-9_-]) so the textContent modal
       * renders them verbatim. */
      if (
         await DawnDialog.confirm(message, {
            title: 'Delete project',
            okText: 'Delete',
            danger: true,
         })
      ) {
         DawnWS.send({ type: 'code_projects_delete', payload: { name: name } });
      }
   }

   /* ---- rendering ------------------------------------------------------ */
   function statusBadge(status) {
      const s = escapeHtml(status);
      const cls = status === 'ready' ? 'ready' : status === 'error' ? 'error' : 'pending';
      return '<span class="dawn-badge code-project-badge ' + cls + '">' + s + '</span>';
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
         const isLocal = p.kind === 'local';
         /* For a linked local repo there is no remote URL — show the kind instead. */
         const src = escapeHtml(isLocal ? 'local repo' : p.source_url);
         const branch = p.branch ? escapeHtml(p.branch) : '';
         const msg = p.status_msg ? escapeHtml(p.status_msg) : '';
         const owned = !!p.owned;
         html +=
            '<div class="code-project-row">' +
            '<div class="code-project-main">' +
            '<div class="code-project-name">' +
            '<span class="code-project-name-text">' +
            name +
            '</span>' +
            (p.is_global ? '<span class="dawn-badge accent">global</span>' : '') +
            (isLocal ? '<span class="dawn-badge muted">local</span>' : '') +
            '</div>' +
            '<div class="code-project-meta">' +
            statusBadge(p.status) +
            (branch ? '<span class="code-project-branch">' + branch + '</span>' : '') +
            '</div>' +
            (msg ? '<div class="code-project-msg">' + msg + '</div>' : '') +
            '<div class="code-project-src">' +
            src +
            '</div>' +
            '</div>' +
            '<div class="code-project-actions">' +
            (owned
               ? '<button class="btn-link code-project-refresh" data-name="' +
                 name +
                 '" title="Re-index (fetch + incremental)" aria-label="Re-index ' +
                 name +
                 '">&#x21bb;</button>' +
                 '<button class="btn-link code-project-rebuild" data-name="' +
                 name +
                 '" title="Clean rebuild" aria-label="Rebuild ' +
                 name +
                 '">&#x267b;</button>' +
                 /* Branch is read-only for linked local repos (tracks the live checkout). */
                 (isLocal
                    ? ''
                    : '<button class="btn-link code-project-branch-btn" data-name="' +
                      name +
                      '" data-branch="' +
                      branch +
                      '" title="Set branch" aria-label="Set branch for ' +
                      name +
                      '">&#x26d6;</button>') +
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
      el.list.querySelectorAll('.code-project-rebuild').forEach((b) => {
         b.addEventListener('click', () => rebuildProject(b.getAttribute('data-name')));
      });
      el.list.querySelectorAll('.code-project-branch-btn').forEach((b) => {
         b.addEventListener('click', () =>
            setBranchProject(b.getAttribute('data-name'), b.getAttribute('data-branch'))
         );
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
      toast(
         payload.message || (payload.ok ? 'Import started' : 'Import failed'),
         payload.ok ? 'success' : 'error'
      );
      if (payload.ok) {
         if (el.urlInput) el.urlInput.value = '';
         if (el.nameInput) el.nameInput.value = '';
         if (el.branchInput) el.branchInput.value = '';
         if (el.globalCheck) el.globalCheck.checked = false;
         requestList();
      }
   }

   function handleLinkResponse(payload) {
      if (!payload) return;
      setStatus('');
      toast(
         payload.message || (payload.ok ? 'Linked' : 'Link failed'),
         payload.ok ? 'success' : 'error'
      );
      if (payload.ok) {
         if (el.linkPathInput) el.linkPathInput.value = '';
         if (el.linkNameInput) el.linkNameInput.value = '';
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

   /* Broadcast: a pending import was rejected by the worker before any row was
    * created (repo not found/unreachable, or a duplicate). No list change to
    * make — just surface why, and clear the "Checking…" status. */
   function handleImportFailed(payload) {
      if (!payload) return;
      setStatus('');
      const name = payload.name ? "'" + payload.name + "' " : '';
      const reason = payload.reason || 'Repository not found or unreachable.';
      toast('Import of ' + name + 'failed: ' + reason, 'error');
   }

   /* ---- visibility (auth-gated) --------------------------------------- */
   function updateVisibility(authState) {
      if (!el.btn) return;
      const st = authState || (typeof DawnState !== 'undefined' ? DawnState.authState : null);
      const authed = !!(st && st.authenticated);
      /* Hide the button entirely unless the subsystem is enabled in config — an
       * always-visible button that only ever reports "disabled" is confusing. */
      const show = authed && featureEnabled;
      el.btn.classList.toggle('hidden', !show);
      if (!show) close();
      const isAdmin = !!(st && st.isAdmin);
      if (el.globalLabel) {
         el.globalLabel.classList.toggle('hidden', !isAdmin);
      }
      /* Linking an arbitrary local path is admin-only — hide the tab + form, and
       * collapse the (now single-option) segmented control. Non-admins always sit
       * on the import path. */
      if (el.tabLink) el.tabLink.classList.toggle('hidden', !isAdmin);
      if (el.tabs) el.tabs.classList.toggle('single', !isAdmin);
      if (!isAdmin) selectTab('import');
   }

   /* Called from dawn.js on get_config_response (fires at connect and after every
    * settings save), so toggling "Enable Code Projects" reveals/hides the button
    * without a reload. */
   function setEnabled(enabled) {
      featureEnabled = !!enabled;
      updateVisibility();
   }

   /* ---- init ----------------------------------------------------------- */
   function init(options) {
      if (options) {
         if (options.trapFocus) callbacks.trapFocus = options.trapFocus;
      }
      el.btn = document.getElementById('coding-btn');
      el.popover = document.getElementById('code-projects-popover');
      el.closeBtn = document.getElementById('code-projects-close');
      el.list = document.getElementById('code-projects-list');
      el.status = document.getElementById('code-projects-status');
      el.form = document.getElementById('code-projects-import-form');
      el.urlInput = document.getElementById('code-projects-url');
      el.nameInput = document.getElementById('code-projects-name');
      el.branchInput = document.getElementById('code-projects-branch');
      el.globalCheck = document.getElementById('code-projects-global-check');
      el.globalLabel = document.getElementById('code-projects-global-label');
      el.importError = document.getElementById('code-projects-import-error');
      el.linkForm = document.getElementById('code-projects-link-form');
      el.linkPathInput = document.getElementById('code-projects-link-path');
      el.linkNameInput = document.getElementById('code-projects-link-name');
      el.linkError = document.getElementById('code-projects-link-error');
      el.tabs = document.querySelector('.code-projects-tabs');
      el.tabImport = document.getElementById('code-projects-tab-import');
      el.tabLink = document.getElementById('code-projects-tab-link');

      if (!el.btn || !el.popover) return;

      el.btn.addEventListener('click', toggle);
      if (el.closeBtn) el.closeBtn.addEventListener('click', close);
      if (el.form) el.form.addEventListener('submit', submitImport);
      if (el.linkForm) el.linkForm.addEventListener('submit', submitLink);
      /* Shared tablist helper owns clicks + arrow-key/Home/End + roving tabindex
       * (WAI-ARIA), consistent with the other popovers. data-tab on each button
       * carries the identifier. */
      if (window.DawnTablist && el.tabs) {
         tablist = window.DawnTablist.bind({
            tabs: el.tabs.querySelectorAll('.code-projects-tab'),
            getActive: () => activeTab,
            onActivate: (name) => selectTab(name),
         });
         tablist.sync();
      }
      // Escape close is handled via DawnEscStack (register-on-open in open() /
      // unregister-on-close in close()).

      updateVisibility();
   }

   window.DawnCodeProjects = {
      init: init,
      open: open,
      close: close,
      toggle: toggle,
      updateVisibility: updateVisibility,
      setEnabled: setEnabled,
      handleListResponse: handleListResponse,
      handleImportResponse: handleImportResponse,
      handleLinkResponse: handleLinkResponse,
      handleActionResponse: handleActionResponse,
      handleStatusChanged: handleStatusChanged,
      handleImportFailed: handleImportFailed,
   };
})();
