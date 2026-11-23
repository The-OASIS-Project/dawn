/**
 * DAWN Turn Wait
 *
 * One turn runs on a conversation at a time.  When this tab sends a message
 * while another session's reply on the same conversation is still running,
 * the server holds the turn and says so (turn_waiting); the question itself is
 * echoed only once the turn starts.  This module shows the held question as a
 * pending bubble at the end of its conversation, with a status line and, after
 * a while, a button to stop the other reply.  If the held turn is stopped
 * before it starts (turn_unsent), nothing of it was saved, so a typed message
 * goes back to the composer.
 *
 * Wire (flag `turn_wait`):
 *   S→C turn_waiting {conversation_id, client_ref?, waiting, holder?, stop_after_ms?}
 *   S→C turn_unsent {conversation_id, client_ref?, text_saved:false}
 *   C→S stop_other_reply {conversation_id}
 *   S→C stop_other_reply_response {conversation_id, success, error?}
 *
 * Usage (hooks from dawn.js):
 *   DawnTurnWait.setEnabled(bool)       // from the config frame's features
 *   DawnTurnWait.noteSent(info) → ref   // before a text frame goes out
 *   DawnTurnWait.handleWaiting(payload) / handleUnsent / handleStopResponse
 *   DawnTurnWait.handleUserEcho(payload) / handleReplyStart / handleIdle
 *   DawnTurnWait.noteStop()             // the user pressed Stop
 *   DawnTurnWait.reset()                // the socket dropped
 */
