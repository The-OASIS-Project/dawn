/**
 * DAWN Settings - Modal Dialogs
 * Confirm and input modal dialogs with focus trap support
 */
(function () {
   'use strict';

   // Modal state
   let confirmModalCleanup = null;
   let inputModalCleanup = null;
   let pendingConfirmCallback = null;
   let pendingInputCallback = null;
   let modalTriggerElement = null; // Track element to return focus to (M13)
   // One-shot outcome callbacks used by the Promise wrapper (DawnDialog).  Fired
   // exactly once on any close path (confirm/cancel/backdrop/Escape) then nulled.
   let pendingConfirmOnClose = null;
   let pendingInputOnClose = null;
   // DawnEscStack tokens — registered while the modal is shown, unregistered on
   // hide, so Escape closes the topmost layer without a standing document listener.
   let confirmEscToken = null;
   let inputEscToken = null;

   /**
    * Focus trap for modals — keeps Tab key cycling within the modal.
    *
    * Focusables are queried on EACH Tab press (not cached at setup),
    * so the trap stays correct when the modal's DOM changes while
    * open — broadcasts updating row lists, filter-driven re-renders,
    * conditionally-rendered buttons all work without re-installing
    * the trap.  Filters out hidden / aria-hidden / disabled elements
    * since querySelectorAll picks those up but they aren't actually
    * focus targets.
    *
    * @param {HTMLElement} modal - Modal element to trap focus in
    * @param {Object} [options]
    * @param {boolean} [options.skipInitialFocus=false] - If true, do
    *   NOT focus the first focusable at setup.  Use when the caller
    *   manages initial focus itself (e.g., scheduler-queue focuses
    *   the close button after a layout-settle setTimeout).
    * @returns {Function} Cleanup function — removes the keydown listener.
    */
   function trapFocus(modal, options) {
      const opts = options || {};
      const focusableSelectors =
         'button, [href], input, select, textarea, [tabindex]:not([tabindex="-1"])';

      function getFocusables() {
         /* Live query per keypress.  Filter out hidden and disabled
          * elements — these match the selector but the browser
          * skips them in Tab order, so we'd otherwise hand focus to
          * an invisible target. */
         const nodes = modal.querySelectorAll(focusableSelectors);
         const out = [];
         for (let i = 0; i < nodes.length; i++) {
            const el = nodes[i];
            if (el.disabled) continue;
            if (el.getAttribute('aria-hidden') === 'true') continue;
            /* `getClientRects().length === 0` is the most orthodox
             * "this element renders nothing on screen" test —
             * handles display:none ancestors, detached subtrees,
             * hidden attribute, AND position:sticky/fixed correctly
             * (where the older `offsetParent === null` check
             * misreported sticky descendants as hidden).  Sub-ms on
             * the small popover element counts we deal with. */
            if (el.getClientRects().length === 0) continue;
            out.push(el);
         }
         return out;
      }

      function handleKeydown(e) {
         if (e.key !== 'Tab') return;
         const focusables = getFocusables();
         if (focusables.length === 0) return;
         const first = focusables[0];
         const last = focusables[focusables.length - 1];

         if (e.shiftKey) {
            if (document.activeElement === first || !modal.contains(document.activeElement)) {
               e.preventDefault();
               last.focus();
            }
         } else {
            if (document.activeElement === last || !modal.contains(document.activeElement)) {
               e.preventDefault();
               first.focus();
            }
         }
      }

      modal.addEventListener('keydown', handleKeydown);

      if (!opts.skipInitialFocus) {
         const initial = getFocusables();
         if (initial.length > 0) initial[0].focus();
      }

      return () => modal.removeEventListener('keydown', handleKeydown);
   }

   /**
    * Show styled confirmation modal (replaces native confirm())
    * @param {string} message - Message to display
    * @param {Function} onConfirm - Callback if user confirms
    * @param {Object} options - Optional settings
    * @param {string} options.title - Modal title (default: "Confirm")
    * @param {string} options.okText - OK button text (default: "OK")
    * @param {string} options.cancelText - Cancel button text (default: "Cancel")
    * @param {boolean} options.danger - If true, styles OK button as danger/red
    * @param {string} options.detail - Optional detail text to show (e.g., item being deleted)
    * @param {boolean} options.okOnly - If true, hide the Cancel button (alert-style)
    * @param {Function} options.onClose - Called once with the boolean outcome on
    *   any close path (confirm/cancel/backdrop/Escape).  Used by DawnDialog.
    */
   function showConfirmModal(message, onConfirm, options = {}) {
      const modal = document.getElementById('confirm-modal');
      if (!modal) {
         // Fallback to native confirm if the modal DOM is absent (e.g. login page).
         // Still resolve onClose so a Promise wrapper never hangs.
         const result = confirm(message);
         if (result && onConfirm) onConfirm();
         if (typeof options.onClose === 'function') options.onClose(result);
         return;
      }

      const content = modal.querySelector('.modal-content');
      const titleEl = document.getElementById('confirm-modal-title');
      const messageEl = document.getElementById('confirm-modal-message');
      const detailEl = document.getElementById('confirm-modal-detail');
      const okBtn = document.getElementById('confirm-modal-ok');
      const cancelBtn = document.getElementById('confirm-modal-cancel');

      // Set content
      if (titleEl) titleEl.textContent = options.title || 'Confirm';
      if (messageEl) messageEl.textContent = message;
      if (okBtn) okBtn.textContent = options.okText || 'OK';
      if (cancelBtn) {
         cancelBtn.textContent = options.cancelText || 'Cancel';
         // Alert-style: hide Cancel entirely (reuses the .hidden idiom).
         cancelBtn.classList.toggle('hidden', !!options.okOnly);
      }

      // Set detail text if provided
      if (detailEl) {
         if (options.detail) {
            detailEl.textContent = options.detail;
            detailEl.classList.remove('hidden');
         } else {
            detailEl.textContent = '';
            detailEl.classList.add('hidden');
         }
      }

      // Apply danger styling if requested
      if (content) {
         content.classList.toggle('confirm-danger', !!options.danger);
      }

      // Store callback and trigger element for focus restoration (M13)
      pendingConfirmCallback = onConfirm;
      pendingConfirmOnClose = typeof options.onClose === 'function' ? options.onClose : null;
      modalTriggerElement = document.activeElement;

      // Show modal.  A destructive confirm starts on the safe choice, so a stray
      // Enter/Space (the dialog may open unprompted, e.g. after a server reply)
      // can't take the irreversible action.
      modal.classList.remove('hidden');
      const safeFirst = !!options.danger && cancelBtn && !options.okOnly;
      confirmModalCleanup = trapFocus(modal, { skipInitialFocus: safeFirst });
      if (safeFirst) cancelBtn.focus();
      // Unregister any stale token first (defensive — a direct show while one is
      // pending would otherwise orphan the prior registration on the stack).
      if (confirmEscToken !== null) DawnEscStack.unregister(confirmEscToken);
      confirmEscToken = DawnEscStack.register(() => {
         hideConfirmModal(false);
         return true;
      });
   }

   /**
    * Hide confirmation modal
    * @param {boolean} confirmed - Whether user confirmed the action
    */
   function hideConfirmModal(confirmed) {
      const modal = document.getElementById('confirm-modal');
      if (modal) {
         modal.classList.add('hidden');
         if (confirmModalCleanup) {
            confirmModalCleanup();
            confirmModalCleanup = null;
         }
      }
      if (confirmEscToken !== null) {
         DawnEscStack.unregister(confirmEscToken);
         confirmEscToken = null;
      }

      if (confirmed && pendingConfirmCallback) {
         pendingConfirmCallback();
      }
      pendingConfirmCallback = null;

      // Fire the one-shot outcome callback (Promise wrapper) exactly once.
      if (pendingConfirmOnClose) {
         const cb = pendingConfirmOnClose;
         pendingConfirmOnClose = null;
         cb(confirmed);
      }

      // Restore focus to trigger element (M13)
      if (modalTriggerElement && typeof modalTriggerElement.focus === 'function') {
         modalTriggerElement.focus();
         modalTriggerElement = null;
      }
   }

   /**
    * Initialize confirmation modal event handlers
    */
   function initConfirmModal() {
      const okBtn = document.getElementById('confirm-modal-ok');
      const cancelBtn = document.getElementById('confirm-modal-cancel');
      const modal = document.getElementById('confirm-modal');

      if (okBtn) {
         okBtn.addEventListener('click', () => hideConfirmModal(true));
      }
      if (cancelBtn) {
         cancelBtn.addEventListener('click', () => hideConfirmModal(false));
      }

      // Close on backdrop click
      if (modal) {
         modal.addEventListener('click', (e) => {
            if (e.target === modal) hideConfirmModal(false);
         });
      }

      // Escape close is handled via DawnEscStack (register-on-show in
      // showConfirmModal / unregister-on-hide in hideConfirmModal).
   }

   /**
    * Show styled input modal (replaces native prompt())
    * @param {string} message - Message to display
    * @param {string} defaultValue - Default value for input field
    * @param {Function} onSubmit - Callback with input value if user submits
    * @param {Object} options - Optional settings
    * @param {string} options.title - Modal title (default: "Enter Value")
    * @param {string} options.okText - OK button text (default: "OK")
    * @param {string} options.cancelText - Cancel button text (default: "Cancel")
    * @param {string} options.placeholder - Input placeholder text
    * @param {Function} options.onClose - Called once with the entered string on
    *   submit, or null on cancel/backdrop/Escape.  Used by DawnDialog.
    */
   function showInputModal(message, defaultValue, onSubmit, options = {}) {
      const modal = document.getElementById('input-modal');
      if (!modal) {
         // Fallback to native prompt if the modal DOM is absent.  Still resolve
         // onClose so a Promise wrapper never hangs.
         const result = prompt(message, defaultValue);
         if (result !== null && onSubmit) onSubmit(result);
         if (typeof options.onClose === 'function') options.onClose(result);
         return;
      }

      const titleEl = document.getElementById('input-modal-title');
      const messageEl = document.getElementById('input-modal-message');
      const inputEl = document.getElementById('input-modal-input');
      const okBtn = document.getElementById('input-modal-ok');
      const cancelBtn = document.getElementById('input-modal-cancel');

      // Set content
      if (titleEl) titleEl.textContent = options.title || 'Enter Value';
      if (messageEl) {
         messageEl.textContent = message || '';
         messageEl.style.display = message ? 'block' : 'none';
      }
      if (inputEl) {
         inputEl.value = defaultValue || '';
         inputEl.placeholder = options.placeholder || '';
      }
      if (okBtn) okBtn.textContent = options.okText || 'OK';
      if (cancelBtn) cancelBtn.textContent = options.cancelText || 'Cancel';

      // Store callback and trigger element for focus restoration (M13)
      pendingInputCallback = onSubmit;
      pendingInputOnClose = typeof options.onClose === 'function' ? options.onClose : null;
      modalTriggerElement = document.activeElement;

      // Show modal and focus input
      modal.classList.remove('hidden');
      inputModalCleanup = trapFocus(modal);
      if (inputEscToken !== null) DawnEscStack.unregister(inputEscToken);
      inputEscToken = DawnEscStack.register(() => {
         hideInputModal(false);
         return true;
      });

      // Focus and select input text
      if (inputEl) {
         inputEl.focus();
         inputEl.select();
      }
   }

   /**
    * Hide input modal
    * @param {boolean} submitted - Whether user submitted the input
    */
   function hideInputModal(submitted) {
      const modal = document.getElementById('input-modal');
      const inputEl = document.getElementById('input-modal-input');

      if (modal) {
         modal.classList.add('hidden');
         if (inputModalCleanup) {
            inputModalCleanup();
            inputModalCleanup = null;
         }
      }
      if (inputEscToken !== null) {
         DawnEscStack.unregister(inputEscToken);
         inputEscToken = null;
      }

      const submittedValue = submitted && inputEl ? inputEl.value : null;
      if (submitted && pendingInputCallback && inputEl) {
         pendingInputCallback(inputEl.value);
      }
      pendingInputCallback = null;

      // Fire the one-shot outcome callback (Promise wrapper): value on submit,
      // null on cancel/backdrop/Escape.
      if (pendingInputOnClose) {
         const cb = pendingInputOnClose;
         pendingInputOnClose = null;
         cb(submittedValue);
      }

      // Restore focus to trigger element (M13)
      if (modalTriggerElement && typeof modalTriggerElement.focus === 'function') {
         modalTriggerElement.focus();
         modalTriggerElement = null;
      }
   }

   /**
    * Initialize input modal event handlers
    */
   function initInputModal() {
      const okBtn = document.getElementById('input-modal-ok');
      const cancelBtn = document.getElementById('input-modal-cancel');
      const inputEl = document.getElementById('input-modal-input');
      const modal = document.getElementById('input-modal');

      if (okBtn) {
         okBtn.addEventListener('click', () => hideInputModal(true));
      }
      if (cancelBtn) {
         cancelBtn.addEventListener('click', () => hideInputModal(false));
      }

      // Submit on Enter key in input field
      if (inputEl) {
         inputEl.addEventListener('keydown', (e) => {
            if (e.key === 'Enter') {
               e.preventDefault();
               hideInputModal(true);
            }
         });
      }

      // Close on backdrop click
      if (modal) {
         modal.addEventListener('click', (e) => {
            if (e.target === modal) hideInputModal(false);
         });
      }

      // Escape close is handled via DawnEscStack (register-on-show in
      // showInputModal / unregister-on-hide in hideInputModal).
   }

   // Export for settings module
   window.DawnSettingsModals = {
      trapFocus,
      showConfirmModal,
      hideConfirmModal,
      initConfirmModal,
      showInputModal,
      hideInputModal,
      initInputModal,
   };
})();
