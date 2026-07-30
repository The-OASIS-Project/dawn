/**
 * DAWN Settings - LLM Controls
 * LLM runtime controls, conversation settings, and model management
 */
(function () {
   'use strict';

   // LLM runtime state (updated from server on connect)
   let llmRuntimeState = {
      type: 'local',
      provider: '',
      model: '',
      openai_available: false,
      claude_available: false,
      gemini_available: false,
   };

   /**
    * Normalize a thinking_mode value from any source (server config, restored
    * conversation, legacy storage) to the two values the dropdown supports.
    * Legacy "auto" was identical to "enabled" in behavior — fold it in so old
    * conversations still load cleanly without an "auto" option in the markup.
    */
   function normalizeThinkingMode(mode) {
      if (mode === 'disabled') return 'disabled';
      if (mode === 'auto') return 'enabled';
      if (mode === 'enabled') return 'enabled';
      return 'enabled';
   }

   /**
    * Return the set of reasoning_effort values the given model accepts.
    * Mirrors the C-side llm_openai_clamp_effort_for_model() rules.
    *
    *   gpt-5 base/-mini/-nano:  low | medium | high                 (no none, no xhigh)
    *   gpt-5.1:                 none | low | medium | high          (no xhigh)
    *   gpt-5.2 / gpt-5.4*:      none | low | medium | high | xhigh  (full set)
    *   o1 / o3 series:          low | medium | high
    *   Gemini 2.5+/3.x:         low | medium | high
    *   Claude (any) / local:    low | medium | high  (mapped to budgets server-side)
    *
    * Server-side clamping handles wrong values gracefully, but trimming the
    * dropdown prevents the user from picking something that gets silently
    * downgraded.
    */
   function validEffortsForModel(modelName) {
      if (!modelName) return ['low', 'medium', 'high']; // safe baseline
      // gpt-5 family detection — match C llm_openai_is_gpt5_base_family
      if (modelName.startsWith('gpt-5')) {
         const next = modelName.charAt(5);
         if (next === '' || next === '-') {
            // gpt-5 / gpt-5-mini / gpt-5-nano / gpt-5-2025-08-07
            return ['low', 'medium', 'high'];
         }
         if (next === '.') {
            const minor = modelName.charAt(6);
            // gpt-5.1: none added; gpt-5.2+ adds xhigh too
            if (minor === '1') {
               return ['none', 'low', 'medium', 'high'];
            }
            if (minor >= '2' && minor <= '9') {
               return ['none', 'low', 'medium', 'high', 'xhigh'];
            }
         }
      }
      // Everything else: o-series, Claude, Gemini, local — base set
      return ['low', 'medium', 'high'];
   }

   /**
    * Sync the per-conversation Effort dropdown to whatever the current model accepts.
    * Hides invalid options and snaps the selected value to the closest valid one
    * (xhigh→high, none→low) when the user switches to a less-capable model.
    */
   function syncEffortDropdownToModel(modelName, skipPersist) {
      const depthSelect = document.getElementById('reasoning-effort-select');
      if (!depthSelect) return;
      const valid = new Set(validEffortsForModel(modelName));

      let selected = depthSelect.value;
      Array.from(depthSelect.options).forEach((opt) => {
         opt.hidden = !valid.has(opt.value);
         opt.disabled = !valid.has(opt.value);
      });

      // Snap to a valid value if the current pick is no longer allowed.
      if (!valid.has(selected)) {
         const fallback = selected === 'xhigh' ? 'high' : selected === 'none' ? 'low' : 'medium';
         depthSelect.value = fallback;
         conversationLlmState.reasoning_effort = fallback;
         if (!skipPersist) {
            setSessionLlm({ reasoning_effort: fallback });
         }
      }
   }

   // Per-conversation LLM settings state
   let conversationLlmState = {
      tools_mode: 'native',
      thinking_mode: 'enabled',
      reasoning_effort: 'medium',
      locked: false,
      is_private: false,
      conversation_id: null, // Current conversation ID for privacy toggle
   };

   // Global defaults from config (populated on config load)
   let globalDefaults = {
      type: 'cloud',
      provider: '',
      use_openrouter: false,
      openai_model: '',
      claude_model: '',
      gemini_model: '',
      openrouter_model: '',
      tools_mode: 'native',
      thinking_mode: 'disabled', // disabled/enabled
      reasoning_effort: 'medium', // low/medium/high - controls token budget
   };

   // Cached model lists from config and local LLM (includes default indices)
   let cloudModelLists = {
      openai: [],
      claude: [],
      gemini: [],
      openrouter: [],
      openaiDefaultIdx: 0,
      claudeDefaultIdx: 0,
      geminiDefaultIdx: 0,
      openrouterDefaultIdx: 0,
   };
   let localModelList = [];
   let localProviderType = 'unknown'; // 'ollama', 'llama.cpp', 'Generic', 'Unknown'

   // Local provider detection timeout
   let detectLocalProviderTimeout = null;
   const DETECT_TIMEOUT_MS = 10000; // 10 seconds

   /**
    * Set or clear an inline hint for a disabled control (touch-friendly)
    * @param {string} hintId - ID of the hint element
    * @param {string|null} text - Hint text, or null to clear
    */
   function setControlHint(hintId, text) {
      const el = document.getElementById(hintId);
      if (!el) return;
      if (text) {
         el.textContent = text;
         el.classList.remove('hidden');
      } else {
         el.textContent = '';
         el.classList.add('hidden');
      }
   }

   /**
    * Get current LLM runtime state
    * @returns {Object} Runtime state object
    */
   function getLlmRuntimeState() {
      return llmRuntimeState;
   }

   /**
    * Get conversation LLM state
    * @returns {Object} Conversation LLM state
    */
   function getConversationLlmState() {
      return conversationLlmState;
   }

   /**
    * Get global defaults
    * @returns {Object} Global defaults object
    */
   function getGlobalDefaults() {
      return globalDefaults;
   }

   /**
    * Initialize LLM quick controls event listeners
    */
   function initLlmControls() {
      const typeSelect = document.getElementById('llm-type-select');
      const providerSelect = document.getElementById('llm-provider-select');
      const modelSelect = document.getElementById('llm-model-select');

      if (typeSelect) {
         typeSelect.addEventListener('change', () => {
            const newType = typeSelect.value;
            setSessionLlm({ type: newType });

            // Request local models when switching to local mode
            if (newType === 'local') {
               requestLocalModels();
            } else if (globalDefaults.use_openrouter) {
               // Gateway on: reflect a read-only OpenRouter provider + slug model list
               // instead of a direct provider picker the gateway would override.
               applyGatewayCloudUI(true);
            } else {
               // Switching to cloud (direct providers): restore provider dropdown and update model
               if (providerSelect) {
                  // Rebuild cloud provider options
                  providerSelect.innerHTML = '';
                  providerSelect.disabled = false;
                  setControlHint('provider-hint', null);
                  if (cloudModelLists.openai.length > 0) {
                     const opt = document.createElement('option');
                     opt.value = 'openai';
                     opt.textContent = 'OpenAI';
                     providerSelect.appendChild(opt);
                  }
                  if (cloudModelLists.claude.length > 0) {
                     const opt = document.createElement('option');
                     opt.value = 'claude';
                     opt.textContent = 'Claude';
                     providerSelect.appendChild(opt);
                  }
                  if (cloudModelLists.gemini.length > 0) {
                     const opt = document.createElement('option');
                     opt.value = 'gemini';
                     opt.textContent = 'Gemini';
                     providerSelect.appendChild(opt);
                  }
                  // Select default provider: prefer global default, fall back to first available
                  const defaultProvider = globalDefaults.provider;
                  if (providerSelect.querySelector(`option[value="${defaultProvider}"]`)) {
                     providerSelect.value = defaultProvider;
                  } else if (providerSelect.options.length > 0) {
                     providerSelect.value = providerSelect.options[0].value;
                  }
                  // Also send provider to session (only if a provider is actually selected)
                  if (providerSelect.value) {
                     setSessionLlm({ provider: providerSelect.value });
                  }
               }
               // Populate cloud models and send model to session
               updateModelDropdownForCloud(true);
            }
         });
      }

      if (providerSelect) {
         providerSelect.addEventListener('change', () => {
            setSessionLlm({ provider: providerSelect.value });
            // Update model dropdown for new provider and send model to session
            updateModelDropdownForCloud(true);
         });
      }

      if (modelSelect) {
         modelSelect.addEventListener('change', () => {
            if (modelSelect.value) {
               setSessionLlm({ model: modelSelect.value });
               syncEffortDropdownToModel(modelSelect.value);
            }
         });
      }
   }

   /**
    * Set the locked state for conversation LLM controls
    * @param {boolean} locked - Whether controls should be locked
    */
   function setConversationLlmLocked(locked) {
      conversationLlmState.locked = locked;
      const grid = document.getElementById('llm-controls-grid');
      const indicator = document.getElementById('llm-lock-indicator');
      const reasoningSelect = document.getElementById('reasoning-mode-select');
      const toolsSelect = document.getElementById('tools-mode-select');

      if (grid) {
         grid.classList.toggle('locked', locked);
      }
      // Reasoning mode + effort stay editable mid-conversation. Effort never
      // errors on any provider; a reasoning-mode change is made safe server-side
      // (Claude clamps an incompatible thinking toggle instead of 400ing). Only
      // tool mode is frozen after the first message (pending a per-provider test).
      if (reasoningSelect) {
         reasoningSelect.disabled = false;
      }
      if (toolsSelect) {
         toolsSelect.disabled = locked;
      }
      if (indicator) {
         indicator.classList.toggle('hidden', !locked);
      }
   }

   /**
    * Initialize per-conversation LLM controls
    */
   function initConversationLlmControls() {
      const reasoningSelect = document.getElementById('reasoning-mode-select');
      const depthSelect = document.getElementById('reasoning-effort-select');
      const toolsSelect = document.getElementById('tools-mode-select');

      // Helper to update depth selector enabled state based on reasoning mode
      function updateDepthEnabled() {
         if (depthSelect) {
            const disabled = reasoningSelect && reasoningSelect.value === 'disabled';
            depthSelect.disabled = disabled;
            setControlHint('effort-hint', disabled ? 'Enable reasoning first' : null);
         }
      }

      if (reasoningSelect) {
         reasoningSelect.addEventListener('change', () => {
            conversationLlmState.thinking_mode = reasoningSelect.value;
            // Update depth selector enabled state
            updateDepthEnabled();
            // Immediately update session config so thinking_mode takes effect
            if (!conversationLlmState.locked) {
               setSessionLlm({ thinking_mode: reasoningSelect.value });
            }
         });
      }

      if (depthSelect) {
         depthSelect.addEventListener('change', () => {
            conversationLlmState.reasoning_effort = depthSelect.value;
            // Immediately update session config so reasoning_effort takes effect
            if (!conversationLlmState.locked) {
               setSessionLlm({ reasoning_effort: depthSelect.value });
            }
         });
         // Initialize enabled state
         updateDepthEnabled();
      }

      if (toolsSelect) {
         toolsSelect.addEventListener('change', () => {
            conversationLlmState.tools_mode = toolsSelect.value;
            // Immediately update session config so tool_mode takes effect
            if (!conversationLlmState.locked) {
               setSessionLlm({ tool_mode: toolsSelect.value });
            }
         });
      }

      // Initial session defaults are applied after config loads via applyGlobalDefaultsToControls()

      // Tools help button popup
      const toolsHelpBtn = document.getElementById('tools-help-btn');
      const toolsHelpPopup = document.getElementById('tools-help-popup');

      if (toolsHelpBtn && toolsHelpPopup) {
         // Escape close goes through DawnEscStack (registered while shown) so it
         // stacks correctly under any layer opened above the popup.
         let escToken = null;
         const closePopup = () => {
            toolsHelpPopup.classList.add('hidden');
            if (escToken !== null) {
               DawnEscStack.unregister(escToken);
               escToken = null;
            }
         };
         const openPopup = () => {
            toolsHelpPopup.classList.remove('hidden');
            if (escToken === null) {
               escToken = DawnEscStack.register(() => {
                  closePopup();
                  return true;
               });
            }
         };

         // Toggle popup on button click
         toolsHelpBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            if (toolsHelpPopup.classList.contains('hidden')) openPopup();
            else closePopup();
         });

         // Close popup when clicking outside.
         // NOTE: This document-level listener is intentionally not removed — the
         // popup is a session-lived singleton; closePopup is a safe no-op when
         // already hidden.
         document.addEventListener('click', (e) => {
            if (!toolsHelpPopup.contains(e.target) && e.target !== toolsHelpBtn) {
               closePopup();
            }
         });
      }

      // Privacy toggle button
      initPrivacyToggle();
   }

   /**
    * Initialize privacy toggle button and keyboard shortcut
    */
   function initPrivacyToggle() {
      const privacyToggle = document.getElementById('privacy-toggle');
      if (!privacyToggle) return;

      // Toggle on click
      privacyToggle.addEventListener('click', () => {
         if (conversationLlmState.is_private) {
            // Turning OFF privacy - show confirmation
            showPrivacyConfirmation();
         } else {
            // Turning ON privacy - no confirmation needed
            setPrivacy(true);
         }
      });

      // Keyboard shortcut: Ctrl+Shift+P (works before and during conversation)
      document.addEventListener('keydown', (e) => {
         if (e.ctrlKey && e.shiftKey && e.key === 'P') {
            e.preventDefault();
            if (conversationLlmState.is_private) {
               showPrivacyConfirmation();
            } else {
               setPrivacy(true);
            }
         }
      });
   }

   /**
    * Show confirmation modal before turning off privacy
    */
   async function showPrivacyConfirmation() {
      const confirmed = await DawnDialog.confirm(
         'Future messages in this conversation may be analyzed to extract memories and preferences. ' +
            'Messages already sent while private will remain unanalyzed.',
         {
            title: 'Disable Private Mode?',
            okText: 'Disable Privacy',
            cancelText: 'Keep Private',
            danger: true,
         }
      );
      if (confirmed) {
         setPrivacy(false);
      }
   }

   /**
    * Set privacy mode for current conversation (or pending state for new conversation)
    * @param {boolean} isPrivate - True to enable private mode
    */
   function setPrivacy(isPrivate) {
      // Always update local state and UI - privacy can be set before conversation exists
      updatePrivacyToggleUI(isPrivate);

      // If no conversation exists yet, just update local state (will be applied when conversation is created)
      if (!conversationLlmState.conversation_id) {
         console.log('Privacy set to', isPrivate, '- will apply when conversation is created');
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(
               isPrivate ? 'Private mode enabled for new conversation' : 'Private mode disabled',
               'info'
            );
         }
         return;
      }

      // Conversation exists - send to server
      if (!DawnWS || !DawnWS.isConnected()) {
         console.error('WebSocket not connected');
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Cannot update privacy: not connected', 'error');
         }
         return;
      }

      DawnWS.send({
         type: 'set_private',
         payload: {
            conversation_id: conversationLlmState.conversation_id,
            is_private: isPrivate,
         },
      });
   }

   /**
    * Handle set_private_response from server
    * @param {Object} payload - Response payload
    */
   function handleSetPrivateResponse(payload) {
      if (payload.success) {
         conversationLlmState.is_private = payload.is_private;
         updatePrivacyToggleUI(payload.is_private);

         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(
               payload.message ||
                  (payload.is_private ? 'Private mode enabled' : 'Private mode disabled'),
               'success'
            );
         }

         // Update history list if visible
         if (typeof DawnHistory !== 'undefined' && DawnHistory.updateConversationPrivacy) {
            DawnHistory.updateConversationPrivacy(payload.conversation_id, payload.is_private);
         }
      } else {
         // Revert UI on failure
         updatePrivacyToggleUI(conversationLlmState.is_private);

         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(
               'Failed to update privacy: ' + (payload.error || 'Unknown error'),
               'error'
            );
         }
      }
   }

   /**
    * Update privacy toggle button UI
    * @param {boolean} isPrivate - Current privacy state
    */
   function updatePrivacyToggleUI(isPrivate) {
      conversationLlmState.is_private = isPrivate;

      const privacyToggle = document.getElementById('privacy-toggle');
      if (!privacyToggle) return;

      privacyToggle.setAttribute('aria-pressed', isPrivate ? 'true' : 'false');

      const eyeIcon = privacyToggle.querySelector('.eye-icon');
      const eyeOffIcon = privacyToggle.querySelector('.eye-off-icon');

      if (eyeIcon && eyeOffIcon) {
         if (isPrivate) {
            eyeIcon.classList.add('hidden');
            eyeOffIcon.classList.remove('hidden');
         } else {
            eyeIcon.classList.remove('hidden');
            eyeOffIcon.classList.add('hidden');
         }
      }
   }

   /**
    * Set the current conversation ID for privacy toggle
    * @param {number|null} convId - Conversation ID or null
    * @param {boolean} isPrivate - Initial privacy state (optional, preserves pending state if not provided)
    */
   function setCurrentConversation(convId, isPrivate) {
      conversationLlmState.conversation_id = convId;
      // Only update privacy if explicitly provided; otherwise preserve pending state
      if (typeof isPrivate === 'boolean') {
         conversationLlmState.is_private = isPrivate;
         updatePrivacyToggleUI(isPrivate);
      }
   }

   /**
    * Get the current (or pending) privacy state
    * @returns {boolean} True if private mode is set
    */
   function getPrivacyState() {
      return conversationLlmState.is_private;
   }

   /**
    * Reset conversation LLM controls for a new conversation
    * Resets all controls (mode, provider, model, reasoning, tools) to global defaults
    */
   function resetConversationLlmControls() {
      setConversationLlmLocked(false);

      // Reset privacy state (new conversations start as public)
      conversationLlmState.conversation_id = null;
      conversationLlmState.is_private = false;
      updatePrivacyToggleUI(false);

      // Clear thinking tokens from previous conversation to prevent stale display
      if (typeof DawnState !== 'undefined') {
         DawnState.metricsState.last_thinking_tokens = 0;
      }

      const typeSelect = document.getElementById('llm-type-select');
      const providerSelect = document.getElementById('llm-provider-select');
      const modelSelect = document.getElementById('llm-model-select');
      const reasoningSelect = document.getElementById('reasoning-mode-select');
      const depthSelect = document.getElementById('reasoning-effort-select');
      const toolsSelect = document.getElementById('tools-mode-select');

      // Reset type (local/cloud)
      if (typeSelect) {
         typeSelect.value = globalDefaults.type;
      }

      // Build session reset payload
      const sessionReset = {
         type: globalDefaults.type,
         tool_mode: globalDefaults.tools_mode,
         thinking_mode: globalDefaults.thinking_mode,
         reasoning_effort: globalDefaults.reasoning_effort,
      };

      // Reset provider and model based on type
      if (globalDefaults.type === 'cloud') {
         if (providerSelect) {
            providerSelect.value = globalDefaults.provider;
         }

         // Reset model to provider's default
         let defaultModel;
         if (globalDefaults.provider === 'claude') {
            defaultModel = globalDefaults.claude_model;
         } else if (globalDefaults.provider === 'gemini') {
            defaultModel = globalDefaults.gemini_model;
         } else {
            defaultModel = globalDefaults.openai_model;
         }

         if (modelSelect && defaultModel) {
            // Update dropdown options first
            updateModelDropdownForCloud();
            modelSelect.value = defaultModel;
         }

         sessionReset.cloud_provider = globalDefaults.provider;
         sessionReset.model = defaultModel;
      }

      // Reset reasoning dropdown to global default
      if (reasoningSelect) {
         reasoningSelect.value = globalDefaults.thinking_mode;
         conversationLlmState.thinking_mode = globalDefaults.thinking_mode;
         sessionReset.thinking_mode = globalDefaults.thinking_mode;
      }

      // Reset depth dropdown and enabled state
      if (depthSelect) {
         depthSelect.value = globalDefaults.reasoning_effort;
         conversationLlmState.reasoning_effort = globalDefaults.reasoning_effort;
         depthSelect.disabled = globalDefaults.thinking_mode === 'disabled';
         setControlHint(
            'effort-hint',
            globalDefaults.thinking_mode === 'disabled' ? 'Enable reasoning first' : null
         );
      }

      // Reset tools dropdown
      if (toolsSelect) {
         toolsSelect.value = globalDefaults.tools_mode;
         conversationLlmState.tools_mode = globalDefaults.tools_mode;
      }

      // Update runtime state
      llmRuntimeState.type = globalDefaults.type;
      llmRuntimeState.provider = globalDefaults.provider;
      if (sessionReset.model) {
         llmRuntimeState.model = sessionReset.model;
      }

      // Trim Effort dropdown to the new model's accepted set.
      // skipPersist: reset path sends its own from_restore-tagged payload.
      if (sessionReset.model) {
         syncEffortDropdownToModel(sessionReset.model, true);
      }

      // Send reset to session. Mark as from_restore so the server's
      // active-conversation persistence cascade is skipped — the new-conversation
      // reset fires BEFORE clear_session, and without the flag it would silently
      // overwrite the locked settings of the conversation the user just left.
      sessionReset.from_restore = true;
      setSessionLlm(sessionReset);
   }

   /**
    * Apply LLM settings from a loaded conversation
    * @param {Object} settings - LLM settings from conversation
    * @param {boolean} isLocked - Whether the conversation is locked
    */
   function applyConversationLlmSettings(settings, isLocked) {
      const reasoningSelect = document.getElementById('reasoning-mode-select');
      const depthSelect = document.getElementById('reasoning-effort-select');
      const toolsSelect = document.getElementById('tools-mode-select');

      if (settings) {
         if (settings.thinking_mode && reasoningSelect) {
            const tm = normalizeThinkingMode(settings.thinking_mode);
            reasoningSelect.value = tm;
            conversationLlmState.thinking_mode = tm;
         }

         if (settings.reasoning_effort && depthSelect) {
            depthSelect.value = settings.reasoning_effort;
            conversationLlmState.reasoning_effort = settings.reasoning_effort;
         }
         // Trim Effort dropdown to what this model accepts (and snap if needed).
         // skipPersist: restore path sends its own from_restore-tagged payload.
         if (settings.model) {
            syncEffortDropdownToModel(settings.model, true);
         }
         // Update depth enabled state based on thinking mode
         if (depthSelect && reasoningSelect) {
            const disabled = reasoningSelect.value === 'disabled';
            depthSelect.disabled = disabled;
            setControlHint('effort-hint', disabled ? 'Enable reasoning first' : null);
         }

         if (settings.tools_mode && toolsSelect) {
            toolsSelect.value = settings.tools_mode;
            conversationLlmState.tools_mode = settings.tools_mode;
         }

         // Push restored settings back to the server session. Without this, the
         // session keeps whichever defaults were sent by applyGlobalDefaultsToControls
         // at page load — the UI shows the conversation's value but the server
         // uses the global default for the next request.
         const sessionPayload = {};
         if (settings.llm_type) sessionPayload.type = settings.llm_type;
         if (settings.cloud_provider) sessionPayload.provider = settings.cloud_provider;
         if (settings.model) sessionPayload.model = settings.model;
         if (settings.tools_mode) sessionPayload.tool_mode = settings.tools_mode;
         if (settings.thinking_mode) sessionPayload.thinking_mode = settings.thinking_mode;
         if (settings.reasoning_effort) sessionPayload.reasoning_effort = settings.reasoning_effort;

         if (Object.keys(sessionPayload).length > 0) {
            // Show loading state while syncing if provider/model is changing
            if (sessionPayload.type || sessionPayload.provider || sessionPayload.model) {
               const llmGrid = document.getElementById('llm-controls-grid');
               if (llmGrid) {
                  llmGrid.classList.add('llm-syncing');
               }
            }

            // from_restore tells the server "apply these to the session, but
            // DON'T cascade them into the active conversation's DB row." Rapid
            // conversation clicks would otherwise race: a restore for conv Y
            // can arrive after the active pointer has moved to conv Z, and the
            // unguarded cascade would silently overwrite Z's locked settings
            // with Y's values.
            sessionPayload.from_restore = true;

            if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
               DawnWS.send({
                  type: 'set_session_llm',
                  payload: sessionPayload,
               });
            }
         }
      }

      setConversationLlmLocked(isLocked);
   }

   /**
    * Get current conversation LLM settings for locking
    * @returns {Object} Current LLM settings
    */
   function getConversationLlmSettings() {
      return {
         llm_type: llmRuntimeState.type,
         cloud_provider: llmRuntimeState.provider,
         model: llmRuntimeState.model,
         tools_mode: conversationLlmState.tools_mode,
         thinking_mode: conversationLlmState.thinking_mode,
         reasoning_effort: conversationLlmState.reasoning_effort,
      };
   }

   /**
    * Lock conversation LLM settings (called when first message is sent)
    * @param {string} conversationId - ID of the conversation to lock
    */
   function lockConversationLlmSettings(conversationId) {
      if (conversationLlmState.locked) {
         return; // Already locked
      }

      const settings = getConversationLlmSettings();
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({
            type: 'lock_conversation_llm',
            payload: {
               conversation_id: conversationId,
               llm_settings: settings,
            },
         });
      }

      // Optimistically lock the UI
      setConversationLlmLocked(true);
   }

   /**
    * Request local models from server
    */
   function requestLocalModels() {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({ type: 'list_llm_models' });

         // Set timeout to show "Unknown" if detection takes too long
         if (detectLocalProviderTimeout) {
            clearTimeout(detectLocalProviderTimeout);
         }
         detectLocalProviderTimeout = setTimeout(() => {
            const providerSelect = document.getElementById('llm-provider-select');
            const typeSelect = document.getElementById('llm-type-select');
            if (
               providerSelect &&
               typeSelect &&
               typeSelect.value === 'local' &&
               providerSelect.value === 'detecting'
            ) {
               providerSelect.innerHTML = '';
               const opt = document.createElement('option');
               opt.value = 'unknown';
               opt.textContent = 'Unknown';
               providerSelect.appendChild(opt);
               providerSelect.title = 'Could not detect local provider';
               setControlHint('provider-hint', 'Could not detect provider');
               console.warn('Local provider detection timed out');
            }
         }, DETECT_TIMEOUT_MS);
      }
   }

   /**
    * Handle list_llm_models response from server
    * @param {Object} payload - Response payload
    */
   function handleListLlmModelsResponse(payload) {
      // Clear detection timeout since we received a response
      if (detectLocalProviderTimeout) {
         clearTimeout(detectLocalProviderTimeout);
         detectLocalProviderTimeout = null;
      }

      localModelList = payload.models || [];
      localProviderType = payload.provider || 'Unknown';

      // Update provider dropdown to show detected local provider
      const providerSelect = document.getElementById('llm-provider-select');
      const typeSelect = document.getElementById('llm-type-select');

      if (providerSelect && typeSelect && typeSelect.value === 'local') {
         providerSelect.innerHTML = '';
         const opt = document.createElement('option');
         opt.value = localProviderType.toLowerCase();
         opt.textContent = localProviderType;
         providerSelect.appendChild(opt);
         providerSelect.disabled = true;
         providerSelect.title = `Local provider: ${localProviderType}`;
         setControlHint('provider-hint', `Provider: ${localProviderType}`);
      }

      const modelSelect = document.getElementById('llm-model-select');
      if (!modelSelect) return;

      // Only update model dropdown if we're in local mode
      if (typeSelect && typeSelect.value !== 'local') return;

      modelSelect.innerHTML = '';

      // llama.cpp doesn't support model switching (only shows loaded model)
      if (localProviderType === 'llama.cpp') {
         const opt = document.createElement('option');
         opt.value = payload.current_model || '';
         opt.textContent = payload.current_model || '(server default)';
         modelSelect.appendChild(opt);
         modelSelect.disabled = true;
         modelSelect.title = 'Model switching not supported with llama.cpp';
         setControlHint('model-hint', 'Set by llama.cpp');
         // Update session with the loaded model (only if changed to avoid feedback loop).
         // from_restore: server-side cascade must NOT persist this auto-detected model
         // onto the active conversation's DB row — the user didn't choose this; the
         // local provider just told us what's loaded.
         if (payload.current_model && llmRuntimeState.model !== payload.current_model) {
            llmRuntimeState.model = payload.current_model;
            setSessionLlm({ model: payload.current_model, from_restore: true });
         }
         // Update collapsed mini bar summary
         if (typeof DAWN !== 'undefined' && DAWN.updateLlmMiniSummary) {
            DAWN.updateLlmMiniSummary();
         }
         return;
      }

      // Ollama or Generic - enable selection
      if (localModelList.length === 0) {
         const opt = document.createElement('option');
         opt.value = '';
         opt.textContent = 'No models available';
         modelSelect.appendChild(opt);
         modelSelect.disabled = true;
         setControlHint('model-hint', null);
         return;
      }

      localModelList.forEach((model) => {
         const opt = document.createElement('option');
         opt.value = model.name;
         opt.textContent = model.name + (model.loaded ? ' (loaded)' : '');
         opt.title = opt.textContent;
         modelSelect.appendChild(opt);
      });

      // Select current model and update session (only if changed to avoid feedback loop).
      // from_restore: same rationale as the llama.cpp branch above — auto-detection
      // sync, not a user-initiated change, must not cascade to the conv DB row.
      if (payload.current_model) {
         modelSelect.value = payload.current_model;
         if (llmRuntimeState.model !== payload.current_model) {
            llmRuntimeState.model = payload.current_model;
            setSessionLlm({ model: payload.current_model, from_restore: true });
         }
      }
      modelSelect.disabled = false;
      modelSelect.title = 'Select local model';
      setControlHint('model-hint', null);

      // Update collapsed mini bar summary if available
      if (typeof DAWN !== 'undefined' && DAWN.updateLlmMiniSummary) {
         DAWN.updateLlmMiniSummary();
      }
   }

   /**
    * Update model dropdown for cloud providers
    * @param {boolean} sendToSession - Whether to send selected model to session
    */
   function updateModelDropdownForCloud(sendToSession = false) {
      const modelSelect = document.getElementById('llm-model-select');
      const providerSelect = document.getElementById('llm-provider-select');
      if (!modelSelect || !providerSelect) return;

      const provider = providerSelect.value?.toLowerCase() || '';
      const models = cloudModelLists[provider] || [];

      modelSelect.innerHTML = '';

      if (models.length === 0) {
         const opt = document.createElement('option');
         opt.value = '';
         opt.textContent = 'Configure in settings';
         modelSelect.appendChild(opt);
         modelSelect.disabled = true;
         modelSelect.title = 'Add models in Settings > LLM > Cloud Settings';
         setControlHint('model-hint', 'Configure in Settings');
         return;
      }

      models.forEach((model) => {
         const opt = document.createElement('option');
         opt.value = model;
         opt.textContent = model;
         opt.title = model;
         modelSelect.appendChild(opt);
      });

      // Select current model if in list, otherwise use provider's default
      const currentModel = llmRuntimeState?.model;
      if (currentModel && models.includes(currentModel)) {
         modelSelect.value = currentModel;
      } else {
         // Use default index for this provider, fall back to first model
         let defaultIdx;
         if (provider === 'claude') {
            defaultIdx = cloudModelLists.claudeDefaultIdx;
         } else if (provider === 'gemini') {
            defaultIdx = cloudModelLists.geminiDefaultIdx;
         } else if (provider === 'openrouter') {
            defaultIdx = cloudModelLists.openrouterDefaultIdx;
         } else {
            defaultIdx = cloudModelLists.openaiDefaultIdx;
         }
         // Ensure index is valid
         if (defaultIdx >= 0 && defaultIdx < models.length) {
            modelSelect.value = models[defaultIdx];
         } else {
            modelSelect.value = models[0];
         }
      }

      modelSelect.disabled = false;
      modelSelect.title = 'Select cloud model';
      setControlHint('model-hint', null);

      // Send selected model to session if requested (e.g., after provider switch)
      // Only send if model actually changed to avoid feedback loops
      if (sendToSession && modelSelect.value && llmRuntimeState.model !== modelSelect.value) {
         llmRuntimeState.model = modelSelect.value;
         setSessionLlm({ model: modelSelect.value });
      }
   }

   /**
    * Reflect OpenRouter gateway mode in the cloud provider/model controls: a single
    * read-only "OpenRouter" provider plus the vendor/model slug list. Shared by the
    * initial render and the Type->Cloud switch so both honor the gateway. Without this
    * on the switch path, picking Cloud rebuilt a direct OpenAI/Claude/Gemini picker and
    * let the user select a bare model that the gateway then silently replaced with a
    * different vendor's default.
    * @param {boolean} sendToSession - Whether to push the selected slug to the session
    */
   function applyGatewayCloudUI(sendToSession = false) {
      const providerSelect = document.getElementById('llm-provider-select');
      if (!providerSelect) return;
      providerSelect.innerHTML = '';
      const opt = document.createElement('option');
      opt.value = 'openrouter';
      opt.textContent = 'OpenRouter';
      providerSelect.appendChild(opt);
      providerSelect.value = 'openrouter';
      providerSelect.disabled = true;
      providerSelect.title = 'Gateway mode — all cloud models routed through OpenRouter';
      setControlHint('provider-hint', 'Gateway: OpenRouter');
      updateModelDropdownForCloud(sendToSession);
   }

   /**
    * Update cloud model lists from config
    * @param {Object} config - Config object
    */
   function updateCloudModelLists(config) {
      // Extract model lists and default indices from config response
      if (config?.llm?.cloud) {
         const cloud = config.llm.cloud;
         cloudModelLists.openai = cloud.openai_models || [];
         cloudModelLists.claude = cloud.claude_models || [];
         cloudModelLists.gemini = cloud.gemini_models || [];
         cloudModelLists.openrouter = cloud.openrouter_models || [];
         cloudModelLists.openaiDefaultIdx = cloud.openai_default_model_idx || 0;
         cloudModelLists.claudeDefaultIdx = cloud.claude_default_model_idx || 0;
         cloudModelLists.geminiDefaultIdx = cloud.gemini_default_model_idx || 0;
         cloudModelLists.openrouterDefaultIdx = cloud.openrouter_default_model_idx || 0;
      }
   }

   /**
    * Extract global defaults from config for resetting new conversations
    * @param {Object} config - Config object
    */
   function extractGlobalDefaults(config) {
      if (!config) return;

      // LLM type (local/cloud)
      if (config.llm?.type) {
         globalDefaults.type = config.llm.type;
      }

      // Cloud provider
      if (config.llm?.cloud?.provider) {
         globalDefaults.provider = config.llm.cloud.provider;
      }
      // OpenRouter gateway flag
      globalDefaults.use_openrouter = !!config.llm?.cloud?.use_openrouter;

      // Default models (resolve idx to model name)
      if (config.llm?.cloud) {
         const cloud = config.llm.cloud;
         const openaiModels = cloud.openai_models || [];
         const claudeModels = cloud.claude_models || [];
         const geminiModels = cloud.gemini_models || [];
         const openrouterModels = cloud.openrouter_models || [];
         const openaiIdx = cloud.openai_default_model_idx || 0;
         const claudeIdx = cloud.claude_default_model_idx || 0;
         const geminiIdx = cloud.gemini_default_model_idx || 0;
         const openrouterIdx = cloud.openrouter_default_model_idx || 0;

         globalDefaults.openai_model = openaiModels[openaiIdx] || openaiModels[0] || '';
         globalDefaults.claude_model = claudeModels[claudeIdx] || claudeModels[0] || '';
         globalDefaults.gemini_model = geminiModels[geminiIdx] || geminiModels[0] || '';
         globalDefaults.openrouter_model =
            openrouterModels[openrouterIdx] || openrouterModels[0] || '';
      }

      // Tools mode
      if (config.llm?.tools?.mode) {
         globalDefaults.tools_mode = config.llm.tools.mode;
      }

      // Thinking mode (Claude/local) — normalize legacy "auto" to "enabled"
      if (config.llm?.thinking?.mode) {
         globalDefaults.thinking_mode = normalizeThinkingMode(config.llm.thinking.mode);
      }

      // Reasoning effort (OpenAI o-series/GPT-5)
      if (config.llm?.thinking?.reasoning_effort) {
         globalDefaults.reasoning_effort = config.llm.thinking.reasoning_effort;
      }
   }

   /**
    * Apply global defaults to conversation LLM controls
    * Called after config loads to set initial UI state
    */
   function applyGlobalDefaultsToControls() {
      const reasoningSelect = document.getElementById('reasoning-mode-select');
      const depthSelect = document.getElementById('reasoning-effort-select');
      const toolsSelect = document.getElementById('tools-mode-select');

      if (reasoningSelect) {
         reasoningSelect.value = globalDefaults.thinking_mode;
         conversationLlmState.thinking_mode = globalDefaults.thinking_mode;
      }

      if (depthSelect) {
         depthSelect.value = globalDefaults.reasoning_effort;
         conversationLlmState.reasoning_effort = globalDefaults.reasoning_effort;
         depthSelect.disabled = globalDefaults.thinking_mode === 'disabled';
         setControlHint(
            'effort-hint',
            globalDefaults.thinking_mode === 'disabled' ? 'Enable reasoning first' : null
         );
      }

      if (toolsSelect) {
         toolsSelect.value = globalDefaults.tools_mode;
         conversationLlmState.tools_mode = globalDefaults.tools_mode;
      }

      // Send initial defaults to session. from_restore: this fires at config-load
      // time, which on a page reload may happen WHILE a conversation is already
      // active server-side — defaults must not cascade onto that conv's row.
      setSessionLlm({
         tool_mode: globalDefaults.tools_mode,
         thinking_mode: globalDefaults.thinking_mode,
         reasoning_effort: globalDefaults.reasoning_effort,
         from_restore: true,
      });
   }

   /**
    * Update LLM controls from runtime state
    * @param {Object} runtime - Runtime state object
    */
   /**
    * Apply the session's actual reasoning settings from an llm_runtime payload to
    * the thinking-mode + effort controls. Before llm_runtime carried thinking_mode
    * and reasoning_effort, these controls could only show the config default until
    * the user touched them; now a fresh connection reflects the live session value.
    */
   function applyRuntimeReasoning(runtime) {
      const reasoningSelect = document.getElementById('reasoning-mode-select');
      const depthSelect = document.getElementById('reasoning-effort-select');
      if (reasoningSelect && runtime.thinking_mode) {
         reasoningSelect.value = normalizeThinkingMode(runtime.thinking_mode);
         conversationLlmState.thinking_mode = reasoningSelect.value;
      }
      if (depthSelect && runtime.reasoning_effort) {
         depthSelect.value = runtime.reasoning_effort;
         // Clamp the selection to what the model accepts (also hides invalid opts).
         if (runtime.model) {
            syncEffortDropdownToModel(runtime.model, true);
         }
         conversationLlmState.reasoning_effort = depthSelect.value;
      }
      // Effort is inert when thinking is disabled — mirror the reset-path UX.
      if (reasoningSelect && depthSelect) {
         const off = reasoningSelect.value === 'disabled';
         depthSelect.disabled = off;
         setControlHint('effort-hint', off ? 'Enable reasoning first' : null);
      }
   }

   function updateLlmControls(runtime) {
      llmRuntimeState = { ...llmRuntimeState, ...runtime };

      const typeSelect = document.getElementById('llm-type-select');
      const providerSelect = document.getElementById('llm-provider-select');

      if (typeSelect) {
         typeSelect.value = runtime.type || 'cloud';
      }

      // OpenRouter gateway: the backend forces every cloud session through OpenRouter,
      // so reflect that honestly as a read-only "OpenRouter" provider instead of leaving
      // a stale direct-provider dropdown that contradicts the actual routing. The full
      // interactive switcher under gateway is a Phase 2 item.
      const gatewayOn = !!globalDefaults.use_openrouter;
      if (providerSelect && gatewayOn && runtime.type !== 'local') {
         applyGatewayCloudUI(false);
         if (runtime.model) {
            syncEffortDropdownToModel(runtime.model, true);
         }
         applyRuntimeReasoning(runtime);
         if (typeof DAWN !== 'undefined' && DAWN.updateLlmMiniSummary) {
            DAWN.updateLlmMiniSummary();
         }
         return;
      }

      if (providerSelect) {
         // Update available options based on API key availability
         providerSelect.innerHTML = '';

         if (runtime.openai_available) {
            const opt = document.createElement('option');
            opt.value = 'openai';
            opt.textContent = 'OpenAI';
            providerSelect.appendChild(opt);
         }

         if (runtime.claude_available) {
            const opt = document.createElement('option');
            opt.value = 'claude';
            opt.textContent = 'Claude';
            providerSelect.appendChild(opt);
         }

         if (runtime.gemini_available) {
            const opt = document.createElement('option');
            opt.value = 'gemini';
            opt.textContent = 'Gemini';
            providerSelect.appendChild(opt);
         }

         // If no providers available, show disabled message
         if (!runtime.openai_available && !runtime.claude_available && !runtime.gemini_available) {
            const opt = document.createElement('option');
            opt.value = '';
            opt.textContent = 'No API keys configured';
            opt.disabled = true;
            providerSelect.appendChild(opt);
            providerSelect.disabled = true;
         } else {
            providerSelect.disabled = false;
            // Set current value
            const currentProvider = runtime.provider?.toLowerCase() || '';
            if (providerSelect.querySelector(`option[value="${currentProvider}"]`)) {
               providerSelect.value = currentProvider;
            }
         }
      }

      // Enable/disable provider selector based on LLM type
      if (providerSelect) {
         if (runtime.type === 'local') {
            // Show detecting state until list_llm_models_response arrives
            providerSelect.innerHTML = '';
            const opt = document.createElement('option');
            opt.value = 'detecting';
            opt.textContent = 'Detecting...';
            providerSelect.appendChild(opt);
            providerSelect.disabled = true;
            providerSelect.title = 'Auto-detecting local provider';
            setControlHint('provider-hint', 'Detecting...');
         } else if (
            runtime.openai_available ||
            runtime.claude_available ||
            runtime.gemini_available
         ) {
            providerSelect.disabled = false;
            providerSelect.title = 'Switch cloud provider';
            setControlHint('provider-hint', null);
         }
      }

      // Update model dropdown based on mode
      if (runtime.type === 'local') {
         // Request local models to populate dropdown
         requestLocalModels();
      } else {
         // Populate cloud models dropdown
         updateModelDropdownForCloud();
      }

      // Effort dropdown's valid set depends on the model. Re-sync whenever the
      // server confirms a new runtime model so user can't pick xhigh on a model
      // that doesn't accept it.  skipPersist: config-load path, not user action.
      if (runtime.model) {
         syncEffortDropdownToModel(runtime.model, true);
      }

      // Apply the session's actual thinking_mode / reasoning_effort (llm_runtime
      // now carries them) so the controls reflect the live session, not the default.
      applyRuntimeReasoning(runtime);

      // Update collapsed mini bar summary if available
      if (typeof DAWN !== 'undefined' && DAWN.updateLlmMiniSummary) {
         DAWN.updateLlmMiniSummary();
      }
   }

   /**
    * Send LLM changes to session
    * @param {Object} changes - Changes to send
    */
   function setSessionLlm(changes) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         console.error('WebSocket not connected');
         return;
      }

      DawnWS.send({
         type: 'set_session_llm',
         payload: changes,
      });
   }

   /**
    * Handle set_session_llm response from server
    * @param {Object} payload - Response payload
    */
   function handleSetSessionLlmResponse(payload) {
      // Clear loading state
      const llmGrid = document.getElementById('llm-controls-grid');
      if (llmGrid) {
         llmGrid.classList.remove('llm-syncing');
      }

      if (payload.success) {
         updateLlmControls(payload);
      } else {
         console.error('Failed to update session LLM:', payload.error);
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Failed to switch LLM: ' + (payload.error || 'Unknown error'), 'error');
         }

         // Revert controls to actual state
         if (llmRuntimeState) {
            updateLlmControls(llmRuntimeState);
         }
      }
   }

   /**
    * Check if conversation LLM is locked
    * @returns {boolean} True if locked
    */
   function isConversationLlmLocked() {
      return conversationLlmState.locked;
   }

   // Export for settings module
   window.DawnSettingsLlm = {
      getLlmRuntimeState,
      getConversationLlmState,
      getGlobalDefaults,
      initLlmControls,
      setConversationLlmLocked,
      initConversationLlmControls,
      resetConversationLlmControls,
      applyConversationLlmSettings,
      getConversationLlmSettings,
      lockConversationLlmSettings,
      requestLocalModels,
      handleListLlmModelsResponse,
      updateModelDropdownForCloud,
      updateCloudModelLists,
      extractGlobalDefaults,
      applyGlobalDefaultsToControls,
      updateLlmControls,
      setSessionLlm,
      handleSetSessionLlmResponse,
      isConversationLlmLocked,
      setCurrentConversation,
      getPrivacyState,
      setPrivacy,
      handleSetPrivateResponse,
      updatePrivacyToggleUI,
   };
})();
