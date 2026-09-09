/**
 * DAWN Transcript Module
 * Renders chat messages with debug/command separation
 *
 * Usage:
 *   DawnTranscript.addEntry(role, text)        // Add transcript entry (routes debug content)
 *   DawnTranscript.addDebug(label, content)    // Add debug entry directly
 *   DawnTranscript.addNormal(role, text)       // Add normal entry directly
 */
(function (global) {
   'use strict';

   // =============================================================================
   // Thinking Block Helpers
   // =============================================================================

   /**
    * Check if text contains thinking content
    * @param {string} text - Text to check
    * @returns {boolean}
    */
   function containsThinkingContent(text) {
      return text.includes('<dawn:thinking');
   }

   /**
    * Extract thinking content and remaining message from text
    * @param {string} text - Text to parse
    * @returns {{thinking: {content: string, provider: string, duration: string}|null, remaining: string}}
    */
   function extractThinkingContent(text) {
      const thinkingRegex =
         /<dawn:thinking\s+provider="([^"]+)"\s+duration="([^"]+)">\n([\s\S]*?)\n<\/dawn:thinking>\n?/;
      const match = thinkingRegex.exec(text);

      if (!match) {
         return { thinking: null, remaining: text };
      }

      return {
         thinking: {
            provider: match[1],
            duration: match[2],
            content: match[3],
         },
         remaining: text.replace(match[0], '').trim(),
      };
   }

   /**
    * Format a thinking-block stats line. Mirrors the live-streaming helper so
    * historical messages render with the same "(2.0s, 43 tokens)" shape.
    */
   function formatThinkingStats(durationSec, tokens) {
      const parts = [];
      if (durationSec) parts.push(`${durationSec}s`);
      if (tokens > 0) parts.push(`${tokens.toLocaleString()} tokens`);
      return parts.length ? `(${parts.join(', ')})` : '';
   }

   /**
    * Coerce a persisted reasoning duration to a clean positive numeric string, or '0'.
    * Guards against a malformed/corrupted server-delivered value rendering verbatim.
    * @param {*} d - raw duration from the reasoning field
    * @returns {string}
    */
   function coerceReasoningDuration(d) {
      const n = parseFloat(d);
      return Number.isFinite(n) && n > 0 ? String(n) : '0';
   }

   /**
    * Create a thinking block element for display
    * @param {Object} thinking - Thinking data {provider, duration, content, tokens?}
    * @returns {HTMLElement}
    */
   function createThinkingBlock(thinking) {
      // SECURITY: on reload, `thinking` is parsed from the persisted (attacker-controllable)
      // `reasoning` column. `thinking.content` is rendered as markdown via DawnFormat.markdown,
      // which runs marked + DOMPurify (the same sanitized pipeline as message bodies). It MUST
      // stay on that DOMPurify-sanitized path — never assign raw content or unsanitized
      // marked output to innerHTML, which would be stored XSS.
      const entry = document.createElement('div');
      entry.className = 'thinking-block collapsed completed';
      entry.setAttribute('role', 'region');
      entry.setAttribute('aria-label', 'AI thinking process');

      // Label by the assistant's name (e.g. "Friday"), never the model/provider —
      // provider is mutable infrastructure detail the user shouldn't see.
      const stats = formatThinkingStats(thinking.duration, thinking.tokens || 0);
      const hasContent = thinking.content && thinking.content.trim().length > 0;
      // "thought" when there's a summary, "reasoned" when only token counts (no summary).
      const verb = hasContent ? 'thought' : 'reasoned';
      const contentHtml = hasContent
         ? DawnFormat.markdown(thinking.content)
         : '<em>No reasoning summary available for this turn.</em>';
      const contentClass = hasContent ? 'thinking-content' : 'thinking-content no-summary';

      // Defense-in-depth: thinking.duration could in principle contain HTML — escape.
      const safeDuration = DawnFormat.escapeHtml(stats);
      const safeLabel = DawnFormat.escapeHtml(`${DawnFormat.assistantName()} ${verb}`);

      entry.innerHTML = `
      <div class="thinking-header" role="button" tabindex="0" aria-expanded="false">
        <span class="thinking-icon" aria-hidden="true">💭</span>
        <span class="thinking-label">${safeLabel}</span>
        <span class="thinking-duration">${safeDuration}</span>
        <span class="thinking-toggle" aria-hidden="true">▼</span>
      </div>
      <div class="${contentClass}">${contentHtml}</div>
    `;

      // Add click handler for toggle
      const header = entry.querySelector('.thinking-header');
      header.addEventListener('click', () => toggleThinkingBlock(entry));
      header.addEventListener('keydown', (e) => {
         if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            toggleThinkingBlock(entry);
         }
      });

      return entry;
   }

   /**
    * Toggle thinking block expanded/collapsed state
    * @param {HTMLElement} entry - The thinking block element
    */
   function toggleThinkingBlock(entry) {
      const isCollapsed = entry.classList.contains('collapsed');
      entry.classList.toggle('collapsed', !isCollapsed);

      const header = entry.querySelector('.thinking-header');
      if (header) {
         header.setAttribute('aria-expanded', isCollapsed ? 'true' : 'false');
      }
   }

   // =============================================================================
   // Reasoning Block Helpers (OpenAI o-series)
   // =============================================================================

   /**
    * Check if text contains reasoning token marker
    * @param {string} text - Text to check
    * @returns {boolean}
    */
   function containsReasoningContent(text) {
      return text.includes('<dawn:reasoning');
   }

   /**
    * Extract reasoning tokens and remaining message from text
    * @param {string} text - Text to parse
    * @returns {{reasoning: {tokens: number}|null, remaining: string}}
    */
   function extractReasoningContent(text) {
      const reasoningRegex = /<dawn:reasoning\s+tokens="(\d+)"\/>\n?/;
      const match = reasoningRegex.exec(text);

      if (!match) {
         return { reasoning: null, remaining: text };
      }

      return {
         reasoning: {
            tokens: parseInt(match[1], 10),
         },
         remaining: text.replace(match[0], '').trim(),
      };
   }

   /**
    * Create a reasoning block element for display (OpenAI o-series)
    * @param {Object} reasoning - Reasoning data {tokens}
    * @returns {HTMLElement}
    */
   function createReasoningBlock(reasoning) {
      // Opaque reasoning has no summary text to reveal, so this is a static,
      // non-interactive note (token count only) — no fake expand button.
      // SECURITY: `reasoning` is attacker-controllable persisted JSON; every value
      // interpolated below MUST go through escapeHtml (never markdown()/raw innerHTML),
      // or a crafted reasoning blob becomes stored XSS. Keep this block on the escaped path.
      // Must stay markup-identical to streaming.js Case 2 so live and reload match.
      const entry = document.createElement('div');
      entry.className = 'thinking-block completed reasoning-only';
      entry.setAttribute('role', 'note');
      entry.setAttribute('aria-label', `${DawnFormat.assistantName()} reasoning summary`);

      const tokens = reasoning.tokens || 0;
      const stats = DawnFormat.escapeHtml(formatThinkingStats(null, tokens));
      const safeLabel = DawnFormat.escapeHtml(`${DawnFormat.assistantName()} reasoned`);
      entry.innerHTML = `
      <div class="thinking-header static">
        <span class="thinking-icon" aria-hidden="true">🧠</span>
        <span class="thinking-label">${safeLabel}</span>
        <span class="thinking-duration">${stats}</span>
      </div>
    `;

      return entry;
   }

   // =============================================================================
   // Image Marker Helpers
   // =============================================================================

   /**
    * Create a secure image element from a validated data URI
    * SECURITY: Only renders images with safe data URI prefixes
    * @param {string} dataUrl - The validated data URI
    * @returns {HTMLElement|null} - Image wrapper element or null if invalid
    */
   function createImageElement(dataUrl) {
      // SECURITY: Validate data URI using DawnVision's security check
      if (!DawnVision || !DawnVision.isSafeDataUri(dataUrl)) {
         console.warn('Transcript: Rejected unsafe image data URI');
         return null;
      }

      const wrapper = document.createElement('div');
      wrapper.className = 'transcript-image-wrapper';

      const img = document.createElement('img');
      img.className = 'transcript-image';
      img.alt = 'Uploaded image';
      img.src = dataUrl;

      /* Click-to-enlarge is handled by the shared image lightbox (delegated
       * click in setupImageLightbox), same as every other transcript image. */

      wrapper.appendChild(img);
      return wrapper;
   }

   // =============================================================================
   // Content Detection Helpers
   // =============================================================================

   /**
    * Check if text contains command tags
    * @param {string} text - Text to check
    * @returns {boolean}
    */
   function containsCommandTags(text) {
      return text.includes('<command>') || text.includes('[Tool Result:');
   }

   /**
    * Check if text is ONLY debug content (no user-facing text)
    * @param {string} text - Text to check
    * @returns {boolean}
    */
   function isOnlyDebugContent(text) {
      // Remove all command tags and tool results, see if anything meaningful remains
      const stripped = text
         .replace(/<command>[\s\S]*?<\/command>/g, '')
         .replace(/\[Tool Result:[\s\S]*?\]/g, '')
         .trim();
      return stripped.length === 0;
   }

   /**
    * Extract non-command text from a mixed message
    * @param {string} text - Text to extract from
    * @returns {string}
    */
   function extractUserFacingText(text) {
      return text
         .replace(/<command>[\s\S]*?<\/command>/g, '')
         .replace(/\[Tool Result:[\s\S]*?\]/g, '')
         .trim();
   }

   /**
    * Extract command/debug portions from a message
    * @param {string} text - Text to extract from
    * @returns {{commands: string[], toolResults: string[]}}
    */
   function extractDebugContent(text) {
      const commands = [];
      const toolResults = [];

      // Extract command tags
      const cmdRegex = /<command>([\s\S]*?)<\/command>/g;
      let match;
      while ((match = cmdRegex.exec(text)) !== null) {
         commands.push(match[0]);
      }

      // Extract tool results
      const toolRegex = /\[Tool Result:[\s\S]*?\]/g;
      while ((match = toolRegex.exec(text)) !== null) {
         toolResults.push(match[0]);
      }

      return { commands, toolResults };
   }

   // =============================================================================
   // Document Chip Rendering
   // =============================================================================

   /** SVG document icon (small, inline) */
   const DOC_ICON_SVG =
      '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
      'stroke-width="2" stroke-linecap="round" stroke-linejoin="round">' +
      '<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/>' +
      '<polyline points="14 2 14 8 20 8"/>' +
      '<line x1="16" y1="13" x2="8" y2="13"/>' +
      '<line x1="16" y1="17" x2="8" y2="17"/>' +
      '<polyline points="10 9 9 9 8 9"/>' +
      '</svg>';

   /** Small download-arrow glyph shown on chips backed by a stored original. */
   const DOWNLOAD_ICON_SVG =
      '<svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
      'stroke-width="2" stroke-linecap="round" stroke-linejoin="round">' +
      '<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/>' +
      '<polyline points="7 10 12 15 17 10"/>' +
      '<line x1="12" y1="15" x2="12" y2="3"/>' +
      '</svg>';

   /**
    * Download a stored original file.  Fetches first so a non-2xx (403/404/410
    * after retention/orphan-sweep) surfaces a toast instead of a silent no-op.
    * @returns {Promise<boolean>} true if the download started, false on failure.
    */
   async function downloadOriginal(blobId, filename) {
      try {
         const resp = await fetch('/api/documents/original/' + encodeURIComponent(blobId));
         if (!resp.ok) {
            const gone = resp.status === 404 || resp.status === 410;
            if (typeof DawnToast !== 'undefined') {
               DawnToast.show(
                  gone
                     ? 'Original file is no longer stored — showing extracted text instead.'
                     : 'Could not download the original file.',
                  'warning'
               );
            }
            return false;
         }
         const blob = await resp.blob();
         const url = URL.createObjectURL(blob);
         const a = document.createElement('a');
         a.href = url;
         a.download = filename || '';
         document.body.appendChild(a);
         a.click();
         a.remove();
         URL.revokeObjectURL(url);
         return true;
      } catch (e) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Could not download the original file.', 'error');
         }
         return false;
      }
   }

   /**
    * Create a container of document chips for a transcript entry.  Reloaded
    * documents render as a file-type chip (matching the upload UI); when the
    * original file was stored (v68 blobId) the chip downloads the real PDF/DOCX,
    * otherwise it falls back to the extracted-text viewer.
    * @param {Array<{filename: string, size: string, content: string, blobId?: string}>} documents
    * @returns {HTMLElement}
    */
   function createDocumentChips(documents) {
      const container = document.createElement('div');
      container.className = 'transcript-doc-container';

      const docs = typeof DawnDocuments !== 'undefined' ? DawnDocuments : null;

      documents.forEach((doc) => {
         // Dot-prefixed (".pdf") to match getFormatCategory's keys; formatExtension
         // strips the dot for display. Fall back to a dot-less ext only when the
         // documents module isn't loaded (category coloring is skipped then anyway).
         const ext = docs
            ? docs.getExtension(doc.filename)
            : (doc.filename.split('.').pop() || '').toLowerCase();
         const hasOriginal = !!doc.blobId;

         const chip = document.createElement('button');
         chip.className = 'transcript-doc-chip';
         if (hasOriginal) chip.classList.add('has-original');
         chip.title = `${doc.filename} \u2014 click to ${hasOriginal ? 'download original' : 'view text'}`;
         chip.setAttribute(
            'aria-label',
            `${hasOriginal ? 'Download original file' : 'View extracted text'}: ${doc.filename}, ${doc.size}`
         );

         const icon = document.createElement('span');
         icon.className = 'transcript-doc-icon';
         icon.innerHTML = DOC_ICON_SVG;

         // Colored type badge (e.g. PDF / DOCX), matching the composer upload chip.
         const typeBadge = document.createElement('span');
         typeBadge.className = 'transcript-doc-type';
         typeBadge.textContent = docs ? docs.formatExtension(ext) : ext.toUpperCase();
         if (docs) typeBadge.dataset.category = docs.getFormatCategory(ext);

         const name = document.createElement('span');
         name.className = 'transcript-doc-name';
         name.textContent = doc.filename;

         const size = document.createElement('span');
         size.className = 'transcript-doc-size';
         size.textContent = doc.size;

         chip.appendChild(icon);
         chip.appendChild(typeBadge);
         chip.appendChild(name);
         chip.appendChild(size);
         // Visible download affordance distinguishes a downloadable chip from a
         // text-viewer chip (the two are otherwise pixel-identical).
         if (hasOriginal) {
            const dl = document.createElement('span');
            dl.className = 'transcript-doc-download';
            dl.innerHTML = DOWNLOAD_ICON_SVG;
            dl.setAttribute('aria-hidden', 'true');
            chip.appendChild(dl);
         }
         chip.addEventListener('click', async () => {
            // On a failed/expired original, fall back to the extracted-text viewer.
            if (hasOriginal && (await downloadOriginal(doc.blobId, doc.filename))) {
               return;
            }
            DawnDocuments.openDocumentViewer(doc.filename, doc.content);
         });
         container.appendChild(chip);
      });

      return container;
   }

   // =============================================================================
   // Entry Formatting
   // =============================================================================

   /**
    * Format command text for debug display
    * @param {string} text - Text to format
    * @returns {string}
    */
   function formatCommandText(text) {
      // Highlight <command> tags (accent) and tool results (success) via theme tokens.
      return text
         .replace(/(<command>)/g, '<span class="cmd-tag">$1</span>')
         .replace(/(<\/command>)/g, '<span class="cmd-tag">$1</span>')
         .replace(
            /(\[Tool Result:[\s\S]*?(?:\](?=\s*$)|$))/g,
            '<span class="tool-result-tag">$1</span>'
         );
   }

   // =============================================================================
   // Entry Rendering
   // =============================================================================

   /**
    * Add a debug entry to the transcript
    * @param {string} label - Debug label (command, tool result, etc.)
    * @param {string} content - Debug content
    */
   function addDebugEntry(label, content) {
      const transcript = DawnElements.transcript;
      if (!transcript) return;

      const entry = document.createElement('div');

      // Determine specific debug class based on label
      let debugClass = 'debug';
      if (label === 'command') {
         debugClass = 'debug command';
      } else if (label === 'tool result') {
         debugClass = 'debug tool-result';
      } else if (label === 'tool call') {
         debugClass = 'debug tool-call';
      }

      entry.className = `transcript-entry ${debugClass}`;
      entry.innerHTML = `
      <div class="role">${DawnFormat.escapeHtml(label)}</div>
      <div class="text">${formatCommandText(DawnFormat.escapeHtml(content))}</div>
    `;
      // Visibility controlled by CSS via body.debug-mode class
      transcript.appendChild(entry);
   }

   /**
    * Add a normal entry to the transcript
    * @param {string} role - Message role (user, assistant, system)
    * @param {string} text - Message text
    */
   async function addNormalEntry(role, text, extractedDocs, messageId) {
      const transcript = DawnElements.transcript;
      if (!transcript) return;

      /* NOTE: the marker pre-render ORDER below (documents -> images -> strip
       * <thinking> -> extract <dawn-visual> -> markdown) is mirrored by the
       * static source viewer in memory_source.js (renderSourceMessage).  If you
       * add or reorder a marker family here, update that mirror too. */

      // Parse document markers (strip from display, extract for chips)
      // If caller already extracted docs (addTranscriptEntry), use those directly
      let docData;
      if (extractedDocs && extractedDocs.length > 0) {
         docData = { cleanText: text, documents: extractedDocs };
      } else if (DawnDocuments && DawnDocuments.parseDocumentMarkers) {
         docData = DawnDocuments.parseDocumentMarkers(text);
      } else {
         docData = { cleanText: text, documents: [] };
      }

      // Parse image markers if present (user messages may have embedded images)
      let parsed = null;
      let displayText = docData.cleanText;

      if (DawnVision && DawnVision.parseImageMarkers) {
         parsed = DawnVision.parseImageMarkers(displayText);
         displayText = parsed.text;
      }

      // Strip self-inserted <thinking> tags (Claude chain-of-thought leaking into text)
      displayText = displayText.replace(/<thinking>[\s\S]*?<\/thinking>\s*/g, '');

      // Extract <dawn-visual> blocks before markdown/sanitize (DOMPurify strips custom tags)
      let visualBlocks = [];
      // Split text around visual tags iteratively for inline positioning.
      // Produces alternating text/visual segments: text, visual, text, visual, text
      let segments = []; // { type: 'text'|'visual', content: string, visual: object }
      if (typeof DawnVisualRender !== 'undefined') {
         const extracted = DawnVisualRender.extractVisuals(displayText);
         visualBlocks = extracted.visuals;
         if (visualBlocks.length > 0) {
            const tagRe =
               /<dawn-visual\s+title="[^"]*"\s+type="(?:svg|html)">[\s\S]*?<\/dawn-visual>/g;
            let lastIdx = 0;
            let match;
            let vi = 0;
            while ((match = tagRe.exec(displayText)) !== null) {
               const before = displayText.substring(lastIdx, match.index).trim();
               if (before) segments.push({ type: 'text', content: before });
               if (vi < visualBlocks.length) {
                  segments.push({ type: 'visual', visual: visualBlocks[vi++] });
               }
               lastIdx = match.index + match[0].length;
            }
            const after = displayText.substring(lastIdx).trim();
            if (after) segments.push({ type: 'text', content: after });
         } else {
            segments.push({ type: 'text', content: extracted.cleanText });
         }
      } else {
         segments.push({ type: 'text', content: displayText });
      }

      // Create entry with first text segment
      const entry = document.createElement('div');
      entry.className = `transcript-entry ${role}`;
      const firstText =
         segments.length > 0 && segments[0].type === 'text' ? segments[0].content : '';
      entry.innerHTML = `
      <div class="role">${DawnFormat.escapeHtml(role)}</div>
      <div class="text">${DawnFormat.markdown(firstText)}</div>
    `;
      transcript.appendChild(entry);
      // Server-authoritative persistence (§6a/§12c): stamp the DB message_id on THIS
      // node SYNCHRONOUSLY (before the image-load await below), so a fanned-out
      // message_appended for the same row dedups even when this render parks on an
      // async image fetch, and so the id lands on the bubble — never a later sibling.
      if (messageId != null) {
         entry.setAttribute('data-message-id', String(messageId));
      }

      // Append remaining segments (visuals + text blocks)
      for (let i = firstText ? 1 : 0; i < segments.length; i++) {
         if (segments[i].type === 'visual') {
            DawnVisualRender.renderVisuals(entry, [segments[i].visual]);
         } else if (segments[i].content) {
            const textDiv = document.createElement('div');
            textDiv.className = 'text';
            textDiv.innerHTML = DawnFormat.markdown(segments[i].content);
            entry.appendChild(textDiv);
         }
      }

      // Scroll immediately after adding entry
      transcript.scrollTop = transcript.scrollHeight;

      // Store raw text and add copy buttons on all .text elements
      const firstTextEl = entry.querySelector('.text');
      entry.querySelectorAll('.text').forEach(function (el) {
         if (!el.getAttribute('data-raw-text')) {
            el.setAttribute('data-raw-text', docData.cleanText);
         }
         DawnFormat.addCopyButtons(el);
         DawnFormat.addMessageCopyButton(el);
      });

      // Render document chips if any documents were attached
      if (docData.documents.length > 0 && firstTextEl) {
         firstTextEl.appendChild(createDocumentChips(docData.documents));
         transcript.scrollTop = transcript.scrollHeight;
      }

      // Load and add images AFTER entry is visible (async, may involve network requests)
      if (parsed && DawnVision.loadParsedImages) {
         const imageDataUrls = await DawnVision.loadParsedImages(parsed);
         if (imageDataUrls.length > 0) {
            const imagesContainer = document.createElement('div');
            imagesContainer.className = 'transcript-images-container';
            for (const dataUrl of imageDataUrls) {
               const imageEl = createImageElement(dataUrl);
               if (imageEl) {
                  imagesContainer.appendChild(imageEl);
               }
            }
            if (imagesContainer.children.length > 0 && firstTextEl) {
               firstTextEl.appendChild(imagesContainer);
               // Scroll again after images are added (they increase height)
               transcript.scrollTop = transcript.scrollHeight;
            }
         }
      }
   }

   /**
    * Add a transcript entry with automatic debug content routing
    * @param {string} role - Message role
    * @param {string} text - Message text
    */
   async function addTranscriptEntry(role, text, reasoning, messageId) {
      const transcript = DawnElements.transcript;
      if (!transcript) return;

      // Remove placeholder if present
      const placeholder = transcript.querySelector('.transcript-placeholder');
      if (placeholder) {
         placeholder.remove();
      }

      // Strip document markers BEFORE any routing logic — document content may
      // contain <command> tags or [Tool Result:] text that would cause misrouting
      let extractedDocs = [];
      if (DawnDocuments && DawnDocuments.parseDocumentMarkers) {
         const docData = DawnDocuments.parseDocumentMarkers(text);
         if (docData.documents.length > 0) {
            extractedDocs = docData.documents;
            text = docData.cleanText;
         }
      }

      // Check for thinking + reasoning markers in assistant messages (from history).
      // Persistence stores them as separate <dawn:thinking> and <dawn:reasoning>
      // tags. When both are present (Responses API), merge token count into the
      // thinking block — single panel with both stats.
      if (role === 'assistant') {
         let thinkingData = null;
         let reasoningData = null;
         if (reasoning && typeof reasoning === 'object') {
            // E3: server-provided structured reasoning ({provider, duration?, content?, tokens?}).
            // Prefer this over inline-marker parsing; markers are the legacy fallback below.
            if (reasoning.content) {
               thinkingData = {
                  duration: coerceReasoningDuration(reasoning.duration),
                  content: reasoning.content,
                  tokens: reasoning.tokens || 0,
               };
            } else if (reasoning.tokens) {
               reasoningData = { tokens: reasoning.tokens };
            }
         } else {
            // Legacy: parse inline <dawn:thinking>/<dawn:reasoning> markers from content.
            if (containsThinkingContent(text)) {
               const r = extractThinkingContent(text);
               thinkingData = r.thinking;
               text = r.remaining;
            }
            if (containsReasoningContent(text)) {
               const r = extractReasoningContent(text);
               reasoningData = r.reasoning;
               text = r.remaining;
            }
         }
         let block = null;
         if (thinkingData) {
            if (reasoningData) thinkingData.tokens = reasoningData.tokens;
            block = createThinkingBlock(thinkingData);
         } else if (reasoningData) {
            block = createReasoningBlock(reasoningData);
         }
         if (block) {
            transcript.appendChild(block);
            if (text.length === 0) {
               transcript.scrollTop = transcript.scrollHeight;
               return;
            }
         }
      }

      // Visual tool results: stash for attachment to the next assistant message.
      // The actual rendering happens in addNormalEntry via extractVisuals() when
      // the assistant response arrives with the visual content appended.
      // (Stashing is handled in dawn.js handleTextMessage via pendingVisualsForSave)
      if (role === 'visual') {
         return;
      }

      // Route tool role messages to debug entries. The server sends BOTH native tool
      // calls (combined "[Tool Call: name(args) -> result]") and plain results under
      // role:"tool", so label by content — a call gets the 'tool call' badge, anything
      // else (e.g. legacy device "[Tool Result: ...]") gets 'tool result' — instead of
      // blanket-labeling every role:tool message as a result. Keeps the live view
      // consistent with the reloaded view (history.js renders calls as 'tool call').
      if (role === 'tool') {
         addDebugEntry(text.startsWith('[Tool Call:') ? 'tool call' : 'tool result', text);
         transcript.scrollTop = transcript.scrollHeight;
         return;
      }

      // Detect Claude native tool format: JSON arrays with tool_use or tool_result objects
      // These come from conversation history and look like:
      // [ { "type": "tool_use", "id": "...", "name": "...", "input": {...} } ]
      // [ { "type": "tool_result", "tool_use_id": "...", "content": "..." } ]
      if (text.trimStart().startsWith('[')) {
         try {
            const parsed = JSON.parse(text);
            if (Array.isArray(parsed) && parsed.length > 0) {
               const firstItem = parsed[0];
               if (firstItem.type === 'tool_use') {
                  // Format tool calls nicely
                  parsed.forEach((item) => {
                     const formatted = `[Tool Call: ${item.name}]\n${JSON.stringify(item.input, null, 2)}`;
                     addDebugEntry('tool call', formatted);
                  });
                  transcript.scrollTop = transcript.scrollHeight;
                  return;
               }
               if (firstItem.type === 'tool_result') {
                  // Format tool results nicely
                  parsed.forEach((item) => {
                     const formatted = `[Tool Result: ${item.tool_use_id}]\n${item.content}`;
                     addDebugEntry('tool result', formatted);
                  });
                  transcript.scrollTop = transcript.scrollHeight;
                  return;
               }
            }
         } catch (e) {
            // Not valid JSON, continue with normal processing
         }
      }

      // Special case: Tool calls/results are sent as complete messages
      // These can contain ] characters in the content, so don't try to parse with regex
      if (text.startsWith('[Tool Result:')) {
         addDebugEntry('tool result', text);
         transcript.scrollTop = transcript.scrollHeight;
         return;
      }
      if (text.startsWith('[Tool Call:')) {
         addDebugEntry('tool call', text);
         transcript.scrollTop = transcript.scrollHeight;
         return;
      }

      const hasDebugContent = containsCommandTags(text);

      // messageId is passed into addNormalEntry so it stamps data-message-id on the
      // bubble SYNCHRONOUSLY at creation (§6a/§12c) — robust against the fan-out dedup
      // racing an async image render, and correct even when other entries interleave.
      if (!hasDebugContent) {
         // Pure user-facing message - show normally
         await addNormalEntry(role, text, extractedDocs, messageId);
      } else if (isOnlyDebugContent(text)) {
         // Pure debug message (only commands/tool results) - debug only
         // Still render document chips if present
         if (extractedDocs.length > 0) {
            await addNormalEntry(role, '', extractedDocs, messageId);
         } else {
            addDebugEntry(`debug (${role})`, text);
         }
      } else {
         // Mixed message - show user-facing text normally AND debug content separately
         const userText = extractUserFacingText(text);
         const { commands, toolResults } = extractDebugContent(text);

         // Add debug entries for commands
         commands.forEach((cmd) => {
            addDebugEntry('command', cmd);
         });

         // Add debug entries for tool results
         toolResults.forEach((result) => {
            addDebugEntry('tool result', result);
         });

         // Add user-facing text if any (or if documents are attached)
         if (userText.length > 0 || extractedDocs.length > 0) {
            await addNormalEntry(role, userText, extractedDocs, messageId);
         }
      }

      transcript.scrollTop = transcript.scrollHeight;
   }

   // =============================================================================
   // Export Module
   // =============================================================================

   // =============================================================================
   // Image Lightbox (click-to-enlarge for chat images)
   // =============================================================================

   function setupImageLightbox() {
      var lightbox = document.createElement('div');
      lightbox.className = 'image-lightbox';
      lightbox.setAttribute('role', 'dialog');
      lightbox.setAttribute('aria-modal', 'true');
      lightbox.setAttribute('aria-label', 'Enlarged image');
      lightbox.innerHTML =
         '<button class="image-lightbox-close" aria-label="Close">&times;</button>' +
         '<img alt="Enlarged view">';
      /* Start hidden — it's only made visible (display:flex) on open. Without
       * this it sits in the DOM as display:flex/opacity:0 from page load until
       * the first close, covering the viewport. */
      lightbox.style.display = 'none';
      document.body.appendChild(lightbox);

      var lightboxImg = lightbox.querySelector('img');
      var closeBtn = lightbox.querySelector('.image-lightbox-close');
      var previousFocus = null;
      var lightboxEscToken = null; // DawnEscStack registration while the lightbox is visible

      function closeLightbox() {
         lightbox.classList.remove('visible');
         if (lightboxEscToken !== null) {
            DawnEscStack.unregister(lightboxEscToken);
            lightboxEscToken = null;
         }
         setTimeout(function () {
            lightbox.style.display = 'none';
         }, 200);
         if (previousFocus) {
            previousFocus.focus();
            previousFocus = null;
         }
      }

      /* Delegated click on transcript — catches all images including dynamically added */
      document.addEventListener('click', function (e) {
         var img = e.target.closest('.transcript-entry img');
         if (!img || !img.src) return;

         previousFocus = document.activeElement;
         lightboxImg.src = img.src;
         lightboxImg.alt = img.alt || 'Enlarged view';
         lightbox.style.display = 'flex';
         /* Trigger reflow for transition */
         lightbox.offsetHeight;
         lightbox.classList.add('visible');
         if (lightboxEscToken === null) {
            lightboxEscToken = DawnEscStack.register(function () {
               closeLightbox();
               return true;
            });
         }
         closeBtn.focus();
      });

      /* Close on backdrop click or close button */
      lightbox.addEventListener('click', function (e) {
         if (e.target === lightbox || e.target === closeBtn) {
            closeLightbox();
         }
      });

      /* Tab trap within lightbox (Escape close is handled via DawnEscStack —
       * register-on-open above / unregister in closeLightbox). */
      document.addEventListener('keydown', function (e) {
         if (!lightbox.classList.contains('visible')) return;

         if (e.key === 'Tab') {
            /* Trap focus within lightbox — only focusable element is close button */
            e.preventDefault();
            closeBtn.focus();
         }
      });
   }

   /* Init lightbox after DOM is ready */
   if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', setupImageLightbox);
   } else {
      setupImageLightbox();
   }

   global.DawnTranscript = {
      addEntry: addTranscriptEntry,
      addDebug: addDebugEntry,
      addNormal: addNormalEntry,
      // Expose helpers for other modules
      containsCommandTags: containsCommandTags,
      isOnlyDebugContent: isOnlyDebugContent,
      extractUserFacingText: extractUserFacingText,
      // Reused by the memory source viewer (mini-chat render)
      createImageElement: createImageElement,
      createDocumentChips: createDocumentChips,
   };
})(window);
