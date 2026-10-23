/**
 * DAWN Home Assistant Integration Module
 * Handles HA status, entity listing, and connection testing
 *
 * Usage:
 *   DawnHomeAssistant.requestStatus()                // Request status from server
 *   DawnHomeAssistant.handleStatusResponse(p)        // Handle status response
 *   DawnHomeAssistant.handleTestConnectionResponse(p)
 *   DawnHomeAssistant.handleEntitiesResponse(p)
 *   DawnHomeAssistant.testConnection()               // Test connection
 *   DawnHomeAssistant.requestEntities()              // Request entity list
 *   DawnHomeAssistant.refreshEntities()              // Force refresh entities
 *   DawnHomeAssistant.setElements(els)               // Set UI elements
 */
(function (global) {
   'use strict';

   // =============================================================================
   // State
   // =============================================================================

   let state = {
      configured: false,
      connected: false,
      entity_count: 0,
      version: '',
      url: '',
      entities: [],
      cached_at: 0,
   };

   // UI Elements (set via setElements)
   let elements = {};

   // Filter debounce timer
   let filterTimer = null;
   const FILTER_DEBOUNCE_MS = 150;

   // Domain icon mapping
   const DOMAIN_ICONS = {
      light: '\u{1F4A1}',
      switch: '\u{1F50C}',
      climate: '\u{1F321}\uFE0F',
      lock: '\u{1F510}',
      cover: '\u{1FA9F}',
      media_player: '\u{1F50A}',
      fan: '\u{1F300}',
      scene: '\u2728',
      script: '\u{1F4DC}',
      automation: '\u2699\uFE0F',
      sensor: '\u{1F4CA}',
      binary_sensor: '\u{1F518}',
      input_boolean: '\u{1F518}',
      vacuum: '\u{1F9F9}',
      alarm_control_panel: '\u{1F6A8}',
   };

   // =============================================================================
   // UI Helpers
   // =============================================================================

   function haShow(el) {
      if (!el) return;
      el.classList.remove('ha-hidden');
      el.classList.add('ha-visible');
   }

   function haHide(el) {
      if (!el) return;
      el.classList.remove('ha-visible');
      el.classList.add('ha-hidden');
   }

   function setBtnLoading(btn, loading) {
      if (!btn) return;
      btn.disabled = loading;
      btn.setAttribute('aria-busy', loading ? 'true' : 'false');
      if (loading) {
         btn.dataset.originalText = btn.textContent;
         btn.textContent = btn.textContent + '...';
      } else if (btn.dataset.originalText) {
         btn.textContent = btn.dataset.originalText;
         delete btn.dataset.originalText;
      }
   }

   function formatRelativeTime(unixSeconds) {
      if (!unixSeconds) return '';
      var now = Math.floor(Date.now() / 1000);
      var diff = now - unixSeconds;
      if (diff < 5) return 'just now';
      if (diff < 60) return diff + 's ago';
      if (diff < 3600) return Math.floor(diff / 60) + 'm ago';
      if (diff < 86400) return Math.floor(diff / 3600) + 'h ago';
      return DawnTime.formatDateTime(unixSeconds);
   }

   function updateStatusUI() {
      var statusEl = elements.statusIndicator;
      var statusText = elements.statusText;
      var entityCountEl = elements.entityCount;
      var versionEl = elements.version;
      var testBtn = elements.testConnectionBtn;
      var refreshBtn = elements.refreshEntitiesBtn;
      var entitySection = elements.entitySection;
      var lastUpdatedEl = elements.lastUpdated;

      if (!statusEl) return;

      if (!state.configured) {
         statusEl.className = 'ha-status-dot ha-status-not-configured';
         if (statusText) statusText.textContent = 'Not Configured';
         haHide(refreshBtn);
         haHide(entitySection);
      } else if (state.connected) {
         statusEl.className = 'ha-status-dot ha-status-connected';
         if (statusText) {
            statusText.textContent =
               'Connected' + (state.version ? ' (v' + state.version + ')' : '');
         }
         haShow(refreshBtn);
         haShow(entitySection);
      } else {
         statusEl.className = 'ha-status-dot ha-status-disconnected';
         if (statusText) statusText.textContent = 'Connection Failed';
         haHide(refreshBtn);
         haHide(entitySection);
      }

      if (entityCountEl) {
         entityCountEl.textContent = state.entity_count + ' entities';
      }
      if (versionEl && state.version) {
         versionEl.textContent = 'v' + state.version;
      }
      if (lastUpdatedEl) {
         if (state.cached_at > 0) {
            lastUpdatedEl.textContent = 'Updated ' + formatRelativeTime(state.cached_at);
            haShow(lastUpdatedEl);
         } else {
            haHide(lastUpdatedEl);
         }
      }

      if (testBtn) {
         haShow(testBtn);
      }
   }

   // =============================================================================
   // Entity Rendering
   // =============================================================================

   function renderEntities(entities) {
      var container = elements.entityList;
      if (!container) return;

      container.innerHTML = '';

      if (!entities || entities.length === 0) {
         container.innerHTML = '<div class="ha-no-entities">No entities found</div>';
         return;
      }

      // Group by domain
      var grouped = {};
      entities.forEach(function (ent) {
         var d = ent.domain || 'unknown';
         if (!grouped[d]) grouped[d] = [];
         grouped[d].push(ent);
      });

      // Sort domains alphabetically
      var domains = Object.keys(grouped).sort();

      domains.forEach(function (domain) {
         var items = grouped[domain];
         var icon = DOMAIN_ICONS[domain] || '\u2753';

         // Domain header
         var header = document.createElement('div');
         header.className = 'ha-domain-header';
         header.textContent = icon + ' ' + domain.replace(/_/g, ' ') + ' (' + items.length + ')';
         container.appendChild(header);

         // Entity items
         items.forEach(function (ent) {
            var item = document.createElement('div');
            item.className = 'ha-entity-item';
            item.dataset.name = (ent.friendly_name || '').toLowerCase();
            item.dataset.id = (ent.entity_id || '').toLowerCase();
            item.dataset.area = (ent.area || '').toLowerCase();

            var html =
               '<span class="ha-entity-name">' +
               escapeHtml(ent.friendly_name || ent.entity_id) +
               '</span>';
            html += '<span class="ha-entity-id">' + escapeHtml(ent.entity_id) + '</span>';
            html += '<span class="ha-entity-state">' + escapeHtml(ent.state) + '</span>';
            if (ent.area) {
               html += '<span class="ha-entity-area">' + escapeHtml(ent.area) + '</span>';
            }
            item.innerHTML = html;
            container.appendChild(item);
         });
      });
   }

   function filterEntities(query) {
      var container = elements.entityList;
      if (!container) return;

      var q = (query || '').toLowerCase().trim();
      var items = container.querySelectorAll('.ha-entity-item');
      var headers = container.querySelectorAll('.ha-domain-header');

      if (!q) {
         items.forEach(function (el) {
            el.classList.remove('ha-hidden');
         });
         headers.forEach(function (el) {
            el.classList.remove('ha-hidden');
         });
         return;
      }

      items.forEach(function (el) {
         var name = el.dataset.name || '';
         var id = el.dataset.id || '';
         var area = el.dataset.area || '';
         var match = name.indexOf(q) !== -1 || id.indexOf(q) !== -1 || area.indexOf(q) !== -1;
         if (match) {
            el.classList.remove('ha-hidden');
         } else {
            el.classList.add('ha-hidden');
         }
      });

      // Hide domain headers with no visible items
      headers.forEach(function (header) {
         var next = header.nextElementSibling;
         var hasVisible = false;
         while (next && !next.classList.contains('ha-domain-header')) {
            if (
               next.classList.contains('ha-entity-item') &&
               !next.classList.contains('ha-hidden')
            ) {
               hasVisible = true;
            }
            next = next.nextElementSibling;
         }
         if (hasVisible) {
            header.classList.remove('ha-hidden');
         } else {
            header.classList.add('ha-hidden');
         }
      });
   }

   function escapeHtml(str) {
      var div = document.createElement('div');
      div.textContent = str;
      return div.innerHTML;
   }

   // =============================================================================
   // WebSocket Handlers
   // =============================================================================

   function handleStatusResponse(payload) {
      state.configured = payload.configured || false;
      state.connected = payload.connected || false;
      state.entity_count = payload.entity_count || 0;
      state.version = payload.version || '';
      if (payload.url !== undefined) state.url = payload.url;
      if (payload.led_hue_correction !== undefined) {
         state.led_hue_correction = payload.led_hue_correction;
      }
      updateStatusUI();

      // Populate URL field from server state
      if (elements.urlInput && state.url && !elements.urlInput.value) {
         elements.urlInput.value = state.url;
      }

      // Populate hue correction slider from server state
      if (elements.hueCorrection && state.led_hue_correction !== undefined) {
         elements.hueCorrection.value = state.led_hue_correction;
         if (elements.hueCorrectionValue) {
            elements.hueCorrectionValue.innerHTML = state.led_hue_correction + '&deg;';
         }
      }

      // Auto-fetch entities if connected and we haven't yet
      if (state.connected && state.entities.length === 0) {
         requestEntities();
      }
   }

   function handleTestConnectionResponse(payload) {
      setBtnLoading(elements.testConnectionBtn, false);

      if (payload.success) {
         state.connected = true;
         state.version = payload.version || '';
         updateStatusUI();
         requestEntities();
      } else {
         state.connected = false;
         updateStatusUI();
         DawnDialog.alert('Connection failed: ' + (payload.error || 'Unknown error'), {
            title: 'Home Assistant',
         });
      }
   }

   function handleEntitiesResponse(payload) {
      setBtnLoading(elements.refreshEntitiesBtn, false);

      if (payload.success) {
         state.entities = payload.entities || [];
         state.entity_count = payload.count || state.entities.length;
         state.cached_at = payload.cached_at || 0;
         updateStatusUI();
         renderEntities(state.entities);
      }
   }

   // =============================================================================
   // Actions
   // =============================================================================

   function requestStatus() {
      if (typeof DawnWS !== 'undefined') {
         DawnWS.send({ type: 'ha_status' });
      }
   }

   function testConnection() {
      setBtnLoading(elements.testConnectionBtn, true);
      if (typeof DawnWS !== 'undefined') {
         DawnWS.send({ type: 'ha_test_connection' });
      }
   }

   function requestEntities() {
      if (typeof DawnWS !== 'undefined') {
         DawnWS.send({ type: 'ha_list_entities' });
      }
   }

   function refreshEntities() {
      setBtnLoading(elements.refreshEntitiesBtn, true);
      if (typeof DawnWS !== 'undefined') {
         DawnWS.send({ type: 'ha_refresh_entities' });
      }
   }

   function saveUrl() {
      if (!elements.urlInput || typeof DawnWS === 'undefined') return;
      var url = elements.urlInput.value.trim();
      DawnWS.send({
         type: 'set_config',
         payload: { home_assistant: { url: url } },
      });
      setBtnLoading(elements.saveUrlBtn, true);
      // Status response from server will confirm the update
      setTimeout(function () {
         setBtnLoading(elements.saveUrlBtn, false);
      }, 2000);
   }

   var hueSaveTimer = null;

   function onHueCorrectionChange() {
      if (!elements.hueCorrection) return;
      var val = parseInt(elements.hueCorrection.value, 10);
      if (elements.hueCorrectionValue) {
         elements.hueCorrectionValue.innerHTML = val + '&deg;';
      }
      // Debounce save — only send after user stops sliding
      if (hueSaveTimer) clearTimeout(hueSaveTimer);
      hueSaveTimer = setTimeout(function () {
         if (typeof DawnWS !== 'undefined') {
            DawnWS.send({
               type: 'set_config',
               payload: { home_assistant: { led_hue_correction: val } },
            });
         }
      }, 500);
   }

   // =============================================================================
   // Initialization
   // =============================================================================

   function setElements(els) {
      elements = els || {};

      // Bind filter input with debounce
      if (elements.filterInput) {
         elements.filterInput.addEventListener('input', function () {
            var value = this.value;
            if (filterTimer) clearTimeout(filterTimer);
            filterTimer = setTimeout(function () {
               filterEntities(value);
            }, FILTER_DEBOUNCE_MS);
         });
      }

      // Bind buttons
      if (elements.testConnectionBtn) {
         elements.testConnectionBtn.addEventListener('click', testConnection);
      }
      if (elements.refreshEntitiesBtn) {
         elements.refreshEntitiesBtn.addEventListener('click', refreshEntities);
      }
      if (elements.saveUrlBtn) {
         elements.saveUrlBtn.addEventListener('click', saveUrl);
      }
      // Allow Enter key in URL input to save
      if (elements.urlInput) {
         elements.urlInput.addEventListener('keydown', function (e) {
            if (e.key === 'Enter') saveUrl();
         });
      }
      // Bind hue correction slider
      if (elements.hueCorrection) {
         elements.hueCorrection.addEventListener('input', onHueCorrectionChange);
      }
   }

   // =============================================================================
   // Public API
   // =============================================================================

   global.DawnHomeAssistant = {
      requestStatus: requestStatus,
      handleStatusResponse: handleStatusResponse,
      handleTestConnectionResponse: handleTestConnectionResponse,
      handleEntitiesResponse: handleEntitiesResponse,
      testConnection: testConnection,
      requestEntities: requestEntities,
      refreshEntities: refreshEntities,
      setElements: setElements,
   };
})(window);
