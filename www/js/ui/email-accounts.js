(function () {
   'use strict';

   /* =========================================================================
    * State
    * ========================================================================= */

   let accounts = [];
   let callbacks = {};
   let pendingOAuthAccountKey = '';

   /* Numeric bounds + defaults for the per-account advanced settings.  Supplied by
    * the server in every email_list_accounts_response (`limits`), so the form never
    * duplicates the C constants; the update handler enforces the same ranges. */
   let limits = {};

   var GOOGLE_EMAIL_SCOPE =
      'https://mail.google.com/ https://www.googleapis.com/auth/userinfo.email';
   var GOOGLE_CALENDAR_SCOPE = 'https://www.googleapis.com/auth/calendar';

   const EMAIL_PRESETS = {
      gmail: {
         imap: 'imap.gmail.com',
         imap_port: 993,
         imap_ssl: true,
         smtp: 'smtp.gmail.com',
         smtp_port: 587,
         smtp_ssl: true,
      },
      icloud: {
         imap: 'imap.mail.me.com',
         imap_port: 993,
         imap_ssl: true,
         smtp: 'smtp.mail.me.com',
         smtp_port: 587,
         smtp_ssl: true,
      },
      outlook: {
         imap: 'outlook.office365.com',
         imap_port: 993,
         imap_ssl: true,
         smtp: 'smtp.office365.com',
         smtp_port: 587,
         smtp_ssl: true,
      },
      fastmail: {
         imap: 'imap.fastmail.com',
         imap_port: 993,
         imap_ssl: true,
         smtp: 'smtp.fastmail.com',
         smtp_port: 465,
         smtp_ssl: true,
      },
   };

   /* =========================================================================
    * API Requests
    * ========================================================================= */

   function isConnected() {
      return typeof DawnWS !== 'undefined' && DawnWS.isConnected && DawnWS.isConnected();
   }

   function showToast(msg, type) {
      if (typeof DawnToast !== 'undefined') {
         DawnToast.show(msg, type || 'info');
      }
   }

   function requestListAccounts() {
      if (!isConnected()) return;
      DawnWS.send({ type: 'email_list_accounts' });
   }

   function requestAddAccount(data) {
      if (!isConnected()) return;
      DawnWS.send({ type: 'email_add_account', payload: data });
   }

   function requestUpdateAccount(data) {
      if (!isConnected()) return;
      DawnWS.send({ type: 'email_update_account', payload: data });
   }

   function requestRemoveAccount(id) {
      if (!isConnected()) return;
      DawnWS.send({ type: 'email_remove_account', payload: { id: id } });
   }

   function requestTestConnection(id) {
      if (!isConnected()) return;
      DawnWS.send({ type: 'email_test_connection', payload: { id: id } });
   }

   function requestSetReadOnly(id, readOnly) {
      if (!isConnected()) return;
      DawnWS.send({ type: 'email_set_read_only', payload: { id: id, read_only: readOnly } });
   }

   function requestSetEnabled(id, enabled) {
      if (!isConnected()) return;
      DawnWS.send({ type: 'email_set_enabled', payload: { id: id, enabled: enabled } });
   }

   /* One whole-number advanced-settings input, labelled and described for
    * assistive tech.  `label`, `name` and `hint` are inserted unescaped, so they
    * must be trusted string literals — never account data.  Every value is
    * coerced with Number() before it reaches the HTML string, and a missing bound
    * (limits not yet received) simply omits that attribute rather than inventing
    * one.  The minimum of 1 passed for the count fields is intentional. */
   function numberField(label, name, value, min, max, hint) {
      const id = 'email-edit-' + name.replace(/_/g, '-');
      const hintId = id + '-hint';
      const n = Number(value);
      const lo = Number(min);
      const hi = Number(max);
      const hasLo = Number.isFinite(lo);
      const hasHi = Number.isFinite(hi);
      let range = '';
      if (hasLo && hasHi) range = ' Range: ' + lo + ' to ' + hi + '.';
      return (
         '  <div class="form-group"><label for="' +
         id +
         '">' +
         label +
         '</label><input type="number" step="1" inputmode="numeric" id="' +
         id +
         '" name="' +
         name +
         '" aria-describedby="' +
         hintId +
         '"' +
         (Number.isFinite(n) ? ' value="' + n + '"' : '') +
         (hasLo ? ' min="' + lo + '"' : '') +
         (hasHi ? ' max="' + hi + '"' : '') +
         '><span class="form-hint" id="' +
         hintId +
         '">' +
         hint +
         range +
         '</span></div>'
      );
   }

   /* =========================================================================
    * Response Handlers
    * ========================================================================= */

   function handleListAccountsResponse(payload) {
      if (!payload.success) {
         showToast('Failed to load email accounts', 'error');
         return;
      }
      accounts = payload.accounts || [];
      limits = payload.limits || {};
      renderAccounts();
   }

   function handleAddAccountResponse(payload) {
      hideModal();
      if (payload.success) {
         showToast('Email account added', 'success');
         requestListAccounts();
      } else if (payload.error === 'Account already exists') {
         /* Silently ignore — likely a cross-service add for an existing account */
         requestListAccounts();
      } else {
         showToast('Failed to add account: ' + (payload.error || 'Unknown error'), 'error');
      }
   }

   function handleUpdateAccountResponse(payload) {
      hideModal();
      if (payload.success) {
         showToast('Email account updated', 'success');
         requestListAccounts();
      } else {
         showToast('Failed to update account: ' + (payload.error || 'Unknown error'), 'error');
      }
   }

   function handleRemoveAccountResponse(payload) {
      if (payload.success) {
         showToast('Email account removed', 'success');
         requestListAccounts();
      } else {
         showToast('Failed to remove account: ' + (payload.error || 'Unknown error'), 'error');
      }
   }

   function handleTestConnectionResponse(payload) {
      const btn = document.querySelector('.email-test-btn.loading');
      if (btn) {
         btn.classList.remove('loading');
         btn.disabled = false;
         btn.textContent = 'Test';
      }

      if (payload.success) {
         showToast('Connection successful — IMAP: OK, SMTP: OK', 'success');
      } else {
         let msg =
            'IMAP: ' +
            (payload.imap_ok ? 'OK' : 'FAILED') +
            ', SMTP: ' +
            (payload.smtp_ok ? 'OK' : 'FAILED');
         if (payload.error) msg += ' — ' + payload.error;
         showToast(msg, 'error');
      }
   }

   function handleSetReadOnlyResponse(payload) {
      if (payload.success) {
         requestListAccounts();
      }
   }

   function handleSetEnabledResponse(payload) {
      if (payload.success) {
         requestListAccounts();
      }
   }

   function handleOAuthExchangeResponse(payload) {
      if (!payload.success) return;
      pendingOAuthAccountKey = payload.account_key || '';

      const statusEl = document.getElementById('email-oauth-status');
      const saveBtn = document.getElementById('email-oauth-save-btn');
      const connectBtn = document.getElementById('email-oauth-connect-btn');
      if (statusEl) statusEl.textContent = 'Connected as ' + pendingOAuthAccountKey;
      if (saveBtn) saveBtn.disabled = false;
      if (connectBtn) {
         connectBtn.disabled = false;
         connectBtn.textContent = 'Re-authorize';
      }
   }

   function handleOAuthDisconnectResponse(payload) {
      if (payload && payload.success) {
         requestListAccounts();
      }
   }

   /* =========================================================================
    * Rendering
    * ========================================================================= */

   function renderAccounts() {
      const list = document.getElementById('email-accounts-list');
      if (!list) return;

      if (accounts.length === 0) {
         list.innerHTML =
            '<div class="email-accounts-empty">No email accounts configured. Click "Add Account" to connect.</div>';
         return;
      }

      let html = '';
      for (const acct of accounts) {
         const isOAuth = acct.auth_type === 'oauth';
         const roClass = acct.read_only ? ' read-only' : '';
         const disabledClass = !acct.enabled ? ' disabled-account' : '';

         html +=
            '<div class="email-account-card' +
            roClass +
            disabledClass +
            '" data-id="' +
            acct.id +
            '">';
         html += '  <div class="email-account-header">';
         html += '    <div class="email-account-title">';
         html += '      <span class="email-account-name">' + escapeHtml(acct.name) + '</span>';
         if (isOAuth) html += '<span class="dawn-badge accent">OAuth</span>';
         if (acct.read_only) html += '<span class="dawn-badge warning">Read-only</span>';
         html += '    </div>';
         html += '    <div class="email-account-actions">';
         html +=
            '      <button class="btn btn-secondary btn-small email-test-btn" data-action="test" data-id="' +
            acct.id +
            '">Test</button>';
         if (!isOAuth) {
            html +=
               '      <button class="btn btn-secondary btn-small" data-action="edit" data-id="' +
               acct.id +
               '">Edit</button>';
         }
         html +=
            '      <button class="btn btn-danger btn-small" data-action="remove" data-id="' +
            acct.id +
            '">Remove</button>';
         html += '    </div>';
         html += '  </div>';
         html += '  <div class="email-account-details">';
         html +=
            '    <div class="detail-row"><span class="detail-label">User:</span> ' +
            escapeHtml(acct.username) +
            '</div>';
         if (isOAuth && acct.oauth_account_key) {
            html +=
               '    <div class="detail-row"><span class="detail-secondary">' +
               escapeHtml(acct.oauth_account_key) +
               getSharedServicesText(acct.oauth_account_key) +
               '</span></div>';
         }
         if (!isOAuth) {
            html +=
               '    <div class="detail-row"><span class="detail-label">IMAP:</span> ' +
               escapeHtml(acct.imap_server) +
               ':' +
               acct.imap_port +
               '</div>';
         }
         html += '  </div>';
         html += '  <div class="email-account-access">';
         html +=
            '    <input type="checkbox" class="dawn-toggle" data-action="toggle-enabled" data-id="' +
            acct.id +
            '"' +
            (acct.enabled ? ' checked' : '') +
            ' aria-label="Enable account">';
         html += '    <span class="dawn-toggle-label">Enabled</span>';
         html +=
            '    <input type="checkbox" class="dawn-toggle" data-action="toggle-ro" data-id="' +
            acct.id +
            '"' +
            (acct.read_only ? ' checked' : '') +
            ' aria-label="Read-only mode">';
         html += '    <span class="dawn-toggle-label">Read-only</span>';
         html += '  </div>';
         html += '</div>';
      }

      list.innerHTML = html;
   }

   function escapeHtml(str) {
      if (!str) return '';
      return str
         .replace(/&/g, '&amp;')
         .replace(/</g, '&lt;')
         .replace(/>/g, '&gt;')
         .replace(/"/g, '&quot;');
   }

   function getSharedServicesText(oauthKey) {
      var services = ['Email'];
      if (typeof DawnCalendarAccounts !== 'undefined' && DawnCalendarAccounts.getAccounts) {
         var calAccounts = DawnCalendarAccounts.getAccounts();
         if (
            calAccounts.some(function (a) {
               return a.auth_type === 'oauth' && a.oauth_account_key === oauthKey;
            })
         ) {
            services.push('Calendar');
         }
      }
      return services.length > 1 ? ' \u00b7 ' + services.join(', ') : '';
   }

   function getAccounts() {
      return accounts;
   }

   /* =========================================================================
    * Event Delegation
    * ========================================================================= */

   async function handleListClick(e) {
      const btn = e.target.closest('[data-action]');
      if (!btn) return;

      const action = btn.dataset.action;
      const id = parseInt(btn.dataset.id, 10);

      if (action === 'test') {
         btn.classList.add('loading');
         btn.disabled = true;
         btn.textContent = 'Testing...';
         requestTestConnection(id);
      } else if (action === 'edit') {
         const acct = accounts.find((a) => a.id === id);
         if (acct) showEditModal(acct);
      } else if (action === 'remove') {
         var acct = accounts.find(function (a) {
            return a.id === id;
         });
         var oauthKey = acct ? acct.oauth_account_key : '';
         var isOAuth = acct && acct.auth_type === 'oauth';

         /* Check if calendar shares this OAuth connection */
         var calShares = false;
         if (
            isOAuth &&
            oauthKey &&
            typeof DawnCalendarAccounts !== 'undefined' &&
            DawnCalendarAccounts.getAccounts
         ) {
            var calAccounts = DawnCalendarAccounts.getAccounts();
            calShares = calAccounts.some(function (a) {
               return a.auth_type === 'oauth' && a.oauth_account_key === oauthKey;
            });
         }

         var msg = 'Remove email account?';
         if (isOAuth && calShares) {
            msg +=
               '\n\nThis Google account is also connected for Calendar. ' +
               'Removing will only disconnect Email — Calendar will continue working.';
         } else if (isOAuth) {
            msg += ' OAuth tokens will be revoked.';
         }

         if (
            await DawnDialog.confirm(msg, {
               title: 'Remove Email Account',
               okText: 'Remove',
               danger: true,
            })
         ) {
            if (isOAuth && oauthKey && !calShares && typeof DawnOAuth !== 'undefined') {
               DawnOAuth.disconnect('google', oauthKey);
            }
            requestRemoveAccount(id);
         }
      }
   }

   function handleListChange(e) {
      const roInput = e.target.closest('[data-action="toggle-ro"]');
      if (roInput) {
         const id = parseInt(roInput.dataset.id, 10);
         requestSetReadOnly(id, roInput.checked);
         return;
      }
      const enInput = e.target.closest('[data-action="toggle-enabled"]');
      if (enInput) {
         const id = parseInt(enInput.dataset.id, 10);
         requestSetEnabled(id, enInput.checked);
         return;
      }
   }

   /* =========================================================================
    * Modal: Add Account
    * ========================================================================= */

   let modalEl = null;

   let previousFocusEl = null;
   let escToken = null; // DawnEscStack registration while the modal is shown

   // Reveal the modal and register its Escape-dismiss on the global stack (the
   // focus trap below stays element-scoped on modalEl).
   function revealModal() {
      modalEl.classList.remove('hidden');
      if (escToken === null) {
         escToken = DawnEscStack.register(function () {
            hideModal();
            return true;
         });
      }
   }

   function handleModalKeydown(e) {
      /* Basic focus trap — exclude hidden elements */
      if (e.key === 'Tab' && modalEl) {
         const focusable = Array.from(
            modalEl.querySelectorAll(
               'button, input, select, textarea, [tabindex]:not([tabindex="-1"])'
            )
         ).filter(function (el) {
            return el.offsetParent !== null;
         });
         if (focusable.length === 0) return;
         const first = focusable[0];
         const last = focusable[focusable.length - 1];
         if (e.shiftKey && document.activeElement === first) {
            e.preventDefault();
            last.focus();
         } else if (!e.shiftKey && document.activeElement === last) {
            e.preventDefault();
            first.focus();
         }
      }
   }

   function createModal() {
      if (modalEl) return modalEl;

      modalEl = document.createElement('div');
      modalEl.className = 'modal hidden';
      modalEl.setAttribute('role', 'dialog');
      modalEl.setAttribute('aria-modal', 'true');
      modalEl.setAttribute('aria-labelledby', 'email-modal-title');
      modalEl.innerHTML =
         '<div class="modal-content email-modal-content" id="email-modal-content"></div>';

      modalEl.addEventListener('click', function (e) {
         if (e.target === modalEl) hideModal();
      });

      modalEl.addEventListener('keydown', handleModalKeydown);

      document.body.appendChild(modalEl);
      return modalEl;
   }

   function showAddModal() {
      previousFocusEl = document.activeElement;
      const modal = createModal();
      const content = document.getElementById('email-modal-content');

      content.innerHTML = buildAddForm();
      revealModal();

      setupFormListeners();

      var firstInput = modal.querySelector('input, select, button');
      if (firstInput) firstInput.focus();
   }

   function showEditModal(acct) {
      previousFocusEl = document.activeElement;
      const modal = createModal();
      const content = document.getElementById('email-modal-content');

      content.innerHTML = buildEditForm(acct);
      revealModal();

      setupEditListeners(acct);

      var firstInput = modal.querySelector('input, select, button');
      if (firstInput) firstInput.focus();
   }

   function hideModal() {
      if (modalEl) modalEl.classList.add('hidden');
      if (escToken !== null) {
         DawnEscStack.unregister(escToken);
         escToken = null;
      }
      pendingOAuthAccountKey = '';
      if (previousFocusEl) {
         previousFocusEl.focus();
         previousFocusEl = null;
      }
   }

   function buildAddForm() {
      let html = '<h3 id="email-modal-title">Add Email Account</h3>';
      html += '<div class="dawn-form email-add-form">';

      // Auth type selector (WAI-ARIA tablist, bound via DawnTablist
      // in setupFormListeners — same shape as the CalDAV add-account
      // selector).
      html += '<div class="auth-type-selector" role="tablist" aria-label="Authentication type">';
      html +=
         '  <button type="button" class="btn btn-small auth-type-option active" data-auth="app_password" role="tab" aria-selected="true">App Password</button>';
      html +=
         '  <button type="button" class="btn btn-small auth-type-option" data-auth="oauth" role="tab" aria-selected="false">Google OAuth</button>';
      html += '</div>';

      // App Password form
      html += '<div id="email-app-form">';
      html +=
         '  <div class="form-group"><label for="email-name">Account Name</label><input type="text" id="email-name" name="name" placeholder="Gmail, Work, etc."></div>';

      // Provider presets
      html += '  <div class="form-group"><label for="email-provider-preset">Provider</label>';
      html += '    <select id="email-provider-preset">';
      html += '      <option value="">Other (manual)</option>';
      html += '      <option value="gmail">Gmail</option>';
      html += '      <option value="icloud">iCloud</option>';
      html += '      <option value="outlook">Outlook / Office 365</option>';
      html += '      <option value="fastmail">Fastmail</option>';
      html += '    </select>';
      html += '  </div>';

      // IMAP Settings
      html += '  <div class="setting-group">';
      html += '    <div class="setting-group-title">IMAP Settings</div>';
      html += '    <div class="server-row">';
      html +=
         '      <input type="text" id="email-imap-server" name="imap_server" placeholder="imap.example.com">';
      html +=
         '      <input type="number" id="email-imap-port" name="imap_port" value="993" min="1" max="65535">';
      html += '    </div>';
      html += '    <div class="ssl-toggle-row">';
      html +=
         '      <input type="checkbox" class="dawn-toggle" id="email-imap-ssl" name="imap_ssl" checked aria-label="IMAP SSL encryption">';
      html += '      <span>Use SSL</span>';
      html += '    </div>';
      html += '  </div>';

      // SMTP Settings
      html += '  <div class="setting-group">';
      html += '    <div class="setting-group-title">SMTP Settings</div>';
      html += '    <div class="server-row">';
      html +=
         '      <input type="text" id="email-smtp-server" name="smtp_server" placeholder="smtp.example.com">';
      html +=
         '      <input type="number" id="email-smtp-port" name="smtp_port" value="465" min="1" max="65535">';
      html += '    </div>';
      html += '    <div class="ssl-toggle-row">';
      html +=
         '      <input type="checkbox" class="dawn-toggle" id="email-smtp-ssl" name="smtp_ssl" checked aria-label="SMTP SSL encryption">';
      html += '      <span>Use SSL</span>';
      html += '    </div>';
      html += '  </div>';

      // Credentials
      html += '  <div class="setting-group">';
      html += '    <div class="setting-group-title">Credentials</div>';
      html +=
         '    <div class="form-group"><label for="email-username">Email / Username</label><input type="text" id="email-username" name="username" placeholder="user@example.com"></div>';
      html +=
         '    <div class="form-group"><label for="email-display-name">Display Name</label><input type="text" id="email-display-name" name="display_name" placeholder="Name shown in From field"></div>';
      html += '    <div class="form-group"><label for="email-password">Password</label>';
      html += '      <div class="password-input-wrapper">';
      html +=
         '        <input type="password" id="email-password" name="password" placeholder="App password">';
      html +=
         '        <button type="button" class="password-toggle btn btn-secondary btn-small">Show</button>';
      html += '      </div>';
      html += '    </div>';
      html += '  </div>';

      html +=
         '  <div class="form-group"><label class="calendar-checkbox-label"><input type="checkbox" name="read_only" /> Read only (prevent AI from sending email)</label><span class="form-hint">You can change this later</span></div>';
      html += '</div>';

      // OAuth form (hidden by default) — mirrors calendar OAuth section structure
      html += '<div id="email-oauth-form" style="display:none">';
      html +=
         '  <div class="form-group"><label for="email-oauth-name">Account Name</label><input type="text" id="email-oauth-name" name="oauth_name" placeholder="Gmail"></div>';
      html += '  <div class="form-group">';
      html +=
         '    <button type="button" class="oauth-connect-btn" id="email-oauth-connect-btn">Connect with Google</button>';
      html +=
         '    <div id="email-oauth-status" class="oauth-flow-status" style="display:none" aria-live="polite"></div>';
      html += '  </div>';
      html +=
         '  <label class="oauth-service-option"><input type="checkbox" id="email-oauth-also-calendar" checked /> Also enable for Calendar</label>';
      html +=
         '  <div class="form-group"><label class="calendar-checkbox-label"><input type="checkbox" name="oauth_read_only" /> Read only (prevent AI from sending email)</label><span class="form-hint">You can change this later</span></div>';
      html += '  <div class="form-actions">';
      html +=
         '    <button type="button" class="btn btn-secondary" id="email-oauth-cancel">Cancel</button>';
      html +=
         '    <button type="button" class="btn btn-primary" id="email-oauth-save-btn" disabled>Save Account</button>';
      html += '  </div>';
      html += '</div>';

      html += '<div class="form-error" id="email-form-error" role="alert"></div>';
      html += '<div class="form-actions" id="email-password-actions">';
      html += '  <button class="btn btn-secondary" id="email-cancel-btn">Cancel</button>';
      html += '  <button class="btn btn-primary" id="email-save-btn">Add Account</button>';
      html += '</div>';

      html += '</div>';
      return html;
   }

   function buildEditForm(acct) {
      let html = '<h3 id="email-modal-title">Edit Email Account</h3>';
      html += '<div class="dawn-form email-add-form">';
      html +=
         '<div class="form-group"><label for="email-edit-name">Account Name</label><input type="text" id="email-edit-name" name="name" value="' +
         escapeHtml(acct.name) +
         '"></div>';

      if (acct.auth_type !== 'oauth') {
         html += '<div class="setting-group">';
         html += '  <div class="setting-group-title">IMAP Settings</div>';
         html += '  <div class="server-row">';
         html +=
            '    <input type="text" name="imap_server" aria-label="IMAP server" value="' +
            escapeHtml(acct.imap_server) +
            '">';
         html +=
            '    <input type="number" name="imap_port" aria-label="IMAP port" value="' +
            Number(acct.imap_port) +
            '">';
         html += '  </div>';
         html += '  <div class="ssl-toggle-row">';
         html +=
            '    <input type="checkbox" class="dawn-toggle" name="imap_ssl"' +
            (acct.imap_ssl ? ' checked' : '') +
            ' aria-label="IMAP SSL encryption">';
         html += '    <span>Use SSL</span>';
         html += '  </div>';
         html += '</div>';

         html += '<div class="setting-group">';
         html += '  <div class="setting-group-title">SMTP Settings</div>';
         html += '  <div class="server-row">';
         html +=
            '    <input type="text" name="smtp_server" aria-label="SMTP server" value="' +
            escapeHtml(acct.smtp_server) +
            '">';
         html +=
            '    <input type="number" name="smtp_port" aria-label="SMTP port" value="' +
            Number(acct.smtp_port) +
            '">';
         html += '  </div>';
         html += '  <div class="ssl-toggle-row">';
         html +=
            '    <input type="checkbox" class="dawn-toggle" name="smtp_ssl"' +
            (acct.smtp_ssl ? ' checked' : '') +
            ' aria-label="SMTP SSL encryption">';
         html += '    <span>Use SSL</span>';
         html += '  </div>';
         html += '</div>';

         html += '<div class="setting-group">';
         html += '  <div class="setting-group-title">Credentials</div>';
         html +=
            '  <div class="form-group"><label for="email-edit-username">Username</label><input type="text" id="email-edit-username" name="username" value="' +
            escapeHtml(acct.username) +
            '"></div>';
         html +=
            '  <div class="form-group"><label for="email-edit-display-name">Display Name</label><input type="text" id="email-edit-display-name" name="display_name" value="' +
            escapeHtml(acct.display_name) +
            '"></div>';
         html +=
            '  <div class="form-group"><label for="email-edit-password">Password (leave blank to keep current)</label>';
         html += '    <div class="password-input-wrapper">';
         html +=
            '      <input type="password" id="email-edit-password" name="password" placeholder="Leave blank to keep current">';
         html +=
            '      <button type="button" class="password-toggle btn btn-secondary btn-small">Show</button>';
         html += '    </div>';
         html += '  </div>';
         html += '</div>';
      }

      // Advanced settings (collapsible)
      html += '<details class="setting-group advanced-settings">';
      html += '  <summary class="setting-group-title">Advanced Settings</summary>';
      html += numberField(
         'Max recent emails',
         'max_recent',
         acct.max_recent || limits.max_recent_default,
         1,
         limits.max_recent_max,
         'Emails shown when you ask for recent mail without a number.'
      );
      html += numberField(
         'Messages checked for daily digest',
         'digest_depth',
         acct.digest_depth || limits.digest_depth_default,
         1,
         limits.digest_depth_max,
         "How many of this inbox's newest messages the daily digest looks at. Lower it for a busy inbox you don't need fully covered; each 50 above the first adds a trip to the mail server."
      );
      html += numberField(
         'Longest email the AI reads (characters)',
         'max_body_chars',
         acct.max_body_chars || limits.body_chars_max,
         limits.body_chars_min,
         limits.body_chars_max,
         'Longer emails are cut off at this length.'
      );
      html += '</details>';

      html += '<div class="form-actions">';
      html += '  <button class="btn btn-secondary" id="email-cancel-btn">Cancel</button>';
      html += '  <button class="btn btn-primary" id="email-update-btn">Update</button>';
      html += '</div>';
      html += '</div>';
      return html;
   }

   function setupFormListeners() {
      // Auth type toggle — shared DawnTablist owns click + arrow-key
      // navigation + aria-selected + roving tabindex.  attr='auth'
      // because the HTML uses data-auth (legacy naming kept to match
      // the existing CSS / preset hook code).
      const authBtns = document.querySelectorAll('.auth-type-option');
      let activeAuth = 'app_password';
      function applyAuthFields() {
         const isOAuth = activeAuth === 'oauth';
         document.getElementById('email-app-form').style.display = isOAuth ? 'none' : 'block';
         document.getElementById('email-oauth-form').style.display = isOAuth ? 'flex' : 'none';
         document.getElementById('email-password-actions').style.display = isOAuth
            ? 'none'
            : 'flex';
      }
      if (window.DawnTablist && authBtns.length > 0) {
         const authTablist = window.DawnTablist.bind({
            tabs: authBtns,
            attr: 'auth',
            getActive: function () {
               return activeAuth;
            },
            onActivate: function (name) {
               activeAuth = name;
               authTablist.sync();
               applyAuthFields();
            },
         });
         authTablist.sync(); /* initial markup → matches activeAuth */
      }

      // Provider preset
      const presetSel = document.getElementById('email-provider-preset');
      if (presetSel) {
         presetSel.addEventListener('change', function () {
            const preset = EMAIL_PRESETS[presetSel.value];
            if (!preset) return;
            const form = document.querySelector('.email-add-form');
            form.querySelector('[name="imap_server"]').value = preset.imap;
            form.querySelector('[name="imap_port"]').value = preset.imap_port;
            form.querySelector('[name="imap_ssl"]').checked = preset.imap_ssl;
            form.querySelector('[name="smtp_server"]').value = preset.smtp;
            form.querySelector('[name="smtp_port"]').value = preset.smtp_port;
            form.querySelector('[name="smtp_ssl"]').checked = preset.smtp_ssl;
         });
      }

      // Password toggle
      const pwToggle = document.querySelector('.password-toggle');
      if (pwToggle) {
         pwToggle.addEventListener('click', function () {
            const input = pwToggle.previousElementSibling;
            if (input.type === 'password') {
               input.type = 'text';
               pwToggle.textContent = 'Hide';
            } else {
               input.type = 'password';
               pwToggle.textContent = 'Show';
            }
         });
      }

      // Cancel (both modes)
      document.getElementById('email-cancel-btn').addEventListener('click', hideModal);
      document.getElementById('email-oauth-cancel').addEventListener('click', hideModal);

      // Save (app password)
      document.getElementById('email-save-btn').addEventListener('click', function () {
         const form = document.querySelector('.email-add-form');
         const data = {
            name: form.querySelector('[name="name"]').value,
            imap_server: form.querySelector('[name="imap_server"]').value,
            imap_port: parseInt(form.querySelector('[name="imap_port"]').value, 10) || 993,
            imap_ssl: form.querySelector('[name="imap_ssl"]').checked,
            smtp_server: form.querySelector('[name="smtp_server"]').value,
            smtp_port: parseInt(form.querySelector('[name="smtp_port"]').value, 10) || 465,
            smtp_ssl: form.querySelector('[name="smtp_ssl"]').checked,
            username: form.querySelector('[name="username"]').value,
            display_name: form.querySelector('[name="display_name"]').value,
            password: form.querySelector('[name="password"]').value,
            read_only: form.querySelector('[name="read_only"]').checked,
            auth_type: 'app_password',
         };

         if (
            !data.name ||
            !data.imap_server ||
            !data.smtp_server ||
            !data.username ||
            !data.password
         ) {
            const errEl = document.getElementById('email-form-error');
            if (errEl) {
               errEl.textContent = 'Please fill in all required fields.';
               errEl.classList.add('visible');
            }
            return;
         }
         requestAddAccount(data);
      });

      // OAuth connect (with scope check for existing tokens)
      const oauthBtn = document.getElementById('email-oauth-connect-btn');
      if (oauthBtn) {
         oauthBtn.addEventListener('click', function () {
            if (typeof DawnOAuth === 'undefined') return;
            var statusEl = document.getElementById('email-oauth-status');
            var saveBtn = document.getElementById('email-oauth-save-btn');

            oauthBtn.disabled = true;
            oauthBtn.textContent = 'Connecting...';
            if (statusEl) statusEl.style.display = 'block';

            var calCheckbox = document.getElementById('email-oauth-also-calendar');
            var requestScopes =
               calCheckbox && calCheckbox.checked
                  ? GOOGLE_EMAIL_SCOPE + ' ' + GOOGLE_CALENDAR_SCOPE
                  : GOOGLE_EMAIL_SCOPE;

            if (statusEl) statusEl.textContent = 'Checking existing connections...';

            // Always launch OAuth popup for email — user may want a different
            // Google account than one already connected. The Google consent
            // screen provides its own account picker.
            if (statusEl) statusEl.textContent = 'Waiting for authorization...';
            DawnOAuth.startFlow('google', requestScopes)
               .then(function () {
                  if (statusEl) statusEl.textContent = 'Exchanging authorization code...';
               })
               .catch(function (err) {
                  oauthBtn.disabled = false;
                  oauthBtn.textContent = 'Connect with Google';
                  if (statusEl) statusEl.textContent = 'OAuth failed: ' + (err.message || err);
               });
         });
      }

      // OAuth save
      const oauthSaveBtn = document.getElementById('email-oauth-save-btn');
      if (oauthSaveBtn) {
         oauthSaveBtn.addEventListener('click', function () {
            const name = document.querySelector('[name="oauth_name"]').value || 'Gmail';
            const readOnly = document.querySelector('[name="oauth_read_only"]').checked;

            requestAddAccount({
               name: name,
               imap_server: 'imap.gmail.com',
               imap_port: 993,
               imap_ssl: true,
               smtp_server: 'smtp.gmail.com',
               smtp_port: 465,
               smtp_ssl: true,
               username: pendingOAuthAccountKey,
               display_name: '',
               read_only: readOnly,
               auth_type: 'oauth',
               oauth_account_key: pendingOAuthAccountKey,
            });

            /* Cross-service: also create calendar account if checkbox checked */
            var calCheckbox = document.getElementById('email-oauth-also-calendar');
            if (
               calCheckbox &&
               calCheckbox.checked &&
               typeof DawnWS !== 'undefined' &&
               DawnWS.isConnected()
            ) {
               var googleCalDavUrl =
                  'https://apidata.googleusercontent.com/caldav/v2/' +
                  encodeURIComponent(pendingOAuthAccountKey) +
                  '/';
               DawnWS.send({
                  type: 'calendar_add_account',
                  payload: {
                     name: 'Google Calendar',
                     caldav_url: googleCalDavUrl,
                     username: '',
                     password: '',
                     read_only: readOnly,
                     auth_type: 'oauth',
                     oauth_account_key: pendingOAuthAccountKey,
                  },
               });
               showToast('Google account connected for Email and Calendar', 'success');
            }
         });
      }
   }

   function setupEditListeners(acct) {
      document.getElementById('email-cancel-btn').addEventListener('click', hideModal);

      // Password toggle
      const pwToggle = document.querySelector('.password-toggle');
      if (pwToggle) {
         pwToggle.addEventListener('click', function () {
            const input = pwToggle.previousElementSibling;
            if (input.type === 'password') {
               input.type = 'text';
               pwToggle.textContent = 'Hide';
            } else {
               input.type = 'password';
               pwToggle.textContent = 'Show';
            }
         });
      }

      document.getElementById('email-update-btn').addEventListener('click', function () {
         const form = document.querySelector('.email-add-form');
         const data = { id: acct.id };

         const nameEl = form.querySelector('[name="name"]');
         if (nameEl) data.name = nameEl.value;

         if (acct.auth_type !== 'oauth') {
            const imapEl = form.querySelector('[name="imap_server"]');
            if (imapEl) data.imap_server = imapEl.value;
            const imapPortEl = form.querySelector('[name="imap_port"]');
            if (imapPortEl) data.imap_port = parseInt(imapPortEl.value, 10);
            const imapSslEl = form.querySelector('[name="imap_ssl"]');
            if (imapSslEl) data.imap_ssl = imapSslEl.checked;
            const smtpEl = form.querySelector('[name="smtp_server"]');
            if (smtpEl) data.smtp_server = smtpEl.value;
            const smtpPortEl = form.querySelector('[name="smtp_port"]');
            if (smtpPortEl) data.smtp_port = parseInt(smtpPortEl.value, 10);
            const smtpSslEl = form.querySelector('[name="smtp_ssl"]');
            if (smtpSslEl) data.smtp_ssl = smtpSslEl.checked;
            const userEl = form.querySelector('[name="username"]');
            if (userEl) data.username = userEl.value;
            const dispEl = form.querySelector('[name="display_name"]');
            if (dispEl) data.display_name = dispEl.value;
            const passEl = form.querySelector('[name="password"]');
            if (passEl && passEl.value) data.password = passEl.value;
         }

         // Advanced numeric fields.  The form isn't a <form>, so min/max/step are
         // never enforced on their own: check them here and stop on the first bad
         // field (opening the collapsed section so it's visible) instead of sending
         // a value the server will reject.  An empty field is omitted so the
         // server keeps the stored value.
         let invalid = null;
         ['max_recent', 'digest_depth', 'max_body_chars'].forEach(function (name) {
            const el = form.querySelector('[name="' + name + '"]');
            if (!el) return;
            // Unparseable typing (e.g. "abc", "1e") reads as '' — treat it as an
            // error, not as "left blank".
            if (el.value === '' && !el.validity.badInput) return;
            const ok = el.checkValidity() && Number.isInteger(Number(el.value));
            el.classList.toggle('input-error', !ok);
            if (!ok) {
               invalid = invalid || el;
               return;
            }
            data[name] = Number(el.value);
         });
         if (invalid) {
            const section = invalid.closest('details');
            if (section) section.open = true;
            invalid.focus();
            invalid.reportValidity();
            return;
         }

         requestUpdateAccount(data);
      });
   }

   /* =========================================================================
    * Initialization
    * ========================================================================= */

   function init() {
      const section = document.getElementById('email-accounts-section');
      if (!section) return;

      const header = section.querySelector('.section-header');
      if (header) {
         header.addEventListener('click', function () {
            setTimeout(function () {
               const authState = callbacks.getAuthState ? callbacks.getAuthState() : {};
               if (!section.classList.contains('collapsed') && authState.authenticated) {
                  requestListAccounts();
               }
            }, 50);
         });
      }

      const addBtn = document.getElementById('email-add-account-btn');
      if (addBtn) addBtn.addEventListener('click', showAddModal);

      const refreshBtn = document.getElementById('email-refresh-accounts-btn');
      if (refreshBtn) refreshBtn.addEventListener('click', requestListAccounts);

      const list = document.getElementById('email-accounts-list');
      if (list) {
         list.addEventListener('click', handleListClick);
         list.addEventListener('change', handleListChange);
      }
   }

   function setCallbacks(cbs) {
      callbacks = cbs || {};
   }

   /* =========================================================================
    * Export
    * ========================================================================= */

   window.DawnEmailAccounts = {
      init: init,
      setCallbacks: setCallbacks,
      getAccounts: getAccounts,
      handleListAccountsResponse: handleListAccountsResponse,
      handleAddAccountResponse: handleAddAccountResponse,
      handleUpdateAccountResponse: handleUpdateAccountResponse,
      handleRemoveAccountResponse: handleRemoveAccountResponse,
      handleTestConnectionResponse: handleTestConnectionResponse,
      handleSetReadOnlyResponse: handleSetReadOnlyResponse,
      handleSetEnabledResponse: handleSetEnabledResponse,
      handleOAuthExchangeResponse: handleOAuthExchangeResponse,
      handleOAuthDisconnectResponse: handleOAuthDisconnectResponse,
   };
})();
