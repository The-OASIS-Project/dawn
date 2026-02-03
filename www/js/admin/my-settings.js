/**
 * DAWN My Settings Module
 * Personal user settings (persona, location, TTS speed, theme)
 */
(function () {
   'use strict';

   /* =============================================================================
    * State
    * ============================================================================= */

   let callbacks = {
      showConfirmModal: null,
      setTheme: null,
      getAuthState: null,
   };

   /* =============================================================================
    * API Requests
    * ============================================================================= */

   function requestGetMySettings() {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({ type: 'get_my_settings' });
      }
   }

   function requestSetMySettings(settings) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({
            type: 'set_my_settings',
            payload: settings,
         });
      }
   }

   /* =============================================================================
    * Response Handlers
    * ============================================================================= */

   function handleGetMySettingsResponse(payload) {
      if (!payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(
               'Failed to load settings: ' + (payload.error || 'Unknown error'),
               'error'
            );
         }
         return;
      }

      // Store base persona for preview
      window.basePersona = payload.base_persona || '';

      // Populate base persona display
      const basePersonaText = document.getElementById('base-persona-text');
      if (basePersonaText) {
         basePersonaText.textContent = payload.base_persona || '(No base persona configured)';
      }

      // Populate form fields
      const persona = document.getElementById('my-persona');
      const location = document.getElementById('my-location');
      const timezone = document.getElementById('my-timezone');
      const ttsSpeed = document.getElementById('my-tts-speed');
      const ttsSpeedValue = document.getElementById('my-tts-speed-value');

      if (persona) {
         persona.value = payload.persona_description || '';
         updatePersonaCharCount();
         updateClearButtonVisibility();
         updatePersonaPreview();
      }
      if (location) location.value = payload.location || '';
      if (timezone) timezone.value = payload.timezone || 'UTC';

      // Set persona mode radio
      const personaMode = payload.persona_mode || 'append';
      const modeRadio = document.querySelector(
         `input[name="persona_mode"][value="${personaMode}"]`
      );
      if (modeRadio) {
         modeRadio.checked = true;
         updatePersonaModeSelection(personaMode);
      }

      // Set units radio
      const unitsRadio = document.querySelector(
         `input[name="units"][value="${payload.units || 'metric'}"]`
      );
      if (unitsRadio) unitsRadio.checked = true;

      // Set TTS speed
      if (ttsSpeed) {
         ttsSpeed.value = payload.tts_length_scale || 1.0;
         if (ttsSpeedValue) ttsSpeedValue.textContent = ttsSpeed.value + 'x';
      }

      // Set theme
      if (payload.theme && callbacks.setTheme) {
         callbacks.setTheme(payload.theme);
      }
   }

   function handleSetMySettingsResponse(payload) {
      if (payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Settings saved successfully', 'success');
         }
      } else {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(
               'Failed to save settings: ' + (payload.error || 'Unknown error'),
               'error'
            );
         }
      }
   }

   /* =============================================================================
    * UI Helpers
    * ============================================================================= */

   function updatePersonaModeSelection(mode) {
      document.querySelectorAll('.persona-mode-option').forEach((opt) => {
         opt.classList.toggle('selected', opt.dataset.mode === mode);
      });
      // Show/hide replace warning
      const warning = document.querySelector('.replace-warning');
      if (warning) {
         warning.classList.toggle('hidden', mode !== 'replace');
      }
      updatePersonaPreview();
   }

   function updatePersonaCharCount() {
      const persona = document.getElementById('my-persona');
      const charCount = document.getElementById('persona-char-count');
      if (!persona || !charCount) return;

      const len = persona.value.length;
      const max = 500;
      charCount.textContent = `${len} / ${max}`;
      charCount.classList.toggle('warning', len > max * 0.8 && len < max);
      charCount.classList.toggle('limit', len >= max);
   }

   function updateClearButtonVisibility() {
      const persona = document.getElementById('my-persona');
      const clearBtn = document.getElementById('clear-persona-btn');
      if (!persona || !clearBtn) return;

      clearBtn.classList.toggle('hidden', persona.value.length === 0);
   }

   function updatePersonaPreview() {
      const persona = document.getElementById('my-persona');
      const previewBase = document.getElementById('preview-base');
      const previewSeparator = document.getElementById('preview-separator');
      const previewCustom = document.getElementById('preview-custom');
      const previewLabel = document.getElementById('preview-label-suffix');
      const modeRadio = document.querySelector('input[name="persona_mode"]:checked');

      if (!previewBase) return;

      const customText = persona?.value.trim() || '';
      const mode = modeRadio?.value || 'append';
      const base = window.basePersona || '';

      // Always show base persona (truncated for readability)
      previewBase.textContent = base.length > 300 ? base.substring(0, 300) + '...' : base;

      // Show custom section only in append mode with content
      const hasCustom = mode === 'append' && customText.length > 0;

      if (previewSeparator) {
         previewSeparator.classList.toggle('hidden', !hasCustom);
      }
      if (previewCustom) {
         previewCustom.classList.toggle('hidden', !hasCustom);
         previewCustom.textContent = hasCustom ? customText : '';
      }
      if (previewLabel) {
         previewLabel.textContent = hasCustom ? '(with your additions)' : '(what the AI sees)';
      }

      // In replace mode, show what will actually be used
      if (mode === 'replace' && customText.length > 0) {
         previewBase.textContent = customText;
         if (previewLabel) {
            previewLabel.textContent = '(your replacement)';
         }
      }
   }

   /* =============================================================================
    * Initialization
    * ============================================================================= */

   function init() {
      const form = document.getElementById('my-settings-form');
      const ttsSpeed = document.getElementById('my-tts-speed');
      const ttsSpeedValue = document.getElementById('my-tts-speed-value');
      const resetBtn = document.getElementById('reset-my-settings-btn');
      const section = document.getElementById('my-settings-section');
      const persona = document.getElementById('my-persona');
      const toggleBasePersona = document.getElementById('toggle-base-persona');
      const basePersonaDisplay = document.getElementById('base-persona-display');
      const clearPersonaBtn = document.getElementById('clear-persona-btn');

      // Base persona expand/collapse toggle
      if (toggleBasePersona && basePersonaDisplay) {
         toggleBasePersona.addEventListener('click', () => {
            const isCollapsed = basePersonaDisplay.classList.contains('collapsed');
            basePersonaDisplay.classList.toggle('collapsed', !isCollapsed);
            basePersonaDisplay.classList.toggle('expanded', isCollapsed);
            toggleBasePersona.textContent = isCollapsed ? 'Collapse' : 'Show full';
         });
      }

      // Persona mode radio buttons
      document.querySelectorAll('input[name="persona_mode"]').forEach((radio) => {
         radio.addEventListener('change', (e) => {
            updatePersonaModeSelection(e.target.value);
         });
      });

      // Persona textarea input handlers
      if (persona) {
         persona.addEventListener('input', () => {
            updatePersonaCharCount();
            updateClearButtonVisibility();
            updatePersonaPreview();
         });
      }

      // Clear persona button
      if (clearPersonaBtn && persona) {
         clearPersonaBtn.addEventListener('click', () => {
            persona.value = '';
            updatePersonaCharCount();
            updateClearButtonVisibility();
            updatePersonaPreview();
         });
      }

      // Update speed display on range change
      if (ttsSpeed && ttsSpeedValue) {
         ttsSpeed.addEventListener('input', () => {
            ttsSpeedValue.textContent = parseFloat(ttsSpeed.value).toFixed(1) + 'x';
         });
      }

      // Form submission
      if (form) {
         form.addEventListener('submit', (e) => {
            e.preventDefault();

            const settings = {
               persona_description: document.getElementById('my-persona')?.value || '',
               persona_mode:
                  document.querySelector('input[name="persona_mode"]:checked')?.value || 'append',
               location: document.getElementById('my-location')?.value || '',
               timezone: document.getElementById('my-timezone')?.value || 'UTC',
               units: document.querySelector('input[name="units"]:checked')?.value || 'metric',
               tts_length_scale: parseFloat(document.getElementById('my-tts-speed')?.value || 1.0),
               theme: document.querySelector('.theme-btn.active')?.dataset.theme || 'cyan',
            };
            requestSetMySettings(settings);
         });
      }

      // Reset to defaults
      if (resetBtn && callbacks.showConfirmModal) {
         resetBtn.addEventListener('click', () => {
            callbacks.showConfirmModal(
               'Reset all personal settings to defaults?',
               () => {
                  requestSetMySettings({
                     persona_description: '',
                     persona_mode: 'append',
                     location: '',
                     timezone: 'UTC',
                     units: 'metric',
                     tts_length_scale: 1.0,
                     theme: 'cyan',
                  });
                  // Apply theme immediately
                  if (callbacks.setTheme) {
                     callbacks.setTheme('cyan');
                  }
                  // Refresh form after a moment
                  setTimeout(requestGetMySettings, 500);
               },
               { title: 'Reset Settings', okText: 'Reset' }
            );
         });
      }

      // Load settings when section is expanded
      if (section) {
         const header = section.querySelector('.section-header');
         if (header) {
            header.addEventListener('click', () => {
               setTimeout(() => {
                  const authState = callbacks.getAuthState ? callbacks.getAuthState() : {};
                  if (!section.classList.contains('collapsed') && authState.authenticated) {
                     requestGetMySettings();
                  }
               }, 50);
            });
         }
      }
   }

   /**
    * Set callbacks for shared utilities
    */
   function setCallbacks(cbs) {
      if (cbs.showConfirmModal) callbacks.showConfirmModal = cbs.showConfirmModal;
      if (cbs.setTheme) callbacks.setTheme = cbs.setTheme;
      if (cbs.getAuthState) callbacks.getAuthState = cbs.getAuthState;
   }

   /* =============================================================================
    * Export
    * ============================================================================= */

   window.DawnMySettings = {
      init: init,
      setCallbacks: setCallbacks,
      requestGet: requestGetMySettings,
      requestSet: requestSetMySettings,
      handleGetResponse: handleGetMySettingsResponse,
      handleSetResponse: handleSetMySettingsResponse,
   };
})();
