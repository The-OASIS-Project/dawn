/**
 * DAWN Settings - Configuration Management
 * Config save/load, persistence, and secrets management
 */
(function () {
   'use strict';

   // Secrets field definitions — drives HTML generation, save, and status updates
   const SECRETS_FIELDS = [
      {
         id: 'secret-openai',
         key: 'openai_api_key',
         label: 'OpenAI API Key',
         placeholder: 'Enter API key...',
         maxlength: 256,
      },
      {
         id: 'secret-claude',
         key: 'claude_api_key',
         label: 'Claude API Key',
         placeholder: 'Enter API key...',
         maxlength: 256,
      },
      {
         id: 'secret-gemini',
         key: 'gemini_api_key',
         label: 'Gemini API Key',
         placeholder: 'Enter API key...',
         maxlength: 256,
      },
      {
         id: 'secret-openrouter',
         key: 'openrouter_api_key',
         label: 'OpenRouter API Key',
         placeholder: 'sk-or-...',
         maxlength: 256,
      },
      {
         id: 'secret-tavily',
         key: 'tavily_api_key',
         label: 'Tavily API Key',
         placeholder: 'tvly-...',
         maxlength: 256,
      },
      {
         id: 'secret-mqtt-user',
         key: 'mqtt_username',
         label: 'MQTT Username',
         placeholder: 'Enter username...',
      },
      {
         id: 'secret-mqtt-pass',
         key: 'mqtt_password',
         label: 'MQTT Password',
         placeholder: 'Enter password...',
      },
      {
         id: 'secret-plex-token',
         key: 'plex_token',
         label: 'Plex Token',
         placeholder: 'Enter Plex token...',
      },
      {
         id: 'secret-ha-token',
         key: 'home_assistant_token',
         label: 'Home Assistant Token',
         placeholder: 'Enter Long-Lived Access Token...',
         maxlength: 256,
         itemId: 'secret-ha-token-item',
      },
      {
         id: 'secret-google-client-id',
         key: 'google_client_id',
         label: 'Google OAuth Client ID',
         placeholder: 'Enter Google client ID...',
         maxlength: 256,
      },
      {
         id: 'secret-google-client-secret',
         key: 'google_client_secret',
         label: 'Google OAuth Client Secret',
         placeholder: 'Enter Google client secret...',
         maxlength: 256,
      },
      {
         id: 'secret-google-redirect-url',
         key: 'google_redirect_url',
         label: 'Google OAuth Redirect URL',
         placeholder: 'https://jetson.example.com:3000/oauth/callback',
         maxlength: 256,
         inputType: 'text',
      },
      {
         id: 'secret-telegram-token',
         key: 'telegram_bot_token',
         label: 'Telegram Bot Token',
         placeholder: 'From @BotFather, e.g. 123456:ABC-DEF...',
         maxlength: 256,
      },
      {
         id: 'secret-discord-token',
         key: 'discord_bot_token',
         label: 'Discord Bot Token',
         placeholder: 'From the Discord Developer Portal...',
         maxlength: 256,
      },
      {
         id: 'secret-slack-app-token',
         key: 'slack_app_token',
         label: 'Slack App Token',
         placeholder: 'xapp-...',
         maxlength: 256,
      },
      {
         id: 'secret-slack-bot-token',
         key: 'slack_bot_token',
         label: 'Slack Bot Token',
         placeholder: 'xoxb-...',
         maxlength: 256,
      },
   ];

   // Configuration state
   let currentConfig = null;
   let currentSecrets = null;
   let restartRequiredFields = [];
   let changedFields = new Set();
   // True once a get_config response has rendered the panel.  Gates the
   // preserve-unsaved-edits guard in handleGetConfigResponse: a reconnect/re-open
   // refetch must not clobber in-progress edits (mirrors the My Settings guard).
   let configLoadedOnce = false;
   let dynamicOptions = {
      asr_models: [],
      tts_voices: [],
      bind_addresses: [],
      memory_extraction_models: [],
      users: [],
   };

   // DOM element references (injected by main settings module)
   let settingsElements = {};

   // Callbacks for external dependencies
   let callbacks = {
      renderSettingsSections: null,
      updateSecretsStatusUI: null,
      updateAudioBackendState: null,
      updateCloudModelLists: null,
      extractGlobalDefaults: null,
      applyGlobalDefaultsToControls: null,
      updateLlmControls: null,
      setAuthState: null,
      updateAuthVisibility: null,
      showRestartConfirmation: null,
   };

   /**
    * Set DOM element references
    * @param {Object} elements - Object containing DOM element references
    */
   function setElements(elements) {
      settingsElements = elements;
   }

   /**
    * Set callbacks for external dependencies
    * @param {Object} cbs - Object containing callback functions
    */
   function setCallbacks(cbs) {
      Object.assign(callbacks, cbs);
   }

   /**
    * Get current config
    * @returns {Object|null} Current config object
    */
   function getCurrentConfig() {
      return currentConfig;
   }

   /**
    * Get current secrets
    * @returns {Object|null} Current secrets object
    */
   function getCurrentSecrets() {
      return currentSecrets;
   }

   /**
    * Get restart required fields array
    * @returns {Array} Array of field keys that require restart
    */
   function getRestartRequiredFields() {
      return restartRequiredFields;
   }

   /**
    * Get changed fields set
    * @returns {Set} Set of changed field keys
    */
   function getChangedFields() {
      return changedFields;
   }

   /**
    * Clear changed fields tracking
    */
   function clearChangedFields() {
      changedFields.clear();
   }

   /**
    * Mark a field as changed
    * @param {string} key - Field key
    */
   function markFieldChanged(key) {
      changedFields.add(key);
   }

   /**
    * Get dynamic options for select fields
    * @returns {Object} Dynamic options object
    */
   function getDynamicOptions() {
      return dynamicOptions;
   }

   /**
    * Request config from server
    */
   function requestConfig() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         console.error('WebSocket not connected');
         return;
      }

      DawnWS.send({ type: 'get_config' });
   }

   /**
    * Request models list from server
    */
   function requestModelsList() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         return;
      }

      DawnWS.send({ type: 'list_models' });
   }

   /**
    * Request network interfaces list from server
    */
   function requestInterfacesList() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         return;
      }

      DawnWS.send({ type: 'list_interfaces' });
   }

   /**
    * Handle models list response from server
    * @param {Object} payload - Response payload
    */
   function handleModelsListResponse(payload) {
      if (payload.asr_models) {
         dynamicOptions.asr_models = payload.asr_models;
      }
      if (payload.tts_voices) {
         dynamicOptions.tts_voices = payload.tts_voices;
      }
      // Update any already-rendered dynamic selects
      updateDynamicSelects();
   }

   /**
    * Handle interfaces list response from server
    * @param {Object} payload - Response payload
    */
   function handleInterfacesListResponse(payload) {
      if (payload.addresses) {
         dynamicOptions.bind_addresses = payload.addresses;
      }

      // Update any already-rendered dynamic selects
      updateDynamicSelects();
   }

   /**
    * Request users list from server (admin only)
    */
   function requestUsersList() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         return;
      }

      DawnWS.send({ type: 'list_users' });
   }

   /**
    * Handle users list response from server
    * Converts to {value, label} format for dynamic select
    * @param {Object} payload - Response payload with users array
    */
   function handleUsersListResponse(payload) {
      if (payload.success && payload.users) {
         // Convert users to {value, label} format for dynamic select
         dynamicOptions.users = payload.users.map((user) => ({
            value: user.id,
            label: user.username + (user.is_admin ? ' (Admin)' : ''),
         }));
      }

      // Update any already-rendered dynamic selects
      updateDynamicSelects();
   }

   /**
    * Update dynamic select dropdowns with server-provided options
    * Supports both plain string options and {value, label} object options
    */
   function updateDynamicSelects() {
      document.querySelectorAll('select[data-dynamic-key]').forEach((select) => {
         const key = select.dataset.dynamicKey;
         const options = dynamicOptions[key] || [];
         const currentValue = select.value;

         // Clear existing options except the first (current value placeholder)
         while (select.options.length > 1) {
            select.remove(1);
         }

         // Check if current value exists in options and update first option's label
         let foundCurrent = false;
         options.forEach((opt) => {
            const optVal = typeof opt === 'object' ? String(opt.value) : opt;
            const optLabel = typeof opt === 'object' ? opt.label : opt;
            if (optVal === currentValue) {
               foundCurrent = true;
               select.options[0].textContent = optLabel;
            }
         });

         // Add options from server (skip current value to avoid duplicate)
         options.forEach((opt) => {
            const optVal = typeof opt === 'object' ? String(opt.value) : opt;
            const optLabel = typeof opt === 'object' ? opt.label : opt;
            if (optVal === currentValue) return; // Skip - already in first option
            const option = document.createElement('option');
            option.value = optVal;
            option.textContent = optLabel;
            select.appendChild(option);
         });

         // If current value isn't in options, mark it as "(current)"
         if (currentValue && !foundCurrent) {
            select.options[0].textContent = currentValue + ' (current)';
         }
      });
   }

   /**
    * Update memory extraction model dropdown based on selected provider
    * Uses cloud model lists from config for openai/claude providers
    */
   function updateMemoryExtractionModels() {
      const providerSelect = document.querySelector(
         'select[data-key="memory.extraction_provider"]'
      );
      const modelSelect = document.querySelector('select[data-key="memory.extraction_model"]');

      if (!providerSelect || !modelSelect) return;

      const provider = providerSelect.value?.toLowerCase() || 'local';
      const currentValue = modelSelect.value || '';

      // Get models based on provider
      let models = [];
      if (currentConfig?.llm?.cloud) {
         if (provider === 'openai') {
            models = currentConfig.llm.cloud.openai_models || [];
         } else if (provider === 'claude') {
            models = currentConfig.llm.cloud.claude_models || [];
         }
      }

      // For local provider, we can't easily get the model list (llama.cpp only has one loaded)
      // Show a helpful placeholder
      if (provider === 'local') {
         modelSelect.innerHTML = '';
         const opt = document.createElement('option');
         opt.value = currentValue || '';
         opt.textContent = currentValue || '(uses loaded llama-server model)';
         modelSelect.appendChild(opt);
         modelSelect.disabled = true;
         modelSelect.title = 'Local mode uses whatever model is loaded in llama-server';
         return;
      }

      // Update dynamic options for the model select
      dynamicOptions.memory_extraction_models = models;

      // Clear and rebuild options
      modelSelect.innerHTML = '';
      modelSelect.disabled = false;
      modelSelect.title = 'Select extraction model';

      // Add current value first if set
      if (currentValue) {
         const currentOpt = document.createElement('option');
         currentOpt.value = currentValue;
         currentOpt.textContent = models.includes(currentValue)
            ? currentValue
            : currentValue + ' (current)';
         currentOpt.selected = true;
         modelSelect.appendChild(currentOpt);
      }

      // Add available models (skip current value to avoid duplicate)
      models.forEach((model) => {
         if (model === currentValue) return;
         const opt = document.createElement('option');
         opt.value = model;
         opt.textContent = model;
         modelSelect.appendChild(opt);
      });

      // If no current value and we have models, select first one
      if (!currentValue && models.length > 0) {
         modelSelect.value = models[0];
      }
   }

   /**
    * Initialize memory extraction provider/model event handlers
    */
   function initMemoryExtractionHandlers() {
      // Use event delegation since elements may not exist yet
      document.addEventListener('change', (e) => {
         if (e.target.matches('select[data-key="memory.extraction_provider"]')) {
            updateMemoryExtractionModels();
         }
      });
   }

   /**
    * Handle get_config response from server
    * @param {Object} payload - Response payload
    */
   // Built-in default persona from backend (for display when config field is empty)
   let defaultPersona = '';

   function handleGetConfigResponse(payload) {
      // Preserve unsaved edits across a refetch (reconnect / panel re-open): if
      // the user has pending config changes and the panel has already rendered,
      // don't clobber their in-place values or clear the change tracking. First
      // load (nothing rendered yet) always populates.
      const preserveEdits = configLoadedOnce && changedFields.size > 0;

      currentConfig = payload.config;
      currentSecrets = payload.secrets;
      restartRequiredFields = payload.requires_restart || [];
      defaultPersona = payload.default_persona || '';
      if (!preserveEdits) changedFields.clear();

      // Update path displays
      if (settingsElements.configPath) {
         settingsElements.configPath.textContent = payload.config_path || 'Unknown';
      }
      if (settingsElements.secretsPath) {
         settingsElements.secretsPath.textContent = payload.secrets_path || 'Unknown';
      }

      // Update logo from AI name (capitalize for display)
      const logoEl = document.getElementById('logo');
      if (logoEl && currentConfig.general && currentConfig.general.ai_name) {
         logoEl.textContent = currentConfig.general.ai_name.toUpperCase();
      }

      // Render settings sections (full rebuild) or update values in-place.
      // Skipped entirely when preserving unsaved edits, so a refetch can't
      // overwrite what the user is currently typing.
      const Schema = window.DawnSettingsSchema;
      if (preserveEdits) {
         // Keep the current DOM (the user's in-progress edits) — no repopulation.
      } else if (Schema && !Schema.needsRebuild()) {
         // DOM already rendered — update values in-place (fast path)
         Schema.updateSettingsValues(currentConfig);
      } else {
         // First render or schema version changed — full rebuild
         if (callbacks.renderSettingsSections) {
            callbacks.renderSettingsSections();
         }
         // Build search index after sections are rendered
         if (callbacks.buildSearchIndex) {
            callbacks.buildSearchIndex();
         }
      }
      configLoadedOnce = true;

      // Update memory extraction model dropdown based on provider
      updateMemoryExtractionModels();

      // Update secrets status
      updateSecretsStatus(currentSecrets);

      // Hide restart notice initially
      if (settingsElements.restartNotice) {
         settingsElements.restartNotice.classList.add('hidden');
      }

      // Initialize audio backend state (grey out or request devices)
      const backendSelect = document.getElementById('setting-audio-backend');
      if (backendSelect && callbacks.updateAudioBackendState) {
         // Add backend change listener (only once)
         if (!backendSelect._dawnListenerAttached) {
            backendSelect.addEventListener('change', () => {
               callbacks.updateAudioBackendState(backendSelect.value);
            });
            backendSelect._dawnListenerAttached = true;
         }

         // Initialize state based on current value
         callbacks.updateAudioBackendState(backendSelect.value);
      }

      // Extract cloud model lists from config for quick controls dropdown
      if (callbacks.updateCloudModelLists) {
         callbacks.updateCloudModelLists(currentConfig);
      }

      // Extract global defaults from config for new conversation reset
      if (callbacks.extractGlobalDefaults) {
         callbacks.extractGlobalDefaults(currentConfig);
      }

      // Apply defaults to conversation controls (reasoning, tools dropdowns)
      if (callbacks.applyGlobalDefaultsToControls) {
         callbacks.applyGlobalDefaultsToControls();
      }

      // Update LLM runtime controls
      if (payload.llm_runtime && callbacks.updateLlmControls) {
         callbacks.updateLlmControls(payload.llm_runtime);
      }

      // Update auth state and UI visibility
      if (callbacks.setAuthState) {
         callbacks.setAuthState({
            authenticated: payload.authenticated || false,
            isAdmin: payload.is_admin || false,
            username: payload.username || '',
         });
      }
      if (callbacks.updateAuthVisibility) {
         callbacks.updateAuthVisibility();
      }
   }

   /**
    * Update secrets status indicators in the UI
    * @param {Object} secrets - Secrets object with boolean indicators
    */
   function updateSecretsStatus(secrets) {
      if (!secrets) return;

      SECRETS_FIELDS.forEach((field) => {
         const statusId = 'status-' + field.id.replace('secret-', '');
         const el = document.getElementById(statusId);
         if (!el) return;
         const isSet = !!secrets[field.key];
         el.textContent = isSet ? 'Set' : 'Not set';
         el.className = `secret-status ${isSet ? 'is-set' : 'not-set'}`;
      });
   }

   /**
    * Collect all config values from the settings form
    * @returns {Object} Config object with all values
    */
   function collectConfigValues() {
      const config = {};
      const container = settingsElements.sectionsContainer;
      if (!container) return config;

      const inputs = container.querySelectorAll('[data-key]');

      inputs.forEach((input) => {
         const key = input.dataset.key;
         const parts = key.split('.');

         let value;
         if (input.type === 'checkbox') {
            value = input.checked;
         } else if (input.type === 'number' || input.type === 'range') {
            const multiplier = parseFloat(input.dataset.multiplier) || 1;
            value = input.value !== '' ? parseFloat(input.value) * multiplier : null;
         } else if (input.dataset.type === 'model_list' || input.dataset.type === 'string_list') {
            // Convert newline-separated text to array, filtering empty lines
            value = input.value
               .split('\n')
               .map((line) => line.trim())
               .filter((line) => line.length > 0);
         } else if (input.dataset.type === 'model_default_select') {
            // Parse as integer index
            value = parseInt(input.value, 10) || 0;
         } else {
            // Trim trailing whitespace from text/textarea values to prevent
            // accumulating blank lines in multi-line TOML strings (e.g., persona)
            value = input.value.replace(/\s+$/, '');
         }

         // Build nested structure
         let obj = config;
         for (let i = 0; i < parts.length - 1; i++) {
            if (!obj[parts[i]]) obj[parts[i]] = {};
            obj = obj[parts[i]];
         }
         obj[parts[parts.length - 1]] = value;
      });

      // AI name should always be lowercase (for wake word detection)
      if (config.general && config.general.ai_name) {
         config.general.ai_name = config.general.ai_name.toLowerCase();
      }

      return config;
   }

   /**
    * Save config to server
    */
   // Track pending saves for orchestrated save flow
   let pendingSaves = 0;
   let saveHadError = false;

   function saveConfig() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         console.error('WebSocket not connected');
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Cannot save: Not connected to server', 'error');
         }
         return;
      }

      // Unified panel Save: commit every pending change on the Settings panel —
      // global config, tools, and the per-user My Settings form — under ONE
      // multi-save counter and one toast.  Each part is sent (and counted) only
      // when actually dirty, so an unchanged section is never rewritten.
      //
      // changedFields is populated solely from edits in the admin-only config
      // sections, so a non-admin's set is always empty — this inherently skips
      // (and does not count) set_config for non-admins, which is required: the
      // server answers a non-admin set_config with a generic error, not a
      // set_config_response, so counting it would hang the button forever.
      const configNeedSave = changedFields.size > 0;
      const toolsNeedSave =
         typeof DawnTools !== 'undefined' &&
         DawnTools.hasUnsavedChanges &&
         DawnTools.hasUnsavedChanges();
      const myNeedSave =
         typeof DawnMySettings !== 'undefined' &&
         DawnMySettings.hasUnsavedChanges &&
         DawnMySettings.hasUnsavedChanges();

      if (!configNeedSave && !toolsNeedSave && !myNeedSave) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('No changes to save', 'info');
         }
         return;
      }

      // Show loading state
      const btn = settingsElements.saveConfigBtn;
      if (btn) {
         btn.disabled = true;
         btn.dataset.originalText = btn.textContent;
         btn.textContent = 'Saving...';
      }

      pendingSaves = (configNeedSave ? 1 : 0) + (toolsNeedSave ? 1 : 0) + (myNeedSave ? 1 : 0);
      saveHadError = false;

      // Global config (decremented in handleSetConfigResponse)
      if (configNeedSave) {
         DawnWS.send({
            type: 'set_config',
            payload: collectConfigValues(),
         });
      }

      // Tools config
      if (toolsNeedSave) {
         DawnTools.onSaveComplete(function (success) {
            if (!success) saveHadError = true;
            pendingSaves--;
            checkAllSavesComplete();
         });
         DawnTools.saveToolsConfig();
      }

      // Per-user My Settings
      if (myNeedSave) {
         DawnMySettings.onSaveComplete(function (success) {
            if (!success) saveHadError = true;
            pendingSaves--;
            checkAllSavesComplete();
         });
         DawnMySettings.save();
      }
   }

   /**
    * Render secrets fields into the secrets section content container.
    * Replaces 300+ lines of repetitive HTML with data-driven generation.
    * @param {HTMLElement} container - The .section-content element inside #secrets-section
    */
   function renderSecretsFields(container) {
      if (!container) return;

      const eyeSvg =
         '<svg class="eye-icon" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">' +
         '<path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/>' +
         '<circle cx="12" cy="12" r="3"/></svg>';

      SECRETS_FIELDS.forEach((field) => {
         const item = document.createElement('div');
         item.className = 'setting-item';
         if (field.itemId) item.id = field.itemId;

         const type = field.inputType || 'password';
         const maxAttr = field.maxlength ? ` maxlength="${field.maxlength}"` : '';
         const toggleBtn =
            type === 'password'
               ? `<button class="secret-toggle" data-target="${field.id}" title="Show/hide" aria-label="Toggle ${field.label} visibility">${eyeSvg}</button>`
               : '';

         item.innerHTML =
            `<label for="${field.id}">${field.label}</label>` +
            '<div class="secret-input-wrapper">' +
            `<input type="${type}" id="${field.id}" class="secret-input" placeholder="${field.placeholder}"${maxAttr}/>` +
            toggleBtn +
            `<span class="secret-status" id="status-${field.id.replace('secret-', '')}" aria-live="polite"></span>` +
            '</div>';

         container.appendChild(item);

         // Wire up toggle button
         const btn = item.querySelector('.secret-toggle');
         if (btn) {
            btn.addEventListener('click', () => {
               const input = document.getElementById(field.id);
               if (input) {
                  input.type = input.type === 'password' ? 'text' : 'password';
               }
            });
         }
      });

      // Add save button
      const saveBtn = document.createElement('button');
      saveBtn.id = 'save-secrets-btn';
      saveBtn.className = 'save-btn';
      saveBtn.textContent = 'Save Secrets';
      saveBtn.addEventListener('click', saveSecrets);
      container.appendChild(saveBtn);
   }

   /**
    * Save secrets to server
    */
   function saveSecrets() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         console.error('WebSocket not connected');
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Cannot save: Not connected to server', 'error');
         }
         return;
      }

      const secrets = {};

      // Only send non-empty values (don't overwrite with empty)
      SECRETS_FIELDS.forEach((field) => {
         const input = document.getElementById(field.id);
         if (input && input.value) {
            secrets[field.key] = input.value;
         }
      });

      if (Object.keys(secrets).length === 0) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('No secrets entered to save', 'warning');
         }
         return;
      }

      DawnWS.send({
         type: 'set_secrets',
         payload: secrets,
      });

      // Clear inputs after sending
      SECRETS_FIELDS.forEach((field) => {
         const input = document.getElementById(field.id);
         if (input) input.value = '';
      });
   }

   /**
    * Handle set_config response from server
    * @param {Object} payload - Response payload
    */
   function handleSetConfigResponse(payload) {
      if (payload.success) {
         // Store restart fields check before clearing
         configSaveRestartFields = getChangedRestartRequiredFields();
      } else {
         saveHadError = true;
         console.error('Failed to save config:', payload.error);
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(
               'Failed to save configuration: ' + (payload.error || 'Unknown error'),
               'error'
            );
         }
      }

      pendingSaves--;
      checkAllSavesComplete();
   }

   // Temporarily stash restart fields between response and completion check
   let configSaveRestartFields = [];

   /**
    * Called when all pending saves (config + tools) have completed
    */
   function checkAllSavesComplete() {
      if (pendingSaves > 0) return;

      // Restore button state
      const btn = settingsElements.saveConfigBtn;
      if (btn) {
         btn.disabled = false;
         btn.textContent = btn.dataset.originalText || 'Save Settings';
      }

      if (!saveHadError) {
         // Check if any changed fields require restart
         if (configSaveRestartFields.length > 0 && callbacks.showRestartConfirmation) {
            callbacks.showRestartConfirmation(configSaveRestartFields);
         } else {
            if (typeof DawnToast !== 'undefined') {
               DawnToast.show('Settings saved successfully!', 'success');
            }
         }

         // Show brief success flash on save button
         if (btn) {
            btn.classList.add('save-success');
            setTimeout(() => btn.classList.remove('save-success'), 1500);
         }

         // Clear changed fields tracking
         changedFields.clear();

         // Clear unsaved indicators
         if (callbacks.clearUnsavedIndicators) {
            callbacks.clearUnsavedIndicators();
         }

         // Refresh config to update globalDefaults and other cached values
         requestConfig();
      } else if (typeof DawnToast !== 'undefined') {
         // At least one part failed.  Each part stays silent under orchestration
         // so the success case shows a single toast — without this, a failed
         // My-Settings- or tools-only save (and a partial failure) would show
         // nothing at all, recreating the "looked saved, wasn't" trap.
         DawnToast.show('Some settings could not be saved. Please try again.', 'error');
      }

      configSaveRestartFields = [];
   }

   /**
    * Handle set_secrets response from server
    * @param {Object} payload - Response payload
    */
   function handleSetSecretsResponse(payload) {
      if (payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Secrets saved successfully!', 'success');
         }

         // Update status indicators
         if (payload.secrets) {
            updateSecretsStatus(payload.secrets);
         }

         // Refresh config to get updated secrets_path
         requestConfig();
      } else {
         console.error('Failed to save secrets:', payload.error);
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(
               'Failed to save secrets: ' + (payload.error || 'Unknown error'),
               'error'
            );
         }
      }
   }

   /**
    * Get list of changed fields that require restart
    * @returns {Array} Array of field keys requiring restart
    */
   function getChangedRestartRequiredFields() {
      const restartFields = [];
      for (const field of changedFields) {
         if (restartRequiredFields.includes(field)) {
            restartFields.push(field);
         }
      }
      return restartFields;
   }

   // Export for settings module
   window.DawnSettingsConfig = {
      setElements,
      setCallbacks,
      getCurrentConfig,
      getCurrentSecrets,
      getRestartRequiredFields,
      getChangedFields,
      clearChangedFields,
      markFieldChanged,
      getDynamicOptions,
      requestConfig,
      requestModelsList,
      requestInterfacesList,
      requestUsersList,
      handleModelsListResponse,
      handleInterfacesListResponse,
      handleUsersListResponse,
      handleGetConfigResponse,
      updateSecretsStatus,
      collectConfigValues,
      saveConfig,
      saveSecrets,
      handleSetConfigResponse,
      handleSetSecretsResponse,
      getChangedRestartRequiredFields,
      updateDynamicSelects,
      initMemoryExtractionHandlers,
      updateMemoryExtractionModels,
      renderSecretsFields,
      getDefaultPersona: function () {
         return defaultPersona;
      },
   };
})();
