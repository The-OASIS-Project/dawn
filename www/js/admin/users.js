/**
 * DAWN User Management Module
 * Admin user CRUD operations and modals
 */
(function () {
   'use strict';

   /* =============================================================================
    * State
    * ============================================================================= */

   let activeModalCleanup = null;
   let activeModalEscToken = null; // DawnEscStack registration for the open user modal

   // Register the open modal's Escape-dismiss on the global stack (single slot —
   // only one user modal is open at a time, mirroring activeModalCleanup).
   function registerModalEsc(hideFn) {
      if (activeModalEscToken !== null) DawnEscStack.unregister(activeModalEscToken);
      activeModalEscToken = DawnEscStack.register(() => {
         hideFn();
         return true;
      });
   }

   function unregisterModalEsc() {
      if (activeModalEscToken !== null) {
         DawnEscStack.unregister(activeModalEscToken);
         activeModalEscToken = null;
      }
   }
   let callbacks = {
      trapFocus: null,
      getAuthState: null,
   };

   /* =============================================================================
    * API Requests
    * ============================================================================= */

   function requestListUsers() {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         // Show loading state
         const userList = document.getElementById('user-list');
         if (userList) {
            userList.innerHTML = '<div class="loading-indicator">Loading users...</div>';
         }
         DawnWS.send({ type: 'list_users' });
      }
   }

   function requestCreateUser(username, password, isAdmin) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({
            type: 'create_user',
            payload: { username, password, is_admin: isAdmin },
         });
      }
   }

   function requestDeleteUser(username) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({
            type: 'delete_user',
            payload: { username },
         });
      }
   }

   function requestChangePassword(username, newPassword, currentPassword) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         const payload = { username, new_password: newPassword };
         if (currentPassword) {
            payload.current_password = currentPassword;
         }
         DawnWS.send({ type: 'change_password', payload });
      }
   }

   function requestUnlockUser(username) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({
            type: 'unlock_user',
            payload: { username },
         });
      }
   }

   /* =============================================================================
    * Response Handlers
    * ============================================================================= */

   function handleListUsersResponse(payload) {
      if (!payload.success) {
         console.error('Failed to list users:', payload.error);
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Failed to load users: ' + (payload.error || 'Unknown error'), 'error');
         }
         return;
      }

      const userList = document.getElementById('user-list');
      if (!userList) return;

      userList.innerHTML = '';
      const authState = callbacks.getAuthState ? callbacks.getAuthState() : {};

      for (const user of payload.users || []) {
         const userItem = document.createElement('div');
         userItem.className = 'user-list-item';

         const userInfo = document.createElement('div');
         userInfo.className = 'user-info';

         const username = document.createElement('span');
         username.className = 'username';
         username.textContent = user.username;

         const details = document.createElement('span');
         details.className = 'user-details';
         details.textContent = user.last_login
            ? 'Last login: ' + DawnTime.formatDateTime(user.last_login)
            : 'Never logged in';

         userInfo.appendChild(username);
         userInfo.appendChild(details);

         const badges = document.createElement('div');
         badges.className = 'user-badges';

         if (user.is_admin) {
            const badge = document.createElement('span');
            badge.className = 'role-badge admin';
            badge.textContent = 'Admin';
            badges.appendChild(badge);
         }

         if (user.is_locked) {
            const badge = document.createElement('span');
            badge.className = 'role-badge locked';
            badge.textContent = 'Locked';
            badges.appendChild(badge);
         }

         const actions = document.createElement('div');
         actions.className = 'user-actions';

         // Password reset button (key icon)
         const resetBtn = document.createElement('button');
         resetBtn.className = 'btn-icon';
         resetBtn.title = 'Reset password';
         resetBtn.innerHTML =
            '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><circle cx="8" cy="12" r="3.2"/><path d="M11 12h9"/><path d="M17 12v2"/><path d="M20 12v2.6"/></svg>';
         resetBtn.addEventListener('click', () => showResetPasswordModal(user.username));
         actions.appendChild(resetBtn);

         // Unlock button (only for locked users)
         if (user.is_locked) {
            const unlockBtn = document.createElement('button');
            unlockBtn.className = 'btn-icon';
            unlockBtn.title = 'Unlock user';
            unlockBtn.innerHTML =
               '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><rect x="4" y="11" width="16" height="10" rx="2"/><path d="M8 11V7a4 4 0 0 1 7.83-1.17"/><circle cx="12" cy="15" r="1.5"/><path d="M12 16.5V18"/></svg>';
            unlockBtn.addEventListener('click', async () => {
               if (
                  await DawnDialog.confirm('Unlock user "' + user.username + '"?', {
                     title: 'Unlock User',
                     okText: 'Unlock',
                  })
               ) {
                  requestUnlockUser(user.username);
               }
            });
            actions.appendChild(unlockBtn);
         }

         // Delete button (not for self)
         if (user.username !== authState.username) {
            const deleteBtn = document.createElement('button');
            deleteBtn.className = 'btn-icon btn-danger';
            deleteBtn.title = 'Delete user';
            deleteBtn.innerHTML =
               '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><path d="M4 7h16"/><path d="M10 3h4"/><path d="M6 7l1.5 13a1 1 0 0 0 1 1h7a1 1 0 0 0 1-1L18 7"/><path d="M10 11v6" opacity="0.6"/><path d="M14 11v6" opacity="0.6"/></svg>';
            deleteBtn.addEventListener('click', async () => {
               if (
                  await DawnDialog.confirm(
                     'Delete user "' + user.username + '"?\n\nThis action cannot be undone.',
                     { title: 'Delete User', okText: 'Delete', danger: true }
                  )
               ) {
                  requestDeleteUser(user.username);
               }
            });
            actions.appendChild(deleteBtn);
         }

         userItem.appendChild(userInfo);
         userItem.appendChild(badges);
         userItem.appendChild(actions);
         userList.appendChild(userItem);
      }
   }

   function handleCreateUserResponse(payload) {
      // Re-enable submit button
      const addUserForm = document.getElementById('add-user-form');
      if (addUserForm) {
         const submitBtn = addUserForm.querySelector('button[type="submit"]');
         if (submitBtn) {
            submitBtn.disabled = false;
            submitBtn.textContent = 'Create User';
         }
      }

      if (payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('User created successfully', 'success');
         }
         hideAddUserModal();
         // Small delay to ensure modal cleanup completes before refresh
         setTimeout(requestListUsers, 100);
      } else {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Failed to create user: ' + (payload.error || 'Unknown error'), 'error');
         }
      }
   }

   function handleDeleteUserResponse(payload) {
      if (payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('User deleted successfully', 'success');
         }
         requestListUsers();
      } else {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Failed to delete user: ' + (payload.error || 'Unknown error'), 'error');
         }
      }
   }

   function handleChangePasswordResponse(payload) {
      // Re-enable submit button
      const resetPasswordForm = document.getElementById('reset-password-form');
      if (resetPasswordForm) {
         const submitBtn = resetPasswordForm.querySelector('button[type="submit"]');
         if (submitBtn) {
            submitBtn.disabled = false;
            submitBtn.textContent = 'Change Password';
         }
      }

      if (payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Password changed successfully', 'success');
         }
         hideResetPasswordModal();
      } else {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(
               'Failed to change password: ' + (payload.error || 'Unknown error'),
               'error'
            );
         }
      }
   }

   function handleUnlockUserResponse(payload) {
      if (payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('User unlocked successfully', 'success');
         }
         requestListUsers();
      } else {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Failed to unlock user: ' + (payload.error || 'Unknown error'), 'error');
         }
      }
   }

   /* =============================================================================
    * Modals
    * ============================================================================= */

   function showAddUserModal() {
      const modal = document.getElementById('add-user-modal');
      if (modal) {
         modal.classList.remove('hidden');
         registerModalEsc(hideAddUserModal);
         const form = document.getElementById('add-user-form');
         if (form) form.reset();
         if (callbacks.trapFocus) {
            activeModalCleanup = callbacks.trapFocus(modal);
         }
      }
   }

   function hideAddUserModal() {
      const modal = document.getElementById('add-user-modal');
      if (modal) {
         modal.classList.add('hidden');
         unregisterModalEsc();
         if (activeModalCleanup) {
            activeModalCleanup();
            activeModalCleanup = null;
         }
      }
   }

   function showResetPasswordModal(username) {
      const modal = document.getElementById('reset-password-modal');
      if (modal) {
         modal.classList.remove('hidden');
         registerModalEsc(hideResetPasswordModal);
         const usernameField = document.getElementById('reset-password-username');
         if (usernameField) usernameField.value = username;
         const passwordField = document.getElementById('reset-password-new');
         if (passwordField) passwordField.value = '';
         if (callbacks.trapFocus) {
            activeModalCleanup = callbacks.trapFocus(modal);
         }
      }
   }

   function hideResetPasswordModal() {
      const modal = document.getElementById('reset-password-modal');
      if (modal) {
         modal.classList.add('hidden');
         unregisterModalEsc();
         if (activeModalCleanup) {
            activeModalCleanup();
            activeModalCleanup = null;
         }
      }
   }

   /* =============================================================================
    * Initialization
    * ============================================================================= */

   function init() {
      // Add user button
      const addUserBtn = document.getElementById('add-user-btn');
      if (addUserBtn) {
         addUserBtn.addEventListener('click', showAddUserModal);
      }

      // Refresh users button
      const refreshUsersBtn = document.getElementById('refresh-users-btn');
      if (refreshUsersBtn) {
         refreshUsersBtn.addEventListener('click', requestListUsers);
      }

      // Add user form
      const addUserForm = document.getElementById('add-user-form');
      if (addUserForm) {
         addUserForm.addEventListener('submit', (e) => {
            e.preventDefault();
            const username = document.getElementById('new-username').value.trim();
            const password = document.getElementById('new-password').value;
            const isAdmin = document.getElementById('new-is-admin').checked;
            if (username && password) {
               const submitBtn = addUserForm.querySelector('button[type="submit"]');
               if (submitBtn) {
                  submitBtn.disabled = true;
                  submitBtn.textContent = 'Creating...';
               }
               requestCreateUser(username, password, isAdmin);
            }
         });
      }

      // Add user modal cancel
      const addUserCancel = document.getElementById('add-user-cancel');
      if (addUserCancel) {
         addUserCancel.addEventListener('click', hideAddUserModal);
      }

      // Reset password form
      const resetPasswordForm = document.getElementById('reset-password-form');
      if (resetPasswordForm) {
         resetPasswordForm.addEventListener('submit', (e) => {
            e.preventDefault();
            const username = document.getElementById('reset-password-username').value;
            const newPassword = document.getElementById('reset-password-new').value;
            if (username && newPassword) {
               const submitBtn = resetPasswordForm.querySelector('button[type="submit"]');
               if (submitBtn) {
                  submitBtn.disabled = true;
                  submitBtn.textContent = 'Changing...';
               }
               requestChangePassword(username, newPassword);
            }
         });
      }

      // Reset password modal cancel
      const resetPasswordCancel = document.getElementById('reset-password-cancel');
      if (resetPasswordCancel) {
         resetPasswordCancel.addEventListener('click', hideResetPasswordModal);
      }

      // User management section header click to expand/refresh
      const userMgmtHeader = document.querySelector('#user-management-section .section-header');
      if (userMgmtHeader) {
         userMgmtHeader.addEventListener('click', (e) => {
            const section = e.target.closest('.settings-section');
            if (section) {
               section.classList.toggle('collapsed');
               if (!section.classList.contains('collapsed')) {
                  requestListUsers();
               }
            }
         });
      }

      // Modal backdrop click to close
      document.querySelectorAll('.modal').forEach((modal) => {
         modal.addEventListener('click', (e) => {
            if (e.target === modal) {
               modal.classList.add('hidden');
            }
         });
      });

      // Escape close is handled per-modal via DawnEscStack (registerModalEsc on
      // show / unregisterModalEsc on hide), so it stacks correctly under any
      // layer opened above a user modal.
   }

   /**
    * Set callbacks for shared utilities
    */
   function setCallbacks(cbs) {
      if (cbs.trapFocus) callbacks.trapFocus = cbs.trapFocus;
      if (cbs.getAuthState) callbacks.getAuthState = cbs.getAuthState;
   }

   /* =============================================================================
    * Export
    * ============================================================================= */

   window.DawnUsers = {
      init: init,
      setCallbacks: setCallbacks,
      requestList: requestListUsers,
      handleListResponse: handleListUsersResponse,
      handleCreateResponse: handleCreateUserResponse,
      handleDeleteResponse: handleDeleteUserResponse,
      handleChangePasswordResponse: handleChangePasswordResponse,
      handleUnlockResponse: handleUnlockUserResponse,
      showAddModal: showAddUserModal,
      showResetPasswordModal: showResetPasswordModal,
   };
})();
