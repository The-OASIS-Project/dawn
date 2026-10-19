/**
 * DAWN Calendar Accounts Module
 * Per-user CalDAV account management in Settings panel.
 */
(function () {
   'use strict';

   let callbacks = {
      getAuthState: null,
   };

   let accounts = [];

   /* =============================================================================
    * API Requests
    * ============================================================================= */

   function requestListAccounts() {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({ type: 'calendar_list_accounts' });
      }
   }

   function requestAddAccount(name, caldavUrl, username, password, readOnly, authType, oauthKey) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         const payload = {
            name: name,
            caldav_url: caldavUrl,
            username: username,
            password: password,
            read_only: readOnly || false,
         };
         if (authType === 'oauth') {
            payload.auth_type = 'oauth';
            payload.oauth_account_key = oauthKey || '';
         }
         DawnWS.send({ type: 'calendar_add_account', payload: payload });
      }
   }

   function requestRemoveAccount(id) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({
            type: 'calendar_remove_account',
            payload: { id: id },
         });
      }
   }

   function requestTestAccount(id) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({
            type: 'calendar_test_account',
            payload: { id: id },
         });
      }
   }

   function requestSyncAccount(id) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({
            type: 'calendar_sync_account',
            payload: { id: id },
         });
      }
   }

   function requestListCalendars(accountId) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({
            type: 'calendar_list_calendars',
            payload: { account_id: accountId },
         });
      }
   }

   function requestToggleCalendar(id, isActive) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({
            type: 'calendar_toggle_calendar',
            payload: { id: id, is_active: isActive },
         });
      }
   }

   function requestEditAccount(id, name, caldavUrl, username, password) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         const payload = { id: id, name: name, caldav_url: caldavUrl, username: username };
         if (password) payload.password = password;
         DawnWS.send({ type: 'calendar_edit_account', payload: payload });
      }
   }

   function requestToggleReadOnly(id, readOnly) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({
            type: 'calendar_toggle_read_only',
            payload: { id: id, read_only: readOnly },
         });
      }
   }

   function requestSetEnabled(id, enabled) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({
            type: 'calendar_set_enabled',
            payload: { id: id, enabled: enabled },
         });
      }
   }

   /* =============================================================================
    * Response Handlers
    * ============================================================================= */

   function handleListAccountsResponse(payload) {
      if (!payload.success) {
         showToast('Failed to load calendar accounts: ' + (payload.error || 'Unknown'), 'error');
         return;
      }
      accounts = payload.accounts || [];
      renderAccounts();
   }

   function handleAddAccountResponse(payload) {
      resetSubmitButton();
      if (!payload.success) {
         if (payload.error === 'Account already exists') {
            /* Silently ignore — likely a cross-service add for an existing account */
            requestListAccounts();
         } else {
            showToast('Failed to add account: ' + (payload.error || 'Unknown'), 'error');
         }
         return;
      }
      showToast('Calendar account added', 'success');
      hideAddAccountModal();
      requestListAccounts();
   }

   function handleEditAccountResponse(payload) {
      const submitBtn = document.getElementById('cal-edit-modal-submit');
      if (submitBtn) {
         submitBtn.disabled = false;
         submitBtn.textContent = 'Save';
      }
      if (!payload.success) {
         showToast('Failed to update account: ' + (payload.error || 'Unknown'), 'error');
         return;
      }
      showToast('Calendar account updated', 'success');
      hideEditAccountModal();
      requestListAccounts();
   }

   function handleRemoveAccountResponse(payload) {
      if (!payload.success) {
         showToast('Failed to remove account: ' + (payload.error || 'Unknown'), 'error');
         return;
      }
      showToast('Calendar account removed', 'success');
      requestListAccounts();
   }

   function handleTestAccountResponse(payload) {
      clearLoadingButtons();
      if (!payload.success) {
         showToast('Connection test failed: ' + (payload.error || 'Unknown'), 'error');
         return;
      }
      showToast(payload.message || 'Connection successful', 'success');
      requestListAccounts();
   }

   /* Accounts whose sync this tab asked for and is waiting on. */
   const pendingSyncs = new Set();

   function handleSyncAccountResponse(payload) {
      if (!payload.success) {
         /* Only this account's request failed; another may still be syncing. */
         if (payload.id !== undefined) {
            pendingSyncs.delete(String(payload.id));
            clearLoadingButton(payload.id);
         } else {
            clearLoadingButtons();
         }
         showToast('Sync failed: ' + (payload.error || 'Unknown'), 'error');
         return;
      }
      if (payload.queued) return; /* calendar_sync_done reports it */
      clearLoadingButtons();
      showToast('Calendar synced successfully', 'success');
      requestListAccounts();
   }

   function handleSyncDone(payload) {
      if (pendingSyncs.delete(String(payload.id))) {
         clearLoadingButton(payload.id);
         if (payload.success) showToast('Calendar synced successfully', 'success');
         else showToast('Sync failed: check the calendar server', 'error');
      }
      requestListAccounts();
   }

   function handleListCalendarsResponse(payload) {
      if (!payload.success) {
         showToast('Failed to load calendars: ' + (payload.error || 'Unknown'), 'error');
         return;
      }
      renderCalendars(payload.calendars || []);
   }

   function handleToggleCalendarResponse(payload) {
      if (!payload.success) {
         showToast('Failed to toggle calendar: ' + (payload.error || 'Unknown'), 'error');
      }
   }

   function handleToggleReadOnlyResponse(payload) {
      if (!payload.success) {
         showToast('Failed to update access: ' + (payload.error || 'Unknown'), 'error');
      }
      requestListAccounts();
   }

   function handleSetEnabledResponse(payload) {
      if (!payload.success) {
         showToast('Failed to update enabled state: ' + (payload.error || 'Unknown'), 'error');
      }
      requestListAccounts();
   }

   /* =============================================================================
    * Rendering
    * ============================================================================= */

   function escapeHtml(str) {
      const div = document.createElement('div');
      div.textContent = str || '';
      return div.innerHTML;
   }

   function formatSyncTime(timestamp) {
      if (!timestamp || timestamp === 0) return 'Never';
      const date = new Date(timestamp * 1000);
      const now = new Date();
      const diff = Math.floor((now - date) / 1000);
      if (diff < 0) return DawnTime.formatDate(timestamp);
      if (diff < 60) return 'Just now';
      if (diff < 3600) return Math.floor(diff / 60) + 'm ago';
      if (diff < 86400) return Math.floor(diff / 3600) + 'h ago';
      return DawnTime.formatDate(timestamp);
   }

   function renderAccounts() {
      const list = document.getElementById('calendar-accounts-list');
      if (!list) return;

      if (accounts.length === 0) {
         list.innerHTML =
            '<div class="calendar-accounts-empty">No calendar accounts configured. Click "Add Account" to connect.</div>';
         return;
      }

      list.innerHTML = accounts
         .map(
            (acct) => `
         <div class="calendar-account-card${acct.read_only ? ' read-only' : ''}${!acct.enabled ? ' disabled-account' : ''}" data-account-id="${acct.id}">
            <div class="calendar-account-header">
               <div class="calendar-account-title">
                  <span class="calendar-account-name">${escapeHtml(acct.name)}</span>${acct.auth_type === 'oauth' ? '<span class="dawn-badge accent">OAuth</span>' : ''}${acct.read_only ? '<span class="dawn-badge warning">Read-only</span>' : ''}
               </div>
               <div class="calendar-account-actions">
                  ${acct.auth_type !== 'oauth' ? `<button class="btn btn-secondary btn-small" data-action="edit" data-id="${acct.id}" title="Edit account">Edit</button>` : ''}
                  <button class="btn btn-secondary btn-small" data-action="test" data-id="${acct.id}" title="Test connection">Test</button>
                  <button class="btn btn-secondary btn-small" data-action="sync" data-id="${acct.id}" title="Sync now">Sync</button>
                  <button class="btn btn-secondary btn-small" data-action="calendars" data-id="${acct.id}" title="Show calendars">Calendars</button>
                  <button class="btn btn-danger btn-small" data-action="remove" data-id="${acct.id}" data-name="${escapeHtml(acct.name)}" data-auth-type="${acct.auth_type || 'password'}" data-oauth-key="${escapeHtml(acct.oauth_account_key || '')}" title="Remove account">Remove</button>
               </div>
            </div>
            <div class="calendar-account-details">
               ${
                  acct.auth_type === 'oauth'
                     ? `
               <div class="detail-row">
                  <span class="detail-label">Type</span>
                  <span><span class="dawn-status-dot success"></span>Google OAuth</span>
               </div>
               ${acct.oauth_account_key ? `<div class="detail-row oauth-shared-info"><span class="detail-label"></span><span class="detail-secondary">${escapeHtml(acct.oauth_account_key)}${getSharedServicesText(acct.oauth_account_key)}</span></div>` : ''}`
                     : `
               <div class="detail-row">
                  <span class="detail-label">URL</span>
                  <span>${escapeHtml(acct.caldav_url)}</span>
               </div>
               <div class="detail-row">
                  <span class="detail-label">User</span>
                  <span>${escapeHtml(acct.username)}</span>
               </div>`
               }
            </div>
            <div class="calendar-account-access">
               <input type="checkbox" class="dawn-toggle"
                  ${acct.enabled ? 'checked' : ''}
                  data-action="toggle-enabled" data-id="${acct.id}"
                  title="${acct.enabled ? 'Account is active' : 'Account is disabled'}"
                  aria-label="Enable ${escapeHtml(acct.name)}"
               />
               <span class="dawn-toggle-label">Enabled</span>
               <input type="checkbox" class="dawn-toggle"
                  ${acct.read_only ? 'checked' : ''}
                  data-action="toggle-read-only" data-id="${acct.id}"
                  title="${acct.read_only ? 'AI cannot modify this account' : 'AI can create/edit/delete events'}"
                  aria-label="Read only for ${escapeHtml(acct.name)}"
               />
               <span class="dawn-toggle-label">Read only</span>
            </div>
            <div class="calendar-account-status">
               <span class="dawn-status-dot ${acct.last_sync > 0 ? 'success' : 'warning'}" title="${acct.last_sync > 0 ? 'Synced' : 'Never synced'}"></span>
               <span>Last sync: ${formatSyncTime(acct.last_sync)}</span>
            </div>
            <div class="calendar-sub-list" id="cal-list-${acct.id}" style="display:none;"></div>
         </div>
      `
         )
         .join('');
      /* A redraw keeps the Sync button busy for syncs still running. */
      pendingSyncs.forEach((id) => setButtonLoading(id, 'Sync'));
   }

   function renderCalendars(calendars) {
      const accountId = currentExpandedAccount;
      const subList = document.getElementById('cal-list-' + accountId);
      if (!subList) return;

      subList.style.display = 'flex';

      if (calendars.length === 0) {
         subList.innerHTML =
            '<div class="calendar-sub-empty">No calendars found. Try "Test" to discover calendars.</div>';
         return;
      }

      subList.innerHTML = calendars
         .map(
            (cal) => `
         <div class="calendar-sub-item">
            <span class="calendar-sub-name">
               <span class="calendar-color-dot" style="background:${escapeHtml(cal.color || '#888')}"></span>
               ${escapeHtml(cal.display_name)}
            </span>
            <input type="checkbox" class="dawn-toggle"
               ${cal.is_active ? 'checked' : ''}
               data-cal-id="${cal.id}"
               title="${cal.is_active ? 'Disable' : 'Enable'} this calendar"
               aria-label="${cal.is_active ? 'Disable' : 'Enable'} ${escapeHtml(cal.display_name)}"
            />
         </div>
      `
         )
         .join('');
   }

   let currentExpandedAccount = null;

   /* =============================================================================
    * Event Delegation
    * ============================================================================= */

   function handleAccountListClick(e) {
      const btn = e.target.closest('[data-action]');
      if (!btn) return;

      const action = btn.dataset.action;
      const id = parseInt(btn.dataset.id, 10);

      switch (action) {
         case 'edit':
            editAccount(id);
            break;
         case 'test':
            testAccount(id);
            break;
         case 'sync':
            syncAccount(id);
            break;
         case 'calendars':
            expandCalendars(id);
            break;
         case 'remove':
            removeAccount(id, btn.dataset.name || '', btn.dataset.authType, btn.dataset.oauthKey);
            break;
      }
   }

   function handleAccountListChange(e) {
      const accessToggle = e.target.closest('[data-action="toggle-read-only"]');
      if (accessToggle) {
         const id = parseInt(accessToggle.dataset.id, 10);
         const readOnly = accessToggle.checked; /* checked = read-only */
         requestToggleReadOnly(id, readOnly);
         return;
      }

      const enabledToggle = e.target.closest('[data-action="toggle-enabled"]');
      if (enabledToggle) {
         const id = parseInt(enabledToggle.dataset.id, 10);
         requestSetEnabled(id, enabledToggle.checked);
         return;
      }

      const toggle = e.target.closest('[data-cal-id]');
      if (!toggle) return;

      const calId = parseInt(toggle.dataset.calId, 10);
      const isActive = toggle.checked;
      const label = isActive ? 'Disable' : 'Enable';
      const name =
         toggle
            .closest('.calendar-sub-item')
            ?.querySelector('.calendar-sub-name')
            ?.textContent?.trim() || '';
      toggle.title = label + ' this calendar';
      toggle.setAttribute('aria-label', label + ' ' + name);
      toggleCalendar(calId, isActive);
   }

   /* =============================================================================
    * Actions
    * ============================================================================= */

   function testAccount(id) {
      setButtonLoading(id, 'Test');
      requestTestAccount(id);
   }

   function syncAccount(id) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         showToast('Not connected; try again when the connection is back', 'error');
         return;
      }
      setButtonLoading(id, 'Sync');
      /* Before the request: its calendar_sync_done can beat the reply. */
      pendingSyncs.add(String(id));
      requestSyncAccount(id);
   }

   /* The connection dropped: a sync's calendar_sync_done may never arrive,
    * so nothing is waited on (the next list shows how each account stands). */
   function clearPendingSyncs() {
      pendingSyncs.clear();
      clearLoadingButtons();
   }

   function expandCalendars(accountId) {
      const subList = document.getElementById('cal-list-' + accountId);
      if (subList && subList.style.display !== 'none') {
         subList.style.display = 'none';
         currentExpandedAccount = null;
         return;
      }
      currentExpandedAccount = accountId;
      requestListCalendars(accountId);
   }

   async function removeAccount(id, name, authType, oauthKey) {
      /* Check if the other service shares this OAuth connection */
      var emailShares = false;
      if (
         authType === 'oauth' &&
         oauthKey &&
         typeof DawnEmailAccounts !== 'undefined' &&
         DawnEmailAccounts.getAccounts
      ) {
         var emailAccounts = DawnEmailAccounts.getAccounts();
         emailShares = emailAccounts.some(function (a) {
            return a.auth_type === 'oauth' && a.oauth_account_key === oauthKey;
         });
      }

      var msg = 'Remove calendar account "' + name + '"? All cached events will be deleted.';
      if (authType === 'oauth' && emailShares) {
         msg +=
            '\n\nThis Google account is also connected for Email. ' +
            'Removing will only disconnect Calendar — Email will continue working.';
      } else if (authType === 'oauth') {
         msg += ' OAuth tokens will be revoked.';
      }

      if (
         await DawnDialog.confirm(msg, {
            title: 'Remove Calendar Account',
            okText: 'Remove',
            danger: true,
         })
      ) {
         /* Only revoke OAuth if no other service uses it */
         if (authType === 'oauth' && oauthKey && !emailShares && typeof DawnOAuth !== 'undefined') {
            DawnOAuth.disconnect('google', oauthKey);
         }
         requestRemoveAccount(id);
      }
   }

   function editAccount(id) {
      /* Find account data from the current list */
      const acct = accounts.find((a) => a.id === id);
      if (!acct) return;
      showEditAccountModal(acct);
   }

   function toggleCalendar(id, isActive) {
      requestToggleCalendar(id, isActive);
   }

   /* =============================================================================
    * Edit Account Modal
    * ============================================================================= */

   let editModalTriggerElement = null;
   let editModalTrapCleanup = null;
   let editEscToken = null; // DawnEscStack registration while the edit modal is open

   function showEditAccountModal(acct) {
      editModalTriggerElement = document.activeElement;

      let modal = document.getElementById('calendar-edit-modal');
      if (!modal) {
         modal = document.createElement('div');
         modal.id = 'calendar-edit-modal';
         modal.className = 'modal hidden';
         modal.setAttribute('role', 'dialog');
         modal.setAttribute('aria-modal', 'true');
         modal.setAttribute('aria-labelledby', 'cal-edit-modal-title');
         modal.innerHTML = `
            <div class="modal-content">
               <h3 id="cal-edit-modal-title">Edit Calendar Account</h3>
               <form class="dawn-form" id="calendar-edit-form" novalidate>
                  <input type="hidden" id="cal-edit-id" />
                  <div class="form-group">
                     <label for="cal-edit-name">Account Name</label>
                     <input type="text" id="cal-edit-name" required />
                  </div>
                  <div class="form-group">
                     <label for="cal-edit-url">CalDAV URL</label>
                     <input type="url" id="cal-edit-url" required />
                  </div>
                  <div class="form-group">
                     <label for="cal-edit-username">Username</label>
                     <input type="text" id="cal-edit-username" required />
                  </div>
                  <div class="form-group">
                     <label for="cal-edit-password">New Password (leave blank to keep current)</label>
                     <div class="password-input-wrapper">
                        <input type="password" id="cal-edit-password" placeholder="Leave blank to keep current" />
                        <button type="button" class="password-toggle" data-target="cal-edit-password" title="Show/hide">
                           <svg class="eye-icon" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                              <path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/>
                              <circle cx="12" cy="12" r="3"/>
                           </svg>
                        </button>
                     </div>
                  </div>
                  <div class="form-actions">
                     <button type="button" class="btn btn-secondary" id="cal-edit-modal-cancel">Cancel</button>
                     <button type="submit" class="btn btn-primary" id="cal-edit-modal-submit">Save</button>
                  </div>
               </form>
            </div>`;
         document.body.appendChild(modal);

         /* Wire up events */
         document
            .getElementById('cal-edit-modal-cancel')
            .addEventListener('click', hideEditAccountModal);
         modal.addEventListener('click', (e) => {
            if (e.target === modal) hideEditAccountModal();
         });
         /* Escape close is handled via DawnEscStack (register-on-show in
          * showEditAccountModal / unregister in hideEditAccountModal).
          * Tab/Shift+Tab cycling is owned by DawnSettingsModals.trapFocus. */
         document
            .getElementById('calendar-edit-form')
            .addEventListener('submit', handleEditFormSubmit);

         /* Password toggle */
         modal.querySelector('.password-toggle').addEventListener('click', function () {
            const targetId = this.dataset.target;
            if (targetId) {
               const input = document.getElementById(targetId);
               if (input) input.type = input.type === 'password' ? 'text' : 'password';
            }
         });
      }

      /* Populate fields */
      document.getElementById('cal-edit-id').value = acct.id;
      document.getElementById('cal-edit-name').value = acct.name || '';
      document.getElementById('cal-edit-url').value = acct.caldav_url || '';
      document.getElementById('cal-edit-username').value = acct.username || '';
      document.getElementById('cal-edit-password').value = '';

      modal.classList.remove('hidden');
      if (editEscToken === null) {
         editEscToken = DawnEscStack.register(() => {
            hideEditAccountModal();
            return true;
         });
      }
      document.getElementById('cal-edit-name').focus();
      /* Trap Tab within the modal.  skipInitialFocus because we
       * focused cal-edit-name above. */
      const M = window.DawnSettingsModals;
      if (M && typeof M.trapFocus === 'function') {
         editModalTrapCleanup = M.trapFocus(modal, { skipInitialFocus: true });
      }
   }

   function hideEditAccountModal() {
      const modal = document.getElementById('calendar-edit-modal');
      if (modal) modal.classList.add('hidden');
      if (editEscToken !== null) {
         DawnEscStack.unregister(editEscToken);
         editEscToken = null;
      }
      if (editModalTrapCleanup) {
         editModalTrapCleanup();
         editModalTrapCleanup = null;
      }
      if (editModalTriggerElement) {
         editModalTriggerElement.focus();
         editModalTriggerElement = null;
      }
   }

   function handleEditFormSubmit(e) {
      e.preventDefault();
      const id = parseInt(document.getElementById('cal-edit-id').value, 10);
      const name = document.getElementById('cal-edit-name').value.trim();
      const url = document.getElementById('cal-edit-url').value.trim();
      const username = document.getElementById('cal-edit-username').value.trim();
      const password = document.getElementById('cal-edit-password').value;

      if (!name || !url || !username) {
         showToast('Name, URL, and username are required', 'error');
         return;
      }

      const submitBtn = document.getElementById('cal-edit-modal-submit');
      if (submitBtn) {
         submitBtn.disabled = true;
         submitBtn.textContent = 'Saving...';
      }
      requestEditAccount(id, name, url, username, password || '');
   }

   /* =============================================================================
    * Button Loading State
    * ============================================================================= */

   function setButtonLoading(accountId, label) {
      const card = document.querySelector('[data-account-id="' + accountId + '"]');
      if (!card) return;
      const buttons = card.querySelectorAll('.calendar-account-actions button');
      buttons.forEach((btn) => {
         if (btn.textContent.trim() === label) {
            btn.classList.add('loading');
            btn.textContent = label + '...';
         }
      });
   }

   function clearLoadingButton(accountId) {
      const card = document.querySelector('[data-account-id="' + accountId + '"]');
      if (!card) return;
      card.querySelectorAll('.calendar-account-actions button.loading').forEach((btn) => {
         btn.classList.remove('loading');
         btn.textContent = btn.textContent.replace('...', '');
      });
   }

   function clearLoadingButtons() {
      document.querySelectorAll('.calendar-account-actions button.loading').forEach((btn) => {
         btn.classList.remove('loading');
         btn.textContent = btn.textContent.replace('...', '');
      });
   }

   /* =============================================================================
    * Add Account Modal
    * ============================================================================= */

   let modalTriggerElement = null;
   let addModalTrapCleanup = null;
   let addEscToken = null; // DawnEscStack registration while the add modal is open

   function showAddAccountModal() {
      modalTriggerElement = document.activeElement;

      let modal = document.getElementById('calendar-add-modal');
      if (!modal) {
         modal = document.createElement('div');
         modal.id = 'calendar-add-modal';
         modal.className = 'modal';
         modal.setAttribute('role', 'dialog');
         modal.setAttribute('aria-modal', 'true');
         modal.setAttribute('aria-labelledby', 'cal-modal-title');
         modal.innerHTML = `
            <div class="modal-content">
               <h3 id="cal-modal-title">Add Calendar Account</h3>
               <form class="dawn-form" id="calendar-add-form" novalidate>
                  <div class="auth-type-selector" role="tablist" aria-label="Authentication type">
                     <button type="button" class="auth-type-option active" data-auth-type="password" role="tab" aria-selected="true">App Password</button>
                     <button type="button" class="auth-type-option" data-auth-type="oauth" role="tab" aria-selected="false">Google OAuth</button>
                  </div>
                  <div id="cal-add-password-fields">
                     <div class="form-group">
                        <label for="cal-add-name">Account Name</label>
                        <input type="text" id="cal-add-name" placeholder="e.g., Work, Personal" required />
                     </div>
                     <div class="form-group">
                        <label for="cal-add-url">CalDAV URL</label>
                        <input type="url" id="cal-add-url" placeholder="https://caldav.example.com/dav/" required />
                        <span class="form-hint">The base CalDAV URL for your provider</span>
                     </div>
                     <div class="form-group">
                        <label for="cal-add-username">Username</label>
                        <input type="text" id="cal-add-username" placeholder="user@example.com" required />
                     </div>
                     <div class="form-group">
                        <label for="cal-add-password">Password / App Password</label>
                        <div class="password-input-wrapper">
                           <input type="password" id="cal-add-password" placeholder="App password or account password" required />
                           <button type="button" class="password-toggle" data-target="cal-add-password" title="Show/hide">
                              <svg class="eye-icon" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                                 <path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/>
                                 <circle cx="12" cy="12" r="3"/>
                              </svg>
                           </button>
                        </div>
                        <span class="form-hint">Stored encrypted in the database</span>
                     </div>
                     <div class="form-group">
                        <label class="calendar-checkbox-label">
                           <input type="checkbox" id="cal-add-view-only" />
                           Read only (prevent AI from modifying events)
                        </label>
                        <span class="form-hint">You can change this later</span>
                     </div>
                     <div class="form-actions">
                        <button type="button" class="btn btn-secondary" id="cal-modal-cancel">Cancel</button>
                        <button type="submit" class="btn btn-primary" id="cal-modal-submit">Add Account</button>
                     </div>
                  </div>
                  <div id="cal-add-oauth-fields" class="oauth-field-hidden">
                     <div class="form-group">
                        <label for="cal-add-oauth-name">Account Name</label>
                        <input type="text" id="cal-add-oauth-name" placeholder="e.g., Google Calendar" />
                     </div>
                     <div id="oauth-flow-container" class="form-group">
                        <button type="button" class="oauth-connect-btn" id="cal-oauth-connect-btn">Connect with Google</button>
                        <div id="oauth-flow-status" class="oauth-flow-status" style="display:none;" aria-live="polite"></div>
                     </div>
                     <label class="oauth-service-option">
                        <input type="checkbox" id="cal-oauth-also-email" checked />
                        Also enable for Email
                     </label>
                     <div class="form-group">
                        <label class="calendar-checkbox-label">
                           <input type="checkbox" id="cal-add-oauth-view-only" />
                           Read only (prevent AI from modifying events)
                        </label>
                        <span class="form-hint">You can change this later</span>
                     </div>
                     <div class="form-actions">
                        <button type="button" class="btn btn-secondary" id="cal-oauth-cancel">Cancel</button>
                        <button type="button" class="btn btn-primary" id="cal-oauth-save" disabled>Save Account</button>
                     </div>
                  </div>
               </form>
            </div>
         `;
         document.body.appendChild(modal);

         /* Form submit (app password mode) */
         document.getElementById('calendar-add-form').addEventListener('submit', function (e) {
            e.preventDefault();
            handleFormSubmit();
         });

         /* Cancel buttons */
         document.getElementById('cal-modal-cancel').addEventListener('click', hideAddAccountModal);
         document.getElementById('cal-oauth-cancel').addEventListener('click', hideAddAccountModal);

         /* Password toggle */
         modal.querySelector('.password-toggle').addEventListener('click', function () {
            const targetId = this.dataset.target;
            if (targetId) {
               const input = document.getElementById(targetId);
               if (input) input.type = input.type === 'password' ? 'text' : 'password';
            }
         });

         /* Auth type selector — shared DawnTablist helper handles
          * click + arrow-key navigation + roving tabindex.  Local
          * `activeAuthType` is the source of truth that getActive()
          * reads (no global module-level state for this modal).
          * attr='authType' because the HTML uses data-auth-type. */
         var activeAuthType = 'password';
         var authTabs = modal.querySelectorAll('.auth-type-option');
         function applyAuthTypeFields() {
            var pwFields = document.getElementById('cal-add-password-fields');
            var oauthFields = document.getElementById('cal-add-oauth-fields');
            if (activeAuthType === 'oauth') {
               pwFields.classList.add('oauth-field-hidden');
               oauthFields.classList.remove('oauth-field-hidden');
            } else {
               pwFields.classList.remove('oauth-field-hidden');
               oauthFields.classList.add('oauth-field-hidden');
            }
         }
         if (window.DawnTablist && authTabs.length > 0) {
            var authTablist = window.DawnTablist.bind({
               tabs: authTabs,
               attr: 'authType',
               getActive: function () {
                  return activeAuthType;
               },
               onActivate: function (name) {
                  activeAuthType = name;
                  authTablist.sync();
                  applyAuthTypeFields();
               },
            });
            authTablist.sync(); /* initial markup → matches activeAuthType */
         }

         /* Google OAuth connect button */
         document
            .getElementById('cal-oauth-connect-btn')
            .addEventListener('click', handleOAuthConnect);

         /* OAuth save button */
         document.getElementById('cal-oauth-save').addEventListener('click', handleOAuthSave);

         /* Escape closes.  Tab cycling owned by Modals.trapFocus
          * wired at show-time (showAddAccountModal below).  The
          * `.oauth-field-hidden` class sets display:none, which the
          * helper's getClientRects-based rendered-check filters out
          * for free — no separate carve-out needed. */
         /* Escape close is handled via DawnEscStack (register-on-show in
          * showAddAccountModal / unregister in hideAddAccountModal). */

         /* Click outside to close */
         modal.addEventListener('click', function (e) {
            if (e.target === modal) {
               hideAddAccountModal();
            }
         });
      }

      /* Reset form */
      const form = document.getElementById('calendar-add-form');
      if (form) form.reset();
      clearFormErrors();
      resetSubmitButton();
      modal.classList.remove('hidden');
      if (addEscToken === null) {
         addEscToken = DawnEscStack.register(() => {
            hideAddAccountModal();
            return true;
         });
      }

      /* Focus first input */
      setTimeout(function () {
         const first = document.getElementById('cal-add-name');
         if (first) first.focus();
      }, 50);
      /* Trap Tab cycling.  skipInitialFocus because the cal-add-name
       * focus above (deferred via setTimeout) is the intended target. */
      const M = window.DawnSettingsModals;
      if (M && typeof M.trapFocus === 'function') {
         addModalTrapCleanup = M.trapFocus(modal, { skipInitialFocus: true });
      }
   }

   function hideAddAccountModal() {
      const modal = document.getElementById('calendar-add-modal');
      if (modal) modal.classList.add('hidden');
      resetSubmitButton();
      resetOAuthState();
      if (addEscToken !== null) {
         DawnEscStack.unregister(addEscToken);
         addEscToken = null;
      }
      if (addModalTrapCleanup) {
         addModalTrapCleanup();
         addModalTrapCleanup = null;
      }
      /* Reset auth type selector to password */
      if (modal) {
         var pwFields = document.getElementById('cal-add-password-fields');
         var oauthFields = document.getElementById('cal-add-oauth-fields');
         if (pwFields) pwFields.classList.remove('oauth-field-hidden');
         if (oauthFields) oauthFields.classList.add('oauth-field-hidden');
         modal.querySelectorAll('.auth-type-option').forEach(function (btn) {
            btn.classList.toggle('active', btn.dataset.authType === 'password');
         });
      }
      if (modalTriggerElement) {
         modalTriggerElement.focus();
         modalTriggerElement = null;
      }
   }

   function handleFormSubmit() {
      clearFormErrors();
      const name = document.getElementById('cal-add-name').value.trim();
      const url = document.getElementById('cal-add-url').value.trim();
      const username = document.getElementById('cal-add-username').value.trim();
      const password = document.getElementById('cal-add-password').value;

      let valid = true;
      if (!name) {
         showFieldError('cal-add-name', 'Account name is required');
         valid = false;
      }
      if (!url) {
         showFieldError('cal-add-url', 'CalDAV URL is required');
         valid = false;
      }
      if (!username) {
         showFieldError('cal-add-username', 'Username is required');
         valid = false;
      }
      if (!password) {
         showFieldError('cal-add-password', 'Password is required');
         valid = false;
      }

      if (valid) {
         const submitBtn = document.getElementById('cal-modal-submit');
         if (submitBtn) {
            submitBtn.disabled = true;
            submitBtn.textContent = 'Adding...';
         }
         const viewOnly = document.getElementById('cal-add-view-only');
         const readOnly = viewOnly ? viewOnly.checked : false;
         requestAddAccount(name, url, username, password, readOnly);
      }
   }

   function showFieldError(inputId, message) {
      const input = document.getElementById(inputId);
      if (input) {
         input.classList.add('input-error');
         input.setAttribute('aria-invalid', 'true');
      }
   }

   function clearFormErrors() {
      const form = document.getElementById('calendar-add-form');
      if (!form) return;
      form.querySelectorAll('.input-error').forEach((el) => {
         el.classList.remove('input-error');
         el.removeAttribute('aria-invalid');
      });
   }

   function resetSubmitButton() {
      const submitBtn = document.getElementById('cal-modal-submit');
      if (submitBtn) {
         submitBtn.disabled = false;
         submitBtn.textContent = 'Add Account';
      }
   }

   /* =============================================================================
    * OAuth Flow
    * ============================================================================= */

   var GOOGLE_CALENDAR_SCOPE = 'https://www.googleapis.com/auth/calendar';
   var GOOGLE_EMAIL_SCOPE =
      'https://mail.google.com/ https://www.googleapis.com/auth/userinfo.email';

   let pendingOAuthAccountKey = null;

   function handleOAuthConnect() {
      var connectBtn = document.getElementById('cal-oauth-connect-btn');
      var statusEl = document.getElementById('oauth-flow-status');

      if (connectBtn) connectBtn.disabled = true;
      if (statusEl) {
         statusEl.style.display = 'block';
         statusEl.className = 'oauth-flow-status';
         statusEl.innerHTML =
            '<span class="oauth-flow-spinner"></span>Checking existing connections...';
      }

      if (typeof DawnOAuth === 'undefined') {
         setOAuthError('OAuth module not loaded');
         return;
      }

      var calendarScopes = GOOGLE_CALENDAR_SCOPE;
      var emailCheckbox = document.getElementById('cal-oauth-also-email');
      var requestScopes =
         emailCheckbox && emailCheckbox.checked
            ? calendarScopes + ' ' + GOOGLE_EMAIL_SCOPE
            : calendarScopes;

      // Always launch OAuth popup — user may want a different Google account
      // than one already connected. Google's consent screen has an account picker.
      if (statusEl) {
         statusEl.innerHTML =
            '<span class="oauth-flow-spinner"></span>Waiting for authorization...';
      }

      DawnOAuth.startFlow('google', requestScopes)
         .then(function () {
            if (statusEl) {
               statusEl.innerHTML =
                  '<span class="oauth-flow-spinner"></span>Exchanging authorization code...';
            }
         })
         .catch(function (err) {
            if (err.message === 'popup_blocked') {
               setOAuthError('Popup was blocked. Please allow popups for this site and try again.');
            } else {
               setOAuthError(err.message || 'Authorization failed');
            }
         });
   }

   function handleOAuthExchangeResponse(payload) {
      var statusEl = document.getElementById('oauth-flow-status');
      var saveBtn = document.getElementById('cal-oauth-save');

      if (!payload.success) {
         setOAuthError(payload.error || 'Token exchange failed');
         return;
      }

      pendingOAuthAccountKey = payload.account_key;

      if (statusEl) {
         statusEl.className = 'oauth-flow-status success';
         statusEl.textContent = 'Connected as ' + pendingOAuthAccountKey;
      }
      if (saveBtn) saveBtn.disabled = false;
      var connectBtn = document.getElementById('cal-oauth-connect');
      if (connectBtn) {
         connectBtn.disabled = false;
         connectBtn.textContent = 'Re-authorize';
      }
   }

   function handleOAuthDisconnectResponse(payload) {
      if (!payload.success) {
         showToast('Failed to disconnect OAuth: ' + (payload.error || 'Unknown'), 'error');
         return;
      }
      showToast('OAuth tokens revoked', 'success');
   }

   function handleOAuthSave() {
      if (!pendingOAuthAccountKey) {
         showToast('Connect with Google first', 'error');
         return;
      }

      var name =
         (document.getElementById('cal-add-oauth-name')?.value || '').trim() || 'Google Calendar';
      var readOnly = document.getElementById('cal-add-oauth-view-only')?.checked || false;

      var saveBtn = document.getElementById('cal-oauth-save');
      if (saveBtn) {
         saveBtn.disabled = true;
         saveBtn.textContent = 'Saving...';
      }

      /* Google CalDAV requires the email in the URL path for discovery */
      var googleCalDavUrl =
         'https://apidata.googleusercontent.com/caldav/v2/' +
         encodeURIComponent(pendingOAuthAccountKey) +
         '/';

      requestAddAccount(name, googleCalDavUrl, '', '', readOnly, 'oauth', pendingOAuthAccountKey);

      /* Cross-service: also create email account if checkbox checked */
      var emailCheckbox = document.getElementById('cal-oauth-also-email');
      if (
         emailCheckbox &&
         emailCheckbox.checked &&
         typeof DawnWS !== 'undefined' &&
         DawnWS.isConnected()
      ) {
         DawnWS.send({
            type: 'email_add_account',
            payload: {
               name: 'Gmail',
               imap_server: 'imap.gmail.com',
               imap_port: 993,
               imap_ssl: true,
               smtp_server: 'smtp.gmail.com',
               smtp_port: 587,
               smtp_ssl: true,
               username: pendingOAuthAccountKey,
               display_name: '',
               read_only: readOnly,
               auth_type: 'oauth',
               oauth_account_key: pendingOAuthAccountKey,
            },
         });
         showToast('Google account connected for Calendar and Email', 'success');
      }
   }

   function setOAuthError(msg) {
      var connectBtn = document.getElementById('cal-oauth-connect-btn');
      var statusEl = document.getElementById('oauth-flow-status');

      if (connectBtn) {
         connectBtn.disabled = false;
         connectBtn.textContent = 'Try Again';
      }
      if (statusEl) {
         statusEl.style.display = 'block';
         statusEl.className = 'oauth-flow-status error';
         statusEl.textContent = msg;
      }
   }

   function resetOAuthState() {
      pendingOAuthAccountKey = null;
      var connectBtn = document.getElementById('cal-oauth-connect-btn');
      var statusEl = document.getElementById('oauth-flow-status');
      var saveBtn = document.getElementById('cal-oauth-save');

      if (connectBtn) {
         connectBtn.disabled = false;
         connectBtn.textContent = 'Connect with Google';
      }
      if (statusEl) {
         statusEl.style.display = 'none';
         statusEl.textContent = '';
      }
      if (saveBtn) {
         saveBtn.disabled = true;
         saveBtn.textContent = 'Save Account';
      }
   }

   /* =============================================================================
    * Helpers
    * ============================================================================= */

   function getSharedServicesText(oauthKey) {
      var services = ['Calendar'];
      if (typeof DawnEmailAccounts !== 'undefined' && DawnEmailAccounts.getAccounts) {
         var emailAccounts = DawnEmailAccounts.getAccounts();
         if (
            emailAccounts.some(function (a) {
               return a.auth_type === 'oauth' && a.oauth_account_key === oauthKey;
            })
         ) {
            services.push('Email');
         }
      }
      return services.length > 1 ? ' \u00b7 ' + services.join(', ') : '';
   }

   function showToast(msg, type) {
      if (typeof DawnToast !== 'undefined') {
         DawnToast.show(msg, type);
      }
   }

   /* =============================================================================
    * Initialization
    * ============================================================================= */

   function init() {
      const section = document.getElementById('calendar-accounts-section');
      if (!section) return;

      const header = section.querySelector('.section-header');
      if (header) {
         header.addEventListener('click', () => {
            setTimeout(() => {
               const authState = callbacks.getAuthState ? callbacks.getAuthState() : {};
               if (!section.classList.contains('collapsed') && authState.authenticated) {
                  requestListAccounts();
               }
            }, 50);
         });
      }

      const addBtn = document.getElementById('calendar-add-account-btn');
      if (addBtn) {
         addBtn.addEventListener('click', showAddAccountModal);
      }

      const refreshBtn = document.getElementById('calendar-refresh-accounts-btn');
      if (refreshBtn) {
         refreshBtn.addEventListener('click', requestListAccounts);
      }

      /* Event delegation on account list */
      const list = document.getElementById('calendar-accounts-list');
      if (list) {
         list.addEventListener('click', handleAccountListClick);
         list.addEventListener('change', handleAccountListChange);
      }
   }

   function setCallbacks(cbs) {
      if (cbs.getAuthState) callbacks.getAuthState = cbs.getAuthState;
   }

   /* =============================================================================
    * Export
    * ============================================================================= */

   function getAccounts() {
      return accounts;
   }

   window.DawnCalendarAccounts = {
      init: init,
      setCallbacks: setCallbacks,
      getAccounts: getAccounts,
      handleListAccountsResponse: handleListAccountsResponse,
      handleAddAccountResponse: handleAddAccountResponse,
      handleEditAccountResponse: handleEditAccountResponse,
      handleRemoveAccountResponse: handleRemoveAccountResponse,
      handleTestAccountResponse: handleTestAccountResponse,
      handleSyncAccountResponse: handleSyncAccountResponse,
      handleSyncDone: handleSyncDone,
      clearPendingSyncs: clearPendingSyncs,
      handleListCalendarsResponse: handleListCalendarsResponse,
      handleToggleCalendarResponse: handleToggleCalendarResponse,
      handleToggleReadOnlyResponse: handleToggleReadOnlyResponse,
      handleSetEnabledResponse: handleSetEnabledResponse,
      handleOAuthExchangeResponse: handleOAuthExchangeResponse,
      handleOAuthDisconnectResponse: handleOAuthDisconnectResponse,
      showAddAccountModal: showAddAccountModal,
      hideAddAccountModal: hideAddAccountModal,
   };
})();