(function (global) {
   'use strict';

   const DEFAULT_STOP_AFTER_MS = 5000;
   const MAX_NOTES = 16;
   // The question isn't sent yet; every line says what it waits for.
   const STATUS_TEXT = {
      turn: 'Sends when the reply in progress finishes…',
      channel: 'Sends when the reply in progress finishes…',
      job: "Sends when the background job's turn finishes…",
   };
   const STOPPING_TEXT = 'Stopping the other reply…';
   // A new chat has no conversation id until its first message is saved; an
   // unsent first message is kept under this key and offered in a new chat.
   const NEW_CHAT = '0';

   let enabled = false;
   let refSeq = 0;
   let observer = null;
   // client_ref → what was sent, so a held turn can show and give back its text.
   const notes = new Map();
   // conversation id (string) → the held turn on it (one per conversation).
   const records = new Map();
   // conversation id (string) → {note, shown, toast}: an unsent typed message,
   // kept until it is put back or the user sends again there.  The server saved
   // nothing of it, so this is the only copy; it is offered again each time
   // the conversation comes back on screen.
   const unsentByConv = new Map();

   function transcriptEl() {
      return document.getElementById('transcript');
   }

   function activeConv() {
      if (typeof DawnHistory === 'undefined' || !DawnHistory.getActiveConversationId) return null;
      const id = DawnHistory.getActiveConversationId();
      return id ? String(id) : NEW_CHAT;
   }

   function setEnabled(on) {
      enabled = !!on;
      if (!enabled) reset();
   }

   /**
    * Remember a text turn about to be sent.  Returns the client_ref to put on
    * the frame, or null when the server has no turn_wait (frames unchanged).
    * info: {text, conversationId, images, documents, fromVisual}
    */
   function noteSent(info) {
      if (!enabled || !info) return null;
      // A new message from the user in this conversation retires an unsent one,
      // except attachments still waiting for room: this send makes the room.
      const conv = activeConv();
      if (!info.fromVisual && conv !== null) {
         const unsent = unsentByConv.get(conv);
         if (unsent && unsent.partial) unsent.shown = false;
         else forgetUnsent(conv);
      }
      const ref = Date.now().toString(36) + '-' + ++refSeq;
      notes.set(ref, {
         text: info.text || '',
         images: Array.isArray(info.images) ? info.images.slice() : [],
         documents: Array.isArray(info.documents) ? info.documents.slice() : [],
         fromVisual: !!info.fromVisual,
      });
      while (notes.size > MAX_NOTES) {
         notes.delete(notes.keys().next().value);
      }
      return ref;
   }

   // --- the pending bubble ---------------------------------------------------

   function buildBubble(rec) {
      const el = document.createElement('div');
      el.className = 'transcript-entry user turn-wait-entry';
      const role = document.createElement('div');
      role.className = 'role';
      role.textContent = 'user';
      el.appendChild(role);

      const text = document.createElement('div');
      if (rec.voice) {
         text.className = 'text turn-wait-voice';
         text.textContent = 'Voice message';
      } else {
         text.className = 'text turn-wait-text';
         text.textContent = rec.note && rec.note.text ? rec.note.text : 'Your message';
      }
      el.appendChild(text);

      const row = document.createElement('div');
      row.className = 'turn-wait-status-row';
      const status = document.createElement('span');
      status.className = 'turn-wait-status';
      status.setAttribute('role', 'status');
      row.appendChild(status);
      el.appendChild(row);

      rec.el = el;
      rec.row = row;
      rec.statusEl = status;
   }

   // Set after a tick so a status region that was just inserted is announced.
   function setStatus(rec, msg) {
      rec.statusText = msg;
      setTimeout(() => {
         if (rec.statusEl) rec.statusEl.textContent = rec.statusText;
      }, 50);
   }

   function showStopButton(rec) {
      if (rec.phase !== 'waiting' || rec.btn) return;
      const btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'coding-card-btn secondary turn-wait-stop';
      btn.textContent = 'Stop the other reply';
      btn.addEventListener('click', () => requestStop(rec));
      rec.row.appendChild(btn);
      rec.btn = btn;
   }

   function setButtonBusy(rec, busy) {
      if (!rec.btn) return;
      // aria-disabled keeps keyboard focus on the button while the request runs.
      if (busy) rec.btn.setAttribute('aria-disabled', 'true');
      else rec.btn.removeAttribute('aria-disabled');
   }

   function requestStop(rec) {
      if (rec.phase !== 'waiting' || rec.stopInFlight || rec.stopAsked) return;
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      rec.stopInFlight = true;
      setButtonBusy(rec, true);
      setStatus(rec, STOPPING_TEXT);
      DawnWS.send({ type: 'stop_other_reply', payload: { conversation_id: rec.convNum } });
   }

   // Removing a focused control would drop focus to <body>; hand it to the composer.
   function releaseFocus(container) {
      if (!container || !container.contains(document.activeElement)) return;
      const input = DawnElements && DawnElements.textInput;
      if (input) input.focus({ preventScroll: true });
   }

   function dropRecord(rec) {
      clearTimeout(rec.timer);
      if (rec.el) {
         releaseFocus(rec.el);
         rec.el.remove();
      }
      records.delete(rec.conv);
   }

   // Keep the held question last in its own conversation, below whatever the
   // other session adds meanwhile, and only on screen while that conversation is.
   // The bubble itself stays put once placed (moving it would replay its fade-in,
   // drop focus from its button and re-announce its status); newcomers are moved
   // in front of it instead.  Those moves are mutations too, but leave the bubble
   // last, so the next call does nothing.
   function sync() {
      const t = transcriptEl();
      if (!t) return;
      const active = activeConv();
      records.forEach((rec) => {
         if (rec.conv !== active && rec.el && rec.el.isConnected) {
            releaseFocus(rec.el);
            rec.el.remove();
         }
      });
      const rec = active !== null ? records.get(active) : null;
      if (rec && rec.el) {
         if (rec.el.parentNode !== t) {
            const placeholder = t.querySelector('.transcript-placeholder');
            if (placeholder) placeholder.remove();
            t.appendChild(rec.el);
            t.scrollTop = t.scrollHeight;
         } else if (rec.el.nextElementSibling) {
            const focused = document.activeElement;
            let n;
            while ((n = rec.el.nextElementSibling)) t.insertBefore(n, rec.el);
            if (focused && focused !== document.activeElement && focused.isConnected) {
               focused.focus({ preventScroll: true });
            }
         }
      }
      offerUnsent();
   }

   function ensureObserver() {
      if (observer) return;
      const t = transcriptEl();
      if (!t || typeof MutationObserver === 'undefined') return;
      observer = new MutationObserver(sync);
      observer.observe(t, { childList: true });
   }

   // --- composer -------------------------------------------------------------

   function composerEmpty() {
      const input = DawnElements && DawnElements.textInput;
      const noText = !input || input.value.trim() === '';
      const noImages = !(typeof DawnVision !== 'undefined' && DawnVision.hasPendingImages());
      const noDocs = !(typeof DawnDocuments !== 'undefined' && DawnDocuments.hasPendingDocuments());
      return noText && noImages && noDocs;
   }

   // Put a message back without taking focus: a draft already there is kept and
   // the unsent text goes after it.
   function putBack(note) {
      const input = DawnElements && DawnElements.textInput;
      if (input && note.text) {
         const draft = input.value;
         input.value = draft.trim() ? draft.replace(/\s+$/, '') + '\n\n' + note.text : note.text;
         input.dispatchEvent(new Event('input', { bubbles: true }));
      }
      // What the composer's limits leave no room for stays with the note.
      let images = note.images;
      let documents = note.documents;
      if (images.length && typeof DawnVision !== 'undefined' && DawnVision.restoreImages) {
         images = DawnVision.restoreImages(images) || [];
      }
      if (
         documents.length &&
         typeof DawnDocuments !== 'undefined' &&
         DawnDocuments.restoreDocuments
      ) {
         documents = DawnDocuments.restoreDocuments(documents) || [];
      }
      return { text: '', images, documents };
   }

   // Put an unsent message back; attachments that didn't fit are kept and
   // offered again, the text given back only once.
   function restoreUnsent(conv, entry) {
      const left = putBack(entry.note);
      if (left.images.length || left.documents.length) {
         entry.note = left;
         entry.partial = true;
         entry.shown = true;
         DawnToast.show(
            "Some attachments didn't fit. They'll be offered again once there's room.",
            'warning'
         );
      } else {
         unsentByConv.delete(conv);
      }
   }

   function forgetUnsent(conv) {
      const entry = unsentByConv.get(conv);
      if (!entry) return;
      unsentByConv.delete(conv);
      if (entry.toast) entry.toast.dismiss();
   }

   function giveBack(conv, entry) {
      entry.shown = true;
      if (composerEmpty()) {
         const rest = entry.partial;
         restoreUnsent(conv, entry);
         DawnToast.show(
            rest
               ? "The attachments that didn't fit are back in the message box."
               : "Your message wasn't sent. It's back in the message box.",
            'info'
         );
         return;
      }
      // The user has started something new: never overwrite it.  The unsent one
      // waits in a toast; dismissing the toast keeps it for the next visit.
      entry.toast = DawnToast.show("Your message wasn't sent.", 'warning', 0, {
         actions: [
            {
               label: 'Put it back',
               onClick: () => {
                  restoreUnsent(conv, entry);
                  if (entry.toast) entry.toast.dismiss();
               },
            },
         ],
         onDismiss: () => {
            entry.toast = null;
         },
      });
   }

   // Offer each unsent message once per visit to its conversation; leaving the
   // conversation takes its toast away (its button would fill the wrong composer).
   function offerUnsent() {
      const active = activeConv();
      unsentByConv.forEach((entry, conv) => {
         if (conv !== active) {
            entry.shown = false;
            if (entry.toast) entry.toast.dismiss();
         } else if (!entry.shown) {
            giveBack(conv, entry);
         }
      });
   }

   // --- frames ---------------------------------------------------------------

   function convKey(payload) {
      return payload && payload.conversation_id ? String(payload.conversation_id) : null;
   }

   function handleWaiting(payload) {
      if (!enabled) return;
      const conv = convKey(payload);
      if (!conv) return;
      if (payload.waiting === false) {
         const rec = records.get(conv);
         if (!rec) return;
         // The turn starts: its own echo replaces the bubble (handleUserEcho), so
         // the question never blinks out between the two frames.
         rec.phase = 'granted';
         clearTimeout(rec.timer);
         if (rec.row) {
            releaseFocus(rec.row);
            rec.row.remove();
         }
         return;
      }
      const ref = typeof payload.client_ref === 'string' ? payload.client_ref : null;
      const holderText = STATUS_TEXT[payload.holder] || STATUS_TEXT.turn;
      const old = records.get(conv);
      // The same wait said again (after a reconnect): keep its bubble and timer.
      if (old && old.phase === 'waiting' && old.ref === ref) {
         const idle = !old.stopping && !old.stopInFlight && !old.stopAsked;
         // Only a changed line is set again, so a reader doesn't hear it twice.
         if (idle && old.holderText !== holderText) setStatus(old, holderText);
         old.holderText = holderText;
         ensureObserver();
         sync();
         return;
      }
      if (old) dropRecord(old);
      // With no note (a reload since it was sent) the bubble says "Your message".
      const rec = {
         conv,
         convNum: payload.conversation_id,
         ref,
         note: ref ? notes.get(ref) || null : null,
         voice: !ref,
         phase: 'waiting',
         stopInFlight: false,
         holderText,
      };
      buildBubble(rec);
      records.set(conv, rec);
      setStatus(rec, holderText);
      const after =
         typeof payload.stop_after_ms === 'number' && payload.stop_after_ms >= 0
            ? payload.stop_after_ms
            : DEFAULT_STOP_AFTER_MS;
      rec.timer = setTimeout(() => showStopButton(rec), after);
      ensureObserver();
      sync();
   }

   function handleUnsent(payload) {
      if (!enabled) return;
      const conv = payload && payload.conversation_id === 0 ? NEW_CHAT : convKey(payload);
      if (!conv) return;
      const rec = records.get(conv);
      const ref = typeof payload.client_ref === 'string' ? payload.client_ref : null;
      const note = (ref && notes.get(ref)) || (rec && rec.note) || null;
      if (ref) notes.delete(ref);

      if (rec && rec.voice) {
         // Nothing to give back (the words were never sent to this tab): the
         // bubble stays, marked, until the conversation is next drawn.
         clearTimeout(rec.timer);
         records.delete(conv);
         rec.phase = 'unsent';
         if (rec.btn) {
            releaseFocus(rec.btn);
            rec.btn.remove();
         }
         rec.el.classList.add('unsent');
         setStatus(rec, "Not sent. Say it again when you're ready.");
         if (conv !== activeConv()) DawnToast.show("Your voice message wasn't sent.", 'info');
         return;
      }
      if (rec) dropRecord(rec);
      if (!note || note.fromVisual) return;
      forgetUnsent(conv);
      unsentByConv.set(conv, { note, shown: false, toast: null });
      // A turn stopped before it ran sends no turn_waiting, so nothing else
      // has started the observer that re-offers or withdraws on a chat switch.
      ensureObserver();
      offerUnsent();
   }

   function handleStopResponse(payload) {
      if (!enabled) return;
      const rec = records.get(convKey(payload) || '');
      if (rec) rec.stopInFlight = false;
      if (payload && payload.success) {
         // The button stays (busy) so keyboard focus isn't dropped; the grant follows.
         if (rec && rec.phase === 'waiting') rec.stopAsked = true;
         return;
      }
      DawnToast.show(
         payload && payload.error === 'NOT_RUNNING'
            ? 'That reply already finished.'
            : "Couldn't stop the other reply.",
         'info'
      );
      if (rec && rec.phase === 'waiting') {
         setButtonBusy(rec, false);
         if (!rec.stopping) setStatus(rec, rec.holderText);
      }
   }

   // The turn's own question arrived: the bubble has done its job.
   function handleUserEcho(payload) {
      if (!enabled || !payload || payload.role !== 'user') return;
      const ref = typeof payload.client_ref === 'string' ? payload.client_ref : null;
      if (ref) notes.delete(ref);
      records.forEach((rec) => {
         if ((ref && rec.ref === ref) || (!ref && rec.voice && rec.conv === convKey(payload))) {
            dropRecord(rec);
         }
      });
   }

   // Only this tab's own granted turn ends the bubble here: while it waits, the
   // reply or background job it waits for streams to every viewer of the
   // conversation, and its stream_start frames say nothing about this turn.
   function handleReplyStart(payload) {
      if (!enabled) return;
      const rec = records.get(convKey(payload) || '');
      if (rec && rec.phase === 'granted') dropRecord(rec);
   }

   // A granted turn that ended without an echo (refused, failed) leaves nothing.
   function handleIdle(payload) {
      if (!enabled) return;
      const conv = convKey(payload);
      Array.from(records.values()).forEach((rec) => {
         if (rec.phase === 'granted' && (conv === null || rec.conv === conv)) dropRecord(rec);
      });
   }

   // Whether it stopped a held message (then nothing of this tab's was streaming).
   function noteStop() {
      let held = false;
      records.forEach((rec) => {
         if (rec.phase !== 'waiting') return;
         held = true;
         clearTimeout(rec.timer);
         if (rec.btn) {
            releaseFocus(rec.btn);
            rec.btn.remove();
         }
         rec.btn = null;
         rec.stopping = true;
         setStatus(rec, 'Not sending…');
      });
      return held;
   }

   // What a dropped socket leaves of a held turn is unknown; the reconnect's
   // reload shows the truth.  Unsent messages stay: the server confirmed nothing
   // of them was saved, and a shutdown sends turn_unsent just before the drop.
   function reset() {
      Array.from(records.values()).forEach(dropRecord);
   }

   global.DawnTurnWait = {
      setEnabled,
      isEnabled: () => enabled,
      noteSent,
      handleWaiting,
      handleUnsent,
      handleStopResponse,
      handleUserEcho,
      handleReplyStart,
      handleIdle,
      noteStop,
      reset,
   };
})(window);
