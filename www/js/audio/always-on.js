/**
 * DAWN Always-On Voice Mode — Browser Coordinator
 *
 * Unified action button controller: dropdown selects the MODE, button activates it.
 * - "Send Text" mode: button clicks send text (default)
 * - "Hold to Talk" mode: button works as push-to-talk
 * - "Continuous Listening" mode: button click toggles continuous on/off
 *
 * Also manages:
 * - resolveButtonState(): single source of truth for button label/style
 * - Text override: typing in any voice mode temporarily shows "Send"
 * - Cancel state: processing/speaking/thinking → "Cancel" (red)
 *
 * Server drives the always-on state machine. Client responsibilities:
 * - Start/stop continuous audio capture on button toggle
 * - Mute streaming during TTS (echo prevention)
 * - Update UI on state changes
 * - ARIA announcements for accessibility
 * - Error recovery (mic revocation, disconnect, tab return)
 */
(function (global) {
   'use strict';

   const STORAGE_KEY = 'dawn_always_on_mode';

   let selectedMode = 'send'; // 'send' | 'push-to-talk' | 'continuous'
   let enabled = false; // Whether continuous listening is currently active
   let state = 'disabled'; // Server state
   let textOverride = false; // Textarea has text in a voice mode
   let pendingVisualState = null; // Defer visual "listening" flip until TTS finishes (cosmetic)
   let resumeAfterReconnect = false; // Always-on was on when the WS dropped; re-enable post-handshake

   /**
    * Initialize always-on module
    */
   function init() {
      setupDropdown();

      // Restore mode preference from localStorage
      var stored = localStorage.getItem(STORAGE_KEY);
      if (stored === 'continuous' || stored === 'push-to-talk') {
         selectedMode = stored;
         updateDropdownActive(stored);
      }

      resolveButtonState();

      document.addEventListener('visibilitychange', handleVisibilityChange);
   }

   /**
    * Set up the split button dropdown for mode selection.
    * The dropdown only selects the mode — it does NOT activate/deactivate.
    */
   function setupDropdown() {
      var dropdownBtn = document.getElementById('action-dropdown-btn');
      var dropdown = document.getElementById('action-dropdown');

      if (!dropdownBtn || !dropdown) {
         return;
      }

      // Toggle dropdown on chevron click
      dropdownBtn.addEventListener('click', function (e) {
         e.preventDefault();
         e.stopPropagation();
         var isOpen = !dropdown.classList.contains('hidden');
         if (isOpen) {
            closeDropdown();
         } else {
            openDropdown();
         }
      });

      // Mode selection — sets preference only, does not activate
      dropdown.addEventListener('click', function (e) {
         if (dropdown.classList.contains('hidden')) return;

         var item = e.target.closest('.dropdown-item');
         if (!item) return;

         // Skip disabled items
         if (item.disabled || item.classList.contains('disabled')) return;

         var mode = item.dataset.mode;

         // If switching away from continuous while it's active, disable first
         if (mode !== 'continuous' && enabled) {
            disable();
         }

         selectedMode = mode;
         localStorage.setItem(STORAGE_KEY, mode);
         updateDropdownActive(mode);
         textOverride = false;
         resolveButtonState();
         closeDropdown();

         var announceMsg = {
            send: 'Send text mode selected.',
            'push-to-talk': 'Hold to talk mode selected.',
            continuous: 'Continuous listening mode selected. Press button to start.',
         };
         announce(announceMsg[mode] || '');
      });

      // Close dropdown on outside click
      document.addEventListener('click', function (e) {
         if (!e.target.closest('.action-btn-wrapper')) {
            closeDropdown();
         }
      });

      // Keyboard navigation (Escape close handled via DawnEscStack in open/close)
      dropdownBtn.addEventListener('keydown', function (e) {
         if (e.key === 'ArrowDown' || e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            if (dropdown.classList.contains('hidden')) {
               openDropdown();
            }
            var firstItem = dropdown.querySelector('.dropdown-item:not(:disabled)');
            if (firstItem) firstItem.focus();
         }
      });

      dropdown.addEventListener('keydown', function (e) {
         var items = Array.prototype.slice.call(
            dropdown.querySelectorAll('.dropdown-item:not(:disabled)')
         );
         var idx = items.indexOf(document.activeElement);

         if (e.key === 'ArrowDown') {
            e.preventDefault();
            var next = items[(idx + 1) % items.length];
            if (next) next.focus();
         } else if (e.key === 'ArrowUp') {
            e.preventDefault();
            var prev = items[(idx - 1 + items.length) % items.length];
            if (prev) prev.focus();
         } else if (e.key === 'Home') {
            e.preventDefault();
            if (items[0]) items[0].focus();
         } else if (e.key === 'End') {
            e.preventDefault();
            if (items[items.length - 1]) items[items.length - 1].focus();
         }
      });
   }

   var alwaysOnEscToken = null; // DawnEscStack registration while the menu is open

   function closeDropdown() {
      var dropdown = document.getElementById('action-dropdown');
      var btn = document.getElementById('action-dropdown-btn');
      if (dropdown) dropdown.classList.add('hidden');
      if (btn) btn.setAttribute('aria-expanded', 'false');
      if (alwaysOnEscToken !== null) {
         DawnEscStack.unregister(alwaysOnEscToken);
         alwaysOnEscToken = null;
      }
   }

   function openDropdown() {
      var dropdown = document.getElementById('action-dropdown');
      var btn = document.getElementById('action-dropdown-btn');
      if (dropdown) dropdown.classList.remove('hidden');
      if (btn) btn.setAttribute('aria-expanded', 'true');
      if (alwaysOnEscToken === null) {
         alwaysOnEscToken = DawnEscStack.register(function () {
            closeDropdown();
            var b = document.getElementById('action-dropdown-btn');
            if (b) b.focus();
            return true;
         });
      }
   }

   /**
    * Toggle continuous listening (called from action button click in continuous mode)
    */
   function toggle() {
      if (enabled) {
         disable();
      } else {
         enable();
      }
   }

   /**
    * Enable always-on continuous listening
    */
   function enable() {
      if (enabled) return;

      if (!window.isSecureContext) {
         console.error('Always-on requires HTTPS');
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Always-on requires HTTPS', 'error');
         }
         return;
      }

      if (DawnState.getIsRecording()) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Finish recording before enabling continuous mode', 'warning');
         }
         return;
      }

      // Start audio capture before notifying server — mic acquisition is async
      // and may prompt user. If it fails, server gets no audio and auto-disables.
      DawnAudioCapture.startContinuous();

      var sampleRate = DawnAudioCapture.getAudioContextSampleRate
         ? DawnAudioCapture.getAudioContextSampleRate()
         : 48000;

      DawnWS.send({
         type: 'always_on_enable',
         payload: { sample_rate: sampleRate },
      });

      enabled = true;
      updateActionButton();
      resolveButtonState();
      announce('Continuous listening started');
   }

   /**
    * Disable always-on mode
    */
   function disable() {
      if (!enabled) return;

      DawnWS.send({ type: 'always_on_disable' });
      DawnAudioCapture.stopContinuous();

      enabled = false;
      state = 'disabled';
      pendingVisualState = null;
      resumeAfterReconnect = false; // explicit user disable overrides any pending auto-resume
      updateActionButton();
      resolveButtonState();
      announce('Continuous listening stopped');
   }

   /**
    * Handle incoming always-on messages from server
    */
   function handleMessage(msg) {
      switch (msg.type) {
         case 'always_on_state':
            onStateChange(msg.payload.state);
            break;
         case 'wake_detected':
            console.log('Wake word detected:', msg.payload.transcript);
            break;
         case 'recording_end':
            break;
         case 'always_on_error':
            handleError(msg.payload);
            break;
      }
   }

   // Lazily-created AudioContext for the short "ready for your command" ding.
   // Separate from the capture/playback contexts so it can't disturb them.
   let dingCtx = null;

   /**
    * Play a short two-note chime when the server enters RECORDING (bare wake word
    * acknowledged, listening for the command). This REPLACES the old server-side
    * spoken "Hello." greeting, which played TTS through the speaker while the
    * recording mic was live and echoed back in — the server VAD scored that speech
    * echo and stalled/hung end-of-speech. A pure enveloped tone is NOT scored as
    * speech by the server's Silero VAD, so it's echo-safe even without AEC. Kept
    * short, low, and click-free (exponential envelope). Purely cosmetic — any
    * failure is swallowed so it can never break the voice flow.
    *
    * DELIBERATELY not gated on TTS-enabled (unlike the old server greeting, which
    * was): the ding is a UI interaction cue for "I'm listening," not Friday's
    * voice, so it fires whenever always-on is active regardless of the TTS toggle.
    */
   function playRecordingDing() {
      try {
         if (!dingCtx) {
            dingCtx = new (window.AudioContext || window.webkitAudioContext)();
         }
         if (dingCtx.state === 'suspended') {
            dingCtx.resume();
         }
         const t = dingCtx.currentTime;
         const osc = dingCtx.createOscillator();
         const gain = dingCtx.createGain();
         osc.type = 'sine';
         osc.frequency.setValueAtTime(784, t); // G5
         osc.frequency.setValueAtTime(1047, t + 0.08); // -> C6, a gentle rising "ready" tone
         gain.gain.setValueAtTime(0.0001, t); // start near-silent so the attack ramp is click-free
         gain.gain.exponentialRampToValueAtTime(0.12, t + 0.015); // quick, low-peak attack
         gain.gain.exponentialRampToValueAtTime(0.0001, t + 0.18); // smooth release (~180ms total)
         osc.connect(gain).connect(dingCtx.destination);
         osc.start(t);
         osc.stop(t + 0.2);
      } catch (e) {
         /* cosmetic ding — never let it affect the flow */
      }
   }

   /**
    * Handle state change from server
    */
   function onStateChange(newState) {
      const prevState = state;
      state = newState;

      // Bare-wake-word "ready for your command" cue (replaces the server greeting).
      // Fire only on the transition INTO recording so it dings once per wake, not
      // on every state frame.
      if (newState === 'recording' && prevState !== 'recording' && enabled) {
         playRecordingDing();
      }

      // Mic mute/unmute is driven by actual TTS PLAYBACK (onPlaybackStart /
      // onPlaybackEnd), NOT by this server state. TTS echo prevention only needs
      // the mic paused while the browser is actually speaking; a turn that
      // produces no browser TTS — e.g. a dedup-suppressed turn — therefore never
      // mutes, so it can't get stuck muted while the server still reports
      // "listening" (the old processing→mute / listening→deferred-unmute path
      // hung exactly there when no playback-end event ever arrived).

      // Handle server-side disable (e.g., auto-disable on no audio).
      if (newState === 'disabled' && enabled) {
         DawnAudioCapture.stopContinuous();
         enabled = false;
      }

      // Cosmetic only: don't flip the UI to "listening" while the browser is
      // still speaking the reply — defer the visual until playback ends
      // (applied in onPlaybackEnd). This does NOT touch the mic.
      if (
         newState === 'listening' &&
         typeof DawnAudioPlayback !== 'undefined' &&
         DawnAudioPlayback.isPlaying()
      ) {
         pendingVisualState = newState;
         return;
      }

      // Safety net: reaching 'listening' with nothing playing means the mic
      // should be live. onPlaybackEnd normally unmutes, but if that event is
      // ever dropped (tab backgrounded / AudioContext suspended mid-playback)
      // the mic could stay muted; ensure it's unmuted here. Idempotent.
      if (newState === 'listening' && enabled && DawnAudioCapture.unmuteContinuous) {
         DawnAudioCapture.unmuteContinuous();
      }

      updateActionButton();
      resolveButtonState();
      updateVisualizer();
      announce(stateAnnouncement(newState));
   }

   /**
    * Handle error from server
    */
   function handleError(payload) {
      console.error('Always-on error:', payload);

      if (payload.code === 'ALREADY_ACTIVE') {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Always-on is active in another tab', 'warning');
         }
      } else {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload.message || 'Always-on error', 'error');
         }
      }

      // Reset local state on any error
      enabled = false;
      state = 'disabled';
      DawnAudioCapture.stopContinuous();
      updateActionButton();
      resolveButtonState();
   }

   /**
    * Update the dropdown active indicator
    */
   function updateDropdownActive(mode) {
      var dropdown = document.getElementById('action-dropdown');
      if (!dropdown) return;

      dropdown.querySelectorAll('.dropdown-item').forEach(function (item) {
         item.classList.toggle('active', item.dataset.mode === mode);
      });
   }

   /**
    * Resolve the action button's label, title, and CSS state.
    * Priority: cancel > PTT recording > text override > mode-specific
    */
   function resolveButtonState() {
      var btn = DawnElements.actionBtn;
      if (!btn) return;

      var appState = DawnState.getAppState ? DawnState.getAppState() : 'idle';

      // 1. Cancel override: processing/speaking/thinking
      var isCancelState =
         appState === 'processing' || appState === 'speaking' || appState === 'thinking';
      if (isCancelState) {
         btn.textContent = 'Cancel';
         btn.title = 'Cancel request';
         btn.setAttribute('aria-label', 'Cancel request');
         btn.classList.add('cancel-active');
         btn.classList.remove('recording');
         return;
      }

      btn.classList.remove('cancel-active');

      // 2. PTT recording: isRecording is true
      if (
         selectedMode === 'push-to-talk' &&
         DawnState.getIsRecording &&
         DawnState.getIsRecording()
      ) {
         btn.textContent = 'Rec';
         btn.title = 'Release to send';
         btn.setAttribute('aria-label', 'Release to send');
         btn.classList.add('recording');
         return;
      }

      btn.classList.remove('recording');

      // 3. Text override: textarea has text AND mode ≠ send
      if (textOverride && selectedMode !== 'send') {
         btn.textContent = 'Send';
         btn.title = 'Send message';
         btn.setAttribute('aria-label', 'Send message');
         return;
      }

      // 4. Mode-specific
      if (selectedMode === 'send') {
         btn.textContent = 'Send';
         btn.title = 'Send message';
         btn.setAttribute('aria-label', 'Send message');
      } else if (selectedMode === 'push-to-talk') {
         btn.textContent = 'Mic';
         btn.title = 'Hold to speak';
         btn.setAttribute('aria-label', 'Hold to speak');
      } else if (selectedMode === 'continuous') {
         if (enabled) {
            btn.textContent = 'Stop';
            btn.title = 'Stop continuous listening';
            btn.setAttribute('aria-label', 'Stop continuous listening');
         } else {
            btn.textContent = 'Listen';
            btn.title = 'Start continuous listening';
            btn.setAttribute('aria-label', 'Start continuous listening');
         }
      }
   }

   /**
    * Update action button visual state based on always-on state
    */
   function updateActionButton() {
      var wrapper = document.querySelector('.action-btn-wrapper');
      if (!wrapper) return;

      wrapper.classList.remove(
         'always-on-listening',
         'always-on-wake-check',
         'always-on-recording',
         'always-on-processing'
      );

      if (!enabled) return;

      switch (state) {
         case 'listening':
            wrapper.classList.add('always-on-listening');
            break;
         case 'wake_check':
         case 'wake_pending':
            wrapper.classList.add('always-on-wake-check');
            break;
         case 'recording':
            wrapper.classList.add('always-on-recording');
            break;
         case 'processing':
            wrapper.classList.add('always-on-processing');
            break;
      }
   }

   /**
    * Update status dot and text to reflect always-on state.
    * Overrides the normal "IDLE" display with always-on state info.
    * Pass null to restore normal display.
    *
    * NOTE: this intentionally writes ONLY the top/mini status bars, NOT the composer
    * working-indicator. Always-on micro-states (LISTENING/RECORDING/CHECKING/wake) are
    * not LLM-busy; the reactor's busy signal comes exclusively from the normal `state`
    * channel (updateState → updateComposerBusy), which the server still emits during
    * always-on LLM processing. So the reactor stays correct without a hook here.
    */
   function updateStatusIndicator(alwaysOnState) {
      var dot = DawnElements.statusDot;
      var text = DawnElements.statusText;
      var miniDot = DawnElements.miniStatusDot;
      var miniText = DawnElements.miniStatusText;
      if (!dot || !text) return;

      if (!alwaysOnState || alwaysOnState === 'disabled') {
         // Restore to current app state
         var appState = DawnState.getAppState() || 'idle';
         dot.className = appState;
         text.textContent = appState.toUpperCase();
         if (miniDot) miniDot.className = 'status-dot ' + appState;
         if (miniText) miniText.textContent = appState.toUpperCase();
         return;
      }

      var statusMap = {
         listening: { css: 'listening', label: 'LISTENING' },
         wake_check: { css: 'listening', label: 'LISTENING' },
         wake_pending: { css: 'thinking', label: 'CHECKING' },
         recording: { css: 'listening', label: 'RECORDING' },
         processing: { css: 'thinking', label: 'THINKING' },
      };

      var info = statusMap[alwaysOnState];
      if (!info) return;

      dot.className = info.css;
      text.textContent = info.label;
      if (miniDot) miniDot.className = 'status-dot ' + info.css;
      if (miniText) miniText.textContent = info.label;
   }

   /**
    * Update visualizer ring state
    */
   function updateVisualizer() {
      var ring = document.getElementById('ring-container');
      if (!ring) return;

      ring.classList.remove(
         'always-on-listening',
         'always-on-wake-check',
         'always-on-recording',
         'always-on-processing'
      );

      if (!enabled) {
         // Stop input visualization when disabled
         if (typeof DawnVisualization !== 'undefined' && DawnVisualization.stopInputFFT) {
            DawnVisualization.stopInputFFT();
         }
         // Restore normal status display
         updateStatusIndicator(null);
         return;
      }

      // Start/stop input FFT visualization based on state
      if (typeof DawnVisualization !== 'undefined') {
         if (
            state === 'listening' ||
            state === 'wake_check' ||
            state === 'wake_pending' ||
            state === 'recording'
         ) {
            if (DawnVisualization.startInputFFT) {
               DawnVisualization.startInputFFT();
            }
         } else {
            if (DawnVisualization.stopInputFFT) {
               DawnVisualization.stopInputFFT();
            }
         }
      }

      // Update status text/dot to reflect always-on state
      updateStatusIndicator(state);

      // Remove idle dimming — always-on ring should stay bright
      ring.classList.remove('idle');

      switch (state) {
         case 'listening':
            ring.classList.add('always-on-listening');
            break;
         case 'wake_check':
         case 'wake_pending':
            ring.classList.add('always-on-wake-check');
            break;
         case 'recording':
            ring.classList.add('always-on-recording');
            break;
         case 'processing':
            ring.classList.add('always-on-processing');
            break;
      }
   }

   /**
    * ARIA announcement for state changes
    */
   function announce(message) {
      if (!message) return;
      var announcer = document.getElementById('always-on-announcer');
      if (!announcer) {
         announcer = document.createElement('div');
         announcer.id = 'always-on-announcer';
         announcer.setAttribute('aria-live', 'polite');
         announcer.setAttribute('aria-atomic', 'true');
         announcer.className = 'sr-only';
         document.body.appendChild(announcer);
      }
      announcer.textContent = message;
   }

   function stateAnnouncement(s) {
      var messages = {
         listening: 'Listening for wake word',
         wake_check: 'Speech detected, checking for wake word',
         wake_pending: 'Checking for wake word',
         recording: 'Wake word detected, recording your command',
         processing: 'Processing your request',
         disabled: 'Continuous listening stopped',
      };
      return messages[s] || '';
   }

   function handleVisibilityChange() {
      if (!document.hidden && enabled) {
         DawnWS.send({ type: 'always_on_state_request' });
      }
   }

   function handleReconnect() {
      // Server-side always_on_ctx is destroyed on disconnect. Tear down the
      // client capture so the UI doesn't show active against a server with no
      // context — but REMEMBER that always-on was on, so we can turn it back on
      // once the reconnect handshake completes.
      //
      // This runs on the raw 'connected' event, BEFORE the reconnect handshake,
      // so we must NOT re-enable here — an always_on_enable sent now would target
      // the throwaway pre-handshake session and be lost (same reason phone
      // rehydration is deferred to the 'session' handler). The actual re-enable
      // happens in resumeAfterReconnectIfNeeded(), called from that handler.
      if (enabled) {
         resumeAfterReconnect = true;
         DawnAudioCapture.stopContinuous();
         enabled = false;
         state = 'disabled';
         pendingVisualState = null;
         updateActionButton();
         resolveButtonState();
      }
   }

   /**
    * Re-enable always-on after a reconnect handshake completes, if it was active
    * when the connection dropped. Called from the 'session' handler (post-restore),
    * NOT from handleReconnect (which runs pre-handshake). Idempotent across the
    * multiple 'session' messages one reconnect emits — the flag is cleared on the
    * first call so we only re-enable once.
    */
   function resumeAfterReconnectIfNeeded() {
      if (!resumeAfterReconnect) return;
      resumeAfterReconnect = false;
      if (enabled) return; // already re-enabled by some other path
      console.log('Always-on: resuming after reconnect');
      enable(); // re-acquires mic (permission persists) + re-sends always_on_enable
   }

   function handleMicRevoked() {
      if (enabled) {
         console.warn('Always-on: mic permission revoked, disabling');
         enabled = false;
         state = 'disabled';
         DawnWS.send({ type: 'always_on_disable' });
         updateActionButton();
         resolveButtonState();
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Microphone access lost — continuous listening disabled', 'warning');
         }
      }
   }

   /**
    * Check if continuous mode is selected (for button behavior in dawn.js)
    */
   function isContinuousMode() {
      return selectedMode === 'continuous';
   }

   function isPushToTalkMode() {
      return selectedMode === 'push-to-talk';
   }

   function isSendMode() {
      return selectedMode === 'send';
   }

   function getSelectedMode() {
      return selectedMode;
   }

   function isEnabled() {
      return enabled;
   }

   function getState() {
      return state;
   }

   /**
    * Set text override state (textarea has text in a voice mode)
    */
   function setTextOverride(hasText) {
      if (textOverride !== hasText) {
         textOverride = hasText;
         resolveButtonState();
      }
   }

   function isTextOverrideActive() {
      return textOverride;
   }

   /**
    * Called when the browser STARTS playing TTS audio (per playback chunk).
    * Pause the mic so it doesn't capture our own spoken reply (echo → false
    * wake). Idempotent — safe to call for each queued chunk. A turn with no
    * browser TTS (e.g. dedup-suppressed) never fires this, so it never mutes.
    */
   function onPlaybackStart() {
      if (enabled && DawnAudioCapture.muteContinuous) {
         DawnAudioCapture.muteContinuous();
      }
   }

   /**
    * Called when ALL queued TTS playback finishes (or is stopped/errors).
    * Resume the mic, and apply any visual "listening" state we deferred while
    * speaking. Tied to the real playback lifecycle, so the mic can't stay muted
    * once we've stopped speaking.
    *
    * If TTS delivery gaps let the queue empty between sentences, this unmutes
    * then the next sentence re-mutes (onPlaybackStart) — a brief flicker. Benign:
    * unmute only happens when nothing is playing (no live echo to capture), and
    * the server is still in PROCESSING and ignores the audio anyway.
    */
   function onPlaybackEnd() {
      if (enabled && DawnAudioCapture.unmuteContinuous) {
         DawnAudioCapture.unmuteContinuous();
      }
      if (pendingVisualState) {
         var deferred = pendingVisualState;
         pendingVisualState = null;
         updateActionButton();
         resolveButtonState();
         updateVisualizer();
         announce(stateAnnouncement(deferred));
      }
   }

   /**
    * Disable audio mode dropdown items (when audio not supported)
    */
   function disableAudioModes() {
      var dropdown = document.getElementById('action-dropdown');
      if (!dropdown) return;

      dropdown.querySelectorAll('.dropdown-item').forEach(function (item) {
         if (item.dataset.mode === 'push-to-talk' || item.dataset.mode === 'continuous') {
            item.disabled = true;
            item.classList.add('disabled');
            item.style.opacity = '0.5';
            item.style.cursor = 'not-allowed';
         }
      });

      // If current mode is a voice mode, fall back to send
      if (selectedMode !== 'send') {
         selectedMode = 'send';
         localStorage.setItem(STORAGE_KEY, 'send');
         updateDropdownActive('send');
         resolveButtonState();
      }
   }

   global.DawnAlwaysOn = {
      init: init,
      enable: enable,
      disable: disable,
      toggle: toggle,
      handleMessage: handleMessage,
      handleReconnect: handleReconnect,
      resumeAfterReconnectIfNeeded: resumeAfterReconnectIfNeeded,
      handleMicRevoked: handleMicRevoked,
      onPlaybackStart: onPlaybackStart,
      onPlaybackEnd: onPlaybackEnd,
      isContinuousMode: isContinuousMode,
      isPushToTalkMode: isPushToTalkMode,
      isSendMode: isSendMode,
      getSelectedMode: getSelectedMode,
      isEnabled: isEnabled,
      getState: getState,
      setTextOverride: setTextOverride,
      isTextOverrideActive: isTextOverrideActive,
      resolveButtonState: resolveButtonState,
      disableAudioModes: disableAudioModes,
   };
})(window);
