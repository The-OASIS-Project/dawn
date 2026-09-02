/**
 * DAWN Messaging Channels Module
 *
 * User-scoped management of the current user's linked messaging channels
 * (list / generate link code / unlink / rename / re-enable).  Mirrors the
 * satellite admin panel's state-array + DawnWS request + response-handler +
 * re-render shape, but is USER-scoped (the backend filters by the
 * authenticated user), so it lives in the always-visible settings area
 * rather than an admin-only section.  Operations key on the stable row id;
 * display_name is editable in place.  See docs/MESSAGING_CHANNELS_DESIGN.md
 * §13 Phase 6.
 */
(function () {
   'use strict';

   let channels = [];
   let refreshInterval = null;
   let codeCountdownTimer = null;

   const REFRESH_INTERVAL_MS = 30000;

   /* =============================================================================
    * API Requests
    * ============================================================================= */

   function wsReady() {
      return typeof DawnWS !== 'undefined' && DawnWS.isConnected();
   }

   function requestList() {
      if (!wsReady()) return;
      const list = document.getElementById('channel-list');
      if (list && channels.length === 0) {
         list.innerHTML = '<div class="loading-indicator">Loading channels...</div>';
      }
      DawnWS.send({ type: 'list_channels' });
   }

   function requestCreateCode(provider) {
      if (!wsReady()) return;
      DawnWS.send({ type: 'create_link_code', payload: provider ? { provider } : {} });
   }

   function requestUnlink(id) {
      if (!wsReady()) return;
      DawnWS.send({ type: 'unlink_channel', payload: { id } });
   }

   function requestRename(id, name) {
      if (!wsReady()) return;
      DawnWS.send({ type: 'rename_channel', payload: { id, name } });
   }

   function requestReenable(id) {
      if (!wsReady()) return;
      DawnWS.send({ type: 'reenable_channel', payload: { id } });
   }

   /* Persist a per-channel LLM change to the channel's conversation row.  The
    * backend overwrites ALL llm_* columns, so we send the channel's full current
    * state with `overrides` merged in — that preserves the model/provider (changed
    * in chat via the switch-model tool) when only reasoning is edited here. */
   function requestSetChannelLlm(ch, overrides) {
      if (!wsReady() || !ch || !ch.conversation_id) return;
      DawnWS.send({
         type: 'set_channel_llm',
         payload: {
            conversation_id: ch.conversation_id,
            llm_type: ch.llm_type || '',
            cloud_provider: ch.cloud_provider || '',
            model: ch.model || '',
            thinking_mode:
               overrides.thinking_mode !== undefined
                  ? overrides.thinking_mode
                  : ch.thinking_mode || '',
            reasoning_effort:
               overrides.reasoning_effort !== undefined
                  ? overrides.reasoning_effort
                  : ch.reasoning_effort || '',
         },
      });
   }

   /* =============================================================================
    * Helpers
    * ============================================================================= */

   function escapeHtml(str) {
      return DawnFormat.escapeHtml(str);
   }

   // Quote-safe escaper for HTML attribute values (escapeHtml does NOT
   // escape quotes — using it inside value="…"/data-…="…" is an attribute
   // breakout / stored-XSS vector since display_name is user-controlled).
   function escapeAttr(str) {
      return DawnFormat.escapeAttr(str);
   }

   function formatLastUsed(timestamp) {
      if (!timestamp) return 'Never';
      const now = Math.floor(Date.now() / 1000);
      const diff = now - timestamp;
      if (diff < 60) return 'Just now';
      if (diff < 3600) return Math.floor(diff / 60) + 'm ago';
      if (diff < 86400) return Math.floor(diff / 3600) + 'h ago';
      return Math.floor(diff / 86400) + 'd ago';
   }

   /* =============================================================================
    * Rendering
    * ============================================================================= */

   // Build one <option>; `sel` marks the current value as selected.
   function opt(value, label, current) {
      return (
         '<option value="' +
         escapeAttr(value) +
         '"' +
         (value === current ? ' selected' : '') +
         '>' +
         escapeHtml(label) +
         '</option>'
      );
   }

   // Resolve the global LLM defaults that an empty ("Default") per-channel value
   // inherits, so the dropdown can say what "Default" actually means.
   function globalLlmDefaults() {
      let mode = '';
      let effort = '';
      try {
         const cfg = window.DawnSettings && DawnSettings.getConfig && DawnSettings.getConfig();
         if (cfg && cfg.llm && cfg.llm.thinking) {
            mode = cfg.llm.thinking.mode || '';
            effort = cfg.llm.thinking.reasoning_effort || '';
         }
      } catch (e) {
         /* config not loaded yet — fall back to a bare "Default" label */
      }
      return { mode, effort };
   }

   function capitalize(s) {
      return s ? s.charAt(0).toUpperCase() + s.slice(1) : s;
   }

   // Resolve the model name a channel with an empty ("Default") model inherits.
   // Mirrors the daemon's resolution: local → local.model; gateway → the
   // OpenRouter default; otherwise the (channel-override-or-global) provider's
   // default model. Returns '' when not knowable client-side (e.g. provider is
   // auto-detected at runtime), so the caller falls back to a generic label.
   function resolveDefaultModel(ch) {
      let cfg = null;
      try {
         cfg = window.DawnSettings && DawnSettings.getConfig && DawnSettings.getConfig();
      } catch (e) {
         /* config not loaded yet */
      }
      if (!cfg || !cfg.llm) return '';
      const llm = cfg.llm;
      const cloud = llm.cloud || {};
      const type = ch.llm_type || llm.type || 'cloud';
      if (type === 'local') return (llm.local && llm.local.model) || '';
      const p = (ch.cloud_provider || cloud.provider || '').toLowerCase();
      const pick = (arr, idx) => (arr || [])[idx || 0] || (arr || [])[0] || '';
      if (p === 'claude') return pick(cloud.claude_models, cloud.claude_default_model_idx);
      if (p === 'gemini') return pick(cloud.gemini_models, cloud.gemini_default_model_idx);
      if (p === 'openai') return pick(cloud.openai_models, cloud.openai_default_model_idx);
      if (p === 'openrouter')
         return pick(cloud.openrouter_models, cloud.openrouter_default_model_idx);
      return ''; // auto/unknown provider — resolved at runtime, not knowable here
   }

   /* Per-channel LLM controls.  The model itself is changed in chat via the
    * switch-model tool (shown read-only here); reasoning is editable and persists
    * to this channel's conversation. */
   function renderLlmControls(ch) {
      // Legacy rows may store "auto"; show it as "On".
      const think = ch.thinking_mode === 'auto' ? 'enabled' : ch.thinking_mode || '';
      const effort = ch.reasoning_effort || '';
      // Show the explicit model if set, else resolve what "Default" inherits.
      let modelLabel;
      if (ch.model) {
         modelLabel = ch.model;
      } else {
         const resolved = resolveDefaultModel(ch);
         modelLabel = resolved ? 'Default (' + resolved + ')' : 'Default (global LLM)';
      }

      // "Default" = inherit the global setting; show its resolved value when known.
      const g = globalLlmDefaults();
      const gThink = g.mode === 'disabled' ? 'Off' : g.mode ? 'On' : '';
      const thinkDefaultLabel = gThink ? 'Default (' + gThink + ')' : 'Default';
      const effortDefaultLabel = g.effort ? 'Default (' + capitalize(g.effort) + ')' : 'Default';

      // The backend accepts none/minimal/low/medium/high/xhigh, but this panel only
      // offers low/medium/high.  If the stored value is one we don't list (e.g. set
      // via the main per-conversation control), preserve it as an extra option so it
      // stays selected — otherwise it would show "Default" and a later edit would
      // silently overwrite it with inherit.
      const effortStd = ['low', 'medium', 'high'];
      const effortExtra =
         effort && !effortStd.includes(effort) ? opt(effort, capitalize(effort), effort) : '';

      const tip =
         'Reasoning for this channel’s conversation. "Default" inherits the system’s global LLM ' +
         'setting. Changing it updates the ongoing conversation (applies on its next message). ' +
         'To change the model itself, ask the assistant in chat (e.g. “switch to Claude”).';
      return (
         '<div class="channel-llm" title="' +
         escapeAttr(tip) +
         '">' +
         '<span class="channel-llm-model" title="Change the model in chat via the switch-model tool">Model: ' +
         escapeHtml(modelLabel) +
         '</span>' +
         '<label class="channel-llm-field">Reasoning ' +
         '<select class="channel-llm-thinking" data-id="' +
         ch.id +
         '">' +
         opt('', thinkDefaultLabel, think) +
         opt('disabled', 'Off', think) +
         opt('enabled', 'On', think) +
         '</select></label>' +
         '<label class="channel-llm-field">Effort ' +
         '<select class="channel-llm-effort" data-id="' +
         ch.id +
         '">' +
         opt('', effortDefaultLabel, effort) +
         effortExtra +
         opt('low', 'Low', effort) +
         opt('medium', 'Medium', effort) +
         opt('high', 'High', effort) +
         '</select></label>' +
         '</div>'
      );
   }

   function renderList() {
      const list = document.getElementById('channel-list');
      if (!list) return;

      // Don't tear down a dropdown the user is actively using — the 30s
      // auto-refresh would otherwise close it and drop focus mid-interaction.
      // Defer; the user's own change (or the next refresh) re-renders.
      const active = document.activeElement;
      if (
         active &&
         active.matches &&
         active.matches('.channel-llm-field select') &&
         list.contains(active)
      ) {
         return;
      }

      if (channels.length === 0) {
         list.innerHTML =
            '<div class="channel-list-empty">' +
            'No channels linked yet. Use "Link a channel" below to connect ' +
            'Telegram, Slack, Discord, or SMS.' +
            '</div>';
         return;
      }

      let html = '';
      for (const ch of channels) {
         const enabled = ch.enabled !== false;
         // provider_available: the provider's driver is loaded (false when e.g.
         // its bot token isn't configured). Absent → assume available (back-compat).
         const available = ch.provider_available !== false;
         let dotClass, textClass, statusLabel;
         if (!enabled) {
            dotClass = '';
            textClass = 'offline';
            statusLabel = 'Unlinked';
         } else if (!available) {
            dotClass = 'warning';
            textClass = 'offline';
            statusLabel = 'Not connected';
         } else {
            dotClass = 'success';
            textClass = 'online';
            statusLabel = 'Active';
         }
         html +=
            '<div class="channel-card' +
            (enabled ? '' : ' channel-disabled') +
            '" data-id="' +
            ch.id +
            '">' +
            '<div class="channel-header">' +
            // Decorative — the visible .channel-status-text below already conveys
            // the status, so hide the dot from screen readers to avoid a double
            // announcement.  Keep title for the sighted hover tooltip.
            '<span class="dawn-status-dot ' +
            dotClass +
            '" title="' +
            statusLabel +
            '" aria-hidden="true"></span>' +
            (enabled
               ? '<input type="text" class="channel-name" data-id="' +
                 ch.id +
                 '" value="' +
                 escapeAttr(ch.name) +
                 '" title="Click to rename" aria-label="Channel name (editable)">'
               : '<span class="channel-name-static">' + escapeHtml(ch.name) + '</span>') +
            '<span class="dawn-badge">' +
            escapeHtml(ch.provider) +
            '</span>' +
            '<span class="channel-status-text ' +
            textClass +
            '">' +
            statusLabel +
            '</span>' +
            '</div>' +
            '<div class="channel-meta">Last active: ' +
            formatLastUsed(ch.last_used_at) +
            '</div>' +
            (enabled && !available
               ? '<div class="channel-warning">This channel’s ' +
                 escapeHtml(ch.provider) +
                 ' driver isn’t running — add its bot token in Settings → Secrets and restart ' +
                 'the daemon. Messages won’t be received until then.</div>'
               : '') +
            (enabled && ch.conversation_id ? renderLlmControls(ch) : '') +
            '<div class="channel-controls">' +
            (enabled
               ? '<button class="btn btn-secondary channel-unlink-btn" data-id="' +
                 ch.id +
                 '" data-name="' +
                 escapeAttr(ch.name) +
                 '">Unlink</button>'
               : '<button class="btn btn-primary channel-reenable-btn" data-id="' +
                 ch.id +
                 '">Re-enable</button>') +
            '</div>' +
            '</div>';
      }

      list.innerHTML = html;
      attachListeners();
   }

   function attachListeners() {
      // Inline rename: edit the name field, blur or Enter to commit.
      document.querySelectorAll('.channel-name').forEach((el) => {
         el.addEventListener('blur', function () {
            const id = parseInt(this.dataset.id, 10);
            const ch = channels.find((c) => c.id === id);
            const newName = this.value.trim();
            if (ch && newName && newName !== ch.name) {
               requestRename(id, newName);
            } else if (ch && !newName) {
               this.value = ch.name; // reject empty rename
            }
         });
         el.addEventListener('keydown', function (e) {
            if (e.key === 'Enter') this.blur();
         });
      });

      document.querySelectorAll('.channel-unlink-btn').forEach((btn) => {
         btn.addEventListener('click', async function () {
            const id = parseInt(this.dataset.id, 10);
            const name = this.dataset.name;
            const msg =
               'Unlink channel "' +
               name +
               '"?\nIt will stop reaching the assistant. You can re-enable it ' +
               'later — the conversation history is preserved.';
            if (
               await DawnDialog.confirm(msg, {
                  title: 'Unlink Channel',
                  okText: 'Unlink',
                  danger: true,
               })
            ) {
               requestUnlink(id);
            }
         });
      });

      document.querySelectorAll('.channel-reenable-btn').forEach((btn) => {
         btn.addEventListener('click', function () {
            requestReenable(parseInt(this.dataset.id, 10));
         });
      });

      document.querySelectorAll('.channel-llm-thinking').forEach((sel) => {
         sel.addEventListener('change', function () {
            const ch = channels.find((c) => c.id === parseInt(this.dataset.id, 10));
            if (ch) requestSetChannelLlm(ch, { thinking_mode: this.value });
         });
      });

      document.querySelectorAll('.channel-llm-effort').forEach((sel) => {
         sel.addEventListener('change', function () {
            const ch = channels.find((c) => c.id === parseInt(this.dataset.id, 10));
            if (ch) requestSetChannelLlm(ch, { reasoning_effort: this.value });
         });
      });
   }

   /* =============================================================================
    * Link-code inline panel
    * ============================================================================= */

   function showLinkCode(payload) {
      const box = document.getElementById('channel-link-code');
      if (!box) return;
      const code = payload.code || '';
      const provider = payload.provider || '';
      let ttl = payload.ttl_seconds || 0;
      const sendForm = (provider === 'slack' ? 'link ' : '/link ') + code;
      const instr = provider
         ? 'Send <code>' +
           escapeHtml(sendForm) +
           '</code> ' +
           (provider === 'sms'
              ? 'as a text to the DAWN number'
              : 'to the bot on ' + escapeHtml(provider)) +
           '.'
         : 'Send <code>/link ' +
           escapeHtml(code) +
           '</code> from the chat client (use "link ' +
           escapeHtml(code) +
           '" on Slack).';

      box.innerHTML =
         '<div class="channel-code-row">' +
         '<input class="channel-code-display" type="text" readonly value="' +
         escapeAttr(code) +
         '" aria-label="Link code">' +
         '<button class="btn btn-secondary channel-code-copy">Copy</button>' +
         '<span class="channel-code-ttl"></span>' +
         '</div>' +
         '<div class="channel-code-instr">' +
         instr +
         '</div>';
      box.classList.remove('hidden');

      const ttlEl = box.querySelector('.channel-code-ttl');
      const copyBtn = box.querySelector('.channel-code-copy');
      if (copyBtn) {
         copyBtn.addEventListener('click', function () {
            DawnFormat.copyToClipboard(code)
               .then(function () {
                  if (typeof DawnToast !== 'undefined')
                     DawnToast.show('Link code copied', 'success');
               })
               .catch(function () {
                  /* clipboard denied — the code is still selectable in the field */
               });
         });
      }

      if (codeCountdownTimer) clearInterval(codeCountdownTimer);
      function tick() {
         if (!ttlEl) return;
         if (ttl <= 0) {
            ttlEl.textContent = 'Expired — generate a new code.';
            clearInterval(codeCountdownTimer);
            codeCountdownTimer = null;
            return;
         }
         const m = Math.floor(ttl / 60);
         const s = ttl % 60;
         ttlEl.textContent = 'Expires in ' + m + ':' + (s < 10 ? '0' : '') + s;
         ttl--;
      }
      tick();
      codeCountdownTimer = setInterval(tick, 1000);
   }

   /* =============================================================================
    * Response Handlers
    * ============================================================================= */

   function handleListResponse(payload) {
      if (payload && Array.isArray(payload.channels)) {
         channels = payload.channels;
      }
      renderList();
   }

   function handleCreateCodeResponse(payload) {
      if (payload) showLinkCode(payload);
   }

   /* Unlink / rename / re-enable success → re-fetch the authoritative list.
    * On failure the daemon sends a generic error toast and the list is left
    * unchanged, so there is no stuck UI to reset. */
   function handleMutationResponse() {
      requestList();
   }

   /* Per-channel LLM change: the select already shows the new value optimistically,
    * so confirm the persist with a toast (the only visible success signal) and
    * re-fetch the authoritative list. */
   function handleSetChannelLlmResponse(payload) {
      if (payload && payload.success && typeof DawnToast !== 'undefined') {
         DawnToast.show('Channel settings updated', 'success');
      }
      requestList();
   }

   /* =============================================================================
    * Auto-Refresh
    * ============================================================================= */

   function startAutoRefresh() {
      stopAutoRefresh();
      refreshInterval = setInterval(requestList, REFRESH_INTERVAL_MS);
   }

   function stopAutoRefresh() {
      if (refreshInterval) {
         clearInterval(refreshInterval);
         refreshInterval = null;
      }
   }

   /* =============================================================================
    * Initialization
    * ============================================================================= */

   function init() {
      const section = document.getElementById('messaging-channels-section');
      if (!section) return;

      const header = section.querySelector('.section-header');
      if (header) {
         header.addEventListener('click', function () {
            // Run after the generic settings toggle flips 'collapsed'.
            setTimeout(function () {
               if (!section.classList.contains('collapsed')) {
                  if (channels.length === 0) requestList();
                  startAutoRefresh();
               } else {
                  stopAutoRefresh();
               }
            }, 0);
         });
      }

      const refreshBtn = document.getElementById('refresh-channels-btn');
      if (refreshBtn) refreshBtn.addEventListener('click', requestList);

      const genBtn = document.getElementById('channel-generate-code-btn');
      const provSel = document.getElementById('channel-provider-select');
      if (genBtn) {
         genBtn.addEventListener('click', function () {
            requestCreateCode(provSel ? provSel.value : '');
         });
      }
   }

   if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', init);
   } else {
      init();
   }

   /* =============================================================================
    * Public API
    * ============================================================================= */

   window.DawnMessaging = {
      handleListResponse: handleListResponse,
      handleCreateCodeResponse: handleCreateCodeResponse,
      handleMutationResponse: handleMutationResponse,
      handleSetChannelLlmResponse: handleSetChannelLlmResponse,
      handleReconnect: function () {
         stopAutoRefresh();
         renderList();
         const section = document.getElementById('messaging-channels-section');
         if (section && !section.classList.contains('collapsed')) {
            requestList();
            startAutoRefresh();
         }
      },
      refresh: requestList,
      stopAutoRefresh: stopAutoRefresh,
   };
})();
