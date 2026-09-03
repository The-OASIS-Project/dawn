/**
 * DAWN Scheduler Notifications
 * Handles alarm/timer/reminder notifications from the daemon scheduler.
 * Displays a notification banner with dismiss/snooze buttons.
 */
(function () {
   'use strict';

   let container = null;
   let chimeCtx = null; // Web Audio context for alarm chime
   const localDismissed = new Set(); // event IDs dismissed locally (suppress rebroadcast)

   /* Active alarm loops keyed by event_id. Each entry is {timer, stop()} so
    * dismiss/snooze/timeout can stop the loop for a specific event without
    * affecting other ringing alarms. */
   const activeAlarmLoops = new Map();

   /* Client-side "Alarm sounds" toggle — gates the chime + ringing-alarm loop
    * ONLY. The spoken alarm announcement is separate: it rides the general TTS
    * mute (set_tts_enabled), not this flag. Defaults ON so existing users keep
    * today's always-chiming behavior. Persisted per-browser via DawnStore. */
   let alarmSoundsEnabled = window.DawnStore
      ? DawnStore.getBool(DawnStore.KEYS.ALARM_SOUNDS, true)
      : true;

   /**
    * Ask the browser for Notification API permission. Must be triggered from a
    * user gesture (button click) — not page load — or browsers block the prompt.
    * No-op if the API is unavailable or permission is already granted/denied.
    */
   function requestNotifPermission() {
      if (!('Notification' in window)) return;
      if (Notification.permission !== 'default') return;
      try {
         Notification.requestPermission();
      } catch (e) {
         /* Older Safari throws on sync call — ignore. */
      }
   }

   /**
    * Show an OS-level browser notification when the tab is backgrounded.
    * No-op if permission has not been granted or the tab is visible.
    */
   function showBrowserNotification(payload) {
      if (!('Notification' in window)) return;
      if (Notification.permission !== 'granted') return;
      if (!document.hidden) return;

      const eventType = payload.event_type || 'timer';
      const prefix = payload.missed ? 'Missed ' : '';
      const title = prefix + typeLabel(eventType);
      const body = payload.message || payload.name || '';

      try {
         const notif = new Notification(title, {
            body: body,
            tag: 'dawn-sched-' + payload.event_id,
            icon: '/favicon.ico',
            requireInteraction: payload.status === 'ringing' && !payload.missed,
         });
         notif.onclick = function () {
            window.focus();
            notif.close();
         };
      } catch (e) {
         /* Safari / mobile may throw if permission was revoked mid-session. */
      }
   }

   function unreadKey() {
      const user =
         typeof DawnState !== 'undefined' && DawnState.authState
            ? DawnState.authState.username
            : '';
      return 'dawn_unread_briefings' + (user ? '_' + user : '');
   }

   function getUnreadBriefings() {
      try {
         return JSON.parse(localStorage.getItem(unreadKey()) || '[]');
      } catch {
         return [];
      }
   }

   function addUnreadBriefing(convId) {
      if (!convId) return;
      const list = getUnreadBriefings();
      if (!list.includes(convId)) {
         list.push(convId);
         localStorage.setItem(unreadKey(), JSON.stringify(list));
      }
   }

   function removeUnreadBriefing(convId) {
      const list = getUnreadBriefings().filter((id) => id !== convId);
      localStorage.setItem(unreadKey(), JSON.stringify(list));
      /* Update sidebar if available */
      if (typeof DawnHistory !== 'undefined' && DawnHistory.refreshUnread) {
         DawnHistory.refreshUnread();
      }
   }

   /**
    * Ensure notification container exists
    */
   function ensureContainer() {
      if (container) return container;
      container = document.createElement('div');
      container.id = 'scheduler-notifications';
      document.body.appendChild(container);
      return container;
   }

   /**
    * Play a browser alarm chime using Web Audio API
    */
   function playChime(eventType) {
      if (!alarmSoundsEnabled) return;
      try {
         if (!chimeCtx) {
            chimeCtx = new (window.AudioContext || window.webkitAudioContext)();
         }
         /* Resume suspended context (browser autoplay policy) */
         if (chimeCtx.state === 'suspended') {
            chimeCtx.resume();
         }

         const duration = eventType === 'alarm' ? 0.6 : 0.3;
         const freq = eventType === 'alarm' ? 880 : eventType === 'reminder' ? 660 : 523;

         const osc = chimeCtx.createOscillator();
         const gain = chimeCtx.createGain();
         osc.connect(gain);
         gain.connect(chimeCtx.destination);
         osc.frequency.value = freq;
         osc.type = 'sine';
         gain.gain.setValueAtTime(0.3, chimeCtx.currentTime);
         gain.gain.exponentialRampToValueAtTime(0.001, chimeCtx.currentTime + duration);
         osc.start(chimeCtx.currentTime);
         osc.stop(chimeCtx.currentTime + duration);
      } catch (e) {
         console.warn('Scheduler: Could not play chime:', e);
      }
   }

   /**
    * Start a looping alarm tone for a ringing alarm event. Mirrors the
    * daemon's local-speaker behavior so WebUI-sourced alarms aren't silent
    * when the session has TTS disabled. Safe to call multiple times for the
    * same event_id — only the first call starts a loop.
    */
   function startAlarmLoop(eventId) {
      if (!alarmSoundsEnabled) return;
      if (activeAlarmLoops.has(eventId)) return;

      try {
         if (!chimeCtx) {
            chimeCtx = new (window.AudioContext || window.webkitAudioContext)();
         }
         if (chimeCtx.state === 'suspended') {
            chimeCtx.resume();
         }
      } catch (e) {
         console.warn('Scheduler: Could not init audio context for alarm loop:', e);
         return;
      }

      /* Beep pattern: 0.5s tone, 0.5s silence, repeat. Matches a familiar
       * alarm clock cadence without being harshly continuous. */
      const BEEP_MS = 500;
      const GAP_MS = 500;

      function beepOnce() {
         try {
            if (!chimeCtx || chimeCtx.state === 'closed') return;
            const osc = chimeCtx.createOscillator();
            const gain = chimeCtx.createGain();
            osc.connect(gain);
            gain.connect(chimeCtx.destination);
            osc.frequency.value = 880;
            osc.type = 'sine';
            gain.gain.setValueAtTime(0.25, chimeCtx.currentTime);
            gain.gain.exponentialRampToValueAtTime(0.001, chimeCtx.currentTime + BEEP_MS / 1000);
            osc.start(chimeCtx.currentTime);
            osc.stop(chimeCtx.currentTime + BEEP_MS / 1000);
         } catch (e) {
            /* Silent — the context may have been closed mid-loop. */
         }
      }

      beepOnce();
      const timer = setInterval(beepOnce, BEEP_MS + GAP_MS);
      activeAlarmLoops.set(eventId, { timer });
   }

   /**
    * Stop a running alarm loop (dismiss/snooze/timeout). No-op if none active.
    */
   function stopAlarmLoop(eventId) {
      const entry = activeAlarmLoops.get(eventId);
      if (!entry) return;
      clearInterval(entry.timer);
      activeAlarmLoops.delete(eventId);
   }

   /**
    * Read the client-side "Alarm sounds" state (chime + ringing loop).
    * @returns {boolean}
    */
   function isAlarmSoundsEnabled() {
      return alarmSoundsEnabled;
   }

   /**
    * Set the client-side "Alarm sounds" state and persist it. Turning it OFF
    * silences any alarm currently looping — the user muting mid-ring expects
    * immediate quiet, not "next time." Does NOT touch the spoken announcement
    * (that follows the general TTS mute).
    * @param {boolean} on
    */
   function setAlarmSounds(on) {
      alarmSoundsEnabled = !!on;
      if (window.DawnStore) {
         DawnStore.setBool(DawnStore.KEYS.ALARM_SOUNDS, alarmSoundsEnabled);
      }
      if (!alarmSoundsEnabled) {
         /* Snapshot keys first — stopAlarmLoop mutates the map. */
         Array.from(activeAlarmLoops.keys()).forEach(stopAlarmLoop);
      }
   }

   /**
    * Get color class for event type
    */
   function typeClass(eventType) {
      switch (eventType) {
         case 'alarm':
            return 'sched-alarm';
         case 'reminder':
            return 'sched-reminder';
         case 'timer':
            return 'sched-timer';
         case 'task':
            return 'sched-task';
         case 'briefing':
            return 'sched-briefing';
         default:
            return 'sched-timer';
      }
   }

   /**
    * Get display label for event type
    */
   function typeLabel(eventType) {
      switch (eventType) {
         case 'alarm':
            return 'ALARM';
         case 'reminder':
            return 'REMINDER';
         case 'timer':
            return 'TIMER';
         case 'task':
            return 'TASK';
         case 'briefing':
            return 'BRIEFING';
         default:
            return 'EVENT';
      }
   }

   /**
    * Send scheduler action to daemon
    */
   function sendAction(action, eventId, snoozeMinutes) {
      const payload = { action, event_id: eventId };
      if (snoozeMinutes) payload.snooze_minutes = snoozeMinutes;
      DawnWS.send({ type: 'scheduler_action', payload });
   }

   /**
    * Create and show a notification banner
    */
   function showNotification(payload) {
      const el = ensureContainer();
      const eventId = payload.event_id;
      const eventType = payload.event_type || 'timer';
      const status = payload.status || 'ringing';
      const name = payload.name || '';
      const message = payload.message || name;
      const isMissed = !!payload.missed;

      // Remove existing notification for same event
      const existing = el.querySelector(`[data-event-id="${eventId}"]`);
      if (existing) existing.remove();

      // Missed notifications are replays, never live alerts — never "ringing".
      const isRinging = status === 'ringing' && !isMissed;

      const banner = document.createElement('div');
      const ringingClass = isRinging ? ' sched-ringing' : '';
      const missedClass = isMissed ? ' sched-missed' : '';
      banner.className = `sched-banner ${typeClass(eventType)}${ringingClass}${missedClass}`;
      banner.dataset.eventId = eventId;
      if (isMissed && payload.missed_notif_id) {
         banner.dataset.missedNotifId = String(payload.missed_notif_id);
      }
      /* role="alert" pairs with aria-live="assertive" for live interrupts.
       * Missed replays are historical and non-interactive — role="status"
       * pairs with aria-live="polite" for a gentler SR announcement. Using
       * "alertdialog" without aria-modal/labelledby/describedby is invalid. */
      banner.setAttribute('role', isMissed ? 'status' : 'alert');
      banner.setAttribute('aria-live', isMissed ? 'polite' : 'assertive');
      banner.setAttribute(
         'aria-label',
         `${isMissed ? 'Missed ' : ''}${typeLabel(eventType)}: ${message}`
      );

      // Only alarms support snooze (timers/reminders auto-dismiss on daemon).
      // Missed notifications cannot be snoozed — the event already fired.
      const canSnooze = isRinging && eventType === 'alarm' && !isMissed;

      const isBriefing = eventType === 'briefing';
      const conversationId = payload.conversation_id;

      let actionsHtml;
      if (isBriefing && conversationId) {
         actionsHtml = `<div class="sched-actions">
               <button class="sched-btn sched-btn-view" title="View">View</button>
               <button class="sched-btn sched-btn-dismiss" title="Close">Close</button>
            </div>`;
      } else if (isRinging) {
         actionsHtml = `<div class="sched-actions">
               ${canSnooze ? '<button class="sched-btn sched-btn-snooze" title="Snooze">Snooze</button>' : ''}
               <button class="sched-btn sched-btn-dismiss" title="Dismiss">Dismiss</button>
            </div>`;
      } else {
         actionsHtml = `<div class="sched-actions">
               <button class="sched-btn sched-btn-dismiss" title="Close">Close</button>
            </div>`;
      }

      const badgeText = isMissed ? `MISSED &middot; ${typeLabel(eventType)}` : typeLabel(eventType);
      banner.innerHTML = `
         <div class="sched-banner-content">
            <span class="sched-type-badge">${badgeText}</span>
            <span class="sched-message">${escapeHtml(message)}</span>
         </div>
         ${actionsHtml}
      `;

      // Wire buttons
      const dismissBtn = banner.querySelector('.sched-btn-dismiss');
      if (dismissBtn) {
         dismissBtn.addEventListener('click', () => {
            requestNotifPermission();
            if (isMissed && payload.missed_notif_id) {
               /* Missed notifications are DB rows. dismiss_missed removes the
                * row; event_id lets the server ALSO dismiss the still-ringing
                * alarm if the user was offline while it fired and the chime is
                * still looping on the local speaker. */
               DawnWS.send({
                  type: 'scheduler_action',
                  payload: {
                     action: 'dismiss_missed',
                     missed_notif_id: payload.missed_notif_id,
                     event_id: eventId,
                  },
               });
            } else {
               localDismissed.add(eventId);
               setTimeout(() => localDismissed.delete(eventId), 5000);
               sendAction('dismiss', eventId);
               /* Stop TTS if scheduler audio is playing */
               if (typeof DawnAudioPlayback !== 'undefined' && DawnAudioPlayback.stop) {
                  DawnAudioPlayback.stop();
               }
            }
            stopAlarmLoop(eventId);
            banner.classList.add('sched-banner-out');
            setTimeout(() => banner.remove(), 300);
         });
      }

      const snoozeBtn = banner.querySelector('.sched-btn-snooze');
      if (snoozeBtn) {
         snoozeBtn.addEventListener('click', () => {
            requestNotifPermission();
            sendAction('snooze', eventId, 0);
            stopAlarmLoop(eventId);
            banner.classList.add('sched-banner-out');
            setTimeout(() => banner.remove(), 300);
         });
      }

      const viewBtn = banner.querySelector('.sched-btn-view');
      if (viewBtn) {
         viewBtn.addEventListener('click', () => {
            if (typeof DawnHistory !== 'undefined' && conversationId) {
               DawnHistory.loadConversation(conversationId);
               DawnHistory.open();
               removeUnreadBriefing(conversationId);
            }
            banner.classList.add('sched-banner-out');
            setTimeout(() => banner.remove(), 300);
         });
      }

      // Keyboard: Escape to dismiss
      banner.tabIndex = 0;
      banner.addEventListener('keydown', (e) => {
         if (e.key === 'Escape') {
            dismissBtn.click();
         }
      });

      el.appendChild(banner);
      /* Only grab focus for live ringing events. Missed banners are replays
       * that may arrive 32 at a time on reconnect — focusing each one churns
       * keyboard/screen-reader focus and yanks the caret out of whatever the
       * user was doing. */
      if (isRinging && !isMissed) {
         banner.focus();
      }

      // Auto-dismiss non-ringing notifications (briefings: 60s, others: 10s).
      // Missed notifications persist until the user explicitly dismisses — the
      // whole point of the replay is that the user has a chance to read them.
      if ((!isRinging || isBriefing) && !isMissed) {
         const dismissMs = isBriefing ? 60000 : 10000;
         setTimeout(() => {
            if (banner.parentNode) {
               banner.classList.add('sched-banner-out');
               setTimeout(() => banner.remove(), 300);
            }
         }, dismissMs);
      }
   }

   /**
    * Simple HTML escape
    */
   function escapeHtml(str) {
      if (typeof DawnFormat !== 'undefined' && DawnFormat.escapeHtml) {
         return DawnFormat.escapeHtml(str);
      }
      const div = document.createElement('div');
      div.textContent = str;
      return div.innerHTML;
   }

   /**
    * Handle scheduler notification from WebSocket
    */
   function handleNotification(payload) {
      if (!payload) return;

      /* Any non-ringing status (dismissed, timed_out, cancelled, snoozed) for an
       * alarm stops the loop regardless of whether we show a banner for it. */
      if (payload.event_type === 'alarm' && payload.status !== 'ringing') {
         stopAlarmLoop(payload.event_id);
      }

      /* Suppress rebroadcast of our own dismiss action */
      if (payload.status !== 'ringing' && localDismissed.has(payload.event_id)) {
         return;
      }

      /* Ignore timed_out / auto-dismiss if banner is already showing —
       * keep it up until user explicitly dismisses */
      if (payload.status === 'timed_out' || payload.message?.startsWith('Auto-')) {
         const el = document.getElementById('scheduler-notifications');
         if (el?.querySelector(`[data-event-id="${payload.event_id}"]`)) {
            return;
         }
      }

      console.log('Scheduler notification:', payload);
      showNotification(payload);
      showBrowserNotification(payload);
      /* Short chime when a live notification arrives. Skip only for missed
       * replays (the event already fired). tts_routed is ignored here — any
       * TTS is one-shot and the chime is brief; overlap is fine. */
      if (!payload.missed) {
         playChime(payload.event_type);
      }

      /* Ringing alarms get a looping tone until dismiss/snooze/timeout,
       * mirroring the daemon's local-speaker behavior. Skip for missed
       * replays; always loop for live ringing alarms even if the session has
       * TTS enabled — the alarm keeps sounding regardless of the one-shot
       * spoken announcement. */
      if (payload.event_type === 'alarm' && payload.status === 'ringing' && !payload.missed) {
         startAlarmLoop(payload.event_id);
      }

      /* Track unread briefings and refresh history sidebar */
      if (payload.event_type === 'briefing' && payload.conversation_id) {
         addUnreadBriefing(payload.conversation_id);
         if (typeof DawnHistory !== 'undefined') {
            DawnHistory.refreshList();
            DawnHistory.open();
         }
      }
   }

   // Public API
   window.DawnScheduler = {
      handleNotification,
      getUnreadBriefings,
      removeUnreadBriefing,
      isAlarmSoundsEnabled,
      setAlarmSounds,
   };
})();
