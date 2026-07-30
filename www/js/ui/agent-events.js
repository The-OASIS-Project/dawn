/*
 * DawnAgentEvents — CP4b: render the durable conversation_events stream as
 * transcript boundary markers for a viewed background job.
 *
 * Scope (deliberate): the browser renders the `complete` disposition and the
 * `resume` boundary — event kinds NOT present in `messages`.  `tool_call`/
 * `tool_result` are ignored because the conversation's `messages` already carry
 * them (rendered debug-gated by the load path); rendering the events too would
 * double-render, and messages survive event pruning while events do not.  `status`
 * is transient and already surfaced by the generating indicator.
 *
 * Ordering: a `complete` event always fires after the final message, so it
 * appends.  A `resume` is a mid-transcript boundary, placed by `created_at` against
 * the message entries (each tagged `data-ts` by the load path — `insertByTs`).
 * Replay events that arrive while the async message render is still in flight are
 * BUFFERED and flushed once the DOM is complete (entries all tagged), so a marker
 * can't land before the entry it should follow.  Live events (post-flush) append.
 *
 * Rendering is text-only (createElement/textContent) — event payloads are
 * untrusted (§8.6/§8.7).  Markers are appended straight into `#transcript`,
 * deliberately OUTSIDE DawnTranscript's ownership: they rely on a full transcript
 * clear on every (re)load to avoid accumulating, so a future DawnTranscript that
 * tracks its own children (virtualization / selective clear) must account for them.
 */
(function () {
   'use strict';

   // Per-viewed-conversation state.  Reset on every (re)attach so a fresh view
   // never inherits another conversation's dedup set or buffer.
   let activeConvId = 0; // conversation currently attached/viewed (0 = none)
   let renderedSeqs = new Set(); // seqs already drawn (dedup across replay + live)
   let flushed = false; // has the message render completed (buffer drained)?
   let buffer = []; // {ev, live} received before the render completed

   // Kinds this renderer draws.  Everything else (tool_call/tool_result/status) is
   // intentionally dropped — see file header.  A set so an unknown or future kind is
   // ignored, not thrown.
   const MARKER_KINDS = { complete: true, resume: true };

   function transcriptEl() {
      return document.getElementById('transcript');
   }

   /* Reset for a new attach.  Called by the load path before it sends
    * attach_conversation, so no marker/dedup state from the previous view leaks. */
   function reset(convId) {
      activeConvId = convId > 0 ? convId : 0;
      renderedSeqs = new Set();
      flushed = false;
      buffer = [];
   }

   /* Called by the load path once the async message render has fully appended —
    * the DOM is now complete, so buffered replay events can append AFTER the last
    * message rather than mid-render. */
   function flush(convId) {
      if (convId !== activeConvId) return;
      flushed = true;
      const pending = buffer;
      buffer = [];
      for (const b of pending) renderMarker(b.ev, b.live);
   }

   /* Human-facing disposition line for a `complete` event. */
   function completeLine(payload) {
      const disp = (payload && payload.disposition) || 'done';
      const err = payload && payload.error;
      switch (disp) {
         case 'done':
            return { cls: 'ok', text: 'Background job completed' };
         case 'failed':
            return { cls: 'err', text: 'Background job failed' + (err ? ': ' + err : '') };
         case 'cancelled':
            return { cls: 'muted', text: 'Background job cancelled' };
         case 'interrupted':
            return { cls: 'muted', text: 'Background job interrupted' };
         default:
            return { cls: 'muted', text: 'Background job ' + disp };
      }
   }

   /* Build the marker element (text-only). Returns null for kinds we don't draw.
    * `live` marks an event happening now — only then is role="status" set, so a
    * replayed marker on reload is not re-announced by a screen reader as if fresh. */
   function buildMarker(kind, payload, live) {
      const el = document.createElement('div');
      el.className = 'agent-event';
      if (kind === 'complete') {
         const info = completeLine(payload);
         el.classList.add('agent-event-' + info.cls);
         el.textContent = info.text; // textContent — payload.error is untrusted
      } else if (kind === 'resume') {
         el.classList.add('agent-event-muted');
         el.textContent = 'Resumed — continuing the task';
      } else {
         return null;
      }
      if (live) el.setAttribute('role', 'status');
      return el;
   }

   /* Insert `el` at the position matching `createdAt` among the transcript's message
    * entries (each tagged data-ts by the load path).  A `complete` (latest ts) and a
    * live event (createdAt=Infinity) append; a replayed `resume` slots in at its
    * boundary — before the first entry created after it. */
   function insertByTs(el, createdAt) {
      const t = transcriptEl();
      if (!t) return;
      if (createdAt !== Infinity) {
         const entries = t.children;
         for (let i = 0; i < entries.length; i++) {
            const ts = parseInt(entries[i].getAttribute('data-ts') || '', 10);
            if (!isNaN(ts) && ts > createdAt) {
               t.insertBefore(el, entries[i]);
               return;
            }
         }
      }
      t.appendChild(el);
   }

   /* Draw one event (already ownership/active-conv checked). Dedups by seq. */
   function renderMarker(ev, live) {
      if (!ev || !MARKER_KINDS[ev.kind]) return;
      if (renderedSeqs.has(ev.seq)) return;
      renderedSeqs.add(ev.seq);

      let payload = null;
      if (typeof ev.payload === 'string' && ev.payload) {
         try {
            payload = JSON.parse(ev.payload);
         } catch (e) {
            payload = null; // pruned/garbled — render the kind's default line
         }
      }
      const el = buildMarker(ev.kind, payload, live);
      if (!el) return;
      // Replay carries created_at (interleave by boundary); a live event is "now".
      insertByTs(el, typeof ev.created_at === 'number' ? ev.created_at : Infinity);
   }

   /* Replay batch: {conversation_id, events:[{seq,kind,created_at,payload}], has_more, last_seq}. */
   function handleConversationEvents(payload) {
      if (!payload || payload.conversation_id !== activeConvId) return; // foreign/stale view
      const events = Array.isArray(payload.events) ? payload.events : [];
      for (const ev of events) {
         if (!ev || !MARKER_KINDS[ev.kind]) continue; // don't buffer what we won't draw
         if (flushed) renderMarker(ev, false);
         else buffer.push({ ev: ev, live: false }); // render after the message DOM is complete
      }
      // Bounded: the replay is the OLDEST CONV_EVENT_REPLAY_MAX (200) events, and a
      // `complete` is the highest seq, so a finished job with >200 durable events
      // (~100+ tool calls — pathological) won't show its completion marker on
      // RELOAD.  It still renders LIVE (the completion fires while watched) and the
      // disposition is in the jobs panel.  Paginating would re-attach, which reloads
      // every message and flickers the transcript — not worth it for this edge.
   }

   /* Live single event: {conversation_id, seq, kind, payload}. */
   function handleConversationEvent(payload) {
      if (!payload || payload.conversation_id !== activeConvId) return; // not the viewed job
      if (!MARKER_KINDS[payload.kind]) return; // tool_call/tool_result/status — not drawn here
      // Before flush (message render still in flight) buffer so ordering holds.
      if (flushed) renderMarker(payload, true);
      else buffer.push({ ev: payload, live: true });
   }

   window.DawnAgentEvents = {
      reset: reset,
      flush: flush,
      handleConversationEvents: handleConversationEvents,
      handleConversationEvent: handleConversationEvent,
   };
})();
