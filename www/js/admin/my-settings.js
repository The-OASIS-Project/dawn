/**
 * DAWN My Settings Module
 * Personal user settings (persona, location, theme)
 */
(function () {
   'use strict';

   /* =============================================================================
    * State
    * ============================================================================= */

   let callbacks = {
      setTheme: null,
      getAuthState: null,
   };

   // Last saved/committed theme.  The theme buttons apply live (localStorage) as a
   // PREVIEW, but only "Save" commits; an unsaved preview reverts to this when My
   // Settings closes.  null until the panel has loaded once (nothing to revert to).
   let savedTheme = null;

   // Tracks unsaved edits in the My Settings form (persona, location, timezone,
   // units, identity fields, theme).  Two jobs:
   //  1. Guards handleGetMySettingsResponse() so a refetch (panel open, section
   //     re-expand, reconnect) can't overwrite in-progress edits with the
   //     last-saved server value — which the next Save would then persist,
   //     silently losing what the user typed.
   //  2. Lets the unified panel Save (config.js) know whether My Settings needs
   //     sending.
   // Set true on any edit; cleared on a successful save or an explicit
   // discard-and-reload, when the form matches the server again.
   let formDirty = false;

   // Completion callback registered by the unified panel Save (config.js) so a
   // My-Settings save reports done into the shared multi-save counter.  Mirrors
   // the DawnTools.onSaveComplete pattern; one-shot — fired and cleared by
   // handleSetMySettingsResponse.
   let saveCompleteCb = null;

   // Whether a get_my_settings response has ever populated the form.  The
   // refetch guard only protects edits AFTER the first load — the first
   // response must always populate, or a user who edits within the sub-RTT
   // window before it arrives would leave untouched fields at HTML defaults and
   // savedTheme null (a later Save would then persist those defaults).
   let loadedOnce = false;

   // Single writer for the dirty state so the "unsaved" dot on the My Settings
   // section header stays in lockstep with the flag (mirrors how the config
   // sections show a has-unsaved dot).
   function setFormDirty(dirty) {
      formDirty = dirty;
      const section = document.getElementById('my-settings-section');
      const header = section && section.querySelector('.section-header');
      if (header) header.classList.toggle('has-unsaved', dirty);
   }

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

   // Collect the current form values and send them.  Invoked by the unified
   // panel Save (config.js) AFTER it has registered onSaveComplete and counted
   // this save in its multi-save total, so it always sends when called.
   function saveMySettings() {
      const settings = {
         persona_description: document.getElementById('my-persona')?.value || '',
         persona_mode:
            document.querySelector('input[name="persona_mode"]:checked')?.value || 'append',
         location: document.getElementById('my-location')?.value || '',
         timezone: document.getElementById('my-timezone')?.value || 'UTC',
         units: document.querySelector('input[name="units"]:checked')?.value || 'metric',
         theme: document.querySelector('.theme-btn.active')?.dataset.theme || 'cyan',
         // v44 identity fields
         real_name: document.getElementById('my-real-name')?.value.trim() || '',
         identity_aliases: document.getElementById('my-identity-aliases')?.value.trim() || '',
         preferred_address: document.getElementById('my-preferred-address')?.value.trim() || '',
      };
      // The previewed theme is now being committed — make it the revert baseline.
      savedTheme = settings.theme;
      requestSetMySettings(settings);
   }

   function hasUnsavedChanges() {
      return formDirty;
   }

   function onSaveComplete(cb) {
      saveCompleteCb = cb;
   }

   // Discard unsaved edits and reload the values currently saved on the server
   // (used by the panel's "Reset to Saved Settings").  Clears the dirty flag
   // FIRST so the get response is allowed to repopulate — the refetch guard in
   // handleGetMySettingsResponse would otherwise preserve the very edits we want
   // to discard — and reverts any unsaved theme preview.
   function discardAndReload() {
      setFormDirty(false);
      revertUnsavedTheme();
      requestGetMySettings();
   }

   // Drop the unsaved-edit state without a refetch — used by the panel-close
   // "Discard" path, where the panel is closing so reloading would be wasted
   // (and reverts any unsaved theme preview, matching discardAndReload).
   function clearDirty() {
      setFormDirty(false);
      revertUnsavedTheme();
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
      const realName = document.getElementById('my-real-name');
      const aliases = document.getElementById('my-identity-aliases');
      const preferredAddr = document.getElementById('my-preferred-address');
      // Guard against clobbering unsaved edits on a refetch (section re-expand,
      // reconnect) — but ONLY once we've loaded at least once.  The first
      // response must always populate: there are no real saved edits to protect
      // yet, and untouched fields + savedTheme need their initial values even if
      // the user typed within the sub-RTT window before this arrived.
      if (loadedOnce && formDirty) {
         // Keep the derived UI (char count / clear button / preview) in sync
         // with whatever the user has typed, but touch no field values.
         updatePersonaCharCount();
         updateClearButtonVisibility();
         updatePersonaPreview();
         return;
      }

      if (persona) {
         persona.value = payload.persona_description || '';
         updatePersonaCharCount();
         updateClearButtonVisibility();
         updatePersonaPreview();
      }
      if (location) location.value = payload.location || '';
      if (timezone) timezone.value = payload.timezone || 'UTC';
      if (realName) realName.value = payload.real_name || '';
      if (aliases) aliases.value = payload.identity_aliases || '';
      if (preferredAddr) preferredAddr.value = payload.preferred_address || '';

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

      // Set theme (the saved server value) and record it as the revert baseline
      // for any unsaved preview the user makes while the panel is open.
      if (payload.theme && callbacks.setTheme) {
         callbacks.setTheme(payload.theme);
      }
      savedTheme =
         typeof DawnTheme !== 'undefined' && DawnTheme.current
            ? DawnTheme.current()
            : payload.theme || 'cyan';

      // The form now reflects the server; future refetches may guard against
      // clobbering, and this load cleared any pre-load edit state.
      loadedOnce = true;
      setFormDirty(false);
   }

   // Revert an unsaved theme PREVIEW back to the last saved value.  Called when My
   // Settings closes (section collapse or Settings-panel close) so a previewed-
   // but-unsaved theme doesn't silently stick.
   function revertUnsavedTheme() {
      if (savedTheme === null) return; // never loaded — nothing to revert to
      if (typeof DawnTheme === 'undefined' || !DawnTheme.current) return;
      if (DawnTheme.current() !== savedTheme) {
         DawnTheme.set(savedTheme);
      }
   }

   function handleSetMySettingsResponse(payload) {
      // One-shot: take and clear the orchestration callback up front.
      const cb = saveCompleteCb;
      saveCompleteCb = null;

      if (payload.success) {
         // Saved state now matches the form; future refetches may repopulate.
         setFormDirty(false);
      }

      if (cb) {
         // Orchestrated by the unified panel Save: report into its shared
         // counter and stay silent so it can show a single toast (mirrors the
         // deliberately-silent DawnTools completion path).
         cb(!!payload.success);
         return;
      }

      // Standalone save (defensive — the panel now routes through the unified
      // Save, so this path is not normally reached).
      if (typeof DawnToast !== 'undefined') {
         DawnToast.show(
            payload.success
               ? 'Settings saved successfully'
               : 'Failed to save settings: ' + (payload.error || 'Unknown error'),
            payload.success ? 'success' : 'error'
         );
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
      const max = 2047;
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
            setFormDirty(true);
            updatePersonaCharCount();
            updateClearButtonVisibility();
            updatePersonaPreview();
         });
      }

      // Clear persona button
      if (clearPersonaBtn && persona) {
         clearPersonaBtn.addEventListener('click', () => {
            persona.value = '';
            setFormDirty(true);
            updatePersonaCharCount();
            updateClearButtonVisibility();
            updatePersonaPreview();
         });
      }

      // Mark the form dirty on any edit so (a) a refetch won't clobber it and
      // (b) the unified Save knows to send it.  A form-level input/change
      // listener covers text inputs, selects and radios; theme is chosen via
      // <button> clicks (wired in theme.js), which fire neither input nor
      // change, so those are hooked explicitly below.
      if (form) {
         form.addEventListener('input', () => {
            setFormDirty(true);
         });
         form.addEventListener('change', () => {
            setFormDirty(true);
         });
      }
      document.querySelectorAll('.theme-btn').forEach((btn) => {
         btn.addEventListener('click', () => {
            setFormDirty(true);
         });
      });

      // Enter in a single-line field submits the form.  Route it through the
      // unified panel Save (config.js) so Enter saves everything on the panel,
      // and keep preventDefault so the SPA never navigates/reloads.  (There is
      // no separate "Save My Settings" button anymore — the footer "Save
      // Settings" is the one Save.)
      if (form) {
         form.addEventListener('submit', (e) => {
            e.preventDefault();
            if (typeof DawnSettingsConfig !== 'undefined' && DawnSettingsConfig.saveConfig) {
               DawnSettingsConfig.saveConfig();
            }
         });
      }

      // Load settings when section is expanded
      if (section) {
         const header = section.querySelector('.section-header');
         if (header) {
            header.addEventListener('click', () => {
               setTimeout(() => {
                  const authState = callbacks.getAuthState ? callbacks.getAuthState() : {};
                  if (section.classList.contains('collapsed')) {
                     // Collapsing discards an unsaved theme preview.
                     revertUnsavedTheme();
                  } else if (authState.authenticated) {
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
      revertUnsavedTheme: revertUnsavedTheme,
      // Unified-panel-save integration (mirrors DawnTools):
      hasUnsavedChanges: hasUnsavedChanges,
      onSaveComplete: onSaveComplete,
      save: saveMySettings,
      discardAndReload: discardAndReload,
      clearDirty: clearDirty,
   };
})();
