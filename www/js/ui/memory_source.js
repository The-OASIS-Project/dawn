/*
 * DAWN Memory Source Viewer (DawnMemorySource)
 *
 * Renders the "Source Conversation" modal for a memory fact as a mini chat
 * window in DAWN transcript style: role-tinted blocks with markdown + inline
 * images, reusing the main transcript's render helpers (DawnFormat.markdown,
 * DawnVision image pipeline, DawnTranscript.createImageElement / createDocumentChips)
 * so a fact's source reads exactly like the chat it came from.
 *
 * Extracted from memory.js (which is over the JS file-size limit); DawnMemory
 * keeps a thin forwarder to handleFactSourceResponse so the WS dispatch in
 * dawn.js is unchanged.
 */
(function () {
   'use strict';

   let sourceModalTrigger = null;
   let sourceEscToken = null; // DawnEscStack registration while the source modal is open
   let requestedFactId = null; // guards against a stale response for a fact we're no longer showing

   const REASON_TEXT = {
      forbidden: "You don't have access to this source conversation.",
      invalid_range: 'Source range is invalid for this fact.',
      error: "Couldn't load the source (server error). Please try again.",
   };

   /* DawnFormat is a hard dependency (format.js loads first), but guard it for
    * parity with the other Dawn* deps here — a missing format.js falls back to
    * safe escaped text rather than throwing (and never to unsanitized HTML). */
   function escapeText(s) {
      if (typeof DawnFormat !== 'undefined' && DawnFormat.escapeHtml) {
         return DawnFormat.escapeHtml(s);
      }
      const d = document.createElement('div');
      d.textContent = s == null ? '' : String(s);
      return d.innerHTML;
   }

   function markdownHtml(text) {
      if (typeof DawnFormat !== 'undefined' && DawnFormat.markdown) {
         return DawnFormat.markdown(text); // DOMPurify-sanitized
      }
      return escapeText(text); // fallback: plain escaped text, no markup
   }

   /* Absolute short timestamp for a message header, e.g. "Feb 22, 3:14 PM".
    * created_at is unix seconds. Self-contained (no memory.js coupling). */
   function formatSourceTime(createdAtSeconds) {
      if (!createdAtSeconds) return '';
      try {
         return new Date(createdAtSeconds * 1000).toLocaleString([], {
            month: 'short',
            day: 'numeric',
            hour: 'numeric',
            minute: '2-digit',
         });
      } catch (e) {
         return '';
      }
   }

   /* Build one source message as a `.transcript-entry` node so it inherits the
    * transcript's markdown styling, role tints, image constraints and the shared
    * image lightbox (delegated on `.transcript-entry img`). Mirrors the pre-render
    * order of DawnTranscript.addNormal: documents -> images -> thinking -> visuals
    * -> markdown, with visuals shown as a placeholder (live render is deferred). */
   function renderSourceMessage(m) {
      const role = m && m.role === 'assistant' ? 'assistant' : 'user';
      const entry = document.createElement('div');
      entry.className = `transcript-entry ${role}`;

      let text = (m && m.content) || '';

      // 1. Document markers -> filename chip (strip the inlined document body).
      let docs = [];
      if (window.DawnDocuments && DawnDocuments.parseDocumentMarkers) {
         const d = DawnDocuments.parseDocumentMarkers(text);
         text = d.cleanText;
         docs = d.documents || [];
      }

      // 2. Image markers -> ids for the async load below.
      let parsed = null;
      if (window.DawnVision && DawnVision.parseImageMarkers) {
         parsed = DawnVision.parseImageMarkers(text);
         text = parsed.text;
      }

      // 3. Strip reasoning/chain-of-thought so only the dialogue shows (the live
      //    chat extracts these upstream, before this render path). Two distinct
      //    persisted forms: <dawn:thinking …>…</dawn:thinking> is PAIRED and holds
      //    the chain-of-thought; <dawn:reasoning tokens="N"/> is SELF-CLOSING (a
      //    token-count marker, no content). Plus a self-inserted plain <thinking>.
      text = text
         .replace(/<dawn:thinking\b[^>]*>[\s\S]*?<\/dawn:thinking>\s*/g, '')
         .replace(/<dawn:reasoning\b[^>]*\/?>\s*/g, '')
         .replace(/<thinking>[\s\S]*?<\/thinking>\s*/g, '');

      // 4. Extract <dawn-visual> blocks (DOMPurify would strip the tag but keep
      //    children) -> a compact placeholder; live render is out of scope here.
      let visuals = [];
      if (typeof DawnVisualRender !== 'undefined' && DawnVisualRender.extractVisuals) {
         const v = DawnVisualRender.extractVisuals(text);
         text = v.cleanText;
         visuals = v.visuals || [];
      }

      const timeStr = formatSourceTime(m && m.created_at);
      const roleLabel = escapeText(role);
      const timeHtml = timeStr
         ? ` · <span class="memory-source-time">${escapeText(timeStr)}</span>`
         : '';
      // markdownHtml returns DOMPurify-sanitized HTML — do NOT double-escape it.
      entry.innerHTML = `
      <div class="role">${roleLabel}${timeHtml}</div>
      <div class="text">${markdownHtml(text)}</div>
    `;
      const textEl = entry.querySelector('.text');
      const anchor = textEl || entry;

      // Document chips (reused from the transcript).
      if (docs.length > 0 && window.DawnTranscript && DawnTranscript.createDocumentChips) {
         anchor.appendChild(DawnTranscript.createDocumentChips(docs));
      }

      // Visual placeholders.
      for (const v of visuals) {
         const ph = document.createElement('div');
         ph.className = 'memory-source-visual';
         ph.textContent = `[Visual: ${(v && v.title) || (v && v.type) || 'diagram'}]`;
         anchor.appendChild(ph);
      }

      // Images load asynchronously (network) — append when resolved, guarding
      // against a stale render (modal already closed / re-opened for another fact).
      const expectedImages =
         parsed && parsed.imageIds
            ? parsed.imageIds.length + (parsed.imageDataUrls || []).length
            : 0;
      if (
         expectedImages > 0 &&
         window.DawnVision &&
         DawnVision.loadParsedImages &&
         window.DawnTranscript &&
         DawnTranscript.createImageElement
      ) {
         DawnVision.loadParsedImages(parsed).then((urls) => {
            if (!entry.isConnected) return; // stale: modal closed or re-opened
            const loaded = urls || [];
            if (loaded.length > 0) {
               const container = document.createElement('div');
               container.className = 'transcript-images-container';
               for (const u of loaded) {
                  const el = DawnTranscript.createImageElement(u);
                  if (el) container.appendChild(el);
               }
               if (container.children.length > 0) anchor.appendChild(container);
            }
            if (loaded.length < expectedImages) {
               const note = document.createElement('div');
               note.className = 'memory-source-image-missing';
               note.textContent = '[image unavailable]';
               anchor.appendChild(note);
            }
         });
      }

      return entry;
   }

   function handleFactSourceResponse(payload) {
      const modal = document.getElementById('memory-source-modal');
      const body = document.getElementById('memory-source-body');
      if (!modal || !body) return;

      // Nothing awaited: the modal was closed (requestedFactId nulled on close)
      // before this response arrived — drop it, so a late reply can't re-open a
      // modal the user already dismissed (whose ESC handler is unregistered).
      if (requestedFactId == null) return;
      // Drop a response for a different fact than the one now being shown.
      if (
         payload &&
         payload.fact_id != null &&
         Number(payload.fact_id) !== Number(requestedFactId)
      ) {
         return;
      }

      if (!payload || !payload.success) {
         const msg = (payload && REASON_TEXT[payload.reason]) || 'Source no longer available.';
         body.innerHTML = `<p class="memory-source-unavailable">${escapeText(msg)}</p>`;
         modal.classList.remove('hidden');
         return;
      }

      const msgs = payload.messages || [];
      body.innerHTML = '';
      if (msgs.length === 0) {
         body.innerHTML = '<p class="memory-source-unavailable">No messages in source range.</p>';
      } else {
         for (const m of msgs) {
            body.appendChild(renderSourceMessage(m));
         }
      }
      modal.classList.remove('hidden');
   }

   function openSourceModal(factId) {
      if (!Number.isFinite(factId)) return; // defensive: never request a NaN id
      const modal = document.getElementById('memory-source-modal');
      const body = document.getElementById('memory-source-body');
      if (!modal || !body) return;
      requestedFactId = factId;
      sourceModalTrigger = document.activeElement;
      body.innerHTML = '<p class="memory-source-loading">Loading…</p>';
      modal.classList.remove('hidden');
      if (sourceEscToken === null && typeof DawnEscStack !== 'undefined') {
         sourceEscToken = DawnEscStack.register(() => {
            closeSourceModal();
            return true;
         });
      }
      const closeBtn = document.getElementById('memory-source-close');
      if (closeBtn) closeBtn.focus();
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({ type: 'get_memory_fact_source', payload: { fact_id: factId } });
      }
   }

   function closeSourceModal() {
      const modal = document.getElementById('memory-source-modal');
      if (modal) modal.classList.add('hidden');
      requestedFactId = null;
      if (sourceEscToken !== null && typeof DawnEscStack !== 'undefined') {
         DawnEscStack.unregister(sourceEscToken);
         sourceEscToken = null;
      }
      if (sourceModalTrigger && typeof sourceModalTrigger.focus === 'function') {
         sourceModalTrigger.focus();
         sourceModalTrigger = null;
      }
   }

   function init() {
      // Close button + overlay click (Escape is handled via DawnEscStack on open).
      const srcClose = document.getElementById('memory-source-close');
      if (srcClose) srcClose.addEventListener('click', closeSourceModal);
      const srcModal = document.getElementById('memory-source-modal');
      if (srcModal) {
         srcModal.addEventListener('click', (e) => {
            if (e.target === srcModal) closeSourceModal();
         });
      }
   }

   if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', init);
   } else {
      init();
   }

   window.DawnMemorySource = {
      handleFactSourceResponse,
      openSourceModal,
      closeSourceModal,
   };
})();
