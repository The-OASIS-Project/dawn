/**
 * DAWN Conversation History Module
 * Manages conversation history panel, listing, loading, and saving
 */
(function () {
   'use strict';

   /* =============================================================================
    * State
    * ============================================================================= */

   let historyState = {
      conversations: [],
      conversationsTotal: 0, // total available on the server (for list pagination)
      conversationsLoading: false, // a list_conversations request is in flight
      pendingListAppend: false, // the in-flight request is a "load more" (append, not replace)
      activeConversationId: null,
      searchQuery: '',
      searchTimeout: null,
      pendingMessages: [], // Messages to save once conversation is created
      creatingConversation: false, // Prevent duplicate creation
      generatingConvIds: new Set(), // conv ids currently streaming a background turn
      // Monotonic token bumped on every conversation load. The async message-render
      // loop captures it and bails if a newer load starts mid-render — prevents two
      // concurrent renders (e.g. on reconnect) from interleaving messages, misplacing
      // the system-prompt block, or orphaning a turn's tool results.
      loadRenderToken: 0,
      // Reassign modal state
      usersList: [],
      reassignModalConvId: null,
   };

   // Callbacks for shared utilities
   let callbacks = {
      trapFocus: null,
      getAuthState: null,
   };

   // Cleanup function for focus trap
   let historyFocusTrapCleanup = null;
   let reassignFocusTrapCleanup = null;
   let historyEscToken = null; // DawnEscStack registration while the history panel is open
   let reassignEscToken = null; // DawnEscStack registration while the reassign modal is open

   // Track pending delete to clear UI if deleting active conversation
   let pendingDeleteId = null;

   // Track if sidebar state has been restored
   let historyStateRestored = false;

   // Track scroll position for restore (M12)
   let savedScrollPosition = 0;

   // After a pin/unpin re-render moves a row between the Pinned section and its
   // date group, refocus that row's pin button (keyboard/AT continuity).
   let pendingPinFocusId = null;

   // Track the last known context_max from the server (dynamic, model-specific)
   let knownContextMax = null;

   // Guard: when a conversation is loaded with saved context, ignore the next
   // context message's "current" value (it comes from set_session_llm before the
   // backend session has been updated with the loaded conversation's token count).
   let loadedContextGuard = null; // { current, max } from load_conversation_response

   /* =============================================================================
    * Constants
    * ============================================================================= */

   // Private conversation icon HTML (shared between renderHistoryItem and updateConversationPrivacy)
   const PRIVATE_ICON_HTML = `<span class="history-item-private" title="Private (no memory extraction)" aria-label="Private conversation" role="img">
      <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
         <path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94"/>
         <path d="M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19"/>
         <line x1="1" y1="1" x2="23" y2="23"/>
      </svg>
   </span>`;

   // Always-visible pushpin glyph in the title row for pinned conversations —
   // the persistent state cue (the hover-only action button alone isn't one).
   const PINNED_ICON_HTML = `<span class="history-item-pinned" title="Pinned" aria-label="Pinned conversation" role="img">
      <svg width="12" height="12" viewBox="0 0 24 24" fill="currentColor">
         <path d="M16 3v2l-1 1v4l3 3v2h-5v5l-1 1-1-1v-5H5v-2l3-3V6L7 5V3h9z"/>
      </svg>
   </span>`;

   // Pin/unpin toggle SVG for the action row (14x14 to match its siblings).
   const PIN_BUTTON_SVG = `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
      <path d="M16 3v2l-1 1v4l3 3v2h-5v5l-1 1-1-1v-5H5v-2l3-3V6L7 5V3z"/>
   </svg>`;

   /* Providers we have explicit colorways + tooltips for.  Conversations
    * with an `origin` of "messaging:<provider>" outside this list still
    * render the generic messaging icon but fall back to neutral coloring
    * (so a future Slack add lights up automatically once it's listed
    * here, and an unknown provider still gets the speech-bubble cue). */
   const KNOWN_MESSAGING_PROVIDERS = ['sms', 'telegram', 'discord', 'slack'];

   const MESSAGING_ICON_LABELS = {
      sms: 'SMS conversation',
      telegram: 'Telegram conversation',
      discord: 'Discord conversation',
      slack: 'Slack conversation',
   };

   /* Per-provider inline SVG (12x12, fill="currentColor" so the CSS
    * colorway controls hue).  Shapes are generic chat / send glyphs —
    * the provider identity is conveyed by the colored left border + the
    * title tooltip, not by mimicking each platform's logo. */
   const MESSAGING_ICON_SVG = {
      // SMS: rounded speech bubble with three dots
      sms: `<svg width="12" height="12" viewBox="0 0 24 24" fill="currentColor"><path d="M20 2H4c-1.1 0-2 .9-2 2v18l4-4h14c1.1 0 2-.9 2-2V4c0-1.1-.9-2-2-2zM7 11c-.55 0-1-.45-1-1s.45-1 1-1 1 .45 1 1-.45 1-1 1zm5 0c-.55 0-1-.45-1-1s.45-1 1-1 1 .45 1 1-.45 1-1 1zm5 0c-.55 0-1-.45-1-1s.45-1 1-1 1 .45 1 1-.45 1-1 1z"/></svg>`,
      // Telegram: paper plane (their iconic send glyph)
      telegram: `<svg width="12" height="12" viewBox="0 0 24 24" fill="currentColor"><path d="M9.78 18.65l.28-4.23 7.68-6.92c.34-.31-.07-.46-.52-.19L7.74 13.3 3.64 12c-.88-.25-.89-.86.2-1.3l15.97-6.16c.73-.33 1.43.18 1.15 1.3l-2.72 12.81c-.19.91-.74 1.13-1.5.71L12.6 16.3l-1.99 1.93c-.23.23-.42.42-.83.42z"/></svg>`,
      // Discord: chat bubble with offset eyes (Discord-ish silhouette)
      discord: `<svg width="12" height="12" viewBox="0 0 24 24" fill="currentColor"><path d="M19.27 5.33C17.94 4.71 16.5 4.26 15 4a.09.09 0 0 0-.07.03c-.18.33-.39.76-.53 1.09a16.09 16.09 0 0 0-4.8 0c-.14-.34-.35-.76-.54-1.09a.09.09 0 0 0-.07-.03c-1.5.26-2.93.71-4.27 1.33a.07.07 0 0 0-.03.03C2.4 9.3 1.74 13.16 2.07 16.96a.08.08 0 0 0 .03.05c1.79 1.32 3.52 2.12 5.22 2.65a.09.09 0 0 0 .1-.03c.4-.55.76-1.14 1.07-1.75a.08.08 0 0 0-.04-.11 11.7 11.7 0 0 1-1.66-.79.08.08 0 0 1-.01-.13c.11-.08.22-.17.33-.25a.09.09 0 0 1 .09-.01c3.48 1.59 7.25 1.59 10.69 0a.08.08 0 0 1 .09.01c.11.09.22.17.33.26.06.04.06.13-.01.13-.53.31-1.08.57-1.66.79a.08.08 0 0 0-.04.11c.32.61.68 1.19 1.07 1.74a.09.09 0 0 0 .1.04c1.7-.53 3.43-1.33 5.22-2.65a.09.09 0 0 0 .03-.05c.4-4.4-.67-8.23-2.85-11.6a.07.07 0 0 0-.03-.03zM8.52 14.65c-1 0-1.83-.92-1.83-2.05 0-1.13.81-2.05 1.83-2.05 1.03 0 1.85.93 1.83 2.05 0 1.13-.81 2.05-1.83 2.05zm6.97 0c-1 0-1.83-.92-1.83-2.05 0-1.13.81-2.05 1.83-2.05 1.03 0 1.85.93 1.83 2.05 0 1.13-.8 2.05-1.83 2.05z"/></svg>`,
      // Slack: hash symbol (#channel)
      slack: `<svg width="12" height="12" viewBox="0 0 24 24" fill="currentColor"><path d="M10 4h-1l-1 4H5v2h2.5L7 14H4v2h2.5L6 20h2l.5-4h3l-.5 4h2l.5-4H17v-2h-3.5l1-4H18V8h-2.5L16 4h-2l-.5 4h-3l.5-4zm-.5 6h3l-1 4h-3l1-4z"/></svg>`,
      // Generic fallback for unknown messaging:<provider> values
      generic: `<svg width="12" height="12" viewBox="0 0 24 24" fill="currentColor"><path d="M20 2H4c-1.1 0-2 .9-2 2v18l4-4h14c1.1 0 2-.9 2-2V4c0-1.1-.9-2-2-2z"/></svg>`,
   };

   /* Renders the inline-SVG span for a messaging-channel conversation.
    * Provider class on the span lets the CSS colorway hit currentColor;
    * unknown providers fall through to the generic speech-bubble icon. */
   function renderMessagingIcon(provider) {
      const known = KNOWN_MESSAGING_PROVIDERS.includes(provider);
      const label =
         (known && MESSAGING_ICON_LABELS[provider]) ||
         (provider ? `${provider} conversation` : 'Messaging conversation');
      const providerClass = known ? ` messaging-${provider}` : '';
      const svg = (known && MESSAGING_ICON_SVG[provider]) || MESSAGING_ICON_SVG.generic;
      return `
      <span class="history-item-messaging${providerClass}" title="${DawnFormat.escapeAttr(label)}" aria-label="${DawnFormat.escapeAttr(label)}" role="img">
        ${svg}
      </span>
    `;
   }

   /* =============================================================================
    * Elements
    * ============================================================================= */

   const historyElements = {
      panel: null,
      overlay: null,
      openBtn: null,
      closeBtn: null,
      newBtn: null,
      searchInput: null,
      searchContentCheckbox: null,
      list: null,
      // Sidebar rail elements
      sidebarRail: null,
      sidebarToggle: null,
      newBtnRail: null,
   };

   /* =============================================================================
    * Active Conversation Management
    * ============================================================================= */

   /**
    * Set the active conversation ID (persists to sessionStorage)
    */
   function setActiveConversationId(id) {
      historyState.activeConversationId = id;
      if (id) {
         sessionStorage.setItem('dawn_active_conversation', id.toString());
      } else {
         sessionStorage.removeItem('dawn_active_conversation');
      }
   }

   /**
    * Restore active conversation ID from sessionStorage
    */
   function restoreActiveConversationId() {
      const saved = sessionStorage.getItem('dawn_active_conversation');
      if (saved) {
         historyState.activeConversationId = parseInt(saved, 10);
         console.log('Restored active conversation:', historyState.activeConversationId);
      }
   }

   /**
    * Get the active conversation ID
    */
   function getActiveConversationId() {
      return historyState.activeConversationId;
   }

   /**
    * Toggle a "generating" indicator on a conversation's sidebar row — used when
    * a background conversation (one you're not currently viewing) is streaming a
    * turn, so its progress is visible without its tokens leaking into the active
    * view. No-op if the row isn't currently rendered in the list.
    * @param {number|string} convId
    * @param {boolean} generating
    */
   function setConversationGenerating(convId, generating) {
      if (!convId) return;
      const key = String(convId); // string key — conversation ids can exceed 2^53
      // Model it in state so a sidebar re-render (rename, unread refresh, new item,
      // …) doesn't wipe the indicator; renderConversationItem re-applies the class.
      if (generating) {
         historyState.generatingConvIds.add(key);
      } else {
         historyState.generatingConvIds.delete(key);
      }
      // Also toggle live on the currently-rendered row (avoids a full re-render).
      const item = historyElements.list?.querySelector(
         `.history-item[data-conv-id="${CSS.escape(String(convId))}"]`
      );
      if (item) {
         item.classList.toggle('generating', !!generating);
      }
   }

   /**
    * Clear all generating indicators — called on WS disconnect, when in-flight
    * background turns are aborted server-side, so no dot is left stuck lit.
    */
   function clearGenerating() {
      historyState.generatingConvIds.clear();
      historyElements.list
         ?.querySelectorAll('.history-item.generating')
         .forEach((el) => el.classList.remove('generating'));
   }

   /* =============================================================================
    * API Requests
    * ============================================================================= */

   const CONVERSATION_PAGE_SIZE = 50;

   function requestListConversations(append) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      if (historyState.conversationsLoading) return; // one page at a time

      // append = "load more" (next page); otherwise a fresh load that replaces.
      const offset = append ? historyState.conversations.length : 0;
      historyState.pendingListAppend = !!append;
      historyState.conversationsLoading = true;
      DawnWS.send({
         type: 'list_conversations',
         payload: { limit: CONVERSATION_PAGE_SIZE, offset: offset },
      });
   }

   function requestNewConversation(title) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;

      const payload = {};
      if (title) {
         payload.title = title;
      }

      DawnWS.send({
         type: 'new_conversation',
         payload: payload,
      });
   }

   function requestSaveMessage(convId, role, content, reasoning) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      if (!convId || !role || !content) return;

      const payload = {
         conversation_id: convId,
         role: role,
         content: content,
      };
      // E3: display-only reasoning persisted server-side as a JSON string field.
      if (reasoning && typeof reasoning === 'object') {
         payload.reasoning = JSON.stringify(reasoning);
      }
      DawnWS.send({ type: 'save_message', payload });
   }

   function requestUpdateContext(convId, contextTokens, contextMax) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      if (!convId || !contextMax) return;

      DawnWS.send({
         type: 'update_context',
         payload: {
            conversation_id: convId,
            context_tokens: contextTokens,
            context_max: contextMax,
         },
      });
   }

   function requestContinueConversation(convId, summary) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      if (!convId) return;

      DawnWS.send({
         type: 'continue_conversation',
         payload: {
            conversation_id: convId,
            summary: summary || '',
         },
      });
   }

   function requestLoadConversation(convId) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;

      /* Clear unread briefing indicator */
      if (typeof DawnScheduler !== 'undefined' && DawnScheduler.removeUnreadBriefing) {
         DawnScheduler.removeUnreadBriefing(convId);
      }

      DawnWS.send({
         type: 'load_conversation',
         payload: { conversation_id: convId },
      });
   }

   function requestDeleteConversation(convId) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;

      // Track which conversation we're deleting
      pendingDeleteId = convId;

      DawnWS.send({
         type: 'delete_conversation',
         payload: { conversation_id: convId },
      });
   }

   function requestRenameConversation(convId, newTitle) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;

      DawnWS.send({
         type: 'rename_conversation',
         payload: { conversation_id: convId, title: newTitle },
      });
   }

   function requestSearchConversations(query, searchContent) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;

      DawnWS.send({
         type: 'search_conversations',
         payload: { query: query, search_content: searchContent || false, limit: 50, offset: 0 },
      });
   }

   function requestSetPinned(convId, pinned) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;

      DawnWS.send({
         type: 'set_pinned',
         payload: { conversation_id: convId, is_pinned: !!pinned },
      });
   }

   /* =============================================================================
    * Response Handlers
    * ============================================================================= */

   function handleListConversationsResponse(payload) {
      const wasAppend = historyState.pendingListAppend;
      historyState.conversationsLoading = false;
      historyState.pendingListAppend = false;

      if (!payload.success) {
         console.error('Failed to list conversations:', payload.error);
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload.error || 'Failed to load conversations', 'error');
         }
         return;
      }

      const incoming = payload.conversations || [];
      if (wasAppend) {
         // An empty page means we've reached the end (e.g. rows were deleted
         // between pages and `total` is stale) — pin total to what we have so
         // the scroll handler stops requesting more.
         if (incoming.length === 0) {
            historyState.conversationsTotal = historyState.conversations.length;
            return;
         }
         // Preserve the user's scroll position across the innerHTML rebuild —
         // new items append at the bottom, so the view shouldn't jump.  The
         // scroller is .history-content, not #history-list.
         const scroller = historyElements.content;
         const scrollTop = scroller ? scroller.scrollTop : 0;
         historyState.conversations = historyState.conversations.concat(incoming);
         historyState.conversationsTotal =
            payload.total != null ? payload.total : historyState.conversations.length;
         renderConversationList();
         if (scroller) {
            scroller.scrollTop = scrollTop;
         }
      } else {
         historyState.conversations = incoming;
         historyState.conversationsTotal = payload.total != null ? payload.total : incoming.length;
         renderConversationList();
      }
   }

   // Infinite-scroll for the conversation LIST: when scrolled near the bottom and
   // more conversations exist on the server, fetch the next page and append.
   // (Disabled while a search is active — search has its own result set.)
   function handleConversationListScroll() {
      const scroller = historyElements.content;
      if (!scroller || historyState.searchQuery) return;
      if (historyState.conversationsLoading) return;
      if (historyState.conversations.length >= historyState.conversationsTotal) return;
      if (scroller.scrollTop + scroller.clientHeight >= scroller.scrollHeight - 150) {
         requestListConversations(true);
      }
   }

   function handleNewConversationResponse(payload) {
      historyState.creatingConversation = false;

      if (!payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload.error || 'Failed to create conversation', 'error');
         }
         historyState.pendingMessages = [];
         return;
      }

      setActiveConversationId(payload.conversation_id);

      // Update privacy toggle with new conversation ID, preserving pending privacy state
      if (typeof DawnSettingsLlm !== 'undefined' && DawnSettingsLlm.setCurrentConversation) {
         // Get the pending privacy state (may have been set before conversation was created)
         const pendingPrivacy = DawnSettingsLlm.getPrivacyState
            ? DawnSettingsLlm.getPrivacyState()
            : false;
         DawnSettingsLlm.setCurrentConversation(payload.conversation_id);

         // If privacy was set before conversation was created, apply it now
         if (pendingPrivacy && DawnSettingsLlm.setPrivacy) {
            DawnSettingsLlm.setPrivacy(true);
         }
      }

      // Lock per-conversation LLM settings on first message
      if (typeof DawnSettings !== 'undefined') {
         DawnSettings.lockConversationLlmSettings(payload.conversation_id);
      }

      // Process any pending messages
      if (historyState.pendingMessages.length > 0) {
         historyState.pendingMessages.forEach((msg) => {
            requestSaveMessage(payload.conversation_id, msg.role, msg.content, msg.reasoning);
         });
         historyState.pendingMessages = [];
      }

      // Only show toast if this was a manual "New Chat" action
      // Note: We don't clear transcript here - startNewChat() already handles that,
      // and if this was an auto-created conversation from sending a message, we
      // definitely don't want to clear (the message is already displayed)
      if (historyElements.panel && !historyElements.panel.classList.contains('hidden')) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('New conversation created', 'success');
         }
      }

      // Refresh list in background
      requestListConversations();
   }

   function handleLoadConversationResponse(payload) {
      if (!payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload.error || 'Failed to load conversation', 'error');
         }
         return;
      }

      const transcript = document.getElementById('transcript');

      // Bump the render token: any earlier in-flight async render will see a newer token
      // and bail, so this load's render runs alone (no interleaving on reconnect/refresh).
      const renderToken = ++historyState.loadRenderToken;

      // Full conversation load (the daemon sends every message in one response — no pagination).
      setActiveConversationId(payload.conversation_id);

      // Update privacy toggle state
      if (typeof DawnSettingsLlm !== 'undefined' && DawnSettingsLlm.setCurrentConversation) {
         DawnSettingsLlm.setCurrentConversation(
            payload.conversation_id,
            payload.is_private || false
         );
      }

      // Track archived state and continuation
      const isArchived = payload.is_archived || false;
      const continuedBy = payload.continued_by || null;

      console.log(`Load conversation: id=${payload.conversation_id}, total=${payload.total}`);

      // Update input area state based on archived status
      setArchivedMode(isArchived);

      // Reset ephemeral UI state
      if (typeof DawnPlanOrchestrator !== 'undefined') {
         DawnPlanOrchestrator.reset();
      }
      if (typeof DawnContextInjection !== 'undefined') {
         DawnContextInjection.reset();
      }
      if (typeof DawnSilentObserve !== 'undefined') {
         DawnSilentObserve.reset();
      }

      // Discard any in-flight streaming state WITHOUT saving before we tear down
      // the transcript — its partial belongs to the conversation we're leaving,
      // not the one being loaded (prevents the stale partial from being persisted
      // into the newly-active conversation on the next stream start/resume).
      if (typeof DawnStreaming !== 'undefined' && DawnStreaming.resetSilently) {
         DawnStreaming.resetSilently();
      }

      // Clear transcript
      if (transcript) {
         transcript.innerHTML = '';

         // Add archived notice at top for archived conversations
         if (isArchived) {
            const archivedBannerHtml = `
          <div class="archived-notice" id="archived-notice">
            <span class="archived-icon">
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                <polyline points="21 8 21 21 3 21 3 8"/>
                <rect x="1" y="3" width="22" height="5"/>
                <line x1="10" y1="12" x2="14" y2="12"/>
              </svg>
            </span>
            <span class="archived-label">Archived Conversation (Read Only)</span>
          </div>
        `;
            transcript.insertAdjacentHTML('beforeend', archivedBannerHtml);
         }

         // Add continuation banner if this is a continued conversation
         if (payload.continued_from) {
            const summary =
               payload.compaction_summary || 'Context from previous conversation was summarized.';
            const bannerHtml = `
          <div class="continuation-banner" id="continuation-banner">
            <div class="continuation-header">
              <span class="continuation-icon">
                <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                  <path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"/>
                  <path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"/>
                </svg>
              </span>
              <span class="continuation-label">Continued from previous conversation</span>
              <span class="continuation-toggle">▼</span>
            </div>
            <div class="continuation-content collapsed">
              <div class="continuation-summary">${DawnFormat.escapeHtml(summary)}</div>
            </div>
          </div>
        `;
            transcript.insertAdjacentHTML('beforeend', bannerHtml);
         }

         // Display messages (sequentially to preserve order with async image loading)
         const messages = payload.messages || [];
         // Pre-index tool results by tool_call_id so each call can be rendered PAIRED
         // with its result — reproducing the live "[Tool Call: name(args) -> result]"
         // display (live sends call+result combined; on reload they're stored separately).
         const toolResultsById = {};
         for (const m of messages) {
            if (m.role === 'tool' && m.tool_call_id) {
               toolResultsById[m.tool_call_id] = m.content || '';
            }
         }
         const consumedResultIds = new Set();
         (async () => {
            for (const msg of messages) {
               // A newer conversation load started — abandon this stale render so the two
               // don't interleave (the awaits below yield, letting a new load slip in).
               if (renderToken !== historyState.loadRenderToken) return;
               if (msg.role === 'system') continue;
               if (typeof DawnTranscript === 'undefined') continue;
               if (msg.role === 'tool') {
                  // Normally skipped — rendered paired with its assistant call above. But if
                  // that call fell into an earlier, not-yet-loaded page (pagination split a
                  // tool turn at the 50-message boundary), there's no call in this page to
                  // pair with — render the result inline at its real position rather than
                  // dropping it or dumping it out of order at the end of the transcript.
                  if (msg.tool_call_id && !consumedResultIds.has(msg.tool_call_id)) {
                     DawnTranscript.addDebug('tool result', `[Tool Result: ${msg.content || ''}]`);
                  }
                  continue;
               }

               // Assistant turn that made tool calls: show the pre-tool text bubble first
               // (model speaks, then calls fire), then each call paired with its result as
               // one debug entry — matching the live "[Tool Call: name(args) -> result]".
               if (Array.isArray(msg.tool_calls) && msg.tool_calls.length > 0) {
                  // Render the pre-tool text bubble and/or the AI-thought panel (E3): a
                  // reasoning-only iteration has empty content but still has reasoning to show.
                  if ((msg.content && msg.content.trim()) || msg.reasoning) {
                     await DawnTranscript.addEntry(msg.role, msg.content || '', msg.reasoning);
                  }
                  for (const tc of msg.tool_calls) {
                     const fn = tc.function || {};
                     const name = fn.name || 'tool';
                     const args = fn.arguments || ''; // already a compact JSON string
                     const result = toolResultsById[tc.id];
                     if (result !== undefined) consumedResultIds.add(tc.id);
                     const combined =
                        result !== undefined
                           ? `[Tool Call: ${name}(${args}) -> ${result}]`
                           : `[Tool Call: ${name}(${args})]`;
                     // Label as a call (the entry IS the call, with its result inlined) so
                     // the debug badge/colour matches the live 'tool call' rendering.
                     DawnTranscript.addDebug('tool call', combined);
                  }
                  continue;
               }

               await DawnTranscript.addEntry(msg.role, msg.content, msg.reasoning);
            }

            // Add continuation link at bottom for archived conversations (after all messages)
            if (isArchived && continuedBy) {
               console.log(`Adding continuation link to conversation ${continuedBy}`);
               addContinuationLink(continuedBy);
            }

            // Scroll to bottom after all messages loaded
            transcript.scrollTop = transcript.scrollHeight;
         })();

         // If debug mode is on (persisted across refreshes), (re-)request the System Prompt
         // AFTER this load. The load just cleared the transcript, which wipes a system-prompt
         // block requested earlier (e.g. on connect); requesting here lets its response
         // prepend to the freshly-rendered transcript and survive.
         if (typeof DawnState !== 'undefined' && DawnState.getDebugMode() && DawnWS.isConnected()) {
            DawnWS.send({ type: 'get_system_prompt' });
         }
      }

      // Reset metrics averages for loaded conversation (fresh start)
      if (typeof DawnMetrics !== 'undefined') {
         DawnMetrics.resetAverages();
      }

      // Restore context gauge if available, otherwise reset to 0
      if (typeof DawnContextGauge !== 'undefined') {
         if (payload.context_tokens && payload.context_max) {
            const usage = (payload.context_tokens / payload.context_max) * 100;
            DawnContextGauge.updateDisplay(
               {
                  current: payload.context_tokens,
                  max: payload.context_max,
                  usage: usage,
               },
               typeof DawnMetrics !== 'undefined' ? DawnMetrics.updatePanel : null
            );
            // Guard against the upcoming set_session_llm context message overwriting
            loadedContextGuard = {
               current: payload.context_tokens,
               max: payload.context_max,
            };
         } else {
            // No saved context - reset to 0 with best known max
            const fallbackMax =
               knownContextMax ||
               (typeof DawnConfig !== 'undefined' ? DawnConfig.DEFAULT_CONTEXT_MAX : 128000);
            DawnContextGauge.updateDisplay(
               { current: 0, max: fallbackMax, usage: 0 },
               typeof DawnMetrics !== 'undefined' ? DawnMetrics.updatePanel : null
            );
         }
      }

      // Apply per-conversation LLM settings
      if (typeof DawnSettings !== 'undefined') {
         DawnSettings.applyConversationLlmSettings(
            payload.llm_settings || null,
            payload.llm_locked || false
         );
      }

      renderConversationList();
      const statusMsg = isArchived
         ? `Loaded archived: ${payload.title}`
         : `Loaded: ${payload.title}`;
      if (typeof DawnToast !== 'undefined') {
         DawnToast.show(statusMsg, 'info');
      }
   }

   function handleDeleteConversationResponse(payload) {
      const deletedId = pendingDeleteId;
      pendingDeleteId = null;

      if (!payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload.error || 'Failed to delete conversation', 'error');
         }
         return;
      }

      // Drop any stale generating-indicator entry for the deleted conversation.
      if (deletedId) {
         historyState.generatingConvIds.delete(String(deletedId));
      }

      // If we deleted the active conversation, start fresh
      if (deletedId && deletedId === historyState.activeConversationId) {
         startNewChat();
      }

      if (typeof DawnToast !== 'undefined') {
         DawnToast.show('Conversation deleted', 'success');
      }
      requestListConversations();
   }

   function handleRenameConversationResponse(payload) {
      if (!payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload.error || 'Failed to rename conversation', 'error');
         }
         return;
      }

      if (typeof DawnToast !== 'undefined') {
         DawnToast.show('Conversation renamed', 'success');
      }
      requestListConversations();
   }

   function handleSetPinnedResponse(payload) {
      if (!payload || !payload.success) {
         pendingPinFocusId = null;
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show((payload && payload.error) || 'Failed to update pinned state', 'error');
         }
         return;
      }

      // Patch the cached conversation in place and re-render (no refetch), so the
      // row moves between the Pinned section and its date group immediately.
      const id = Number(payload.conversation_id);
      const isPinned = payload.is_pinned === true;
      const conv = historyState.conversations.find((c) => c.id === id);
      if (conv) {
         conv.is_pinned = isPinned;
      }
      renderConversationList();

      // Toast doubles as the aria-live (polite) announcement for AT users.
      if (typeof DawnToast !== 'undefined') {
         DawnToast.show(isPinned ? 'Conversation pinned' : 'Conversation unpinned', 'success');
      }
   }

   function handleConversationRenamed(payload) {
      const { conversation_id, title } = payload;
      if (!conversation_id || !title) return;

      // Update sidebar item if visible
      const id = Number(conversation_id);
      if (!Number.isInteger(id) || id <= 0) return;
      const item = historyElements.list?.querySelector(
         `.history-item[data-conv-id="${CSS.escape(String(id))}"] .history-item-title`
      );
      if (item) {
         // Remove existing text nodes, preserve icon elements in-place
         Array.from(item.childNodes)
            .filter((n) => n.nodeType === Node.TEXT_NODE)
            .forEach((n) => n.remove());
         item.appendChild(document.createTextNode(title));

         // Brief highlight to signal the update
         item.classList.add('title-updated');
         item.addEventListener('animationend', () => item.classList.remove('title-updated'), {
            once: true,
         });
      }

      // Update in cached conversations list
      const conv = historyState.conversations.find((c) => c.id === conversation_id);
      if (conv) {
         conv.title = title;
      }
   }

   /**
    * Server pushed `conversation_messages_appended` — an external writer
    * (messaging engine: SMS / Telegram / future Discord) appended new
    * turns to a conversation.  When that conversation is the one
    * currently open in the WebUI, re-fetch so the new turns render
    * without the user having to reload.  Otherwise it's a no-op (the
    * sidebar item will pick up the new last-activity timestamp on the
    * next list refresh).
    */
   function handleConversationMessagesAppended(payload) {
      if (!payload || !payload.conversation_id) return;
      const id = Number(payload.conversation_id);
      if (!Number.isInteger(id) || id <= 0) return;

      // Only act when the affected conversation is currently being viewed.
      if (historyState.activeConversationId !== id) return;

      // Re-fetch and full-re-render the conversation so the new
      // external turns appear.  TODO: switch to an incremental-append
      // path when forever-conversations get long enough that the full
      // re-render's scroll-jump becomes annoying.
      requestLoadConversation(id);
   }

   function handleSearchConversationsResponse(payload) {
      if (!payload.success) {
         console.error('Search failed:', payload.error);
         return;
      }

      historyState.conversations = payload.conversations || [];
      renderConversationList();
   }

   function handleSaveMessageResponse(payload) {
      if (!payload.success) {
         console.error('Failed to save message:', payload.error);
      }
   }

   function handleContextCompacted(payload) {
      console.log('Context compacted:', payload);

      const tokensBefore = payload.tokens_before || 0;
      const tokensAfter = payload.tokens_after || 0;
      console.log(`Compaction: ${tokensBefore} -> ${tokensAfter} tokens`);

      // v67: compaction no longer splits the conversation. The server records an
      // in-conversation watermark + summary and bounds context on reload, so the
      // conversation stays single and writable — no continuation, no archive/lock.
      // We only reset the per-turn trust-tier surfaces so stale provenance and turn
      // dedup don't carry across the compaction boundary. (requestContinueConversation
      // / handleContinueConversationResponse remain for legacy already-split convs.)
      if (typeof DawnContextInjection !== 'undefined') {
         DawnContextInjection.reset();
      }
      if (typeof DawnSilentObserve !== 'undefined') {
         DawnSilentObserve.reset();
      }
   }

   function handleContinueConversationResponse(payload) {
      if (!payload.success) {
         console.error('Failed to continue conversation:', payload.error);
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Failed to archive conversation', 'error');
         }
         return;
      }

      const newId = payload.new_conversation_id;
      if (newId) {
         setActiveConversationId(newId);
         console.log(`Conversation continued: old=${payload.old_conversation_id} -> new=${newId}`);

         setArchivedMode(false);

         const summary = payload.summary || 'Context from previous conversation was summarized.';
         addContinuationBanner(summary);

         requestListConversations();
      }
   }

   /* =============================================================================
    * UI Helpers
    * ============================================================================= */

   function setArchivedMode(isArchived) {
      const textInput = document.getElementById('text-input');
      const actionBtn = document.getElementById('action-btn');
      const actionDropdownBtn = document.getElementById('action-dropdown-btn');
      const inputArea = document.getElementById('input-area');

      if (textInput) {
         textInput.disabled = isArchived;
         textInput.placeholder = isArchived
            ? 'This conversation is archived (read only)'
            : 'Type a message...';
      }
      if (actionBtn) actionBtn.disabled = isArchived;
      if (actionDropdownBtn && isArchived) actionDropdownBtn.disabled = true;
      if (inputArea) {
         inputArea.classList.toggle('archived', isArchived);
      }
   }

   function addContinuationLink(continuedByConvId) {
      const transcript = document.getElementById('transcript');
      if (!transcript) return;

      const linkHtml = `
      <div class="continuation-footer" id="continuation-footer">
        <button class="continuation-link" data-conv-id="${continuedByConvId}">
          <span class="continuation-link-icon">
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
              <line x1="5" y1="12" x2="19" y2="12"/>
              <polyline points="12 5 19 12 12 19"/>
            </svg>
          </span>
          View Continuation
        </button>
      </div>
    `;
      transcript.insertAdjacentHTML('beforeend', linkHtml);
   }

   function addContinuationBanner(summary) {
      const transcript = document.getElementById('transcript');
      if (!transcript) return;

      const existing = document.getElementById('continuation-banner');
      if (existing) existing.remove();

      const bannerHtml = `
      <div class="continuation-banner" id="continuation-banner">
        <div class="continuation-header">
          <span class="continuation-icon">
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
              <path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"/>
              <path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"/>
            </svg>
          </span>
          <span class="continuation-label">Context compacted</span>
          <span class="continuation-toggle">▼</span>
        </div>
        <div class="continuation-content collapsed">
          <div class="continuation-summary">${DawnFormat.escapeHtml(summary)}</div>
        </div>
      </div>
    `;
      const lastEntry = transcript.querySelector('.transcript-entry:last-child');
      if (lastEntry) {
         lastEntry.insertAdjacentHTML('beforebegin', bannerHtml);
      } else {
         transcript.insertAdjacentHTML('beforeend', bannerHtml);
      }
   }

   function generateTitleFromMessage(content) {
      if (!content) return 'New conversation';
      const firstLine = content.split('\n')[0].trim();
      if (firstLine.length <= 50) return firstLine;
      return firstLine.substring(0, 47) + '...';
   }

   /* =============================================================================
    * Rendering
    * ============================================================================= */

   function renderConversationItem(conv, isChainChild) {
      const isActive = conv.id === historyState.activeConversationId;
      const isArchived = conv.is_archived;
      const time = DawnFormat.relativeTime(new Date(conv.updated_at * 1000));
      const chainIcon = conv.continued_from
         ? `
      <span class="history-item-chain" title="Continued from previous conversation">
        <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
          <path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"/>
          <path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"/>
        </svg>
      </span>
    `
         : '';
      const archivedIcon = isArchived
         ? `
      <span class="history-item-archived" title="Archived (continued in another conversation)">
        <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
          <polyline points="21 8 21 21 3 21 3 8"/>
          <rect x="1" y="3" width="22" height="5"/>
          <line x1="10" y1="12" x2="14" y2="12"/>
        </svg>
      </span>
    `
         : '';

      const isPrivate = conv.is_private === true;
      const privateIcon = isPrivate ? PRIVATE_ICON_HTML : '';

      const isPinned = conv.is_pinned === true;
      const pinnedIcon = isPinned ? PINNED_ICON_HTML : '';

      const isVoice = conv.origin === 'voice';
      const voiceIcon = isVoice
         ? `
      <span class="history-item-voice" title="Voice conversation">
        <svg width="12" height="12" viewBox="0 0 24 24" fill="currentColor">
          <path d="M12 14c1.66 0 3-1.34 3-3V5c0-1.66-1.34-3-3-3S9 3.34 9 5v6c0 1.66 1.34 3 3 3zm5.91-3c-.49 0-.9.36-.98.85C16.52 14.2 14.47 16 12 16s-4.52-1.8-4.93-4.15c-.08-.49-.49-.85-.98-.85-.61 0-1.09.54-1 1.14.49 3 2.89 5.35 5.91 5.78V20H9c-.55 0-1 .45-1 1s.45 1 1 1h6c.55 0 1-.45 1-1s-.45-1-1-1h-2v-2.08c3.02-.43 5.42-2.78 5.91-5.78.1-.6-.39-1.14-1-1.14z"/>
        </svg>
      </span>
    `
         : '';

      const isBriefing = conv.origin === 'briefing';
      const unreadBriefings =
         typeof DawnScheduler !== 'undefined' ? DawnScheduler.getUnreadBriefings() : [];
      const isUnread = isBriefing && unreadBriefings.includes(conv.id);
      const briefingIcon = isBriefing
         ? `
      <span class="history-item-briefing${isUnread ? ' unread' : ''}" title="Briefing">
        <svg width="12" height="12" viewBox="0 0 24 24" fill="currentColor">
          <path d="M18 11v2h4v-2h-4zm-2 6.61c.96.71 2.21 1.65 3.2 2.39.4-.53.8-1.07 1.2-1.6-.99-.74-2.24-1.68-3.2-2.4-.4.54-.8 1.07-1.2 1.61zM20.4 5.6c-.4-.53-.8-1.07-1.2-1.6-.99.74-2.24 1.68-3.2 2.4.4.53.8 1.07 1.2 1.6.96-.72 2.21-1.65 3.2-2.4zM4 9c-1.1 0-2 .9-2 2v2c0 1.1.9 2 2 2h1l5 3V6L5 9H4zm11.5 3c0-1.33-.58-2.53-1.5-3.35v6.69c.92-.81 1.5-2.01 1.5-3.34z"/>
        </svg>
      </span>
    `
         : '';

      // Messaging-channel origin (sms, telegram, discord, slack, ...).
      // Origin format from messaging_engine.c:505 is "messaging:<provider>".
      const isMessaging = typeof conv.origin === 'string' && conv.origin.startsWith('messaging:');
      const messagingProvider = isMessaging ? conv.origin.substring('messaging:'.length) : '';
      const messagingIcon = isMessaging ? renderMessagingIcon(messagingProvider) : '';

      // Reassign button (voice conversations + admin only)
      const isAdmin =
         typeof DawnState !== 'undefined' && DawnState.authState && DawnState.authState.isAdmin;
      const reassignButton =
         isVoice && isAdmin
            ? `
          <button class="reassign" title="Reassign to user" data-action="reassign">
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
              <path d="M16 21v-2a4 4 0 0 0-4-4H6a4 4 0 0 0-4 4v2"/>
              <circle cx="9" cy="7" r="4"/>
              <path d="M22 21v-2a4 4 0 0 0-3-3.87"/>
              <path d="M16 3.13a4 4 0 0 1 0 7.75"/>
            </svg>
          </button>
        `
            : '';

      const classes = ['history-item'];
      if (isActive) classes.push('active');
      if (isArchived) classes.push('archived');
      if (isPrivate) classes.push('private');
      if (isVoice) classes.push('voice');
      if (isBriefing) classes.push('briefing');
      // Messaging-channel conversations are identified solely by the
      // inline icon next to the title — no parent `.messaging` class
      // pushed since there's no row-level styling that depends on it.
      // Provider colorway lives on the icon span's modifier class
      // (see renderMessagingIcon).
      if (isUnread) classes.push('unread');
      if (isPinned) classes.push('pinned');
      if (isChainChild) classes.push('chain-child');
      if (historyState.generatingConvIds.has(String(conv.id))) classes.push('generating');

      // Pin/unpin toggle. Stable aria-label + aria-pressed reflects state (do not
      // also swap the label text — that double-announces); dynamic title tooltips.
      const pinButton = `
          <button class="pin${isPinned ? ' pinned' : ''}" title="${isPinned ? 'Unpin' : 'Pin'}" aria-label="Pin conversation" aria-pressed="${isPinned ? 'true' : 'false'}" data-action="pin">
            ${PIN_BUTTON_SVG}
          </button>
        `;

      return `
      <div class="${classes.join(' ')}" data-conv-id="${conv.id}">
        <div class="history-item-content">
          <div class="history-item-title">${pinnedIcon}${privateIcon}${voiceIcon}${briefingIcon}${messagingIcon}${archivedIcon}${chainIcon}${DawnFormat.escapeHtml(conv.title)}</div>
          <div class="history-item-meta">
            <span class="history-item-time">${time}</span>
            <span class="history-item-count">${conv.message_count} messages</span>
          </div>
        </div>
        <div class="history-item-actions">
          ${pinButton}
          <button class="rename" title="Rename" data-action="rename">
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
              <path d="M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7"/>
              <path d="M18.5 2.5a2.121 2.121 0 0 1 3 3L12 15l-4 1 1-4 9.5-9.5z"/>
            </svg>
          </button>
          ${reassignButton}
          <button class="delete" title="Delete" data-action="delete">
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
              <polyline points="3 6 5 6 21 6"/>
              <path d="M19 6l-2 14H7L5 6"/>
              <path d="M10 11v6M14 11v6"/>
              <path d="M9 6V4a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2"/>
            </svg>
          </button>
          <button class="export" title="Export" aria-label="Export conversation" data-action="export">
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
              <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/>
              <polyline points="7 10 12 15 17 10"/>
              <line x1="12" y1="15" x2="12" y2="3"/>
            </svg>
          </button>
        </div>
      </div>
    `;
   }

   function renderConversationList() {
      if (!historyElements.list) return;

      const conversations = historyState.conversations;

      if (!conversations || conversations.length === 0) {
         historyElements.list.innerHTML =
            '<div class="history-list-empty">No conversations yet</div>';
         return;
      }

      // Build chain relationships
      const childrenOf = {};
      const isChild = {};
      const convById = {};

      conversations.forEach((conv) => {
         convById[conv.id] = conv;
      });

      conversations.forEach((conv) => {
         if (conv.continued_from && convById[conv.continued_from]) {
            if (!childrenOf[conv.continued_from]) {
               childrenOf[conv.continued_from] = [];
            }
            childrenOf[conv.continued_from].push(conv);
            isChild[conv.id] = true;
         }
      });

      // Render a top-level row plus its continuation chain (if any).  Pinning is
      // partitioned at the top-level-row granularity so a pinned chain-parent
      // carries its whole chain group into the Pinned section — rendering it flat
      // would leave its children skipped-but-never-emitted (they vanish).
      const renderTopLevel = (conv) => {
         const children = childrenOf[conv.id] || [];
         if (children.length > 0) {
            const chainCount = children.length + 1;
            return `
            <div class="history-chain-group collapsed" data-chain-parent="${conv.id}">
              <div class="history-chain-header">
                <button class="history-chain-toggle" title="Expand chain">
                  <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                    <polyline points="9 18 15 12 9 6"/>
                  </svg>
                </button>
                <span class="history-chain-badge" title="${chainCount} linked conversations">${chainCount}</span>
              </div>
              ${renderConversationItem(conv, false)}
              <div class="history-chain-children">
                ${children.map((child) => renderConversationItem(child, true)).join('')}
              </div>
            </div>
          `;
         }
         return renderConversationItem(conv, false);
      };

      // Pinned browse-section is suppressed while searching (search is
      // find-not-browse; flat results are clearer).
      const searchActive = !!(historyState.searchQuery && historyState.searchQuery.length);
      const isPinnedTop = (conv) => !searchActive && !isChild[conv.id] && conv.is_pinned === true;

      let html = '';

      // Pinned section at the top, most-recent-active first.  The server already
      // sorts is_pinned DESC, updated_at DESC; sort defensively in case the cached
      // array was mutated in place (pin toggle patches without a refetch).
      const pinned = conversations.filter(isPinnedTop).sort((a, b) => b.updated_at - a.updated_at);
      if (pinned.length > 0) {
         // Decorative glyph (aria-hidden) — the visible "Pinned" text is the label.
         html += `<div class="history-date-group pinned-group"><span class="history-item-pinned" aria-hidden="true"><svg width="12" height="12" viewBox="0 0 24 24" fill="currentColor"><path d="M16 3v2l-1 1v4l3 3v2h-5v5l-1 1-1-1v-5H5v-2l3-3V6L7 5V3h9z"/></svg></span><span>Pinned</span></div>`;
         pinned.forEach((conv) => {
            html += renderTopLevel(conv);
         });
      }

      // Group the remaining top-level rows by date (pinned rows already emitted).
      const groups = {};
      const now = new Date();
      const today = new Date(now.getFullYear(), now.getMonth(), now.getDate()).getTime();
      const yesterday = today - 86400000;
      const weekAgo = today - 604800000;

      conversations.forEach((conv) => {
         if (isChild[conv.id]) return;
         if (isPinnedTop(conv)) return; // already in the Pinned section

         const timestamp = conv.updated_at * 1000;
         let groupKey;

         if (timestamp >= today) {
            groupKey = 'Today';
         } else if (timestamp >= yesterday) {
            groupKey = 'Yesterday';
         } else if (timestamp >= weekAgo) {
            groupKey = 'This Week';
         } else {
            const date = new Date(timestamp);
            groupKey = date.toLocaleDateString('en-US', { month: 'long', year: 'numeric' });
         }

         if (!groups[groupKey]) {
            groups[groupKey] = [];
         }
         groups[groupKey].push(conv);
      });

      // Render date groups
      for (const [groupName, convs] of Object.entries(groups)) {
         html += `<div class="history-date-group">${groupName}</div>`;
         convs.forEach((conv) => {
            html += renderTopLevel(conv);
         });
      }

      historyElements.list.innerHTML = html;

      // Add chain toggle event listeners
      historyElements.list.querySelectorAll('.history-chain-toggle').forEach((toggle) => {
         toggle.addEventListener('click', (e) => {
            e.stopPropagation();
            const group = toggle.closest('.history-chain-group');
            if (group) {
               group.classList.toggle('collapsed');
            }
         });
      });

      // Add event listeners for items
      historyElements.list.querySelectorAll('.history-item').forEach((item) => {
         const convId = parseInt(item.dataset.convId, 10);

         item.addEventListener('click', (e) => {
            if (e.target.closest('.history-item-actions')) return;
            requestLoadConversation(convId);
            // On mobile, dismiss the history panel after selecting a conversation
            if (window.innerWidth <= 600) {
               closeHistory();
            }
         });

         const pinBtn = item.querySelector('[data-action="pin"]');
         if (pinBtn) {
            pinBtn.addEventListener('click', (e) => {
               e.stopPropagation();
               const conv = convById[convId];
               const nextPinned = !(conv && conv.is_pinned === true);
               // Remember which row to refocus after the re-render moves it between
               // the Pinned section and its date group.
               pendingPinFocusId = convId;
               requestSetPinned(convId, nextPinned);
            });
         }

         const renameBtn = item.querySelector('[data-action="rename"]');
         if (renameBtn) {
            renameBtn.addEventListener('click', async (e) => {
               e.stopPropagation();
               const title = item.querySelector('.history-item-title').textContent;
               const newTitle = await DawnDialog.prompt('', title, {
                  title: 'Rename Conversation',
                  okText: 'Rename',
               });
               if (newTitle && newTitle.trim() && newTitle !== title) {
                  requestRenameConversation(convId, newTitle.trim());
               }
            });
         }

         const deleteBtn = item.querySelector('[data-action="delete"]');
         if (deleteBtn) {
            deleteBtn.addEventListener('click', async (e) => {
               e.stopPropagation();
               if (
                  await DawnDialog.confirm('Delete this conversation?', {
                     title: 'Delete Conversation',
                     okText: 'Delete',
                     danger: true,
                  })
               ) {
                  requestDeleteConversation(convId);
               }
            });
         }

         // Export button — uses format from settings config
         const exportBtn = item.querySelector('[data-action="export"]');
         if (exportBtn) {
            exportBtn.addEventListener('click', (e) => {
               e.stopPropagation();
               exportBtn.disabled = true;
               exportBtn.classList.add('exporting');
               requestExportConversation(convId);
            });
         }

         // Reassign button (voice conversations, admin only)
         const reassignBtn = item.querySelector('[data-action="reassign"]');
         if (reassignBtn) {
            reassignBtn.addEventListener('click', (e) => {
               e.stopPropagation();
               showReassignModal(convId);
            });
         }
      });

      // Restore scroll position after render (M12)
      if (savedScrollPosition > 0 && historyElements.list) {
         historyElements.list.scrollTop = savedScrollPosition;
      }

      // Refocus the pin button of a just-toggled row (it moved sections). The
      // innerHTML rebuild dropped the old focused element to <body>.
      if (pendingPinFocusId != null && historyElements.list) {
         const btn = historyElements.list.querySelector(
            `.history-item[data-conv-id="${CSS.escape(String(pendingPinFocusId))}"] [data-action="pin"]`
         );
         if (btn) btn.focus();
         pendingPinFocusId = null;
      }
   }

   /* =============================================================================
    * Panel Control
    * ============================================================================= */

   function toggleHistory() {
      if (!historyElements.panel) return;

      if (historyElements.panel.classList.contains('hidden')) {
         openHistory();
      } else {
         closeHistory();
      }
   }

   function openHistory(focusSearch) {
      if (!historyElements.panel) return;

      historyElements.panel.classList.remove('hidden');
      historyElements.overlay.classList.remove('hidden');
      /* Panel overlays without pushing content (like settings) */

      if (historyElements.openBtn) {
         historyElements.openBtn.classList.add('active');
      }
      if (historyElements.sidebarToggle) {
         historyElements.sidebarToggle.classList.add('active');
         historyElements.sidebarToggle.setAttribute('aria-expanded', 'true');
      }

      if (window.innerWidth > 768) {
         localStorage.setItem('dawn_history_open', 'true');
      }

      requestListConversations();

      // Re-sync the clear (×) visibility in case search text persisted across an
      // open/close cycle (the toggle otherwise only runs on input/clear).
      if (historyElements.searchClear && historyElements.searchInput) {
         historyElements.searchClear.classList.toggle(
            'hidden',
            historyElements.searchInput.value.trim().length === 0
         );
      }

      // Focus the search box only on an interactive open (the user clicked to
      // open the sidebar and likely wants to search).  On a passive restore of a
      // persisted-open sidebar after a page refresh (focusSearch === false), do
      // NOT grab focus — that would steal it from the message composer.
      if (focusSearch !== false) {
         setTimeout(() => {
            if (historyElements.searchInput) {
               historyElements.searchInput.focus();
            } else if (historyElements.closeBtn) {
               historyElements.closeBtn.focus();
            }
         }, 100);
      } else {
         // Passive restore (page refresh with the sidebar persisted open): put
         // focus on the message composer so the user can type immediately,
         // rather than leaving it in the sidebar.
         setTimeout(() => {
            const composer = document.getElementById('text-input');
            if (composer) {
               composer.focus();
            }
         }, 100);
      }

      // The focus trap pulls focus into the panel (onto the first focusable
      // element — the "+ New" button).  That's right for an interactive open,
      // but on a passive restore after refresh it would steal focus from the
      // message composer just like the search-focus above, so skip it there.
      if (focusSearch !== false && callbacks.trapFocus) {
         historyFocusTrapCleanup = callbacks.trapFocus(historyElements.panel);
      }

      if (historyEscToken === null) {
         historyEscToken = DawnEscStack.register(() => {
            closeHistory();
            return true;
         });
      }
   }

   function closeHistory() {
      if (!historyElements.panel) return;
      if (historyEscToken !== null) {
         DawnEscStack.unregister(historyEscToken);
         historyEscToken = null;
      }

      // Save scroll position before closing (M12)
      if (historyElements.list) {
         savedScrollPosition = historyElements.list.scrollTop;
      }

      if (historyFocusTrapCleanup) {
         historyFocusTrapCleanup();
         historyFocusTrapCleanup = null;
      }

      historyElements.panel.classList.add('hidden');
      historyElements.overlay.classList.add('hidden');
      /* Panel overlays without pushing content (like settings) */

      if (historyElements.openBtn) {
         historyElements.openBtn.classList.remove('active');
      }
      if (historyElements.sidebarToggle) {
         historyElements.sidebarToggle.classList.remove('active');
         historyElements.sidebarToggle.setAttribute('aria-expanded', 'false');
      }

      if (window.innerWidth > 768) {
         localStorage.setItem('dawn_history_open', 'false');
      }

      if (historyElements.searchInput) {
         historyElements.searchInput.value = '';
      }
      historyState.searchQuery = '';

      // Clear conversations array to free memory when panel closed (M6)
      historyState.conversations = [];

      // Focus the appropriate toggle button
      if (window.innerWidth > 600 && historyElements.sidebarToggle) {
         historyElements.sidebarToggle.focus();
      } else if (historyElements.openBtn) {
         historyElements.openBtn.focus();
      }
   }

   function restoreHistorySidebarState() {
      if (historyStateRestored) return;
      const authState = callbacks.getAuthState ? callbacks.getAuthState() : {};
      if (window.innerWidth > 768 && authState.authenticated) {
         historyStateRestored = true;
         const savedState = localStorage.getItem('dawn_history_open');
         if (savedState === 'true') {
            openHistory(false); // passive restore — don't steal focus from the composer
         }
      }
   }

   function updateHistoryButtonVisibility() {
      const authState = callbacks.getAuthState ? callbacks.getAuthState() : {};
      const authenticated = authState.authenticated;

      // Mobile header button
      if (historyElements.openBtn) {
         if (authenticated) {
            historyElements.openBtn.classList.remove('hidden');
         } else {
            historyElements.openBtn.classList.add('hidden');
         }
      }

      // Sidebar rail (desktop/tablet)
      if (historyElements.sidebarRail) {
         if (authenticated) {
            historyElements.sidebarRail.classList.remove('hidden');
            document.body.classList.add('has-sidebar-rail');
         } else {
            historyElements.sidebarRail.classList.add('hidden');
            document.body.classList.remove('has-sidebar-rail');
         }
      }
   }

   /* =============================================================================
    * Public Functions
    * ============================================================================= */

   /**
    * Save a message to the current conversation (auto-creates conversation if needed)
    */
   function saveMessageToHistory(role, content, reasoning) {
      if (role !== 'user' && role !== 'assistant' && role !== 'tool') return;
      if (!content || !content.trim()) return;

      const authState = callbacks.getAuthState ? callbacks.getAuthState() : {};
      if (!authState.authenticated) return;

      if (historyState.activeConversationId) {
         // Lock LLM settings on user message (only applies if conversation has 0 messages)
         if (role === 'user' && typeof DawnSettings !== 'undefined') {
            DawnSettings.lockConversationLlmSettings(historyState.activeConversationId);
         }
         requestSaveMessage(historyState.activeConversationId, role, content, reasoning);
         return;
      }

      if (role === 'user') {
         historyState.pendingMessages.push({ role, content });

         if (!historyState.creatingConversation) {
            historyState.creatingConversation = true;
            const title = generateTitleFromMessage(content);
            requestNewConversation(title);
         }
      } else {
         historyState.pendingMessages.push({ role, content, reasoning });
      }
   }

   /**
    * Create the conversation row BEFORE the first message is sent, so the server
    * tags that turn's frames with the real conversation id instead of 0.
    *
    * WebSocket messages are processed in order per connection, so sending
    * new_conversation here (before the text message) guarantees the server sets
    * conn->active_conversation_id before it dispatches the text — closing the
    * fresh-chat race where the row didn't exist yet at dispatch time. No-op if a
    * conversation is already active or one is already being created; the existing
    * saveMessageToHistory guard (creatingConversation) prevents a double-create.
    * @param {string} text - the user's message (used only for the auto-title)
    */
   function beginConversationBeforeSend(text) {
      const authState = callbacks.getAuthState ? callbacks.getAuthState() : {};
      if (!authState.authenticated) return;
      if (historyState.activeConversationId || historyState.creatingConversation) return;
      historyState.creatingConversation = true;
      requestNewConversation(generateTitleFromMessage(text || ''));
   }

   /**
    * Start a new chat (clears current conversation)
    */
   function startNewChat() {
      setActiveConversationId(null);
      historyState.pendingMessages = [];
      historyState.creatingConversation = false;
      loadedContextGuard = null;

      setArchivedMode(false);

      const transcript = document.getElementById('transcript');
      if (transcript) {
         transcript.innerHTML = '';
      }

      // Reset per-conversation LLM settings
      if (typeof DawnSettings !== 'undefined') {
         DawnSettings.resetConversationLlmControls();
      }

      // Reset context gauge to 0 (new conversation has no tokens)
      if (typeof DawnContextGauge !== 'undefined') {
         const fallbackMax =
            knownContextMax ||
            (typeof DawnConfig !== 'undefined' ? DawnConfig.DEFAULT_CONTEXT_MAX : 128000);
         DawnContextGauge.updateDisplay(
            { current: 0, max: fallbackMax, usage: 0 },
            typeof DawnMetrics !== 'undefined' ? DawnMetrics.updatePanel : null
         );
      }

      // Reset metrics averages for new conversation
      if (typeof DawnMetrics !== 'undefined') {
         DawnMetrics.resetAverages();
      }

      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({ type: 'clear_session' });
      }
   }

   /* =============================================================================
    * Initialization
    * ============================================================================= */

   function initHistoryElements() {
      historyElements.panel = document.getElementById('history-panel');
      historyElements.overlay = document.getElementById('history-overlay');
      historyElements.openBtn = document.getElementById('history-btn');
      historyElements.closeBtn = document.getElementById('history-close');
      historyElements.newBtn = document.getElementById('new-conversation-btn');
      historyElements.searchInput = document.getElementById('history-search-input');
      historyElements.searchClear = document.getElementById('history-search-clear');
      historyElements.searchContentCheckbox = document.getElementById('history-search-content');
      historyElements.list = document.getElementById('history-list');
      // The scrollable container is .history-content (overflow-y:auto), NOT
      // #history-list (display:flex) — infinite-scroll must listen on this.
      historyElements.content = historyElements.panel
         ? historyElements.panel.querySelector('.history-content')
         : null;
      // Sidebar rail
      historyElements.sidebarRail = document.getElementById('sidebar-rail');
      historyElements.sidebarToggle = document.getElementById('sidebar-toggle');
      historyElements.newBtnRail = document.getElementById('new-conversation-btn-rail');
   }

   function initHistoryListeners() {
      if (historyElements.openBtn) {
         historyElements.openBtn.addEventListener('click', toggleHistory);
      }

      if (historyElements.closeBtn) {
         historyElements.closeBtn.addEventListener('click', closeHistory);
      }

      // Conversation-list infinite scroll (load older conversations near the bottom).
      // Listen on the scrollable container (.history-content), not #history-list.
      if (historyElements.content) {
         historyElements.content.addEventListener('scroll', handleConversationListScroll);
      }

      if (historyElements.overlay) {
         historyElements.overlay.addEventListener('click', closeHistory);
      }

      // Sidebar rail: toggle button
      if (historyElements.sidebarToggle) {
         historyElements.sidebarToggle.addEventListener('click', toggleHistory);
      }

      // Sidebar rail: new conversation button
      if (historyElements.newBtnRail) {
         historyElements.newBtnRail.addEventListener('click', () => {
            startNewChat();
         });
      }

      if (historyElements.newBtn) {
         historyElements.newBtn.addEventListener('click', () => {
            startNewChat();
            // On mobile, dismiss the history panel after starting a new chat
            if (window.innerWidth <= 600) {
               closeHistory();
            }
         });
      }

      if (historyElements.searchInput) {
         historyElements.searchInput.addEventListener('input', (e) => {
            const query = e.target.value.trim();
            historyState.searchQuery = query;

            // Show the clear button only when there's text.
            if (historyElements.searchClear) {
               historyElements.searchClear.classList.toggle('hidden', query.length === 0);
            }

            if (historyState.searchTimeout) {
               clearTimeout(historyState.searchTimeout);
            }
            historyState.searchTimeout = setTimeout(() => {
               if (query) {
                  const searchContent = historyElements.searchContentCheckbox?.checked || false;
                  requestSearchConversations(query, searchContent);
               } else {
                  requestListConversations();
               }
            }, 300);
         });
      }

      // Clear (×) button: reset the search and restore the full conversation list.
      if (historyElements.searchClear) {
         historyElements.searchClear.addEventListener('click', () => {
            if (historyState.searchTimeout) {
               clearTimeout(historyState.searchTimeout);
               historyState.searchTimeout = null;
            }
            if (historyElements.searchInput) {
               historyElements.searchInput.value = '';
               historyElements.searchInput.focus();
            }
            historyState.searchQuery = '';
            historyElements.searchClear.classList.add('hidden');
            requestListConversations();
         });
      }

      if (historyElements.searchContentCheckbox) {
         historyElements.searchContentCheckbox.addEventListener('change', () => {
            const query = historyState.searchQuery;
            if (query) {
               const searchContent = historyElements.searchContentCheckbox.checked;
               requestSearchConversations(query, searchContent);
            }
         });
      }
   }

   function init() {
      initHistoryElements();
      initHistoryListeners();
      restoreActiveConversationId();
   }

   function setCallbacks(cbs) {
      if (cbs.trapFocus) callbacks.trapFocus = cbs.trapFocus;
      if (cbs.getAuthState) callbacks.getAuthState = cbs.getAuthState;
   }

   /* =============================================================================
    * Global Functions (for inline handlers)
    * ============================================================================= */

   window.toggleContinuationBanner = function () {
      const content = document.querySelector('.continuation-content');
      const toggle = document.querySelector('.continuation-toggle');
      if (content && toggle) {
         content.classList.toggle('collapsed');
         toggle.textContent = content.classList.contains('collapsed') ? '▼' : '▲';
      }
   };

   window.loadConversation = function (convId) {
      requestLoadConversation(convId);
   };

   /**
    * Update a conversation's privacy state in the history list and local state
    * Called when privacy is toggled via the UI
    * @param {number} convId - Conversation ID
    * @param {boolean} isPrivate - New privacy state
    */
   function updateConversationPrivacy(convId, isPrivate) {
      // Update local state
      const conv = historyState.conversations.find((c) => c.id === convId);
      if (conv) {
         conv.is_private = isPrivate;
      }

      // Update DOM element if visible
      const item = historyElements.list?.querySelector(`.history-item[data-conv-id="${convId}"]`);
      if (item) {
         item.classList.toggle('private', isPrivate);

         // Update the private icon in the title
         const title = item.querySelector('.history-item-title');
         if (title) {
            // Remove existing private icon
            const existingPrivate = title.querySelector('.history-item-private');
            if (existingPrivate) {
               existingPrivate.remove();
            }

            // Add private icon if now private
            if (isPrivate) {
               title.insertAdjacentHTML('afterbegin', PRIVATE_ICON_HTML);
            }
         }
      }
   }

   /* =============================================================================
    * Conversation Export
    * ============================================================================= */

   /**
    * Request export of a conversation
    * @param {number} convId - Conversation ID to export
    * @param {string} [format] - Export format ('json' or 'html'), uses config default if omitted
    */
   function requestExportConversation(convId, format) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         // Use provided format, or fall back to config default
         const fmt =
            format ||
            (typeof DawnSettings !== 'undefined'
               ? DawnSettings.getConfig()?.webui?.export_format
               : null) ||
            'json';
         DawnWS.send({
            type: 'export_conversation',
            payload: { conversation_id: convId, format: fmt },
         });
      }
   }

   /**
    * Sanitize a string for use as a filename
    * @param {string} str - Input string
    * @param {number} maxLen - Maximum length
    * @returns {string} Safe filename fragment
    */
   function sanitizeFilename(str, maxLen) {
      return str
         .replace(/[^a-zA-Z0-9_-]/g, '-')
         .replace(/-+/g, '-')
         .replace(/^-|-$/g, '')
         .substring(0, maxLen || 50);
   }

   /**
    * Build a date+time string for filenames from an ISO 8601 timestamp
    * @param {string} iso - ISO 8601 timestamp
    * @returns {string} "YYYY-MM-DD-HHmm" or date-only fallback
    */
   function filenameDatetime(iso) {
      if (!iso) return new Date().toISOString().substring(0, 10);
      const d = new Date(iso);
      const pad = (n) => String(n).padStart(2, '0');
      return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}-${pad(d.getHours())}${pad(d.getMinutes())}`;
   }

   /**
    * Handle export conversation response — trigger browser download
    * @param {Object} payload - Response payload with data object
    */
   function handleExportConversationResponse(payload) {
      // Re-enable all export buttons
      document.querySelectorAll('.history-item-actions .export').forEach((btn) => {
         btn.disabled = false;
         btn.classList.remove('exporting');
      });

      if (!payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Export failed: ' + (payload.error || 'Unknown error'), 'error');
         }
         return;
      }

      const data = payload.data;
      const format = payload.format || 'json';
      const title = sanitizeFilename(data.conversation?.title || 'conversation', 50);
      const datetime = filenameDatetime(data.conversation?.updated_at);

      let blob, ext;
      if (format === 'html') {
         const html =
            typeof DawnExport !== 'undefined'
               ? DawnExport.buildHtml(data)
               : `<pre>${JSON.stringify(data, null, 2)}</pre>`;
         blob = new Blob([html], { type: 'text/html;charset=utf-8' });
         ext = 'html';
      } else {
         blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
         ext = 'json';
      }

      const filename = `${title}-${datetime}.${ext}`;

      // Trigger download
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = filename;
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
      URL.revokeObjectURL(url);

      if (typeof DawnToast !== 'undefined') {
         DawnToast.show(`Exported: ${filename}`, 'success');
      }
   }

   /* =============================================================================
    * Reassign Modal (Admin Only)
    * ============================================================================= */

   /**
    * Show the reassign user modal for a voice conversation
    * @param {number} convId - Conversation ID to reassign
    */
   function showReassignModal(convId) {
      historyState.reassignModalConvId = convId;

      // Request users list from server
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({ type: 'list_users' });
      }
   }

   /**
    * Handle users list response for reassign modal
    * @param {Object} payload - Response payload with users array
    */
   function handleUsersListForReassign(payload) {
      if (!payload.success || !historyState.reassignModalConvId) {
         return;
      }

      historyState.usersList = payload.users || [];
      renderReassignModal();
   }

   /**
    * Render the reassign modal with user list
    */
   function renderReassignModal() {
      // Remove existing modal if any
      const existing = document.getElementById('reassign-modal');
      if (existing) {
         existing.remove();
      }

      const users = historyState.usersList;
      const convId = historyState.reassignModalConvId;

      // Find conversation title for display
      const conv = historyState.conversations.find((c) => c.id === convId);
      const convTitle = conv ? DawnFormat.escapeHtml(conv.title) : `Conversation #${convId}`;

      // Build user options
      const userOptions = users
         .map(
            (user) => `
            <option value="${user.id}">${DawnFormat.escapeHtml(user.username)}${user.is_admin ? ' (Admin)' : ''}</option>
         `
         )
         .join('');

      const modalHtml = `
         <div id="reassign-modal" class="modal-overlay" role="dialog" aria-modal="true" aria-labelledby="reassign-title">
            <div class="modal-content">
               <h3 id="reassign-title" class="modal-header">Reassign Conversation</h3>
               <p class="modal-description">Reassign "<span class="modal-conv-title">${convTitle}</span>" to:</p>
               <div class="modal-form">
                  <label for="reassign-user-select" class="sr-only">Select User</label>
                  <select id="reassign-user-select" class="modal-select" aria-label="Select user">
                     <option value="">-- Select User --</option>
                     ${userOptions}
                  </select>
               </div>
               <div class="modal-actions">
                  <button id="reassign-cancel" class="modal-btn secondary">Cancel</button>
                  <button id="reassign-confirm" class="modal-btn primary" disabled>Reassign</button>
               </div>
            </div>
         </div>
      `;

      document.body.insertAdjacentHTML('beforeend', modalHtml);

      const modal = document.getElementById('reassign-modal');
      const select = document.getElementById('reassign-user-select');
      const confirmBtn = document.getElementById('reassign-confirm');
      const cancelBtn = document.getElementById('reassign-cancel');

      // Enable confirm button when user is selected
      select.addEventListener('change', () => {
         confirmBtn.disabled = !select.value;
      });

      // Handle confirm
      confirmBtn.addEventListener('click', () => {
         const userId = parseInt(select.value, 10);
         if (userId > 0) {
            requestReassignConversation(convId, userId);
            closeReassignModal();
         }
      });

      // Handle cancel
      cancelBtn.addEventListener('click', closeReassignModal);

      // Close on overlay click
      modal.addEventListener('click', (e) => {
         if (e.target === modal) {
            closeReassignModal();
         }
      });

      // Escape close via DawnEscStack (unregistered in closeReassignModal).
      if (reassignEscToken === null) {
         reassignEscToken = DawnEscStack.register(() => {
            closeReassignModal();
            return true;
         });
      }

      // Focus the select and trap focus within modal
      select.focus();
      if (callbacks.trapFocus) {
         reassignFocusTrapCleanup = callbacks.trapFocus(modal);
      }
   }

   /**
    * Close the reassign modal
    */
   function closeReassignModal() {
      if (reassignEscToken !== null) {
         DawnEscStack.unregister(reassignEscToken);
         reassignEscToken = null;
      }
      if (reassignFocusTrapCleanup) {
         reassignFocusTrapCleanup();
         reassignFocusTrapCleanup = null;
      }
      const modal = document.getElementById('reassign-modal');
      if (modal) {
         modal.remove();
      }
      historyState.reassignModalConvId = null;
   }

   /**
    * Request to reassign a conversation to a new user
    * @param {number} convId - Conversation ID
    * @param {number} newUserId - New user ID
    */
   function requestReassignConversation(convId, newUserId) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({
            type: 'reassign_conversation',
            payload: {
               conversation_id: convId,
               new_user_id: newUserId,
            },
         });
      }
   }

   /**
    * Handle reassign conversation response
    * @param {Object} payload - Response payload
    */
   function handleReassignResponse(payload) {
      if (payload.success) {
         // Remove the conversation from current user's list
         const convId = payload.conversation_id;
         historyState.conversations = historyState.conversations.filter((c) => c.id !== convId);
         renderConversationList();

         // Clear active conversation if it was the reassigned one
         if (historyState.activeConversationId === convId) {
            historyState.activeConversationId = null;
            localStorage.removeItem('dawn_active_conv');
         }

         console.log('Conversation reassigned successfully');
      } else {
         console.error('Failed to reassign conversation:', payload.error);
      }
   }

   /* =============================================================================
    * Export
    * ============================================================================= */

   /**
    * Update the known context max from server-provided data.
    * Called when a 'context' message arrives with model-specific max.
    * @param {number} max - Context max from server
    */
   function setKnownContextMax(max) {
      if (max && max > 0) {
         knownContextMax = max;
      }
   }

   /**
    * Get the loaded context guard (non-destructive).
    * Returns { current, max } if a conversation was just loaded with saved context,
    * or null if no guard is active. The guard stays active until a new conversation
    * is loaded or a new chat is started (multiple set_session_llm responses can
    * arrive after a single load). Called by dawn.js context handler.
    */
   function getContextGuard() {
      return loadedContextGuard;
   }

   /**
    * Clear the loaded context guard. Called when a real LLM response arrives
    * with fresh context data, so subsequent context messages aren't overridden.
    */
   function clearContextGuard() {
      loadedContextGuard = null;
   }

   window.DawnHistory = {
      init: init,
      setCallbacks: setCallbacks,
      toggle: toggleHistory,
      open: openHistory,
      close: closeHistory,
      updateButtonVisibility: updateHistoryButtonVisibility,
      restoreSidebarState: restoreHistorySidebarState,
      saveMessage: saveMessageToHistory,
      startNewChat: startNewChat,
      requestUpdateContext: requestUpdateContext,
      getActiveConversationId: getActiveConversationId,
      setConversationGenerating: setConversationGenerating,
      clearGenerating: clearGenerating,
      beginConversationBeforeSend: beginConversationBeforeSend,
      setKnownContextMax: setKnownContextMax,
      getContextGuard: getContextGuard,
      clearContextGuard: clearContextGuard,
      // Response handlers
      handleListResponse: handleListConversationsResponse,
      handleNewResponse: handleNewConversationResponse,
      handleLoadResponse: handleLoadConversationResponse,
      handleDeleteResponse: handleDeleteConversationResponse,
      handleRenameResponse: handleRenameConversationResponse,
      handleSetPinnedResponse: handleSetPinnedResponse,
      handleSearchResponse: handleSearchConversationsResponse,
      handleSaveResponse: handleSaveMessageResponse,
      handleContextCompacted: handleContextCompacted,
      handleContinueResponse: handleContinueConversationResponse,
      updateConversationPrivacy: updateConversationPrivacy,
      // Export handler
      handleExportResponse: handleExportConversationResponse,
      // Reassign modal handlers (admin only)
      handleUsersListForReassign: handleUsersListForReassign,
      handleReassignResponse: handleReassignResponse,
      // Auto-title broadcast handler
      handleConversationRenamed: handleConversationRenamed,
      // External-writer append broadcast handler (SMS/Telegram/future Discord)
      handleConversationMessagesAppended: handleConversationMessagesAppended,
      // Briefing support
      loadConversation: requestLoadConversation,
      refreshList: requestListConversations,
      refreshUnread: renderConversationList,
   };
})();
