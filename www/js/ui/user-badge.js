/**
 * DAWN User Badge Dropdown Module
 * Header badge with dropdown for user actions (settings, sessions, metrics, logout)
 *
 * Usage:
 *   DawnUserBadge.init({ openSection, metricsToggle })
 */
(function (global) {
   'use strict';

   let callbacks = {
      openSection: null, // (sectionId: string) => void
      metricsToggle: null, // () => void
   };
   let badgeEscToken = null; // DawnEscStack registration while the dropdown is open

   function init(cbs) {
      if (cbs) callbacks = cbs;

      var badge = document.getElementById('user-badge');
      var dropdown = document.getElementById('user-badge-dropdown');

      if (!badge || !dropdown) return;

      // Toggle dropdown on badge click
      badge.addEventListener('click', function (e) {
         e.stopPropagation();
         var isOpen = dropdown.classList.contains('open');
         if (isOpen) {
            close();
         } else {
            open();
         }
      });

      // Handle keyboard navigation
      badge.addEventListener('keydown', function (e) {
         if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            badge.click();
         }
         // Escape close is handled via DawnEscStack (open()/close()).
      });

      // Handle keyboard navigation in dropdown
      dropdown.addEventListener('keydown', function (e) {
         var items = Array.from(dropdown.querySelectorAll('.dropdown-item:not(.dropdown-divider)'));
         var currentIndex = items.indexOf(document.activeElement);

         switch (e.key) {
            case 'ArrowDown':
               e.preventDefault();
               var nextIndex = currentIndex < items.length - 1 ? currentIndex + 1 : 0;
               if (items[nextIndex]) items[nextIndex].focus();
               break;
            case 'ArrowUp':
               e.preventDefault();
               var prevIndex = currentIndex > 0 ? currentIndex - 1 : items.length - 1;
               if (items[prevIndex]) items[prevIndex].focus();
               break;
            case 'Home':
               e.preventDefault();
               if (items[0]) items[0].focus();
               break;
            case 'End':
               e.preventDefault();
               if (items[items.length - 1]) items[items.length - 1].focus();
               break;
         }
      });

      // Handle dropdown item clicks
      dropdown.addEventListener('click', function (e) {
         var item = e.target.closest('.dropdown-item');
         if (!item) return;

         var action = item.dataset.action;

         // Alarm-sounds is an in-place toggle, not a navigation action — flip it
         // and leave the menu open so the state change is visible.
         if (action === 'alarm-sounds') {
            if (window.DawnScheduler) {
               DawnScheduler.setAlarmSounds(!DawnScheduler.isAlarmSoundsEnabled());
               syncAlarmSoundsItem();
            }
            return;
         }

         close();

         switch (action) {
            case 'my-settings':
               if (callbacks.openSection) callbacks.openSection('my-settings-section');
               break;
            case 'my-sessions':
               if (callbacks.openSection) callbacks.openSection('my-sessions-section');
               break;
            case 'system-metrics':
               if (callbacks.metricsToggle) callbacks.metricsToggle();
               break;
            case 'logout':
               handleLogout();
               break;
         }
      });

      // Close dropdown when clicking outside
      document.addEventListener('click', function (e) {
         if (!e.target.closest('.user-badge-container')) {
            close();
         }
      });
   }

   // Reflect the persisted "Alarm sounds" state on its menu item (aria-checked
   // for AT + the visible check glyph). No-op if the item or DawnScheduler is
   // absent, so the header still works if scheduler.js failed to load.
   function syncAlarmSoundsItem() {
      var item = document.getElementById('alarm-sounds-item');
      if (!item || !window.DawnScheduler) return;
      // aria-checked is the single source of truth — the .dropdown-item-check
      // glyph follows it via CSS, so we never touch the span directly.
      item.setAttribute('aria-checked', DawnScheduler.isAlarmSoundsEnabled() ? 'true' : 'false');
   }

   function open() {
      var badge = document.getElementById('user-badge');
      var dropdown = document.getElementById('user-badge-dropdown');
      if (!badge || !dropdown) return;

      badge.setAttribute('aria-expanded', 'true');
      dropdown.classList.add('open');
      syncAlarmSoundsItem();
      if (badgeEscToken === null) {
         badgeEscToken = DawnEscStack.register(function () {
            close();
            var b = document.getElementById('user-badge');
            if (b) b.focus();
            return true;
         });
      }

      // Focus first menu item for keyboard users
      var firstItem = dropdown.querySelector('.dropdown-item');
      if (firstItem) {
         setTimeout(function () {
            firstItem.focus();
         }, 50);
      }
   }

   function close() {
      var badge = document.getElementById('user-badge');
      var dropdown = document.getElementById('user-badge-dropdown');
      if (!badge || !dropdown) return;

      badge.setAttribute('aria-expanded', 'false');
      dropdown.classList.remove('open');
      if (badgeEscToken !== null) {
         DawnEscStack.unregister(badgeEscToken);
         badgeEscToken = null;
      }
   }

   async function handleLogout() {
      // Clear WebSocket session token from localStorage
      DawnStore.remove(DawnStore.KEYS.SESSION_TOKEN);

      try {
         // Use GET - logout has no request body
         await fetch('/api/auth/logout', {
            credentials: 'same-origin',
         });

         // Always redirect to login page regardless of response
         window.location.href = '/login.html';
      } catch (err) {
         console.error('Logout error:', err);
         // Redirect anyway on network error
         window.location.href = '/login.html';
      }
   }

   global.DawnUserBadge = {
      init: init,
   };
})(window);
