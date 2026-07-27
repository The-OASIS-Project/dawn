/**
 * DAWN Settings Module
 * Orchestrator for settings panel, configuration management, modals, and auth visibility
 *
 * Sub-modules:
 * - settings/utils.js - Pure utility functions
 * - settings/modals.js - Confirm and input modal dialogs
 * - settings/audio.js - Audio device management
 * - settings/config.js - Config save/load/persistence
 * - settings/llm.js - LLM runtime and conversation controls
 * - settings/schema.js - Settings schema and rendering
 */
(function () {
   'use strict';

   // Import sub-modules
   const Utils = window.DawnSettingsUtils;
   const Modals = window.DawnSettingsModals;
   const Audio = window.DawnSettingsAudio;
   const Config = window.DawnSettingsConfig;
   const Llm = window.DawnSettingsLlm;
   const Schema = window.DawnSettingsSchema;
   const Search = window.DawnSettingsSearch;

   // Settings DOM elements (populated after init)
   const settingsElements = {};

   // Callbacks for external dependencies
   let callbacks = {
      getAuthState: null,
      setAuthState: null,
      updateHistoryButtonVisibility: null,
      updateMemoryButtonVisibility: null,
      updateSchedulerButtonVisibility: null,
      restoreHistorySidebarState: null,
   };

   /* =============================================================================
    * DOM Element Initialization
    * ============================================================================= */

   function initElements() {
      settingsElements.panel = document.getElementById('settings-panel');
      settingsElements.overlay = document.getElementById('settings-overlay');
      settingsElements.closeBtn = document.getElementById('settings-close');
      settingsElements.openBtn = document.getElementById('settings-btn');
      settingsElements.configPath = document.getElementById('config-path-display');
      settingsElements.secretsPath = document.getElementById('secrets-path-display');
      settingsElements.sectionsContainer = document.getElementById('settings-sections');
      settingsElements.saveConfigBtn = document.getElementById('save-config-btn');
      settingsElements.resetBtn = document.getElementById('reset-config-btn');
      settingsElements.restartNotice = document.getElementById('restart-notice');

      // Generate secrets fields from data-driven schema
      const secretsContent = document.getElementById('secrets-content');
      if (secretsContent) {
         Config.renderSecretsFields(secretsContent);
      }
   }

   /* =============================================================================
    * Panel Open/Close
    * ============================================================================= */

   // DawnEscStack token while the panel is open (staged: first Escape clears the
   // search box, second closes the panel).
   let settingsEscToken = null;

   function open() {
      if (!settingsElements.panel) return;

      settingsElements.panel.classList.remove('hidden');
      settingsElements.overlay.classList.remove('hidden');

      if (settingsEscToken === null) {
         settingsEscToken = DawnEscStack.register(() => {
            const searchInput = document.getElementById('settings-search-input');
            if (searchInput && searchInput.value.length > 0) {
               if (Search) Search.clearSearch();
               return true; // first Escape just clears the search
            }
            close();
            return true;
         });
      }

      // Restore advanced toggle state from localStorage
      const showAdvanced = DawnStore.getBool(DawnStore.KEYS.SETTINGS_SHOW_ADVANCED, false);
      const advancedToggle = document.getElementById('settings-advanced-toggle');
      const container = document.getElementById('settings-sections');
      if (advancedToggle) {
         advancedToggle.classList.toggle('active', showAdvanced);
         advancedToggle.title = showAdvanced ? 'Hide advanced settings' : 'Show advanced settings';
      }
      if (container) {
         container.classList.toggle('show-advanced', showAdvanced);
      }

      // Request config, models, and interfaces from server
      Config.requestConfig();
      Config.requestModelsList();
      Config.requestInterfacesList();
      // Request users list for admin settings (default voice user dropdown)
      if (typeof DawnState !== 'undefined' && DawnState.authState && DawnState.authState.isAdmin) {
         Config.requestUsersList();
      }
      if (typeof DawnHomeAssistant !== 'undefined') {
         DawnHomeAssistant.requestStatus();
      }
      if (typeof DawnPhoneAudio !== 'undefined') {
         DawnPhoneAudio.requestConfig();
      }
   }

   async function close() {
      if (!settingsElements.panel) return;

      // Check for unsaved changes
      const configCount = Config.getChangedFields().size;
      const toolsUnsaved =
         typeof DawnTools !== 'undefined' && DawnTools.hasUnsavedChanges
            ? DawnTools.hasUnsavedChanges()
            : false;
      const totalUnsaved = configCount + (toolsUnsaved ? 1 : 0);

      if (totalUnsaved > 0) {
         if (
            await DawnDialog.confirm(
               'You have ' + totalUnsaved + ' unsaved change(s). Close without saving?',
               { title: 'Unsaved Changes', okText: 'Discard', cancelText: 'Go Back' }
            )
         ) {
            // Discard: clear tracking and close
            Config.clearChangedFields();
            if (typeof DawnTools !== 'undefined' && DawnTools.clearUnsavedChanges) {
               DawnTools.clearUnsavedChanges();
            }
            clearUnsavedIndicators();
            doClose();
         }
         return;
      }

      doClose();
   }

   function doClose() {
      if (!settingsElements.panel) return;
      // Closing the panel discards an unsaved My Settings theme preview.
      if (typeof DawnMySettings !== 'undefined' && DawnMySettings.revertUnsavedTheme) {
         DawnMySettings.revertUnsavedTheme();
      }
      settingsElements.panel.classList.add('hidden');
      settingsElements.overlay.classList.add('hidden');
      if (settingsEscToken !== null) {
         DawnEscStack.unregister(settingsEscToken);
         settingsEscToken = null;
      }
      if (Search) Search.clearSearch();
   }

   /**
    * Open settings panel and expand a specific section
    * @param {string} sectionId - The ID of the section to expand (e.g., 'my-settings-section')
    */
   function openSection(sectionId) {
      // Open the settings panel first
      open();

      // Find and expand the target section
      const targetSection = document.getElementById(sectionId);
      if (targetSection) {
         // Expand it (remove collapsed)
         targetSection.classList.remove('collapsed');

         // Scroll section into view after a brief delay for panel to open
         setTimeout(function () {
            targetSection.scrollIntoView({ behavior: 'smooth', block: 'start' });
         }, 100);

         // Trigger section-specific data loading
         triggerSectionLoad(sectionId);
      }
   }

   /**
    * Trigger data loading for sections that need it when opened programmatically
    * @param {string} sectionId - The ID of the section being opened
    */
   function triggerSectionLoad(sectionId) {
      // Allow time for WebSocket to be ready
      setTimeout(function () {
         switch (sectionId) {
            case 'my-settings-section':
               if (typeof DawnMySettings !== 'undefined' && DawnMySettings.requestGet) {
                  DawnMySettings.requestGet();
               }
               break;
            case 'my-sessions-section':
               if (typeof DawnMySessions !== 'undefined' && DawnMySessions.requestList) {
                  DawnMySessions.requestList();
               }
               break;
            case 'user-management-section':
               if (typeof DawnUserManagement !== 'undefined' && DawnUserManagement.requestList) {
                  DawnUserManagement.requestList();
               }
               break;
         }
      }, 150);
   }

   /* =============================================================================
    * Auth Visibility
    * ============================================================================= */

   function updateAuthVisibility() {
      const authState = callbacks.getAuthState ? callbacks.getAuthState() : {};

      // Toggle auth classes on body - CSS handles visibility of .admin-only elements
      document.body.classList.toggle('user-is-admin', authState.isAdmin || false);
      document.body.classList.toggle('user-authenticated', authState.authenticated || false);

      // Update user badge in header
      updateUserBadge();

      // Update history button visibility
      if (callbacks.updateHistoryButtonVisibility) {
         callbacks.updateHistoryButtonVisibility();
      }

      // Update memory button visibility
      if (callbacks.updateMemoryButtonVisibility) {
         callbacks.updateMemoryButtonVisibility();
      }

      // Update scheduler button visibility
      if (callbacks.updateSchedulerButtonVisibility) {
         callbacks.updateSchedulerButtonVisibility();
      }

      // Update document library button visibility
      if (typeof DawnDocLibrary !== 'undefined') {
         DawnDocLibrary.updateVisibility();
      }

      // Restore history sidebar state on desktop (only when authenticated)
      if (callbacks.restoreHistorySidebarState) {
         callbacks.restoreHistorySidebarState();
      }
   }

   function updateUserBadge() {
      const badgeContainer = document.getElementById('user-badge-container');
      const nameEl = document.getElementById('user-badge-name');
      const roleEl = document.getElementById('user-badge-role');

      if (!badgeContainer || !nameEl || !roleEl) return;

      const authState = callbacks.getAuthState ? callbacks.getAuthState() : {};

      if (authState.authenticated && authState.username) {
         nameEl.textContent = authState.username;
         roleEl.textContent = authState.isAdmin ? 'Admin' : 'User';
         roleEl.className = 'user-badge-role ' + (authState.isAdmin ? 'admin' : 'user');
         badgeContainer.classList.remove('hidden');
      } else {
         badgeContainer.classList.add('hidden');
      }
   }

   /* =============================================================================
    * Setting Change Handler
    * ============================================================================= */

   function handleSettingChange(key, input) {
      Config.markFieldChanged(key);

      // Check if this field requires restart
      const restartRequiredFields = Config.getRestartRequiredFields();
      if (restartRequiredFields.includes(key)) {
         if (settingsElements.restartNotice) {
            settingsElements.restartNotice.classList.remove('hidden');
         }
      }

      // If this is a model_list textarea, update any dropdowns that depend on it
      if (input.dataset.type === 'model_list') {
         Schema.updateDependentModelSelects(key, input.value);
      }

      // Add unsaved indicators
      const settingItem = input.closest('.setting-item, .setting-item-row');
      if (settingItem) {
         settingItem.classList.add('is-changed');
      }
      const sectionHeader = input.closest('.settings-section')?.querySelector('.section-header');
      if (sectionHeader) {
         sectionHeader.classList.add('has-changes');
      }
      updateSaveButtonState();
   }

   /**
    * Update save button state based on pending changes
    */
   function updateSaveButtonState() {
      const btn = settingsElements.saveConfigBtn;
      if (!btn) return;

      const configCount = Config.getChangedFields().size;
      const toolsUnsaved =
         typeof DawnTools !== 'undefined' && DawnTools.hasUnsavedChanges
            ? DawnTools.hasUnsavedChanges()
            : false;
      const count = configCount + (toolsUnsaved ? 1 : 0);

      if (count > 0) {
         btn.classList.add('has-unsaved');
         btn.dataset.unsavedCount = count;
      } else {
         btn.classList.remove('has-unsaved');
         delete btn.dataset.unsavedCount;
      }
   }

   /**
    * Clear all unsaved change indicators
    */
   function clearUnsavedIndicators() {
      document.querySelectorAll('.is-changed').forEach((el) => el.classList.remove('is-changed'));
      document.querySelectorAll('.has-changes').forEach((el) => el.classList.remove('has-changes'));
      updateSaveButtonState();
   }

   /* =============================================================================
    * Restart Confirmation
    * ============================================================================= */

   async function showRestartConfirmation(changedRestartFields) {
      const fieldList = changedRestartFields.map((f) => '  • ' + f).join('\n');
      const message =
         'Configuration saved successfully!\n\n' +
         'The following changes require a restart to take effect:\n' +
         fieldList +
         '\n\n' +
         'Do you want to restart DAWN now?';

      if (
         await DawnDialog.confirm(message, {
            title: 'Restart Required',
            okText: 'Restart Now',
            cancelText: 'Later',
         })
      ) {
         requestRestart();
      }
   }

   function requestRestart() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         console.error('WebSocket not connected');
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Cannot restart: Not connected to server', 'error');
         }
         return;
      }

      DawnWS.send({ type: 'restart' });
   }

   function handleRestartResponse(payload) {
      if (payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(
               'DAWN is restarting. The page will attempt to reconnect automatically.',
               'info'
            );
         }
      } else {
         console.error('Restart failed:', payload.error);
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Failed to restart: ' + (payload.error || 'Unknown error'), 'error');
         }
      }
   }

   /* =============================================================================
    * System Prompt Display
    * ============================================================================= */

   function handleSystemPromptResponse(payload) {
      if (!payload.success) {
         console.warn('Failed to get system prompt:', payload.error);
         return;
      }

      const transcript =
         typeof DawnElements !== 'undefined'
            ? DawnElements.transcript
            : document.getElementById('transcript');

      // Build one collapsible debug entry (shared shape for the prompt and the
      // tool schema).  afterEl, when given, places this entry directly below it
      // so the prompt stays above the tools block.
      function renderDebugEntry(id, icon, title, stats, bodyText, afterEl) {
         const existing = document.getElementById(id);
         if (existing) {
            existing.remove();
         }

         const entry = document.createElement('div');
         entry.id = id;
         entry.className = 'transcript-entry debug system-prompt';
         entry.innerHTML = `
      <div class="system-prompt-header" role="button" tabindex="0" aria-expanded="false">
        <span class="system-prompt-icon">${icon}</span>
        <span class="system-prompt-title">${title}</span>
        <span class="system-prompt-stats">${stats}</span>
        <span class="system-prompt-toggle">&#x25BC;</span>
      </div>
      <div class="system-prompt-content">
        <pre>${DawnFormat.escapeHtml(bodyText)}</pre>
      </div>
    `;

         const header = entry.querySelector('.system-prompt-header');
         const toggle = function () {
            const expanded = entry.classList.toggle('expanded');
            header.setAttribute('aria-expanded', expanded ? 'true' : 'false');
         };
         header.addEventListener('click', toggle);
         header.addEventListener('keydown', function (e) {
            if (e.key === 'Enter' || e.key === ' ') {
               e.preventDefault();
               toggle();
            }
         });

         if (afterEl) {
            afterEl.after(entry);
         } else if (transcript) {
            const placeholder = transcript.querySelector('.transcript-placeholder');
            if (placeholder) {
               placeholder.after(entry);
            } else {
               transcript.prepend(entry);
            }
         }

         if (typeof DawnState !== 'undefined' && !DawnState.getDebugMode()) {
            entry.style.display = 'none';
         }
         return entry;
      }

      const promptLength = payload.length || payload.prompt.length;
      const promptTokens = Math.round(promptLength / 4); // Rough estimate
      const promptEntry = renderDebugEntry(
         'system-prompt-entry',
         '&#x2699;',
         'System Prompt',
         `${promptLength.toLocaleString()} chars (~${promptTokens.toLocaleString()} tokens)`,
         payload.prompt
      );

      // Tools are a separate `tools` array in the API request (native tool
      // calling), not part of the prompt text — show them exactly as the LLM
      // receives them so descriptions can be inspected for truncation.
      if (payload.tools) {
         const toolsLen = payload.tools.length;
         const toolsTokens = Math.round(toolsLen / 4);
         renderDebugEntry(
            'system-tools-entry',
            '&#x1F527;',
            'Tools (as sent to the LLM)',
            `${Number(payload.tools_count) || 0} tools, ${toolsLen.toLocaleString()} chars (~${toolsTokens.toLocaleString()} tokens)`,
            payload.tools,
            promptEntry
         );
      }
   }

   /* =============================================================================
    * Event Listener Initialization
    * ============================================================================= */

   function initListeners() {
      // Open button
      if (settingsElements.openBtn) {
         settingsElements.openBtn.addEventListener('click', open);
      }

      // Close button
      if (settingsElements.closeBtn) {
         settingsElements.closeBtn.addEventListener('click', close);
      }

      // Overlay click to close
      if (settingsElements.overlay) {
         settingsElements.overlay.addEventListener('click', close);
      }

      // Save config button
      if (settingsElements.saveConfigBtn) {
         settingsElements.saveConfigBtn.addEventListener('click', Config.saveConfig);
      }

      // Reset button
      if (settingsElements.resetBtn) {
         settingsElements.resetBtn.addEventListener('click', async () => {
            if (
               await DawnDialog.confirm(
                  'Reset all settings to defaults?\n\nThis will reload the current configuration.',
                  { title: 'Reset Configuration', okText: 'Reset' }
               )
            ) {
               Config.requestConfig();
            }
         });
      }

      // Password toggle buttons (modals — secrets toggles are wired in renderSecretsFields)
      document.querySelectorAll('.password-toggle').forEach((btn) => {
         btn.addEventListener('click', () => {
            const targetId = btn.dataset.target;
            if (targetId) {
               Audio.toggleSecretVisibility(targetId);
            }
         });
      });

      // Section header toggle (toggle on parent .settings-section)
      document.querySelectorAll('.section-header').forEach((header) => {
         // Skip user-management-section - has its own handler
         if (header.closest('#user-management-section')) {
            return;
         }
         header.addEventListener('click', () => {
            const section = header.closest('.settings-section');
            if (section) {
               section.classList.toggle('collapsed');
            }
         });
      });

      // Ctrl+F / Cmd+F — focus search when the panel is open.  (Escape close is
      // handled via DawnEscStack — register-on-open in open() / unregister in
      // doClose() — so it stacks correctly under modals.)
      document.addEventListener('keydown', (e) => {
         if (!settingsElements.panel || settingsElements.panel.classList.contains('hidden')) return;

         // Don't handle if a modal dialog is open
         const openModal = document.querySelector('.modal:not(.hidden)');
         if (openModal) return;

         if ((e.ctrlKey || e.metaKey) && e.key === 'f') {
            const searchInput = document.getElementById('settings-search-input');
            if (searchInput) {
               e.preventDefault();
               searchInput.focus();
               searchInput.select();
            }
         }
      });

      // Advanced toggle button
      const advancedToggle = document.getElementById('settings-advanced-toggle');
      if (advancedToggle) {
         advancedToggle.addEventListener('click', () => {
            const container = document.getElementById('settings-sections');
            if (!container) return;

            const isActive = advancedToggle.classList.toggle('active');
            container.classList.toggle('show-advanced', isActive);
            advancedToggle.title = isActive ? 'Hide advanced settings' : 'Show advanced settings';
            DawnStore.setBool(DawnStore.KEYS.SETTINGS_SHOW_ADVANCED, isActive);

            // Rebuild search index so it reflects current visibility
            if (Search) Search.buildIndex();
         });
      }

      // LLM quick controls event listeners
      Llm.initLlmControls();

      // Home Assistant initialization
      if (typeof DawnHomeAssistant !== 'undefined') {
         DawnHomeAssistant.setElements({
            statusIndicator: document.getElementById('ha-status-dot'),
            statusText: document.getElementById('ha-status-text'),
            entityCount: document.getElementById('ha-entity-count'),
            lastUpdated: document.getElementById('ha-last-updated'),
            testConnectionBtn: document.getElementById('ha-test-connection-btn'),
            refreshEntitiesBtn: document.getElementById('ha-refresh-entities-btn'),
            entitySection: document.getElementById('ha-entity-section'),
            entityList: document.getElementById('ha-entity-list'),
            filterInput: document.getElementById('ha-filter-input'),
            urlInput: document.getElementById('ha-url-input'),
            saveUrlBtn: document.getElementById('ha-save-url-btn'),
            hueCorrection: document.getElementById('ha-hue-correction'),
            hueCorrectionValue: document.getElementById('ha-hue-correction-value'),
         });
      }

      // Phone call-audio panel initialization
      if (typeof DawnPhoneAudio !== 'undefined') {
         DawnPhoneAudio.setElements({
            section: document.getElementById('phone-audio-section'),
            controls: document.getElementById('phone-audio-controls'),
            status: document.getElementById('phone-audio-status'),
         });
      }
   }

   /* =============================================================================
    * Initialization
    * ============================================================================= */

   function init() {
      initElements();

      // Wire up sub-modules with dependencies
      Config.setElements(settingsElements);
      Config.setCallbacks({
         renderSettingsSections: Schema.renderSettingsSections,
         updateAudioBackendState: Audio.updateAudioBackendState,
         updateCloudModelLists: Llm.updateCloudModelLists,
         extractGlobalDefaults: Llm.extractGlobalDefaults,
         applyGlobalDefaultsToControls: Llm.applyGlobalDefaultsToControls,
         updateLlmControls: Llm.updateLlmControls,
         setAuthState: callbacks.setAuthState,
         updateAuthVisibility: updateAuthVisibility,
         showRestartConfirmation: showRestartConfirmation,
         clearUnsavedIndicators: clearUnsavedIndicators,
         buildSearchIndex: Search ? Search.buildIndex : null,
      });

      Audio.setHandleSettingChange(handleSettingChange);

      Schema.setDependencies({
         sectionsContainer: settingsElements.sectionsContainer,
         handleSettingChange: handleSettingChange,
         getCurrentConfig: Config.getCurrentConfig,
         getRestartRequiredFields: Config.getRestartRequiredFields,
         getDynamicOptions: Config.getDynamicOptions,
      });

      // Initialize modals
      Modals.initConfirmModal();
      Modals.initInputModal();

      // Initialize search
      if (Search) Search.init();

      // Initialize listeners
      initListeners();

      // Initialize memory extraction provider/model handlers
      Config.initMemoryExtractionHandlers();
   }

   /**
    * Set callbacks for external dependencies
    */
   function setCallbacks(cbs) {
      if (cbs.getAuthState) callbacks.getAuthState = cbs.getAuthState;
      if (cbs.setAuthState) callbacks.setAuthState = cbs.setAuthState;
      if (cbs.updateHistoryButtonVisibility)
         callbacks.updateHistoryButtonVisibility = cbs.updateHistoryButtonVisibility;
      if (cbs.updateMemoryButtonVisibility)
         callbacks.updateMemoryButtonVisibility = cbs.updateMemoryButtonVisibility;
      /* Was never stored, so updateAuthVisibility's call to it (above) could
       * never fire.  It went unnoticed because both panels behind it survived
       * by accident: #scheduler-btn has no `.hidden` CSS rule at all, so its
       * gate is inert and the button is simply always visible, and #coding-btn
       * is revealed a second way (setEnabled, on get_config_response).  The
       * jobs button is the first with a real `.hidden` rule AND no second
       * path — i.e. the first one actually gated by this callback. */
      if (cbs.updateSchedulerButtonVisibility)
         callbacks.updateSchedulerButtonVisibility = cbs.updateSchedulerButtonVisibility;
      if (cbs.restoreHistorySidebarState)
         callbacks.restoreHistorySidebarState = cbs.restoreHistorySidebarState;

      // Update Config module's callbacks with setAuthState
      Config.setCallbacks({
         setAuthState: cbs.setAuthState,
         updateAuthVisibility: updateAuthVisibility,
      });
   }

   /* =============================================================================
    * Export - Maintain backward compatibility with window.DawnSettings API
    * ============================================================================= */

   window.DawnSettings = {
      // Initialization
      init: init,
      setCallbacks: setCallbacks,

      // Panel control
      open: open,
      close: close,
      openSection: openSection,

      // Config access and requests (delegated to Config module)
      getConfig: Config.getCurrentConfig,
      requestConfig: Config.requestConfig,

      // Response handlers (delegated to appropriate modules)
      handleGetConfigResponse: Config.handleGetConfigResponse,
      handleSetConfigResponse: Config.handleSetConfigResponse,
      handleSetSecretsResponse: Config.handleSetSecretsResponse,
      handleModelsListResponse: Config.handleModelsListResponse,
      handleInterfacesListResponse: Config.handleInterfacesListResponse,
      handleUsersListResponse: Config.handleUsersListResponse,
      handleRestartResponse: handleRestartResponse,
      handleGetAudioDevicesResponse: Audio.handleGetAudioDevicesResponse,
      handleSystemPromptResponse: handleSystemPromptResponse,
      handleSetSessionLlmResponse: Llm.handleSetSessionLlmResponse,
      handleListLlmModelsResponse: Llm.handleListLlmModelsResponse,
      handleSetPrivateResponse: Llm.handleSetPrivateResponse,

      // LLM controls (delegated to Llm module)
      updateLlmControls: Llm.updateLlmControls,
      updateCloudModelLists: Llm.updateCloudModelLists,

      // Per-conversation LLM settings (delegated to Llm module)
      initConversationLlmControls: Llm.initConversationLlmControls,
      resetConversationLlmControls: Llm.resetConversationLlmControls,
      applyConversationLlmSettings: Llm.applyConversationLlmSettings,
      lockConversationLlmSettings: Llm.lockConversationLlmSettings,
      getConversationLlmSettings: Llm.getConversationLlmSettings,
      isConversationLlmLocked: Llm.isConversationLlmLocked,

      // Modal focus-trap (styled confirm/prompt/alert now go through DawnDialog).
      trapFocus: Modals.trapFocus,

      // Auth visibility
      updateAuthVisibility: updateAuthVisibility,

      // Unsaved indicators
      updateSaveButtonState: updateSaveButtonState,
      clearUnsavedIndicators: clearUnsavedIndicators,
   };
})();
