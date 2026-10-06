/**
 * DAWN Settings - Reasoning Controls
 * Renders the Reasoning (mode) and Effort selects from the server's per-model
 * capabilities, so they offer only what the current model accepts.
 *
 * Capabilities (from get_config_response.reasoning_capabilities, a
 * list_llm_models_response model's reasoning_capabilities, or a frame's
 * reasoning_capabilities) look like:
 *   { source, modes: [ { mode, efforts: [...], budget, budget_tokens } ],
 *     default: { mode, effort } }
 * "modes": [] means the model has no reasoning control.
 */
(function () {
   'use strict';

   /* Every effort name, lowest first: nearest-level snapping measures in it. */
   const EFFORT_ORDER = ['none', 'minimal', 'low', 'medium', 'high', 'xhigh', 'max'];
   const EFFORT_LABELS = {
      none: 'None',
      minimal: 'Minimal',
      low: 'Low',
      medium: 'Medium',
      high: 'High',
      xhigh: 'Extra high',
      max: 'Max',
   };
   const MODE_TITLES = {
      disabled: 'No reasoning',
      adaptive: 'The model decides how much to think, up to the effort level',
      budget: 'A fixed thinking budget per request',
      enabled: 'Reasoning at the chosen effort level',
   };

   let cloudCaps = {}; /* { provider: { model: caps } } */
   let localCaps = {}; /* { model: caps } */

   function setCloudCapabilities(map) {
      cloudCaps = map && typeof map === 'object' ? map : {};
   }

   function setLocalCapabilities(models) {
      localCaps = {};
      (models || []).forEach((m) => {
         if (m && m.name && m.reasoning_capabilities) {
            localCaps[m.name] = m.reasoning_capabilities;
         }
      });
   }

   /** The capabilities for a model, or null if the server hasn't said. */
   function capsFor(type, provider, model) {
      if (!model) return null;
      if (type === 'local') return localCaps[model] || null;
      const byModel = cloudCaps[(provider || '').toLowerCase()];
      return (byModel && byModel[model]) || null;
   }

   function modeEntry(caps, mode) {
      return (caps && caps.modes.find((m) => m.mode === mode)) || null;
   }

   function modeLabel(entry) {
      if (entry.mode === 'disabled') return 'Off';
      if (entry.mode === 'adaptive') return 'Adaptive';
      return entry.budget ? 'Budget' : 'On';
   }

   function effortLabel(effort) {
      return EFFORT_LABELS[effort] || effort;
   }

   function tokenLabel(tokens) {
      return tokens >= 1024 ? `${Math.round(tokens / 1024)}K` : `${tokens}`;
   }

   /** The offered effort nearest @p wanted (ties go lower); '' if none offered. */
   function nearestEffort(efforts, wanted) {
      if (!efforts || efforts.length === 0) return '';
      if (efforts.includes(wanted)) return wanted;
      let want = EFFORT_ORDER.indexOf(wanted);
      if (want < 0) want = EFFORT_ORDER.indexOf('medium');
      let best = efforts[0];
      let bestDistance = Infinity;
      efforts.forEach((e) => {
         const distance = Math.abs(EFFORT_ORDER.indexOf(e) - want);
         if (distance < bestDistance) {
            best = e;
            bestDistance = distance;
         }
      });
      return best;
   }

   function fillSelect(select, items) {
      while (select.firstChild) select.removeChild(select.firstChild);
      items.forEach(({ value, label }) => {
         const opt = document.createElement('option');
         opt.value = value;
         opt.textContent = label;
         select.appendChild(opt);
      });
   }

   function setHint(text) {
      const hint = document.getElementById('effort-hint');
      if (!hint) return;
      hint.textContent = text || '';
      hint.classList.toggle('hidden', !text);
   }

   /** The effort a mode should carry after switching to it: the current pick,
    * snapped to what the mode offers. */
   function effortForMode(caps, mode, currentEffort) {
      /* A mode with no levels (Off, Ollama's On) keeps the pick for later. */
      const entry = modeEntry(caps, mode);
      return entry && entry.efforts.length
         ? nearestEffort(entry.efforts, currentEffort)
         : currentEffort;
   }

   /**
    * Render both selects for @p caps with @p mode / @p effort selected.
    * @returns {{mode: string, effort: string}} what the selects now show
    */
   function render(caps, mode, effort) {
      const modeSelect = document.getElementById('reasoning-mode-select');
      const effortSelect = document.getElementById('reasoning-effort-select');
      if (!modeSelect || !effortSelect) return { mode, effort };

      if (!caps) {
         /* The server hasn't described this model yet: the plain choices. */
         fillSelect(modeSelect, [
            { value: 'disabled', label: 'Off' },
            { value: 'enabled', label: 'On' },
         ]);
         modeSelect.value = mode === 'disabled' ? 'disabled' : 'enabled';
         modeSelect.disabled = false;
         modeSelect.title = MODE_TITLES[modeSelect.value];
         fillSelect(
            effortSelect,
            ['low', 'medium', 'high'].map((e) => ({ value: e, label: EFFORT_LABELS[e] }))
         );
         effortSelect.value = nearestEffort(['low', 'medium', 'high'], effort);
         effortSelect.disabled = modeSelect.value === 'disabled';
         setHint(effortSelect.disabled ? 'Turn reasoning on first' : null);
         return { mode: modeSelect.value, effort: effortSelect.value };
      }

      if (caps.modes.length === 0) {
         fillSelect(modeSelect, [{ value: 'disabled', label: 'Unsupported' }]);
         modeSelect.disabled = true;
         modeSelect.title = 'This model has no reasoning setting';
         fillSelect(effortSelect, [{ value: '', label: '\u2014' }]);
         effortSelect.disabled = true;
         setHint('This model has no reasoning setting');
         return { mode: 'disabled', effort };
      }

      modeSelect.disabled = false;
      fillSelect(
         modeSelect,
         caps.modes.map((m) => ({ value: m.mode, label: modeLabel(m) }))
      );
      let entry = modeEntry(caps, mode) || modeEntry(caps, caps.default && caps.default.mode);
      if (!entry) entry = caps.modes[0];
      modeSelect.value = entry.mode;
      modeSelect.title = MODE_TITLES[entry.budget ? 'budget' : entry.mode];

      /* While reasoning is off, show the levels it would use (disabled), so the
       * pick stays visible for when it's turned on. */
      const levels =
         entry.mode === 'disabled' ? caps.modes.find((m) => m.mode !== 'disabled') || entry : entry;
      fillSelect(
         effortSelect,
         levels.efforts.map((e) => ({ value: e, label: effortLabel(e) }))
      );
      const shown = nearestEffort(levels.efforts, effort);
      if (shown) effortSelect.value = shown;
      effortSelect.disabled = entry.mode === 'disabled' || levels.efforts.length === 0;
      if (entry.mode === 'disabled') {
         setHint('Turn reasoning on first');
      } else if (levels.efforts.length === 0) {
         setHint('On or off only');
      } else if (entry.budget && entry.budget_tokens && entry.budget_tokens[shown]) {
         setHint(`Thinking budget: ${tokenLabel(entry.budget_tokens[shown])} tokens`);
      } else {
         setHint(null);
      }
      /* While reasoning is off the effort keeps the pick, for when it's on. */
      return { mode: entry.mode, effort: shown || effort };
   }

   /**
    * The selects' current reasoning in words ("Adaptive, Low", "Off"), for the
    * collapsed summary and adjustment notices.
    */
   function summary() {
      const modeSelect = document.getElementById('reasoning-mode-select');
      const effortSelect = document.getElementById('reasoning-effort-select');
      const mode = modeSelect && modeSelect.options[modeSelect.selectedIndex];
      if (!mode) return '';
      const effort = effortSelect && effortSelect.options[effortSelect.selectedIndex];
      if (modeSelect.value === 'disabled' || !effort || effortSelect.disabled) {
         return mode.text;
      }
      return `${mode.text}, ${effort.text}`;
   }

   window.DawnReasoning = {
      summary,
      setCloudCapabilities,
      setLocalCapabilities,
      capsFor,
      effortForMode,
      render,
   };
})();
