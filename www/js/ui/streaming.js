/**
 * DAWN Streaming Module
 * Handles LLM streaming responses (ChatGPT-style real-time text)
 *
 * Usage:
 *   DawnStreaming.handleStart(payload)     // Handle stream_start message
 *   DawnStreaming.handleDelta(payload)     // Handle stream_delta message
 *   DawnStreaming.handleEnd(payload)       // Handle stream_end message
 *   DawnStreaming.finalize()               // Force finalize current stream
 *   DawnStreaming.setCallbacks({ onStateChange, onSaveMessage })
 */
(function (global) {
   'use strict';

   // Callbacks
   let callbacks = {
      onStateChange: null, // (state, detail) => void - notify state changes
      onSaveMessage: null, // (role, text) => void - save message to history
   };

   /**
    * Header label for the live thinking block — the assistant's name (via
    * DawnFormat.assistantName()), matching the reload path in transcript.js. No
    * provider vocabulary to keep in sync any more.
    */
   function providerDisplayLabel(provider) {
      // Label by the assistant's name (e.g. "Friday"), never the model/provider —
      // provider is mutable infrastructure the user shouldn't see. `provider` is
      // accepted for call-site compatibility but ignored.
      void provider;
      return DawnFormat.assistantName();
   }

   // =============================================================================
   // Stream Start
   // =============================================================================

   /**
    * Handle stream_start: Create new assistant entry for streaming
    * @param {Object} payload - Stream start payload with stream_id
    */
   /**
    * True if this stream belongs to a conversation OTHER than the one currently
    * being viewed — a background turn whose tokens must NOT render into the active
    * view. A stream with no conversation_id, or when the view isn't yet bound to a
    * conversation, is treated as the active stream (render normally).
    * @param {Object} payload
    */
   function isBackgroundStream(payload) {
      const streamConv = payload && payload.conversation_id;
      // No conversation id (fresh chat's first turn before the row exists, or a
      // non-WebUI client) → render in place.
      if (!streamConv) return false;
      const activeConv =
         typeof DawnHistory !== 'undefined' && DawnHistory.getActiveConversationId
            ? DawnHistory.getActiveConversationId()
            : null;
      // View not bound to a conversation (e.g. a fresh "New Chat" pane): a frame
      // that carries a real conversation id can only be a background turn — never
      // render it here.
      if (!activeConv) return true;
      // Purely conversation-based: a frame is background iff it belongs to a
      // conversation other than the one on screen — independent of any
      // (possibly stale) local streamingState. Compare as strings — conversation
      // ids are DB rowids that can exceed 2^53.
      return String(streamConv) !== String(activeConv);
   }

   /**
    * Clear streaming state WITHOUT persisting it. Used when the user switches
    * away from a streaming conversation (the partial belongs to that conversation
    * and will be re-established via stream_resume on switch-back or persisted by
    * the server once server-side persistence lands). Distinct from
    * finalizeStream(), which SAVES the content — calling that here would leak the
    * partial into the newly-active conversation.
    */
   function resetStreamingStateSilently() {
      if (DawnState.streamingState.entryElement) {
         DawnState.streamingState.entryElement.classList.remove('streaming');
      }
      DawnState.streamingState.active = false;
      DawnState.streamingState.streamId = null;
      DawnState.streamingState.entryElement = null;
      DawnState.streamingState.textElement = null;
      DawnState.streamingState.content = '';
      DawnState.streamingState.preVisualContent = '';
      DawnState.streamingState.pendingRender = false;
      // Drop any stale thinking + reasoning state (it belonged to the conversation
      // we left — otherwise the next conversation's finalizeStream would prepend
      // this turn's pre-visual text / attach its reasoning to the wrong message).
      if (DawnState.thinkingState) {
         DawnState.thinkingState.active = false;
      }
      clearReasoningState();
      if (typeof DawnVisualization !== 'undefined' && DawnVisualization.stopHesitation) {
         DawnVisualization.stopHesitation();
      }
      // Reset the status pill for the conversation we're switching to — it isn't
      // generating from this view's perspective.  If the conversation we land on
      // has an in-flight turn, handleStreamResume re-raises the pill to "speaking".
      if (callbacks.onStateChange) {
         callbacks.onStateChange('idle', '');
      }
   }

   function markGenerating(convId, generating) {
      if (typeof DawnHistory !== 'undefined' && DawnHistory.setConversationGenerating) {
         DawnHistory.setConversationGenerating(convId, generating);
      }
   }

   function handleStreamStart(payload) {
      console.log('Stream start:', payload);

      // A turn for a conversation the user isn't viewing: show a sidebar
      // indicator and do NOT open a bubble in the active transcript.
      if (isBackgroundStream(payload)) {
         markGenerating(payload.conversation_id, true);
         return;
      }

      // Update status to show responding
      if (callbacks.onStateChange) {
         callbacks.onStateChange('speaking', 'Responding');
      }

      // Cancel any existing stream
      if (DawnState.streamingState.active) {
         console.warn('Stream start received while already streaming');
         finalizeStream();
      }

      // Remove placeholder if present
      const transcript = DawnElements.transcript;
      if (transcript) {
         const placeholder = transcript.querySelector('.transcript-placeholder');
         if (placeholder) {
            placeholder.remove();
         }

         // Create new streaming entry
         const entry = document.createElement('div');
         entry.className = 'transcript-entry assistant streaming';
         entry.setAttribute('aria-live', 'polite'); // Screen readers announce updates
         entry.setAttribute('aria-atomic', 'false'); // Only announce new content
         entry.innerHTML = `
        <div class="role">assistant</div>
        <div class="text"></div>
      `;
         transcript.appendChild(entry);

         // Update streaming state
         DawnState.streamingState.active = true;
         DawnState.streamingState.streamId = payload.stream_id;
         DawnState.streamingState.entryElement = entry;
         DawnState.streamingState.textElement = entry.querySelector('.text');
         DawnState.streamingState.content = '';

         // Reset thinking tokens for new stream (will be populated by reasoning_summary if applicable)
         DawnState.metricsState.last_thinking_tokens = 0;

         transcript.scrollTop = transcript.scrollHeight;
      }

      // Start hesitation ring animation for token timing visualization
      if (typeof DawnVisualization !== 'undefined') {
         DawnVisualization.startHesitation();
      }
   }

   // =============================================================================
   // Stream Delta
   // =============================================================================

   /**
    * Handle stream_delta: Append text to current streaming entry
    * Uses debounced markdown rendering (max 10Hz) to avoid heavyweight parsing per token
    * @param {Object} payload - Stream delta payload with stream_id and delta
    */
   function handleStreamDelta(payload) {
      // Background conversation's token — keep the sidebar indicator live, but do
      // not render into the active view. The text accumulates server-side in the
      // replay ring and is replayed on switch-back (stream_resume).
      if (isBackgroundStream(payload)) {
         markGenerating(payload.conversation_id, true);
         return;
      }

      // Ignore deltas for different stream IDs
      if (
         !DawnState.streamingState.active ||
         payload.stream_id !== DawnState.streamingState.streamId
      ) {
         console.warn(
            'Ignoring stale stream delta:',
            payload.stream_id,
            'expected:',
            DawnState.streamingState.streamId
         );
         return;
      }

      // Track token timing for hesitation ring
      if (typeof DawnVisualization !== 'undefined') {
         DawnVisualization.onTokenEvent();
      }

      // Emit token event for decoupled visualization modules
      if (typeof DawnEvents !== 'undefined') {
         DawnEvents.emit('token', { timestamp: performance.now() });
      }

      // Append delta to content
      DawnState.streamingState.content += payload.delta;

      // Render with markdown formatting for better readability during streaming
      // Debounced to avoid heavy parsing on every token
      const nowMs = performance.now();
      if (
         nowMs - DawnState.streamingState.lastRenderMs >=
         DawnState.streamingState.renderDebounceMs
      ) {
         renderStreamingContent();
         DawnState.streamingState.lastRenderMs = nowMs;
         DawnState.streamingState.pendingRender = false;
      } else if (!DawnState.streamingState.pendingRender) {
         // Schedule a render for when debounce period ends
         DawnState.streamingState.pendingRender = true;
         const delay =
            DawnState.streamingState.renderDebounceMs -
            (nowMs - DawnState.streamingState.lastRenderMs);
         setTimeout(() => {
            if (DawnState.streamingState.active && DawnState.streamingState.pendingRender) {
               renderStreamingContent();
               DawnState.streamingState.lastRenderMs = performance.now();
               DawnState.streamingState.pendingRender = false;
               if (DawnElements.transcript) {
                  DawnElements.transcript.scrollTop = DawnElements.transcript.scrollHeight;
               }
            }
         }, delay);
      }

      if (DawnElements.transcript) {
         DawnElements.transcript.scrollTop = DawnElements.transcript.scrollHeight;
      }
   }

   /**
    * Render streaming content with markdown formatting
    */
   function renderStreamingContent() {
      if (!DawnState.streamingState.textElement) return;

      // Strip command tags and self-inserted <thinking> tags before rendering.
      // Claude sometimes emits <thinking>...</thinking> in its text even when
      // extended thinking is disabled — these are chain-of-thought, not output.
      const cleanText = DawnState.streamingState.content
         .replace(/<command>[\s\S]*?<\/command>/g, '')
         .replace(/<thinking>[\s\S]*?<\/thinking>\s*/g, '');

      // Apply markdown formatting (visuals are siblings of .text, not children,
      // so innerHTML overwrite does not affect them)
      DawnState.streamingState.textElement.innerHTML = DawnFormat.markdown(cleanText);
   }

   // =============================================================================
   // Stream End
   // =============================================================================

   /**
    * Handle stream_end: Finalize streaming entry
    * @param {Object} payload - Stream end payload with stream_id
    */
   function handleStreamEnd(payload) {
      console.log('Stream end:', payload);

      // Background conversation's turn: keep the sidebar indicator lit across
      // tool-loop iteration boundaries (each iteration re-issues stream_start/end);
      // clear it only when the whole turn is actually done.
      if (isBackgroundStream(payload)) {
         if (payload.reason !== 'tool_iteration') {
            markGenerating(payload.conversation_id, false);
            // Turn finished while you were viewing elsewhere — leave a steady "done"
            // mark on its sidebar row (cleared when you view it) so it isn't silent.
            if (typeof DawnHistory !== 'undefined' && DawnHistory.setConversationUnread) {
               DawnHistory.setConversationUnread(payload.conversation_id, true);
            }
         }
         return;
      }

      // Ignore end for different stream IDs
      if (payload.stream_id !== DawnState.streamingState.streamId) {
         console.warn(
            'Ignoring stale stream end:',
            payload.stream_id,
            'expected:',
            DawnState.streamingState.streamId
         );
         return;
      }

      // Tool-loop iteration boundary (server reason; must match webui_text_processing.c):
      // seal the current bubble so this iteration's tool entries render after its text and
      // the next iteration opens a fresh bubble — but DON'T go idle or save (the turn
      // continues; the daemon persists intermediate tool-turn text, and only the final
      // stream_end saves the visible answer).
      if (payload.reason === 'tool_iteration') {
         finalizeStreamBubble();
         return;
      }

      finalizeStream();
   }

   /**
    * Handle stream_resume: sent by the server when the client switches TO a
    * conversation that has an in-flight turn. Rebuilds the streaming bubble from
    * the partial text accumulated server-side and resumes live rendering, so
    * subsequent (now-foreground) deltas append seamlessly.
    * @param {Object} payload - { conversation_id, stream_id, partial, truncated }
    */
   function handleStreamResume(payload) {
      if (!payload || typeof payload.partial !== 'string') return;

      // Only resume into the conversation actually being viewed.
      const activeConv =
         typeof DawnHistory !== 'undefined' && DawnHistory.getActiveConversationId
            ? DawnHistory.getActiveConversationId()
            : null;
      if (activeConv && String(payload.conversation_id) !== String(activeConv)) return;

      // Clear the sidebar indicator for the now-foreground conversation.
      markGenerating(payload.conversation_id, false);

      // Discard any prior streaming state WITHOUT saving (its bubble belonged to
      // the old view; saving it here would persist it into this conversation).
      resetStreamingStateSilently();

      const transcript = DawnElements.transcript;
      if (!transcript) return;
      const placeholder = transcript.querySelector('.transcript-placeholder');
      if (placeholder) placeholder.remove();

      const entry = document.createElement('div');
      entry.className = 'transcript-entry assistant streaming';
      entry.setAttribute('aria-live', 'polite');
      entry.setAttribute('aria-atomic', 'false');
      entry.innerHTML = `
        <div class="role">assistant</div>
        <div class="text"></div>
      `;
      transcript.appendChild(entry);

      DawnState.streamingState.active = true;
      DawnState.streamingState.streamId = payload.stream_id;
      DawnState.streamingState.entryElement = entry;
      DawnState.streamingState.textElement = entry.querySelector('.text');
      DawnState.streamingState.content = payload.partial;
      DawnState.streamingState.lastRenderMs = 0;
      DawnState.streamingState.pendingRender = false;
      renderStreamingContent();
      transcript.scrollTop = transcript.scrollHeight;

      if (callbacks.onStateChange) {
         callbacks.onStateChange('speaking', 'Responding');
      }
   }

   /**
    * Lightweight finalize for an iteration-boundary bubble: render its final content and
    * mark the stream inactive so the next iteration's stream_start opens a new bubble.
    * Unlike finalizeStream() it does NOT transition to idle or save to the conversation.
    */
   function finalizeStreamBubble() {
      if (!DawnState.streamingState.active) {
         return;
      }

      if (DawnState.streamingState.textElement && DawnState.streamingState.content) {
         var finalContent = DawnState.streamingState.content.replace(
            /<thinking>[\s\S]*?<\/thinking>\s*/g,
            ''
         );
         DawnState.streamingState.textElement.innerHTML = DawnFormat.markdown(finalContent);
         DawnFormat.addCopyButtons(DawnState.streamingState.textElement);
         DawnState.streamingState.textElement.setAttribute('data-raw-text', finalContent);
      }

      if (DawnState.streamingState.entryElement) {
         DawnState.streamingState.entryElement.classList.remove('streaming');
      }

      // Reset so the next iteration's stream_start creates a fresh bubble. Matches the
      // fields handleStreamStart re-initializes, plus preVisualContent so intermediate
      // content never leaks into the final saved message.
      DawnState.streamingState.active = false;
      DawnState.streamingState.entryElement = null;
      DawnState.streamingState.textElement = null;
      DawnState.streamingState.content = '';
      DawnState.streamingState.preVisualContent = '';

      // E3: clear this iteration's finalized reasoning so it doesn't leak into the final
      // answer's save. The intermediate iteration's reasoning is persisted server-side on
      // its own tool_calls row, so nothing is lost by clearing here.
      clearReasoningState();
   }

   /**
    * Clear the finalized-reasoning state so a tool-calling iteration's reasoning (which is
    * persisted server-side on its tool_calls row) is NOT also saved by the browser on the
    * final answer. Called at every iteration boundary: by finalizeStreamBubble for an
    * iteration that streamed text, AND on tool-call arrival for a reasoning-only iteration
    * that never opened a text bubble (so finalizeStreamBubble doesn't run).
    */
   function clearReasoningState() {
      DawnState.streamingState.reasoningTokens = 0;
      DawnState.thinkingState.finalizedContent = '';
      DawnState.thinkingState.finalizedDuration = '0';
      DawnState.thinkingState.finalizedProvider = null;
   }

   /**
    * Finalize the current streaming entry
    */
   function finalizeStream() {
      if (!DawnState.streamingState.active) {
         return;
      }

      // Update status back to idle
      if (callbacks.onStateChange) {
         callbacks.onStateChange('idle', null);
      }

      // Stop hesitation ring animation
      if (typeof DawnVisualization !== 'undefined') {
         DawnVisualization.stopHesitation();
      }

      // Clean up any stale visual progress placeholders (tool failed before completing)
      if (DawnState.streamingState.entryElement) {
         var staleCards =
            DawnState.streamingState.entryElement.querySelectorAll('.dawn-visual-progress');
         staleCards.forEach(function (card) {
            var titleEl = card.querySelector('.dawn-visual-progress-title');
            if (titleEl) titleEl.textContent = 'Visual generation failed';
            if (card._progressTimer) {
               clearInterval(card._progressTimer);
               card._progressTimer = null;
            }
         });
      }

      // Final render to ensure all content is displayed (in case debounce was pending)
      if (DawnState.streamingState.textElement && DawnState.streamingState.content) {
         // Strip self-inserted <thinking> tags from final render
         var finalContent = DawnState.streamingState.content.replace(
            /<thinking>[\s\S]*?<\/thinking>\s*/g,
            ''
         );
         DawnState.streamingState.textElement.innerHTML = DawnFormat.markdown(finalContent);

         // Add copy buttons to code blocks and message copy button
         DawnFormat.addCopyButtons(DawnState.streamingState.textElement);
         DawnState.streamingState.textElement.setAttribute('data-raw-text', finalContent);
         DawnFormat.addMessageCopyButton(DawnState.streamingState.textElement);
      }

      // Remove streaming class
      if (DawnState.streamingState.entryElement) {
         DawnState.streamingState.entryElement.classList.remove('streaming');
      }

      /* Drain pending visuals and interleave with text content for history save.
       * Order: pre-visual text + visual tags + post-visual text.
       * This ensures replay renders visuals inline, not at the end. */
      var visualContent = '';
      if (callbacks.getPendingVisuals) {
         var visuals = callbacks.getPendingVisuals();
         if (visuals && visuals.length > 0) {
            visualContent = '\n' + visuals.join('\n') + '\n';
         }
      }

      var fullContent =
         (DawnState.streamingState.preVisualContent || '') +
         visualContent +
         DawnState.streamingState.content;
      /* Strip self-inserted <thinking> tags from save content */
      fullContent = fullContent.replace(/<thinking>[\s\S]*?<\/thinking>\s*/g, '');
      if (fullContent && callbacks.onSaveMessage) {
         // E3: reasoning is persisted server-side as a structured field (NOT inline
         // <dawn:thinking>/<dawn:reasoning> markers in content). Build the reasoning object
         // {provider, duration, content?, tokens?} from the finalized thinking state; the
         // server writes it to the messages.reasoning column on this (final-answer) row.
         const finContent = DawnState.thinkingState.finalizedContent;
         const finProvider = DawnState.thinkingState.finalizedProvider || 'unknown';
         const finDuration = DawnState.thinkingState.finalizedDuration || '0';
         const hasThinkingContent = finContent && finContent.trim();
         const hasReasoningTokens = DawnState.streamingState.reasoningTokens > 0;
         let reasoning = null;
         if (hasThinkingContent || hasReasoningTokens) {
            reasoning = { provider: finProvider, duration: finDuration };
            if (hasThinkingContent) reasoning.content = finContent;
            if (hasReasoningTokens) reasoning.tokens = DawnState.streamingState.reasoningTokens;
         }

         /* Note: visual content is NOT appended here for client-side save.
          * The server appends pending_visual to the assistant message in
          * session_add_message(), so the DB copy already includes it.
          * The client save_message is a backup that may be skipped on
          * server_saved replay. Visual rendering on replay is handled by
          * extractVisuals() in addNormalEntry. */

         callbacks.onSaveMessage('assistant', fullContent, reasoning);

         // Clear finalized thinking content after saving
         DawnState.thinkingState.finalizedContent = '';
         DawnState.thinkingState.finalizedDuration = '0';
         DawnState.thinkingState.finalizedProvider = null;
      }

      // Reset state
      DawnState.streamingState.active = false;
      DawnState.streamingState.streamId = null;
      DawnState.streamingState.entryElement = null;
      DawnState.streamingState.textElement = null;
      DawnState.streamingState.content = '';
      DawnState.streamingState.preVisualContent = '';
      DawnState.streamingState.pendingRender = false;
      DawnState.streamingState.reasoningTokens = 0;
   }

   /**
    * Set callbacks
    * @param {Object} cbs - Callback functions
    */
   function setCallbacks(cbs) {
      if (cbs.onStateChange) callbacks.onStateChange = cbs.onStateChange;
      if (cbs.onSaveMessage) callbacks.onSaveMessage = cbs.onSaveMessage;
      if (cbs.getPendingVisuals) callbacks.getPendingVisuals = cbs.getPendingVisuals;
   }

   // =============================================================================
   // Thinking Block Handlers (Extended Reasoning)
   // =============================================================================

   /**
    * Handle thinking_start: Create collapsible thinking block
    * @param {Object} payload - { stream_id, provider }
    */
   function handleThinkingStart(payload) {
      console.log('Thinking start:', payload);

      // Thinking for a conversation the user isn't viewing: sidebar indicator
      // only, never render into the active transcript.
      if (isBackgroundStream(payload)) {
         markGenerating(payload.conversation_id, true);
         return;
      }

      // Cancel any existing thinking block
      if (DawnState.thinkingState.active) {
         console.warn('Thinking start received while already thinking');
         finalizeThinking(true);
      }

      const transcript = DawnElements.transcript;
      if (!transcript) return;

      // Remove placeholder if present
      const placeholder = transcript.querySelector('.transcript-placeholder');
      if (placeholder) {
         placeholder.remove();
      }

      // Create thinking block entry
      const entry = document.createElement('div');
      entry.className = 'thinking-block collapsed';
      entry.setAttribute('role', 'region');
      entry.setAttribute('aria-label', 'AI thinking process');

      const providerLabel = providerDisplayLabel(payload.provider);

      entry.innerHTML = `
      <div class="thinking-header" role="button" tabindex="0" aria-expanded="false">
        <span class="thinking-icon" aria-hidden="true">💭</span>
        <span class="thinking-label">${providerLabel} is thinking...</span>
        <span class="thinking-duration"></span>
        <span class="thinking-toggle" aria-hidden="true">▼</span>
      </div>
      <div class="thinking-content" aria-live="polite"></div>
    `;

      // Add click handler for toggle
      const header = entry.querySelector('.thinking-header');
      header.addEventListener('click', () => toggleThinking(entry));
      header.addEventListener('keydown', (e) => {
         if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            toggleThinking(entry);
         }
      });

      // For multi-iteration tool calls (Responses API), the in-flight assistant
      // entry from the prior iteration's stream is in the transcript. Insert the
      // new thinking block before it so the visual order stays
      //   USER → thinking → (tool calls) → thinking → ASSISTANT.
      //
      // Anchor strictly to `.streaming` — querying any `.assistant` matches
      // prior-turn entries too, which would put the first thinking block of a
      // new turn ABOVE the just-typed user message.
      const streamingEntry = transcript.querySelector('.transcript-entry.assistant.streaming');
      if (streamingEntry) {
         transcript.insertBefore(entry, streamingEntry);
      } else {
         /* First thinking_start of a turn — stream hasn't begun yet, just append. */
         transcript.appendChild(entry);
      }

      // Update thinking state
      DawnState.thinkingState.active = true;
      DawnState.thinkingState.streamId = payload.stream_id;
      DawnState.thinkingState.provider = payload.provider;
      DawnState.thinkingState.entryElement = entry;
      DawnState.thinkingState.contentElement = entry.querySelector('.thinking-content');
      DawnState.thinkingState.content = '';
      DawnState.thinkingState.collapsed = true;
      DawnState.thinkingState.startTime = performance.now();

      transcript.scrollTop = transcript.scrollHeight;
   }

   /**
    * Handle thinking_delta: Append text to thinking block
    * @param {Object} payload - { stream_id, delta }
    */
   function handleThinkingDelta(payload) {
      // Background conversation's thinking — do not render into the active view.
      if (isBackgroundStream(payload)) {
         markGenerating(payload.conversation_id, true);
         return;
      }

      // Ignore deltas for inactive or mismatched streams
      if (!DawnState.thinkingState.active) {
         return;
      }

      // Append delta to content
      DawnState.thinkingState.content += payload.delta;

      // Debounced rendering (same pattern as stream_delta)
      const nowMs = performance.now();
      if (
         nowMs - DawnState.thinkingState.lastRenderMs >=
         DawnState.thinkingState.renderDebounceMs
      ) {
         renderThinkingContent();
         DawnState.thinkingState.lastRenderMs = nowMs;
         DawnState.thinkingState.pendingRender = false;
      } else if (!DawnState.thinkingState.pendingRender) {
         DawnState.thinkingState.pendingRender = true;
         const delay =
            DawnState.thinkingState.renderDebounceMs -
            (nowMs - DawnState.thinkingState.lastRenderMs);
         setTimeout(() => {
            if (DawnState.thinkingState.active && DawnState.thinkingState.pendingRender) {
               renderThinkingContent();
               DawnState.thinkingState.lastRenderMs = performance.now();
               DawnState.thinkingState.pendingRender = false;
            }
         }, delay);
      }
   }

   /**
    * Render thinking content to the element
    */
   function renderThinkingContent() {
      if (!DawnState.thinkingState.contentElement) return;

      // Render thinking as sanitized markdown (marked + DOMPurify), matching the finalized/
      // reload block and the message-body pipeline. Caller debounces this (100ms).
      DawnState.thinkingState.contentElement.innerHTML = DawnFormat.markdown(
         DawnState.thinkingState.content
      );

      // Auto-scroll transcript
      if (DawnElements.transcript) {
         DawnElements.transcript.scrollTop = DawnElements.transcript.scrollHeight;
      }
   }

   /**
    * Handle thinking_end: Finalize thinking block
    * @param {Object} payload - { stream_id, has_content }
    */
   function handleThinkingEnd(payload) {
      console.log('Thinking end:', payload);
      // Background conversation's thinking ended — nothing to finalize in view.
      if (isBackgroundStream(payload)) {
         return;
      }
      finalizeThinking(payload.has_content);
   }

   /**
    * Finalize the current thinking block
    * @param {boolean} hasContent - Whether thinking had content
    */
   function finalizeThinking(hasContent) {
      if (!DawnState.thinkingState.active) {
         return;
      }

      const entry = DawnState.thinkingState.entryElement;

      // Calculate duration for saving
      let durationSec = '0';
      if (DawnState.thinkingState.startTime) {
         const durationMs = performance.now() - DawnState.thinkingState.startTime;
         durationSec = (durationMs / 1000).toFixed(1);
      }

      // Store finalized data for saving with the assistant message
      DawnState.thinkingState.finalizedDuration = durationSec;
      DawnState.thinkingState.finalizedProvider = DawnState.thinkingState.provider || 'unknown';
      DawnState.thinkingState.finalizedContent = DawnState.thinkingState.content;

      if (entry) {
         // Update label and duration
         const label = entry.querySelector('.thinking-label');
         const duration = entry.querySelector('.thinking-duration');

         if (label) {
            const providerLabel = providerDisplayLabel(DawnState.thinkingState.provider);
            // Use "reasoned" instead of "thought" when there's no summary text —
            // OpenAI Responses keeps an empty block alive for the token-count merge.
            const verb = hasContent
               ? 'thought'
               : DawnState.thinkingState.provider === 'openai'
                 ? 'reasoned'
                 : 'thinking';
            label.textContent = `${providerLabel} ${verb}`;
         }

         if (duration) {
            // Token count (if it arrives) is appended later by handleReasoningSummary.
            duration.textContent = formatThinkingStats(durationSec, 0);
         }

         // Add completed class for styling
         entry.classList.add('completed');

         // Final content render
         if (DawnState.thinkingState.contentElement && DawnState.thinkingState.content) {
            const escapedContent = DawnFormat.escapeHtml(DawnState.thinkingState.content);
            DawnState.thinkingState.contentElement.innerHTML = escapedContent.replace(
               /\n/g,
               '<br>'
            );
         }

         // Empty content handling. For OpenAI Responses, reasoning_summary is about
         // to fire with token counts — keep the block so the merge logic can attach
         // tokens, and replace the empty content area with a brief placeholder. For
         // other providers (Claude/local) an empty thinking-block is just noise, so
         // remove it as before.
         if (!hasContent && DawnState.thinkingState.content.trim() === '') {
            if (DawnState.thinkingState.provider === 'openai') {
               if (DawnState.thinkingState.contentElement) {
                  DawnState.thinkingState.contentElement.classList.add('no-summary');
                  DawnState.thinkingState.contentElement.innerHTML =
                     '<em>No reasoning summary emitted for this turn.</em>';
               }
            } else {
               entry.remove();
            }
         }
      }

      // Reset active state but preserve finalized content for saving
      DawnState.thinkingState.active = false;
      DawnState.thinkingState.streamId = null;
      DawnState.thinkingState.entryElement = null;
      DawnState.thinkingState.contentElement = null;
      DawnState.thinkingState.content = '';
      DawnState.thinkingState.pendingRender = false;
      DawnState.thinkingState.startTime = 0;
      // Note: provider, finalizedDuration, finalizedContent preserved for finalizeStream
   }

   /**
    * Toggle thinking block expanded/collapsed state
    * @param {HTMLElement} entry - The thinking block element
    */
   function toggleThinking(entry) {
      const isCollapsed = entry.classList.contains('collapsed');
      entry.classList.toggle('collapsed', !isCollapsed);

      const header = entry.querySelector('.thinking-header');

      if (header) {
         header.setAttribute('aria-expanded', isCollapsed ? 'true' : 'false');
      }
      // Note: Toggle icon rotation is handled by CSS transform, not textContent swap

      // Update state if this is the active thinking block
      if (DawnState.thinkingState.entryElement === entry) {
         DawnState.thinkingState.collapsed = !isCollapsed;
      }
   }

   // =============================================================================
   // Reasoning Summary (OpenAI o-series)
   // =============================================================================

   /**
    * Format a finalized thinking-block duration line with whichever stats are available.
    * @param {string|null} durationSec - "2.0" style duration (no unit), or null
    * @param {number} tokens - reasoning token count, 0 if unknown
    * @returns {string} text like "(2.0s, 43 tokens)" / "(2.0s)" / "(43 tokens)"
    */
   function formatThinkingStats(durationSec, tokens) {
      const parts = [];
      if (durationSec) parts.push(`${durationSec}s`);
      if (tokens > 0) parts.push(`${tokens.toLocaleString()} tokens`);
      return parts.length ? `(${parts.join(', ')})` : '';
   }

   /**
    * Handle reasoning_summary: token count for the most recent reasoning turn.
    *
    * Two cases:
    *   1. A thinking-block was just streamed (e.g. /v1/responses path that surfaces
    *      reasoning_summary_text deltas) — merge the token count into that block's
    *      duration line so the user sees both metrics without two side-by-side panels.
    *   2. No thinking-block exists (e.g. legacy chat-completions o-series, where the
    *      reasoning content is opaque) — fall back to a standalone reasoning-only
    *      block that surfaces the token count alongside an "(no content)" note.
    *
    * @param {Object} payload - { stream_id, reasoning_tokens }
    */
   function handleReasoningSummary(payload) {
      console.log('Reasoning summary:', payload);

      // Background conversation's reasoning summary — don't render it into the
      // active view, and (critically) don't clobber the active view's
      // streamingState.reasoningTokens with this background turn's count.
      if (isBackgroundStream(payload)) {
         markGenerating(payload.conversation_id, true);
         return;
      }

      const transcript = DawnElements.transcript;
      if (!transcript) return;

      const tokens = payload.reasoning_tokens || 0;
      if (tokens <= 0) return;

      // Store reasoning tokens for saving with the message regardless of UI path
      DawnState.streamingState.reasoningTokens = tokens;

      // Case 1: merge into the current turn's finalized thinking-block (Responses API).
      // Scope to blocks after the last .entry.user to avoid merging into a prior
      // turn's thinking panel when the provider changes mid-conversation.
      const lastUserEntry = Array.from(transcript.querySelectorAll('.transcript-entry.user')).pop();
      const candidates = transcript.querySelectorAll(
         '.thinking-block.completed:not(.reasoning-only)'
      );
      let target = null;
      for (let i = candidates.length - 1; i >= 0; i--) {
         if (
            !lastUserEntry ||
            candidates[i].compareDocumentPosition(lastUserEntry) & Node.DOCUMENT_POSITION_PRECEDING
         ) {
            target = candidates[i];
            break;
         }
      }
      if (target) {
         const durationEl = target.querySelector('.thinking-duration');
         if (durationEl) {
            const match = (durationEl.textContent || '').match(/(\d+(?:\.\d+)?)s/);
            const durationSec = match ? match[1] : null;
            durationEl.textContent = formatThinkingStats(durationSec, tokens);
         }
         return;
      }

      // Case 2: no thinking-block — opaque reasoning (no summary text to reveal), so this
      // is a static, non-interactive note: token count only, no expand affordance. Must stay
      // markup-identical to createReasoningBlock (transcript.js) so live and reload match.
      const entry = document.createElement('div');
      entry.className = 'thinking-block completed reasoning-only';
      entry.setAttribute('role', 'note');
      entry.setAttribute('aria-label', `${DawnFormat.assistantName()} reasoning summary`);

      const stats = DawnFormat.escapeHtml(formatThinkingStats(null, tokens));
      const safeLabel = DawnFormat.escapeHtml(`${DawnFormat.assistantName()} reasoned`);
      entry.innerHTML = `
      <div class="thinking-header static">
        <span class="thinking-icon" aria-hidden="true">🧠</span>
        <span class="thinking-label">${safeLabel}</span>
        <span class="thinking-duration">${stats}</span>
      </div>
    `;

      // Find the most recent assistant entry and insert before it
      const assistantEntries = transcript.querySelectorAll('.transcript-entry.assistant');
      if (assistantEntries.length > 0) {
         const lastAssistant = assistantEntries[assistantEntries.length - 1];
         transcript.insertBefore(entry, lastAssistant);
      } else {
         // Fallback: append if no assistant entry found
         transcript.appendChild(entry);
      }

      transcript.scrollTop = transcript.scrollHeight;
   }

   // =============================================================================
   // Export Module
   // =============================================================================

   global.DawnStreaming = {
      handleStart: handleStreamStart,
      handleDelta: handleStreamDelta,
      handleEnd: handleStreamEnd,
      handleResume: handleStreamResume,
      resetSilently: resetStreamingStateSilently,
      finalize: finalizeStream,
      setCallbacks: setCallbacks,
      // Thinking handlers
      handleThinkingStart: handleThinkingStart,
      handleThinkingDelta: handleThinkingDelta,
      handleThinkingEnd: handleThinkingEnd,
      finalizeThinking: finalizeThinking,
      // Reasoning summary (OpenAI o-series)
      handleReasoningSummary: handleReasoningSummary,
      // Clear finalized reasoning at an iteration boundary (E3, called on tool-call arrival)
      clearReasoningState: clearReasoningState,
   };
})(window);
