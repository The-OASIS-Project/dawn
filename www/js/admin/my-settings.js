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
      const myTimezone = document.getElementById('my-timezone');
      const settings = {
         persona_values: document.getElementById('my-persona-values')?.value || '',
         persona_values_mode: checkedMode('values'),
         persona_voice: document.getElementById('my-persona-voice')?.value || '',
         persona_voice_mode: checkedMode('voice'),
         // The older single text: only ever cleared from here (its box shows
         // while it's set), so its mode isn't sent.
         persona_description: document.getElementById('my-persona')?.value || '',
         location: document.getElementById('my-location')?.value || '',
         // Not sent until the list is filled, so a save can't clear the zone
         timezone: myTimezone?.options.length ? myTimezone.value : undefined,
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

   // cb(success, error): error is the server's reason when it refused.
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

      // The server's half of each (read-only)
      const baseValues = document.getElementById('base-values-text');
      if (baseValues) baseValues.textContent = payload.base_values || '(none)';
      const baseVoice = document.getElementById('base-voice-text');
      if (baseVoice) {
         baseVoice.textContent =
            payload.base_voice ||
            '(None. The server uses one persona text, shown under Values. ' +
               'Your voice text is still added.)';
      }

      // Populate form fields
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
         // Keep the derived UI (char counts / clear buttons) in sync with
         // whatever the user has typed, but touch no field values.
         updateAllPersonaBoxes();
         return;
      }

      setPersonaText('my-persona-values', payload.persona_values);
      setPersonaText('my-persona-voice', payload.persona_voice);
      setPersonaText('my-persona', payload.persona_description);
      showLegacyPersona(payload.persona_description, payload.persona_mode);
      if (location) location.value = payload.location || '';
      window.DawnTimezones.fill(
         timezone,
         payload.timezone,
         `Same as server (${payload.server_timezone || 'UTC'})`
      );
      if (realName) realName.value = payload.real_name || '';
      if (aliases) aliases.value = payload.identity_aliases || '';
      if (preferredAddr) preferredAddr.value = payload.preferred_address || '';

      // Set each half's mode radio
      for (const half of ['values', 'voice']) {
         const mode = payload[`persona_${half}_mode`] === 'replace' ? 'replace' : 'append';
         const radio = document.getElementById(`persona-${half}-${mode}`);
         if (radio) radio.checked = true;
         updatePersonaModeSelection(half, mode);
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
         cb(!!payload.success, payload.error);
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

   function checkedMode(half) {
      return (
         document.querySelector(`input[name="persona_${half}_mode"]:checked`)?.value || 'append'
      );
   }

   function updatePersonaModeSelection(half, mode) {
      const block = document.querySelector(`.persona-half[data-half="${half}"]`);
      if (!block) return;
      block.querySelectorAll('.persona-mode-option').forEach((opt) => {
         opt.classList.toggle('selected', opt.dataset.mode === mode);
      });
      const warning = block.querySelector('.replace-warning');
      if (warning) warning.classList.toggle('hidden', mode !== 'replace');
   }

   // A persona box's character count and Clear button follow its text.
   function updatePersonaBox(textarea) {
      const group = textarea.closest('.settings-group');
      if (!group) return;
      const len = textarea.value.length;
      const max = textarea.maxLength; /* every persona box sets maxlength */
      const charCount = group.querySelector('.char-count');
      if (charCount) {
         charCount.textContent = `${len} / ${max}`;
         charCount.classList.toggle('warning', len > max * 0.8 && len < max);
         charCount.classList.toggle('limit', len >= max);
      }
      const clearBtn = group.querySelector('.persona-clear');
      if (clearBtn) clearBtn.classList.toggle('hidden', len === 0);
   }

   function updateAllPersonaBoxes() {
      document.querySelectorAll('#my-settings-form .persona-text').forEach(updatePersonaBox);
   }

   function setPersonaText(id, text) {
      const textarea = document.getElementById(id);
      if (!textarea) return;
      textarea.value = text || '';
      updatePersonaBox(textarea);
   }

   // The older single persona text shows only while the server has one, with
   // what it does now; decided on load, so clearing it doesn't hide the box.
   function showLegacyPersona(text, mode) {
      const block = document.getElementById('persona-legacy');
      if (!block) return;
      block.classList.toggle('hidden', !text);
      const note = document.getElementById('persona-legacy-note');
      if (note) {
         const effect = mode === 'replace' ? 'replaces both bases' : 'is added after both bases';
         note.textContent =
            `While it's here, it ${effect}. Move what you want into Values and Voice ` +
            'above, then clear this.';
      }
   }

   /* =============================================================================
    * Initialization
    * ============================================================================= */

   function init() {
      const form = document.getElementById('my-settings-form');
      const section = document.getElementById('my-settings-section');
      // Each base's expand/collapse toggle
      document.querySelectorAll('#my-settings-form .persona-base-toggle').forEach((btn) => {
         const display = document.getElementById(btn.getAttribute('aria-controls'));
         if (!display) return;
         btn.addEventListener('click', () => {
            const expand = display.classList.contains('collapsed');
            display.classList.toggle('collapsed', !expand);
            display.classList.toggle('expanded', expand);
            btn.setAttribute('aria-expanded', String(expand));
            btn.textContent = expand ? 'Collapse' : 'Show full';
         });
      });

      // Each half's mode radios
      for (const half of ['values', 'voice']) {
         document.querySelectorAll(`input[name="persona_${half}_mode"]`).forEach((radio) => {
            radio.addEventListener('change', (e) => {
               updatePersonaModeSelection(half, e.target.value);
            });
         });
      }

      // Persona boxes: count and Clear follow the text
      document.querySelectorAll('#my-settings-form .persona-text').forEach((textarea) => {
         textarea.addEventListener('input', () => {
            setFormDirty(true);
            updatePersonaBox(textarea);
         });
      });
      document.querySelectorAll('#my-settings-form .persona-clear').forEach((btn) => {
         const textarea = document.getElementById(btn.getAttribute('aria-controls'));
         if (!textarea) return;
         btn.addEventListener('click', () => {
            textarea.value = '';
            setFormDirty(true);
            updatePersonaBox(textarea);
            textarea.focus();
         });
      });

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
