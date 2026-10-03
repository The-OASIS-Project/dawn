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
   // SMS verification, per channel id: the request in flight ('verify' or
   // 'resend') and its safety timer, and the refusal shown on the code box
   // ({ message, invalid }).  Kept here, not only in the DOM, so a re-render
   // (a refresh, a server push) keeps them.  Plus the element to focus after
   // the next render and the per-second expiry tick.
   const verifyInFlight = new Map();
   const verifyTimers = new Map();
   const verifyErrors = new Map();
   // { sel, until }: the element to focus once a render shows it (kept until
   // then, briefly, since an older list response may render first).
   let focusAfterRender = null;
   const FOCUS_WAIT_MS = 5000;
   let verifyTtlTimer = null;
   const VERIFY_IN_FLIGHT_MS = 10000;

   const REFRESH_INTERVAL_MS = 30000;

   /* =============================================================================
    * API Requests
    * ============================================================================= */

   function wsReady() {
      return typeof DawnWS !== 'undefined' && DawnWS.isConnected();
   }

   let forceNextRender = false;

   // force: re-render even if a control in the list has focus (the user's own
   // action asked for the fresh list).
   function requestList(force) {
      if (!wsReady()) return;
      if (force === true) forceNextRender = true;
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

   function requestVerify(id, code) {
      if (!wsReady()) return false;
      DawnWS.send({ type: 'verify_channel', payload: { id, code } });
      return true;
   }

   function requestResend(id) {
      if (!wsReady()) return false;
      DawnWS.send({ type: 'resend_channel_code', payload: { id } });
      return true;
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

   function renderList(force) {
      const list = document.getElementById('channel-list');
      if (!list) return;

      // Don't tear down a dropdown, code box or name field the user is actively
      // using — a refresh or server push would otherwise close it and drop
      // focus (and a half-typed code or name) mid-interaction. Defer; the
      // user's own change (or the next refresh) re-renders.  Code boxes are
      // still brought up to date in place (a new or voided code's countdown).
      const active = document.activeElement;
      if (
         !force &&
         active &&
         active.matches &&
         active.matches('.channel-llm-field select, .channel-verify-input, .channel-name') &&
         list.contains(active)
      ) {
         refreshVerifyInPlace();
         return;
      }
      // A forced render keeps digits typed into any code box.
      const typed = {};
      list.querySelectorAll('.channel-verify-input').forEach((el) => {
         if (el.value) typed[el.dataset.id] = el.value;
      });

      if (channels.length === 0) {
         list.innerHTML =
            '<div class="channel-list-empty">' +
            'No channels linked yet. Use "Link a channel" below to connect ' +
            'Telegram, Slack, Discord, or SMS.' +
            '</div>';
         return;
      }

      let html = '';
      // A channel waiting for its SMS code needs the user, so it comes first.
      const ordered = channels
         .filter((c) => c.enabled !== false && c.verified === false)
         .concat(channels.filter((c) => !(c.enabled !== false && c.verified === false)));
      for (const ch of ordered) {
         const enabled = ch.enabled !== false;
         // provider_available: the provider's driver is loaded (false when e.g.
         // its bot token isn't configured). Absent → assume available (back-compat).
         const available = ch.provider_available !== false;
         // verified false: an SMS link waiting for the code DAWN texted to the
         // number. Absent → verified (back-compat).
         const pending = enabled && ch.verified === false;
         let dotClass, textClass, statusLabel;
         if (!enabled) {
            dotClass = '';
            textClass = 'offline';
            statusLabel = 'Unlinked';
         } else if (pending) {
            dotClass = 'warning';
            textClass = 'offline';
            statusLabel = 'Waiting for code';
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
            (pending ? ' channel-pending' : '') +
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
            (pending ? renderVerify(ch) : '') +
            (enabled && !pending && !available
               ? '<div class="channel-warning">This channel’s ' +
                 escapeHtml(ch.provider) +
                 ' driver isn’t running — add its bot token in Settings → Secrets and restart ' +
                 'the daemon. Messages won’t be received until then.</div>'
               : '') +
            (enabled && !pending && ch.conversation_id ? renderLlmControls(ch) : '') +
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
      Object.keys(typed).forEach((id) => {
         const el = document.getElementById('channel-verify-' + id);
         if (el) el.value = typed[id];
      });
      attachListeners();
      startVerifyTtlTicker();
      if (focusAfterRender !== null) {
         const el = list.querySelector(focusAfterRender.sel);
         if (el) {
            focusAfterRender = null;
            el.focus();
            el.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
         } else if (Date.now() > focusAfterRender.until) {
            focusAfterRender = null;
         }
      }
   }

   // Labels of the code box's buttons for its current state (visible text and
   // accessible name kept in step).
   function verifyLabels(id, name) {
      const busy = verifyInFlight.get(id);
      return {
         verify: busy === 'verify' ? 'Verifying…' : 'Verify',
         verifyAria: (busy === 'verify' ? 'Verifying code for ' : 'Verify code for ') + name,
         resend: busy === 'resend' ? 'Sending…' : 'Send a new code',
         resendAria: (busy === 'resend' ? 'Sending a new code to ' : 'Send a new code to ') + name,
      };
   }

   // The code box of an SMS channel waiting for the code DAWN texted to it.
   function renderVerify(ch) {
      const id = Number(ch.id) | 0;
      const busy = verifyInFlight.has(id);
      const error = verifyErrors.get(id);
      const l = verifyLabels(id, ch.name);
      return (
         '<div class="channel-verify" data-id="' +
         id +
         '" data-name="' +
         escapeAttr(ch.name) +
         '">' +
         '<label class="channel-verify-label" for="channel-verify-' +
         id +
         '">Enter the 6-digit code DAWN texted to this number.</label>' +
         '<div class="channel-code-row">' +
         '<input class="dawn-input channel-verify-input" id="channel-verify-' +
         id +
         '" data-id="' +
         id +
         '" type="text" inputmode="numeric" autocomplete="one-time-code" maxlength="7" ' +
         'placeholder="123456" aria-describedby="channel-verify-ttl-' +
         id +
         ' channel-verify-err-' +
         id +
         '"' +
         (busy ? ' readonly' : '') +
         (error && error.invalid ? ' aria-invalid="true"' : '') +
         '>' +
         '<button type="button" class="btn btn-primary channel-verify-btn" data-id="' +
         id +
         '" aria-label="' +
         escapeAttr(l.verifyAria) +
         '"' +
         (busy ? ' disabled' : '') +
         '>' +
         escapeHtml(l.verify) +
         '</button>' +
         '<button type="button" class="btn btn-secondary channel-resend-btn" data-id="' +
         id +
         '" aria-label="' +
         escapeAttr(l.resendAria) +
         '"' +
         (busy ? ' disabled' : '') +
         '>' +
         escapeHtml(l.resend) +
         '</button>' +
         '</div>' +
         '<span class="channel-code-ttl channel-verify-ttl" id="channel-verify-ttl-' +
         id +
         '" data-expires="' +
         (Number(ch.verify_expires_local) || 0) +
         '"></span>' +
         '<div class="channel-verify-error" id="channel-verify-err-' +
         id +
         '" role="alert"' +
         (error ? '>' + escapeHtml(error.message) : ' hidden>') +
         '</div>' +
         '</div>'
      );
   }

   // Bring one code box in line with its state, in place (a full re-render
   // would take focus from whatever the user is doing elsewhere).
   // @return false when the box isn't on screen.
   function paintVerify(id) {
      const box = document.querySelector('.channel-verify[data-id="' + id + '"]');
      if (!box) return false;
      const busy = verifyInFlight.has(id);
      const error = verifyErrors.get(id);
      const l = verifyLabels(id, box.dataset.name || '');
      const input = box.querySelector('.channel-verify-input');
      const vbtn = box.querySelector('.channel-verify-btn');
      const rbtn = box.querySelector('.channel-resend-btn');
      const err = box.querySelector('.channel-verify-error');
      if (input) {
         input.readOnly = busy;
         if (error && error.invalid) input.setAttribute('aria-invalid', 'true');
         else input.removeAttribute('aria-invalid');
      }
      if (vbtn) {
         vbtn.disabled = busy;
         vbtn.textContent = l.verify;
         vbtn.setAttribute('aria-label', l.verifyAria);
      }
      if (rbtn) {
         rbtn.disabled = busy;
         rbtn.textContent = l.resend;
         rbtn.setAttribute('aria-label', l.resendAria);
      }
      if (err) {
         err.textContent = error ? error.message : ''; // server text: textContent only
         err.hidden = !error;
      }
      return true;
   }

   // Without a full render: each shown code box takes its channel's current
   // code deadline and state.
   function refreshVerifyInPlace() {
      channels.forEach((c) => {
         const id = Number(c.id) | 0;
         const ttl = document.getElementById('channel-verify-ttl-' + id);
         if (ttl) ttl.dataset.expires = String(Number(c.verify_expires_local) || 0);
         paintVerify(id);
      });
      updateVerifyTtls();
   }

   // Expiry countdown on every code box, ticking once a second while any is shown.
   function updateVerifyTtls() {
      const els = document.querySelectorAll('.channel-verify-ttl');
      if (els.length === 0) {
         clearInterval(verifyTtlTimer);
         verifyTtlTimer = null;
         return;
      }
      const now = Math.floor(Date.now() / 1000);
      els.forEach((el) => {
         const expires = parseInt(el.dataset.expires, 10) || 0;
         const left = expires - now;
         if (expires === 0) {
            el.textContent = 'No code sent yet. Use “Send a new code”.';
         } else if (left <= 0) {
            el.textContent = 'This code has expired. Use “Send a new code”.';
         } else {
            const m = Math.floor(left / 60);
            const sec = left % 60;
            el.textContent = 'Expires in ' + m + ':' + (sec < 10 ? '0' : '') + sec;
         }
      });
   }

   function startVerifyTtlTicker() {
      updateVerifyTtls();
      if (!verifyTtlTimer && document.querySelector('.channel-verify-ttl')) {
         verifyTtlTimer = setInterval(updateVerifyTtls, 1000);
      }
   }

   function startVerifyRequest(id, kind) {
      verifyInFlight.set(id, kind);
      verifyErrors.delete(id);
      clearTimeout(verifyTimers.get(id));
      // Safety: a lost response (socket dropped) must not leave the box locked,
      // nor leave the user thinking the code was checked.
      verifyTimers.set(
         id,
         setTimeout(function () {
            finishVerifyRequest(id);
            showVerifyError(id, 'No answer from DAWN. Try again.', false);
         }, VERIFY_IN_FLIGHT_MS)
      );
      paintVerify(id);
   }

   function finishVerifyRequest(id) {
      verifyInFlight.delete(id);
      clearTimeout(verifyTimers.get(id));
      verifyTimers.delete(id);
   }

   // Show a refusal on the channel's code box; `invalid` marks the digits as
   // the problem (and selects them to retype).  With no box on screen the
   // refusal still reaches the user, as a toast.
   function showVerifyError(id, message, invalid) {
      verifyErrors.set(id, { message: message, invalid: invalid });
      if (!paintVerify(id)) {
         if (typeof DawnToast !== 'undefined') DawnToast.show(message, 'error');
         return;
      }
      const input = document.getElementById('channel-verify-' + id);
      if (input && invalid) {
         input.focus();
         input.select();
      }
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

      function submitVerify(id) {
         if (verifyInFlight.has(id)) return;
         const input = document.getElementById('channel-verify-' + id);
         const digits = input ? input.value.replace(/[\s-]/g, '') : '';
         if (!/^\d{6}$/.test(digits)) {
            showVerifyError(id, 'Enter the 6 digits from the text.', true);
            return;
         }
         if (requestVerify(id, digits)) startVerifyRequest(id, 'verify');
      }
      document.querySelectorAll('.channel-verify-btn').forEach((btn) => {
         btn.addEventListener('click', function () {
            submitVerify(parseInt(this.dataset.id, 10));
         });
      });
      document.querySelectorAll('.channel-verify-input').forEach((el) => {
         el.addEventListener('keydown', function (e) {
            if (e.key === 'Enter') submitVerify(parseInt(this.dataset.id, 10));
         });
         // Retyping clears the refusal.
         el.addEventListener('input', function () {
            const id = parseInt(this.dataset.id, 10);
            if (verifyErrors.delete(id)) paintVerify(id);
         });
      });
      document.querySelectorAll('.channel-resend-btn').forEach((btn) => {
         btn.addEventListener('click', function () {
            const id = parseInt(this.dataset.id, 10);
            if (verifyInFlight.has(id)) return;
            if (requestResend(id)) startVerifyRequest(id, 'resend');
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
      box.dataset.code = code;
      let ttl = payload.ttl_seconds || 0;
      const sendForm = (provider === 'slack' ? 'link ' : '/link ') + code;
      const instr = provider
         ? 'Send <code>' +
           escapeHtml(sendForm) +
           '</code> ' +
           (provider === 'sms'
              ? 'as a text to the DAWN number. DAWN replies with a 6-digit code — enter it on ' +
                'the new SMS channel below'
              : 'to the bot on ' + escapeHtml(provider)) +
           '.'
         : 'Send <code>/link ' +
           escapeHtml(code) +
           '</code> from the chat client (use "link ' +
           escapeHtml(code) +
           '" on Slack), or text it to the DAWN number: DAWN then replies with a 6-digit ' +
           'code to enter on the new SMS channel below.';

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
         // The server says how long each texted code has left; turn that into
         // a local deadline for the countdown.
         const now = Math.floor(Date.now() / 1000);
         channels.forEach((c) => {
            const left = Number(c.verify_ttl_seconds);
            c.verify_expires_local = left > 0 ? now + left : left === 0 ? now : 0;
         });
      }
      const force = forceNextRender;
      forceNextRender = false;
      renderList(force);
   }

   function handleCreateCodeResponse(payload) {
      if (payload) showLinkCode(payload);
   }

   /* Unlink / rename / re-enable: show a refusal, and re-fetch the
    * authoritative list either way (a refused rename puts the old name back;
    * a refusal can also mean the list on screen was out of date). */
   function handleMutationResponse(payload) {
      if (payload && payload.success === false && typeof DawnToast !== 'undefined') {
         DawnToast.show(payload.message || 'That didn’t work.', 'error');
      }
      requestList();
   }

   /* The server says the channel list changed (a /link arrived from a chat, a
    * code was verified, another tab unlinked one): re-read it if the panel is
    * open, else on next open. */
   function handleChannelsChanged(payload) {
      const change = (payload && payload.change) || '';
      const id = Number(payload && payload.channel_id) | 0;
      // The link code on screen was just used (a chat sent /link with it, and
      // the push names it): close it and take the user to the channel it made —
      // to its code box when an SMS number is waiting for its code.
      const box = document.getElementById('channel-link-code');
      const shown = box && !box.classList.contains('hidden') ? box.dataset.code || '' : '';
      const used = String((payload && payload.link_code) || '');
      if (
         (change === 'pending' || change === 'linked') &&
         shown &&
         used.toUpperCase() === shown.toUpperCase()
      ) {
         hideLinkCode();
         if (id > 0) {
            focusAfterRender = {
               sel:
                  change === 'pending'
                     ? '#channel-verify-' + id
                     : '.channel-name[data-id="' + id + '"]',
               until: Date.now() + FOCUS_WAIT_MS,
            };
         }
         requestList(true);
         return;
      }
      const section = document.getElementById('messaging-channels-section');
      if (section && !section.classList.contains('collapsed')) {
         requestList();
      } else {
         channels = [];
      }
   }

   function hideLinkCode() {
      const box = document.getElementById('channel-link-code');
      if (box) {
         box.classList.add('hidden');
         box.textContent = '';
         delete box.dataset.code;
      }
      if (codeCountdownTimer) {
         clearInterval(codeCountdownTimer);
         codeCountdownTimer = null;
      }
   }

   /* An SMS code entered: on success the channel is live (re-render, focus its
    * name); on a refusal the message shows on its code box. */
   function handleVerifyResponse(payload) {
      if (!payload) return;
      const id = Number(payload.id) | 0;
      finishVerifyRequest(id);
      if (payload.success) {
         verifyErrors.delete(id);
         if (typeof DawnToast !== 'undefined') DawnToast.show('Number linked', 'success');
         focusAfterRender = {
            sel: '.channel-name[data-id="' + id + '"]',
            until: Date.now() + FOCUS_WAIT_MS,
         };
         requestList(true);
         return;
      }
      showVerifyError(id, payload.message || 'That code didn’t work.', payload.code === 'BAD_CODE');
      if (payload.code === 'NOT_FOUND' || payload.code === 'ALREADY_LINKED') requestList();
   }

   /* A new code asked for: re-render so the countdown restarts, with focus in
    * the code box for the next step. */
   function handleResendResponse(payload) {
      if (!payload) return;
      const id = Number(payload.id) | 0;
      finishVerifyRequest(id);
      if (payload.success) {
         verifyErrors.delete(id);
         if (typeof DawnToast !== 'undefined') DawnToast.show('Sending a new code', 'success');
         focusAfterRender = { sel: '#channel-verify-' + id, until: Date.now() + FOCUS_WAIT_MS };
         requestList(true);
         return;
      }
      showVerifyError(id, payload.message || 'Couldn’t send a code.', false);
      if (payload.code === 'NOT_FOUND') requestList();
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
                  // Always re-read: a change may have been missed while closed.
                  requestList();
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
      handleVerifyResponse: handleVerifyResponse,
      handleResendResponse: handleResendResponse,
      handleChannelsChanged: handleChannelsChanged,
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
