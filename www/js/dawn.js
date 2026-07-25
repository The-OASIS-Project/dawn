/**
 * DAWN WebUI JavaScript
 * Phase 4: Voice input and audio playback
 */

(function () {
   'use strict';

   // =============================================================================
   // State (core state in js/core/state.js, websocket in js/core/websocket.js)
   // =============================================================================

   // UI state
   let visualizerCollapsed = false;
   let pendingIdleState = false; // Server sent "idle" but audio still playing
   // The conversation already restored for this connection. The server emits multiple
   // 'session' messages per (re)connect (reconnect + capability-update), and each would
   // otherwise re-issue a full load_conversation. Reset on disconnect (updateConnectionStatus).
   let restoredConvIdThisConnection = null;
   let pendingThumbnailsForSave = []; // Thumbnails to attach when saving user message
   let pendingVisualsForSave = []; // <dawn-visual> content to attach to next assistant message

   // Opus codec state
   let opusWorker = null;
   let opusReady = false;
   let pendingDecodes = [];
   let pendingOpusData = [];
   let pendingDecodePlayback = false;
   let segmentIsOpus = null; // Codec locked on first chunk of each audio segment

   // =============================================================================
   // WebSocket Callbacks (connection managed by DawnWS module)
   // =============================================================================
   function initWebSocketCallbacks() {
      DawnWS.setCallbacks({
         onStatus: updateConnectionStatus,
         onTextMessage: handleTextMessage,
         onBinaryMessage: handleBinaryMessage,
         getOpusReady: () => opusReady,
         getTtsEnabled: () => DawnTts.isEnabled(),
      });
   }

   // =============================================================================
   // Message Handling
   // =============================================================================

   // True if a payload carries a conversation_id that belongs to a conversation
   // OTHER than the one on screen — a background turn's frame that must not render
   // into the active view (mirrors streaming.js isBackgroundStream). Untagged
   // payloads (no conversation_id — global/control frames) are never foreign.
   function isForeignConvFrame(payload) {
      const c = payload && payload.conversation_id;
      if (!c) return false;
      const a =
         typeof DawnHistory !== 'undefined' && DawnHistory.getActiveConversationId
            ? DawnHistory.getActiveConversationId()
            : null;
      return String(c) !== String(a || '');
   }

   function handleTextMessage(data) {
      try {
         const msg = JSON.parse(data);
         console.log('Received:', msg);

         switch (msg.type) {
            case 'state':
               // A background turn's state (thinking/speaking/idle for a conversation
               // not on screen) must not drive the pill. Untagged states (no
               // conversation_id — global: idle-at-rest, listening) always apply.
               if (isForeignConvFrame(msg.payload)) break;
               updateState(msg.payload.state, msg.payload.detail, msg.payload.tools);
               break;
            case 'force_logout':
               // Session was revoked - force immediate logout
               console.warn('Force logout received:', msg.payload.reason);
               DawnToast.show(msg.payload.reason || 'Session revoked', 'error');
               DawnStore.remove(DawnStore.KEYS.SESSION_TOKEN);
               sessionStorage.removeItem('dawn_active_conversation');
               DawnWS.disconnect();
               setTimeout(() => {
                  window.location.href = '/login.html';
               }, 1500);
               break;
            case 'transcript':
               // Check for special LLM state update (sent with role '__llm_state__')
               if (msg.payload.role === '__llm_state__') {
                  try {
                     const stateMsg = JSON.parse(msg.payload.text);
                     if (stateMsg.type === 'llm_state_update' && stateMsg.payload) {
                        console.log('LLM state update received:', stateMsg.payload);
                        DawnSettings.updateLlmControls(stateMsg.payload);
                     }
                  } catch (e) {
                     console.error('Failed to parse LLM state update:', e);
                  }
               } else if (isForeignConvFrame(msg.payload)) {
                  // Live transcript frame (tool debug / inline visual / assistant /
                  // user echo) from a turn in a conversation NOT on screen — skip so
                  // a background turn's rows/visuals don't render into the active view.
                  // (The sidebar generating dot is already driven by stream/thinking.)
               } else if (msg.payload.role === 'tool') {
                  // Tool debug messages - display only, don't save to history.
                  // E3: a native tool call means the preceding iteration's reasoning is
                  // persisted server-side (on its tool_calls row), so discard the browser's
                  // copy — otherwise a reasoning-only iteration (which never opened a text
                  // bubble, so finalizeStreamBubble didn't run) leaks its reasoning into the
                  // final answer's save, producing a duplicate "AI thought" panel on reload.
                  if (
                     typeof DawnStreaming !== 'undefined' &&
                     typeof msg.payload.text === 'string' &&
                     msg.payload.text.startsWith('[Tool Call:')
                  ) {
                     DawnStreaming.clearReasoningState();
                  }
                  DawnTranscript.addEntry(msg.payload.role, msg.payload.text);
               } else if (msg.payload.role === 'visual') {
                  // Render visual inline. If a progress placeholder exists (from
                  // visual_progress_start), swap it in-place. Otherwise, split the
                  // streaming entry (text → diagram → more text).
                  if (typeof DawnVisualRender !== 'undefined') {
                     var extracted = DawnVisualRender.extractVisuals(msg.payload.text);
                     if (extracted.visuals.length > 0) {
                        var sEntry = DawnState.streamingState.entryElement;
                        if (sEntry) {
                           // Check for existing progress placeholder (in-place swap)
                           var placeholder = sEntry.querySelector('.dawn-visual-progress');
                           if (placeholder) {
                              // Placeholder already split the text — just replace it
                              extracted.visuals.forEach(function (v) {
                                 var frame = DawnVisualRender.createFrame(v.title, v.type, v.code);
                                 frame.classList.add('dawn-visual-entering');
                                 placeholder.parentNode.insertBefore(frame, placeholder);
                              });
                              // Clear interval timer and remove placeholder
                              if (placeholder._progressTimer) {
                                 clearInterval(placeholder._progressTimer);
                              }
                              placeholder.remove();
                           } else {
                              // No placeholder — run the full split logic (history replay etc.)
                              if (
                                 DawnState.streamingState.textElement &&
                                 DawnState.streamingState.content
                              ) {
                                 DawnState.streamingState.textElement.innerHTML =
                                    DawnFormat.markdown(DawnState.streamingState.content);
                              }

                              extracted.visuals.forEach(function (v) {
                                 var frame = DawnVisualRender.createFrame(v.title, v.type, v.code);
                                 sEntry.appendChild(frame);
                              });

                              var newTextEl = document.createElement('div');
                              newTextEl.className = 'text';
                              sEntry.appendChild(newTextEl);

                              DawnState.streamingState.preVisualContent =
                                 (DawnState.streamingState.preVisualContent || '') +
                                 DawnState.streamingState.content;
                              DawnState.streamingState.content = '';
                              DawnState.streamingState.textElement = newTextEl;
                           }
                        } else {
                           /* No active streaming entry: the visual arrived during a tool
                            * phase before the answer streamed (a diagram-only turn). Render
                            * it as its own assistant entry so it shows live — otherwise it
                            * would only appear on reload, since finalizeStream() drains
                            * pending visuals into the save payload, not the live view.
                            * Mirrors addNormalEntry's visual-segment entry structure. */
                           var vEntry = document.createElement('div');
                           vEntry.className = 'transcript-entry assistant';
                           vEntry.innerHTML =
                              '<div class="role">assistant</div><div class="text"></div>';
                           DawnElements.transcript.appendChild(vEntry);
                           DawnVisualRender.renderVisuals(vEntry, extracted.visuals);
                        }
                        DawnElements.transcript.scrollTop = DawnElements.transcript.scrollHeight;
                     }
                  }
                  // Stash for history persistence (appended to next assistant save)
                  pendingVisualsForSave.push(msg.payload.text);
               } else {
                  // For user messages with pending images, format with thumbnail markers
                  // This ensures images display immediately AND are saved to history
                  let displayContent = msg.payload.text;
                  if (msg.payload.role === 'user' && pendingThumbnailsForSave.length > 0) {
                     displayContent = DawnVision.formatMessageWithImages(
                        msg.payload.text,
                        pendingThumbnailsForSave
                     );
                     pendingThumbnailsForSave = []; // Clear after use
                  }

                  // Append pending visuals to assistant messages for history persistence
                  // On replay, extractVisuals() in addNormalEntry strips and re-renders them
                  // Append pending visuals to non-streamed assistant messages
                  // (Streamed messages are handled in streaming.js finalize)
                  if (msg.payload.role === 'assistant' && pendingVisualsForSave.length > 0) {
                     displayContent += '\n' + pendingVisualsForSave.join('\n');
                     pendingVisualsForSave = [];
                  }

                  DawnTranscript.addEntry(msg.payload.role, displayContent);

                  // Save to conversation history (auto-creates conversation on first message)
                  // Skip replay messages (history replay on reconnect, already in DB)
                  // Skip server_saved messages (server persisted to DB, client save would dupe)
                  if (!msg.payload.replay && !msg.payload.server_saved) {
                     DawnHistory.saveMessage(msg.payload.role, displayContent);
                  }
               }
               break;
            case 'error':
               // Handle INFO_ prefixed codes as info notifications (not errors)
               if (msg.payload.code && msg.payload.code.startsWith('INFO_')) {
                  console.log('Server notification:', msg.payload);
                  DawnToast.show(msg.payload.message, 'info');
                  break;
               }
               console.error('Server error:', msg.payload);
               // Handle max clients error - don't auto-reconnect
               if (msg.payload.code === 'MAX_CLIENTS') {
                  DawnWS.setMaxClientsReached(true);
                  DawnElements.connectionStatus.className = 'disconnected';
                  // Responsive message based on screen width
                  DawnElements.connectionStatus.textContent =
                     window.innerWidth > 500 ? 'Server Full - Click to Retry' : 'Full';
                  DawnElements.connectionStatus.title = 'Server at capacity - click to retry';
               } else if (msg.payload.code === 'UNAUTHORIZED') {
                  // Session expired or revoked - stop everything and redirect to login
                  DawnToast.show(msg.payload.message, 'error');
                  // Clear session data
                  DawnStore.remove(DawnStore.KEYS.SESSION_TOKEN);
                  sessionStorage.removeItem('dawn_active_conversation');
                  // Disconnect WebSocket immediately to prevent further processing
                  DawnWS.disconnect();
                  // Redirect after brief delay for toast visibility
                  setTimeout(() => {
                     window.location.href = '/login.html';
                  }, 1500);
               } else if (msg.payload.code === 'FORBIDDEN') {
                  // Permission error (e.g., not admin) - just show toast
                  DawnToast.show(msg.payload.message, 'error');
               } else if (msg.payload.code && msg.payload.code.startsWith('LLM_')) {
                  // LLM errors - show toast and add to transcript
                  DawnToast.show(msg.payload.message, 'error');
                  DawnTranscript.addEntry('system', `Error: ${msg.payload.message}`);
               } else if (msg.payload.code === 'OTA_ERROR') {
                  // OTA push/rollout failures: surface in the admin panel (toast +
                  // re-enable the disabled Push/Roll Out buttons), not the chat
                  // transcript hidden behind the settings overlay.
                  DawnToast.show(msg.payload.message, 'error');
                  if (typeof DawnSatellites !== 'undefined') {
                     DawnSatellites.handleOtaError();
                  }
               } else {
                  DawnTranscript.addEntry('system', `Error: ${msg.payload.message}`);
               }
               break;
            case 'session':
               console.log('Session token received');
               DawnStore.set(DawnStore.KEYS.SESSION_TOKEN, msg.payload.token);
               // Server has processed our init/reconnect with capabilities
               DawnWS.setCapabilitiesSynced(true);
               // Reconnect music stream with fresh token (fixes stale-token failures)
               if (
                  typeof DawnMusicPlayback !== 'undefined' &&
                  DawnMusicPlayback.reconnectMusicStream
               ) {
                  DawnMusicPlayback.reconnectMusicStream();
               }
               // Auth state is now included in session response (avoids extra config fetch)
               if (msg.payload.authenticated !== undefined) {
                  DawnState.authState.authenticated = msg.payload.authenticated;
                  DawnState.authState.isAdmin = msg.payload.is_admin || false;
                  DawnState.authState.username = msg.payload.username || '';
                  DawnSettings.updateAuthVisibility();
               }
               /* Phase 2 entity-merge: prime the memory-icon dot with the
                * current pending-proposal count once the session is auth'd.
                * Without this the dot stays dark on page-refresh until the
                * next extraction creates a new proposal — even if there are
                * already pending rows from earlier sessions. */
               if (
                  msg.payload.authenticated &&
                  window.DawnMemoryAliases &&
                  typeof DawnMemoryAliases.requestProposalList === 'function'
               ) {
                  DawnMemoryAliases.requestProposalList();
               }
               // Request full config to populate LLM controls
               DawnSettings.requestConfig();
               // Rehydrate the phone banner / in-call panel if a call is live.
               // Done here (not on raw 'connected') so the reply lands on the
               // restored session rather than the throwaway reconnect session.
               if (typeof DawnPhone !== 'undefined') {
                  DawnPhone.handleReconnect();
               }
               // Rehydrate the background-jobs pills (per-conversation + global).
               if (typeof DawnJobsActivity !== 'undefined') {
                  DawnJobsActivity.handleReconnect();
               }
               // Re-enable always-on if it was active before the connection
               // dropped. Deferred to here (not raw 'connected') for the same
               // reason as phone: always_on_enable must land on the restored
               // session, and this fires once per reconnect (flag-guarded).
               if (typeof DawnAlwaysOn !== 'undefined') {
                  DawnAlwaysOn.resumeAfterReconnectIfNeeded();
               }
               // Restore active conversation context (backend session may have lost it on restart)
               // This loads the conversation history into the LLM context so subsequent
               // messages have proper context
               const savedConvId = DawnHistory.getActiveConversationId();
               if (
                  savedConvId &&
                  DawnState.authState.authenticated &&
                  savedConvId !== restoredConvIdThisConnection
               ) {
                  // First 'session' of this connection for this conversation — restore once.
                  // Later duplicate 'session' messages (capability update) are skipped.
                  restoredConvIdThisConnection = savedConvId;
                  console.log('Restoring active conversation:', savedConvId);
                  DawnWS.send({
                     type: 'load_conversation',
                     payload: { conversation_id: savedConvId },
                  });
               }
               break;
            case 'config':
               // Config payload may contain sensitive data - don't log full contents
               if (msg.payload.audio_chunk_ms) {
                  DawnAudioCapture.setAudioChunkMs(msg.payload.audio_chunk_ms);
                  console.log('Audio chunk size set to:', msg.payload.audio_chunk_ms, 'ms');
               }
               break;
            case 'server_features':
               if (msg.payload.home_assistant) {
                  document.body.classList.add('feature-homeassistant');
               }
               break;
            case 'get_config_response':
               DawnSettings.handleGetConfigResponse(msg.payload);
               // Update vision limits from server (prefer config.vision, fall back to vision_limits)
               const visionCfg = msg.payload.config?.vision || msg.payload.vision_limits;
               if (visionCfg) {
                  DawnVision.updateLimits(visionCfg);
               }
               // Update document limits from server config
               if (msg.payload.config?.documents) {
                  const docs = msg.payload.config.documents;
                  if (docs.max_documents) DawnState.documentState.maxDocuments = docs.max_documents;
                  if (docs.max_file_size_kb)
                     DawnState.documentState.maxFileSize = docs.max_file_size_kb * 1024;
               }
               // Update vision button state based on model capabilities
               if (msg.payload.config) {
                  DawnVision.checkVisionSupport(msg.payload.config);
               }
               // Gate the Coding popover button on [code_projects].enabled (fires
               // at connect and after every settings save).
               if (typeof DawnCodeProjects !== 'undefined') {
                  DawnCodeProjects.setEnabled(msg.payload.config?.code_projects?.enabled === true);
               }
               // Set known context_max from current model (for gauge fallback)
               if (msg.payload.llm_runtime && msg.payload.llm_runtime.context_max) {
                  DawnHistory.setKnownContextMax(msg.payload.llm_runtime.context_max);
               }
               break;
            case 'set_config_response':
               DawnSettings.handleSetConfigResponse(msg.payload);
               break;
            case 'set_secrets_response':
               DawnSettings.handleSetSecretsResponse(msg.payload);
               break;
            case 'get_audio_devices_response':
               DawnSettings.handleGetAudioDevicesResponse(msg.payload);
               break;
            case 'list_models_response':
               DawnSettings.handleModelsListResponse(msg.payload);
               break;
            case 'list_interfaces_response':
               DawnSettings.handleInterfacesListResponse(msg.payload);
               break;
            case 'list_llm_models_response':
               DawnSettings.handleListLlmModelsResponse(msg.payload);
               break;
            case 'restart_response':
               DawnSettings.handleRestartResponse(msg.payload);
               break;
            case 'set_session_llm_response':
               DawnSettings.handleSetSessionLlmResponse(msg.payload);
               break;
            case 'ha_status_response':
               if (typeof DawnHomeAssistant !== 'undefined') {
                  DawnHomeAssistant.handleStatusResponse(msg.payload);
               }
               break;
            case 'phone_audio_config_response':
               if (typeof DawnPhoneAudio !== 'undefined') {
                  DawnPhoneAudio.handleConfigResponse(msg.payload);
               }
               break;
            case 'ha_test_connection_response':
               if (typeof DawnHomeAssistant !== 'undefined') {
                  DawnHomeAssistant.handleTestConnectionResponse(msg.payload);
               }
               break;
            case 'ha_entities_response':
               if (typeof DawnHomeAssistant !== 'undefined') {
                  DawnHomeAssistant.handleEntitiesResponse(msg.payload);
               }
               break;
            case 'system_prompt_response':
               DawnSettings.handleSystemPromptResponse(msg.payload);
               break;
            case 'get_tools_config_response':
               DawnTools.handleGetConfigResponse(msg.payload);
               break;
            case 'set_tools_config_response':
               DawnTools.handleSetConfigResponse(msg.payload);
               break;
            case 'context': {
               // If a conversation was just loaded with saved context, set_session_llm
               // context messages arrive with stale "current" values (multiple can arrive
               // from cascading dropdown updates). Keep using the saved current until a
               // real LLM response provides fresh data.
               const guard = DawnHistory.getContextGuard();
               let ctxPayload = msg.payload;
               if (guard) {
                  ctxPayload = {
                     ...msg.payload,
                     current: guard.current,
                     usage: msg.payload.max ? (guard.current / msg.payload.max) * 100 : 0,
                  };
               }
               DawnContextGauge.updateDisplay(ctxPayload, DawnMetrics.updatePanel);
               // Track latest context_max so fallback values are model-accurate
               if (ctxPayload.max) {
                  DawnHistory.setKnownContextMax(ctxPayload.max);
               }
               // Save context to active conversation for restore on reload
               const activeConvId = DawnHistory.getActiveConversationId();
               if (activeConvId && ctxPayload.current && ctxPayload.max) {
                  DawnHistory.requestUpdateContext(
                     activeConvId,
                     ctxPayload.current,
                     ctxPayload.max
                  );
               }
               break;
            }
            case 'context_compacted':
               DawnHistory.handleContextCompacted(msg.payload);
               break;
            case 'silent_observation':
               // Phase 0 of Dynamic Context Injection — see
               // docs/DYNAMIC_CONTEXT_INJECTION_DESIGN.md.  The DawnSilentObserve
               // module owns the badge/peek/warning chip surface.
               if (window.DawnSilentObserve) {
                  window.DawnSilentObserve.handleEvent(msg.payload);
               }
               break;
            case 'context_injection':
               // Phase 1g-ii.  Per-turn context-injection rendering.  Note the
               // wire format is FLAT at the root (user_id / conversation_id /
               // turn_id / items / filter_rejections all top-level) — NOT
               // wrapped under msg.payload like silent_observation.  See
               // src/webui/webui_server.c::webui_broadcast_context_injection.
               // A background turn's injection must not update the active view's
               // "Context for this turn" panel — conversation_id is at the root, so
               // gate on msg itself (not msg.payload).
               if (isForeignConvFrame(msg)) break;
               if (window.DawnContextInjection) {
                  window.DawnContextInjection.handleEvent(msg);
               }
               break;
            case 'get_metrics_response':
               DawnMetricsPanel.handleResponse(msg.payload);
               break;
            case 'stream_start':
               DawnStreaming.handleStart(msg.payload);
               break;
            case 'stream_delta':
               DawnStreaming.handleDelta(msg.payload);
               break;
            case 'stream_end':
               DawnStreaming.handleEnd(msg.payload);
               // LLM response complete — next context message will have fresh data
               DawnHistory.clearContextGuard();
               break;
            case 'stream_resume':
               // Server replay of an in-flight turn when switching to its conversation
               DawnStreaming.handleResume(msg.payload);
               break;
            // --- Phase-2 observe stream (durable steps + persisted answers) -----
            // The browser does NOT need these to render a turn: it has its own
            // live path (stream_start/delta/end) and loads history over
            // load_conversation. They exist for consumers that have neither — the
            // Phase-5 TUI tails them, and CP4's jobs panel will render them as
            // .agent-event rows. Claimed here rather than left to fall through,
            // because a long-running job emits dozens per turn and an unhandled
            // frame logs a line each time, burying everything else in the console.
            case 'conversation_event':
               if (typeof DawnJobs !== 'undefined' && DawnJobs.handleConversationEvent) {
                  DawnJobs.handleConversationEvent(msg.payload);
               }
               break;
            case 'conversation_events':
               // Durable replay batch, sent in response to attach_conversation.
               if (typeof DawnJobs !== 'undefined' && DawnJobs.handleConversationEvents) {
                  DawnJobs.handleConversationEvents(msg.payload);
               }
               break;
            case 'message_appended':
               // Final assistant text for an event-only consumer. The browser
               // already has it from the stream, so this is a no-op here.
               break;
            case 'thinking_start':
               DawnStreaming.handleThinkingStart(msg.payload);
               break;
            case 'thinking_delta':
               DawnStreaming.handleThinkingDelta(msg.payload);
               break;
            case 'thinking_end':
               DawnStreaming.handleThinkingEnd(msg.payload);
               break;
            case 'reasoning_summary':
               DawnStreaming.handleReasoningSummary(msg.payload);
               break;
            case 'visual_progress_start':
               if (isForeignConvFrame(msg.payload)) break; // background turn's visual — skip
               if (typeof DawnVisualRender !== 'undefined' && DawnVisualRender.showProgress) {
                  DawnVisualRender.showProgress(msg.payload);
               }
               break;
            case 'metrics_update':
               // A background turn's tok/s / TTFT must not update the footer of the
               // view you're on. payload carries conversation_id; untagged (conv 0)
               // idle/global metrics still apply.
               if (isForeignConvFrame(msg.payload)) break;
               DawnMetrics.handleUpdate(msg.payload);
               break;
            // User management responses
            case 'list_users_response':
               DawnUsers.handleListResponse(msg.payload);
               // Also handle for reassign modal in history panel
               DawnHistory.handleUsersListForReassign(msg.payload);
               // Also handle for settings config (default voice user dropdown)
               DawnSettings.handleUsersListResponse(msg.payload);
               break;
            case 'create_user_response':
               DawnUsers.handleCreateResponse(msg.payload);
               break;
            case 'delete_user_response':
               DawnUsers.handleDeleteResponse(msg.payload);
               break;
            case 'change_password_response':
               DawnUsers.handleChangePasswordResponse(msg.payload);
               break;
            case 'unlock_user_response':
               DawnUsers.handleUnlockResponse(msg.payload);
               break;
            // Satellite management responses
            case 'list_satellites_response':
               if (typeof DawnSatellites !== 'undefined')
                  DawnSatellites.handleListResponse(msg.payload);
               break;
            case 'update_satellite_response':
               if (typeof DawnSatellites !== 'undefined')
                  DawnSatellites.handleUpdateResponse(msg.payload);
               break;
            case 'delete_satellite_response':
               if (typeof DawnSatellites !== 'undefined')
                  DawnSatellites.handleDeleteResponse(msg.payload);
               break;
            case 'satellite_registration_key_response':
               if (typeof DawnSatellites !== 'undefined')
                  DawnSatellites.handleRegistrationKeyResponse(msg.payload);
               break;
            case 'ota_list_response':
               if (typeof DawnSatellites !== 'undefined')
                  DawnSatellites.handleOtaListResponse(msg.payload);
               break;
            case 'ota_push_response':
               if (typeof DawnSatellites !== 'undefined')
                  DawnSatellites.handleOtaPushResponse(msg.payload);
               break;
            case 'ota_push_all_response':
               if (typeof DawnSatellites !== 'undefined')
                  DawnSatellites.handleOtaPushAllResponse(msg.payload);
               break;
            case 'ota_rollout_status_response':
               if (typeof DawnSatellites !== 'undefined')
                  DawnSatellites.handleOtaRolloutStatusResponse(msg.payload);
               break;
            case 'ota_rollout_abort_response':
               if (typeof DawnSatellites !== 'undefined')
                  DawnSatellites.handleOtaRolloutAbortResponse(msg.payload);
               break;
            // Messaging channel management responses (user-scoped)
            case 'list_channels_response':
               if (typeof DawnMessaging !== 'undefined')
                  DawnMessaging.handleListResponse(msg.payload);
               break;
            case 'create_link_code_response':
               if (typeof DawnMessaging !== 'undefined')
                  DawnMessaging.handleCreateCodeResponse(msg.payload);
               break;
            case 'unlink_channel_response':
            case 'rename_channel_response':
            case 'reenable_channel_response':
               if (typeof DawnMessaging !== 'undefined')
                  DawnMessaging.handleMutationResponse(msg.payload);
               break;
            case 'set_channel_llm_response':
               if (typeof DawnMessaging !== 'undefined')
                  DawnMessaging.handleSetChannelLlmResponse(msg.payload);
               break;
            case 'get_my_settings_response':
               DawnMySettings.handleGetResponse(msg.payload);
               break;
            case 'set_my_settings_response':
               DawnMySettings.handleSetResponse(msg.payload);
               break;
            case 'list_my_sessions_response':
               DawnMySessions.handleListResponse(msg.payload);
               break;
            case 'revoke_session_response':
               DawnMySessions.handleRevokeResponse(msg.payload);
               break;
            case 'list_conversations_response':
               DawnHistory.handleListResponse(msg.payload);
               break;
            case 'new_conversation_response':
               DawnHistory.handleNewResponse(msg.payload);
               break;
            case 'clear_session_response':
               // No handler needed - just log success/failure
               if (!msg.payload.success) {
                  console.error('Failed to clear session:', msg.payload.error);
               }
               break;
            case 'load_conversation_response':
               DawnHistory.handleLoadResponse(msg.payload);
               break;
            case 'delete_conversation_response':
               DawnHistory.handleDeleteResponse(msg.payload);
               break;
            case 'rename_conversation_response':
               DawnHistory.handleRenameResponse(msg.payload);
               break;
            case 'search_conversations_response':
               DawnHistory.handleSearchResponse(msg.payload);
               break;
            case 'save_message_response':
               DawnHistory.handleSaveResponse(msg.payload);
               break;
            case 'continue_conversation_response':
               DawnHistory.handleContinueResponse(msg.payload);
               break;
            case 'set_private_response':
               DawnSettings.handleSetPrivateResponse(msg.payload);
               break;
            case 'set_pinned_response':
               DawnHistory.handleSetPinnedResponse(msg.payload);
               break;
            case 'export_conversation_response':
               DawnHistory.handleExportResponse(msg.payload);
               break;
            case 'reassign_conversation_response':
               DawnHistory.handleReassignResponse(msg.payload);
               break;
            // Memory management responses
            case 'get_memory_stats_response':
               DawnMemory.handleStatsResponse(msg.payload);
               break;
            case 'list_memory_facts_response':
               DawnMemory.handleFactsResponse(msg.payload);
               break;
            case 'list_memory_preferences_response':
               DawnMemory.handlePreferencesResponse(msg.payload);
               break;
            case 'list_memory_summaries_response':
               DawnMemory.handleSummariesResponse(msg.payload);
               break;
            case 'list_memory_entities_response':
               DawnMemory.handleEntitiesResponse(msg.payload);
               break;
            case 'search_memory_response':
               DawnMemory.handleSearchResponse(msg.payload);
               break;
            case 'delete_memory_fact_response':
               DawnMemory.handleDeleteFactResponse(msg.payload);
               break;
            case 'delete_memory_preference_response':
               DawnMemory.handleDeletePreferenceResponse(msg.payload);
               break;
            case 'delete_memory_summary_response':
               DawnMemory.handleDeleteSummaryResponse(msg.payload);
               break;
            case 'delete_memory_entity_response':
               DawnMemory.handleDeleteEntityResponse(msg.payload);
               break;
            case 'merge_memory_entities_response':
               DawnMemory.handleMergeEntityResponse(msg.payload);
               break;
            // Phase 1 entity-merge: alias + proposal responses
            case 'entity_aliases_response':
               DawnMemory.handleEntityAliasesResponse(msg.payload);
               break;
            case 'entity_merge_proposal_list_response':
               DawnMemory.handleEntityMergeProposalListResponse(msg.payload);
               break;
            case 'entity_link_response':
               DawnMemory.handleEntityLinkResponse(msg.payload);
               break;
            case 'entity_unlink_response':
               DawnMemory.handleEntityUnlinkResponse(msg.payload);
               break;
            case 'entity_proposal_resolve_response':
               DawnMemory.handleEntityProposalResolveResponse(msg.payload);
               break;
            case 'memory_proposals_changed':
               /* Server-pushed pending-proposal count.  Phase 2: lights up
                * the memory-icon dot when count > 0, clears when zero. */
               if (window.DawnMemoryAliases) {
                  DawnMemoryAliases.setProposalPendingCount(
                     msg.payload && typeof msg.payload.count === 'number' ? msg.payload.count : 0
                  );
               }
               break;
            case 'delete_all_memories_response':
               DawnMemory.handleDeleteAllResponse(msg.payload);
               break;
            case 'export_memories_response':
               DawnMemory.handleExportResponse(msg.payload);
               break;
            case 'import_memories_response':
               DawnMemory.handleImportResponse(msg.payload);
               break;
            case 'get_memory_fact_source_response':
               DawnMemory.handleFactSourceResponse(msg.payload);
               break;
            // Contacts management responses
            case 'contacts_list_response':
               if (typeof DawnContacts !== 'undefined')
                  DawnContacts.handleListResponse(msg.payload);
               break;
            case 'contacts_search_response':
               if (typeof DawnContacts !== 'undefined')
                  DawnContacts.handleSearchResponse(msg.payload);
               break;
            case 'contacts_add_response':
               if (typeof DawnContacts !== 'undefined') DawnContacts.handleAddResponse(msg.payload);
               break;
            case 'contacts_update_response':
               if (typeof DawnContacts !== 'undefined')
                  DawnContacts.handleUpdateResponse(msg.payload);
               break;
            case 'contacts_delete_response':
               if (typeof DawnContacts !== 'undefined')
                  DawnContacts.handleDeleteResponse(msg.payload);
               break;
            case 'contacts_search_entities_response':
               if (typeof DawnContacts !== 'undefined')
                  DawnContacts.handleSearchEntitiesResponse(msg.payload);
               break;
            case 'entity_set_photo_response':
               if (typeof DawnContacts !== 'undefined')
                  DawnContacts.handleSetPhotoResponse(msg.payload);
               break;
            case 'entity_ensure_response':
               if (typeof DawnContacts !== 'undefined')
                  DawnContacts.handleEnsureEntityResponse(msg.payload);
               break;
            // Document library (RAG) responses
            case 'doc_library_list_response':
               if (typeof DawnDocLibrary !== 'undefined')
                  DawnDocLibrary.handleListResponse(msg.payload);
               break;
            case 'doc_library_delete_response':
               if (typeof DawnDocLibrary !== 'undefined')
                  DawnDocLibrary.handleDeleteResponse(msg.payload);
               break;
            case 'doc_library_index_response':
               if (typeof DawnDocLibrary !== 'undefined')
                  DawnDocLibrary.handleIndexResponse(msg.payload);
               break;
            case 'doc_library_toggle_global_response':
               if (typeof DawnDocLibrary !== 'undefined')
                  DawnDocLibrary.handleToggleGlobalResponse(msg.payload);
               break;
            case 'doc_library_note_save_response':
               if (typeof DawnDocLibrary !== 'undefined')
                  DawnDocLibrary.handleNoteSaveResponse(msg.payload);
               break;
            case 'doc_library_note_update_response':
               if (typeof DawnDocLibrary !== 'undefined')
                  DawnDocLibrary.handleNoteUpdateResponse(msg.payload);
               break;
            case 'doc_library_version_list_response':
               if (typeof DawnDocLibrary !== 'undefined')
                  DawnDocLibrary.handleVersionListResponse(msg.payload);
               break;
            case 'doc_library_version_restore_response':
               if (typeof DawnDocLibrary !== 'undefined')
                  DawnDocLibrary.handleVersionRestoreResponse(msg.payload);
               break;
            case 'doc_library_deleted_list_response':
               if (typeof DawnDocLibrary !== 'undefined')
                  DawnDocLibrary.handleDeletedListResponse(msg.payload);
               break;
            // Code projects (coding harness)
            case 'code_projects_list_response':
               if (typeof DawnCodeProjects !== 'undefined')
                  DawnCodeProjects.handleListResponse(msg.payload);
               break;
            case 'code_projects_import_response':
               if (typeof DawnCodeProjects !== 'undefined')
                  DawnCodeProjects.handleImportResponse(msg.payload);
               break;
            case 'code_projects_link_response':
               if (typeof DawnCodeProjects !== 'undefined')
                  DawnCodeProjects.handleLinkResponse(msg.payload);
               break;
            case 'code_projects_action_response':
               if (typeof DawnCodeProjects !== 'undefined')
                  DawnCodeProjects.handleActionResponse(msg.payload);
               break;
            case 'code_project_status_changed':
               if (typeof DawnCodeProjects !== 'undefined')
                  DawnCodeProjects.handleStatusChanged(msg.payload);
               break;
            case 'code_project_import_failed':
               if (typeof DawnCodeProjects !== 'undefined')
                  DawnCodeProjects.handleImportFailed(msg.payload);
               break;
            // Calendar account management
            case 'calendar_list_accounts_response':
               if (typeof DawnCalendarAccounts !== 'undefined')
                  DawnCalendarAccounts.handleListAccountsResponse(msg.payload);
               break;
            case 'calendar_add_account_response':
               if (typeof DawnCalendarAccounts !== 'undefined')
                  DawnCalendarAccounts.handleAddAccountResponse(msg.payload);
               break;
            case 'calendar_edit_account_response':
               if (typeof DawnCalendarAccounts !== 'undefined')
                  DawnCalendarAccounts.handleEditAccountResponse(msg.payload);
               break;
            case 'calendar_remove_account_response':
               if (typeof DawnCalendarAccounts !== 'undefined')
                  DawnCalendarAccounts.handleRemoveAccountResponse(msg.payload);
               break;
            case 'calendar_test_account_response':
               if (typeof DawnCalendarAccounts !== 'undefined')
                  DawnCalendarAccounts.handleTestAccountResponse(msg.payload);
               break;
            case 'calendar_sync_account_response':
               if (typeof DawnCalendarAccounts !== 'undefined')
                  DawnCalendarAccounts.handleSyncAccountResponse(msg.payload);
               break;
            case 'calendar_list_calendars_response':
               if (typeof DawnCalendarAccounts !== 'undefined')
                  DawnCalendarAccounts.handleListCalendarsResponse(msg.payload);
               break;
            case 'calendar_toggle_calendar_response':
               if (typeof DawnCalendarAccounts !== 'undefined')
                  DawnCalendarAccounts.handleToggleCalendarResponse(msg.payload);
               break;
            case 'calendar_toggle_read_only_response':
               if (typeof DawnCalendarAccounts !== 'undefined')
                  DawnCalendarAccounts.handleToggleReadOnlyResponse(msg.payload);
               break;
            case 'calendar_set_enabled_response':
               if (typeof DawnCalendarAccounts !== 'undefined')
                  DawnCalendarAccounts.handleSetEnabledResponse(msg.payload);
               break;
            // OAuth
            case 'oauth_get_auth_url_response':
               if (typeof DawnOAuth !== 'undefined') DawnOAuth.handleAuthUrlResponse(msg.payload);
               break;
            case 'oauth_exchange_code_response':
               if (typeof DawnOAuth !== 'undefined')
                  DawnOAuth.handleExchangeCodeResponse(msg.payload);
               if (typeof DawnCalendarAccounts !== 'undefined')
                  DawnCalendarAccounts.handleOAuthExchangeResponse(msg.payload);
               if (typeof DawnEmailAccounts !== 'undefined')
                  DawnEmailAccounts.handleOAuthExchangeResponse(msg.payload);
               break;
            case 'oauth_disconnect_response':
               if (typeof DawnCalendarAccounts !== 'undefined')
                  DawnCalendarAccounts.handleOAuthDisconnectResponse(msg.payload);
               if (
                  typeof DawnEmailAccounts !== 'undefined' &&
                  DawnEmailAccounts.handleOAuthDisconnectResponse
               )
                  DawnEmailAccounts.handleOAuthDisconnectResponse(msg.payload);
               break;
            case 'oauth_check_scopes_response':
               if (typeof DawnOAuth !== 'undefined')
                  DawnOAuth.handleCheckScopesResponse(msg.payload);
               break;
            // Email account management
            case 'email_list_accounts_response':
               if (typeof DawnEmailAccounts !== 'undefined')
                  DawnEmailAccounts.handleListAccountsResponse(msg.payload);
               break;
            case 'email_add_account_response':
               if (typeof DawnEmailAccounts !== 'undefined')
                  DawnEmailAccounts.handleAddAccountResponse(msg.payload);
               break;
            case 'email_update_account_response':
               if (typeof DawnEmailAccounts !== 'undefined')
                  DawnEmailAccounts.handleUpdateAccountResponse(msg.payload);
               break;
            case 'email_remove_account_response':
               if (typeof DawnEmailAccounts !== 'undefined')
                  DawnEmailAccounts.handleRemoveAccountResponse(msg.payload);
               break;
            case 'email_test_connection_response':
               if (typeof DawnEmailAccounts !== 'undefined')
                  DawnEmailAccounts.handleTestConnectionResponse(msg.payload);
               break;
            case 'email_set_read_only_response':
               if (typeof DawnEmailAccounts !== 'undefined')
                  DawnEmailAccounts.handleSetReadOnlyResponse(msg.payload);
               break;
            case 'email_set_enabled_response':
               if (typeof DawnEmailAccounts !== 'undefined')
                  DawnEmailAccounts.handleSetEnabledResponse(msg.payload);
               break;
            // Watches (SAGE proactive attention) management
            case 'watch_list_response':
               if (typeof DawnWatches !== 'undefined') DawnWatches.handleListResponse(msg.payload);
               break;
            case 'watch_add_response':
               if (typeof DawnWatches !== 'undefined') DawnWatches.handleAddResponse(msg.payload);
               break;
            case 'watch_update_response':
               if (typeof DawnWatches !== 'undefined')
                  DawnWatches.handleUpdateResponse(msg.payload);
               break;
            case 'watch_set_enabled_response':
               if (typeof DawnWatches !== 'undefined')
                  DawnWatches.handleSetEnabledResponse(msg.payload);
               break;
            case 'watch_remove_response':
               if (typeof DawnWatches !== 'undefined')
                  DawnWatches.handleRemoveResponse(msg.payload);
               break;
            case 'conversation_reset':
               // Tool triggered conversation reset - sync frontend
               console.log('Conversation reset by tool');
               // Start new chat (saves current conversation and clears transcript)
               DawnHistory.startNewChat();
               DawnToast.show('Conversation reset', 'info');
               break;
            // Music streaming messages
            case 'music_state':
            case 'music_position':
            case 'music_error':
            case 'music_search_response':
            case 'music_library_response':
            case 'music_queue_response':
               if (typeof DawnMusicPlayback !== 'undefined') {
                  DawnMusicPlayback.handleJsonMessage(msg);
               }
               break;
            case 'scheduler_notification':
               if (typeof DawnScheduler !== 'undefined') {
                  DawnScheduler.handleNotification(msg.payload);
               }
               break;
            case 'attention_alert': {
               // SAGE proactive-attention banner. Its own channel (not the
               // scheduler's) so it shows an ATTENTION badge and never chimes.
               const ap = msg.payload || {};
               if (typeof DawnToast !== 'undefined' && ap.summary) {
                  // 'alert' is accompanied by server-side voice (urgent → assertive,
                  // longer dwell); 'ambient' is a silent, non-interrupting banner
                  // (polite live-region — the a11y equivalent of not chiming).
                  const isAlert = ap.level !== 'ambient';
                  DawnToast.show(ap.summary, 'attention', isAlert ? 10000 : 6000, {
                     badge: 'ATTENTION',
                     assertive: isAlert,
                  });
               }
               break;
            }
            case 'job_notification': {
               // Background-job completion: a silent in-browser toast (no voice).
               const jp = msg.payload || {};
               if (typeof DawnToast !== 'undefined' && jp.text) {
                  DawnToast.show(jp.text, 'info', 8000, { badge: 'JOB' });
               }
               break;
            }
            case 'job_update':
               // One job's lifecycle transition → upsert into the active set.
               if (typeof DawnJobsActivity !== 'undefined' && msg.payload) {
                  DawnJobsActivity.upsertJob(msg.payload.job);
               }
               break;
            case 'jobs_snapshot':
               // Connect/reconnect rehydration of the complete active set.
               if (typeof DawnJobsActivity !== 'undefined' && msg.payload) {
                  DawnJobsActivity.applySnapshot(msg.payload.jobs, msg.payload.truncated);
               }
               break;
            case 'list_jobs_response':
               // A page of terminal jobs — the jobs panel's history feed (CP4).
               if (typeof DawnJobs !== 'undefined' && DawnJobs.handleHistoryPage) {
                  DawnJobs.handleHistoryPage(msg.payload);
               }
               break;
            case 'phone_call_notification':
               if (typeof DawnPhone !== 'undefined') {
                  DawnPhone.handleNotification(msg.payload);
               }
               break;
            case 'scheduler_events_response':
               if (window.DawnSchedulerQueue) {
                  DawnSchedulerQueue.handleListResponse(msg.payload);
               }
               break;
            case 'scheduler_events_changed':
               if (window.DawnSchedulerQueue) {
                  DawnSchedulerQueue.refresh();
               }
               break;
            case 'memory_extraction_notice':
               if (msg.payload) {
                  showMemoryExtractionNotice(msg.payload.level, msg.payload.message);
               }
               break;
            case 'plan_progress':
               if (typeof DawnPlanOrchestrator !== 'undefined') {
                  DawnPlanOrchestrator.handlePlanProgress(msg.payload);
               }
               break;
            case 'conversation_renamed':
               DawnHistory.handleConversationRenamed(msg.payload);
               break;
            case 'conversation_messages_appended':
               DawnHistory.handleConversationMessagesAppended(msg.payload);
               break;
            case 'lock_conversation_llm_response':
               // Background ack for per-conversation LLM settings lock. Silent on
               // success; surface only genuine failures (success === false).
               if (msg.payload && msg.payload.success === false) {
                  console.warn(
                     'Failed to lock conversation LLM settings:',
                     msg.payload.error || 'unknown error'
                  );
               }
               break;
            case 'always_on_state':
            case 'wake_detected':
            case 'recording_end':
            case 'always_on_error':
               if (typeof DawnAlwaysOn !== 'undefined') {
                  DawnAlwaysOn.handleMessage(msg);
               }
               break;
            default:
               console.log('Unknown message type:', msg.type);
         }
      } catch (e) {
         console.error('Failed to parse message:', e, data);
      }
   }

   function handleBinaryMessage(data) {
      try {
         if (data.byteLength < 1) {
            console.warn('Empty binary message');
            return;
         }

         const bytes = new Uint8Array(data);
         const msgType = bytes[0];
         console.log('Binary message: type=0x' + msgType.toString(16) + ', len=' + bytes.length);

         switch (msgType) {
            case DawnConfig.WS_BIN_AUDIO_OUT:
               // TTS audio chunk - accumulate until segment end
               // Skip if TTS was disabled (race: server may still be sending)
               if (!DawnTts.isEnabled()) {
                  break;
               }
               if (bytes.length > 1) {
                  const payload = bytes.slice(1);
                  // Lock codec on first chunk of segment — prevents race where
                  // Opus loads mid-segment and we decode raw PCM as Opus
                  if (segmentIsOpus === null) {
                     segmentIsOpus = !!(opusReady && opusWorker);
                  }
                  // Store raw data (Opus or PCM) until segment end
                  // Bound array to prevent memory exhaustion on protocol errors
                  const MAX_PENDING_CHUNKS = 100;
                  if (pendingOpusData.length >= MAX_PENDING_CHUNKS) {
                     console.warn('Audio buffer overflow, dropping oldest chunk');
                     pendingOpusData.shift();
                  }
                  pendingOpusData.push(payload);
               }
               break;

            case DawnConfig.WS_BIN_AUDIO_SEGMENT_END:
               // End of TTS audio segment - decode and play accumulated data
               // Skip if TTS was disabled (also clear any pending data)
               if (!DawnTts.isEnabled()) {
                  pendingOpusData = [];
                  break;
               }
               if (pendingOpusData.length > 0) {
                  // Concatenate all pending data
                  const totalLen = pendingOpusData.reduce((sum, chunk) => sum + chunk.length, 0);
                  const combined = new Uint8Array(totalLen);
                  let offset = 0;
                  for (const chunk of pendingOpusData) {
                     combined.set(chunk, offset);
                     offset += chunk.length;
                  }
                  pendingOpusData = [];

                  // Server includes codec flag in segment end payload:
                  // payload[0] = 1 for Opus, 0 for PCM (or absent for legacy)
                  const payload = bytes.slice(1);
                  const serverSaysOpus = payload.length > 0 ? payload[0] === 1 : segmentIsOpus;

                  if (serverSaysOpus && opusReady && opusWorker) {
                     // Decode complete Opus stream via worker
                     pendingDecodePlayback = true;
                     opusWorker.postMessage({ type: 'decode', data: combined }, [combined.buffer]);
                  } else {
                     // Raw PCM: 16-bit signed, 48kHz, mono
                     DawnAudioPlayback.queueAudio(combined);
                     DawnAudioPlayback.play();
                  }
                  segmentIsOpus = null;
               }
               break;

            case DawnConfig.WS_BIN_MUSIC_DATA:
            case DawnConfig.WS_BIN_MUSIC_SEGMENT_END:
               // Music streaming messages - route to music playback handler
               if (typeof DawnMusicPlayback !== 'undefined') {
                  DawnMusicPlayback.handleBinaryMessage(data);
               }
               break;

            default:
               console.log('Unknown binary message type:', '0x' + msgType.toString(16));
         }
      } catch (e) {
         console.error('Error handling binary message:', e);
      }
   }

   /**
    * Show a transient notification banner for memory extraction events
    */
   function showMemoryExtractionNotice(level, message) {
      const existing = document.querySelector('.memory-notice');
      if (existing) existing.remove();
      const banner = document.createElement('div');
      banner.className = 'memory-notice memory-notice--' + (level || 'info');
      banner.textContent = message;
      document.body.appendChild(banner);
      setTimeout(() => {
         banner.classList.add('memory-notice--fade');
         setTimeout(() => banner.remove(), 500);
      }, 8000);
   }

   /**
    * Update action button visual state for PTT recording
    */
   function updateMicButton(recording) {
      DawnState.setIsRecording(recording);
      if (typeof DawnAlwaysOn !== 'undefined') {
         DawnAlwaysOn.resolveButtonState();
      }
   }

   function sendTextMessage(text) {
      if (!DawnWS.isConnected()) {
         console.error('WebSocket not connected');
         return false;
      }

      // Prepend any attached document content
      let messageText = text;
      if (typeof DawnDocuments !== 'undefined') {
         const docs = DawnDocuments.getAndClearDocuments();
         if (docs.length > 0) {
            const docText = docs
               .map((d) => {
                  // v68: link the stored original file so a reloaded chip can
                  // offer the real PDF/DOCX (older messages just omit it).
                  // Marker "blob:<id>]" format is mirrored by the parser
                  // (documents.js DOC_MARKER_RE) and the orphan-sweep SQL; kept in
                  // sync by scripts/check_blob_marker_sync.sh — change all together.
                  const blobSuffix = d.original_blob_id ? ` blob:${d.original_blob_id}` : '';
                  return `[ATTACHED DOCUMENT: ${d.filename} (${d.size} bytes)${blobSuffix}]\n${d.content}\n[END DOCUMENT]`;
               })
               .join('\n\n');
            messageText = docText + '\n\n' + messageText;
         }
      }

      // Fresh chat: create the conversation row FIRST (ordered before this text on
      // the wire) so the server tags this turn's frames with the real conversation
      // id, not 0 — otherwise a backgrounded turn can't be routed and its output
      // bleeds into whatever conversation is on screen.
      if (
         typeof DawnHistory !== 'undefined' &&
         DawnHistory.beginConversationBeforeSend &&
         !DawnHistory.getActiveConversationId()
      ) {
         DawnHistory.beginConversationBeforeSend(text);
      }

      const msg = {
         type: 'text',
         payload: { text: messageText },
      };

      // Tell the server which conversation this message belongs to, so it tags
      // this turn's frames explicitly instead of inferring from the live view
      // (robust across reconnect/multi-tab). Null for a fresh chat — the
      // pre-create above establishes it by wire ordering instead.
      const activeConvId =
         typeof DawnHistory !== 'undefined' && DawnHistory.getActiveConversationId
            ? DawnHistory.getActiveConversationId()
            : null;
      if (activeConvId) {
         msg.payload.conversation_id = activeConvId;
      }

      // Add vision images if pending (supports multiple)
      const pendingImages = DawnVision.getPendingImages();
      if (pendingImages.length > 0) {
         msg.payload.images = pendingImages.map((img) => ({
            data: img.data,
            mime_type: img.mimeType,
         }));
         // Save image IDs for history storage (before clearing)
         // Images are now stored server-side and referenced by ID
         pendingThumbnailsForSave = DawnVision.getPendingImageIds
            ? DawnVision.getPendingImageIds()
            : [];
         DawnVision.clearImages(); // Clear after adding to message
      } else {
         pendingThumbnailsForSave = [];
      }

      DawnWS.send(msg);
      // Note: Server echoes user message back as transcript, so no local entry needed
      // State update also comes from server
      return true;
   }

   // =============================================================================
   // UI Updates
   // =============================================================================
   function updateConnectionStatus(status, reason) {
      // A new (re)connect: allow the next 'session' message to restore the active
      // conversation once (the duplicate-restore guard in the session handler).
      if (status !== 'connected') {
         restoredConvIdThisConnection = null;
         // Background turns are aborted server-side on disconnect — drop stale dots.
         if (typeof DawnHistory !== 'undefined' && DawnHistory.clearGenerating) {
            DawnHistory.clearGenerating();
         }
      }
      DawnElements.connectionStatus.className = status;
      DawnElements.connectionStatus.title = ''; // Clear any previous tooltip
      if (status === 'connected') {
         DawnElements.connectionStatus.textContent = 'Connected';
         // Debug mode persists across refreshes; re-request the System Prompt block on
         // (re)connect so it reappears when debug is restored on.
         if (typeof DawnState !== 'undefined' && DawnState.getDebugMode()) {
            DawnWS.send({ type: 'get_system_prompt' });
         }
         if (typeof DawnSatellites !== 'undefined') {
            DawnSatellites.handleReconnect();
         }
         if (typeof DawnMessaging !== 'undefined') {
            DawnMessaging.handleReconnect();
         }
         if (typeof DawnAlwaysOn !== 'undefined') {
            DawnAlwaysOn.handleReconnect();
         }
         // NOTE: DawnPhone rehydration is requested from the 'session' handler,
         // not here — 'connected' fires before the reconnect handshake, so a
         // request sent now would be answered on the throwaway session that the
         // reconnect then destroys (the reply would be lost).
      } else if (status === 'connecting') {
         DawnElements.connectionStatus.textContent = 'Connecting...';
      } else {
         // Show disconnect reason if available (truncate for display)
         DawnElements.connectionStatus.textContent = reason
            ? 'Disconnected: ' + (reason.length > 30 ? reason.substring(0, 30) + '...' : reason)
            : 'Disconnected';
         // Tear down stale phone call UI — a persistent "On call" pill must not
         // outlive the connection that authenticates its state.
         if (typeof DawnPhone !== 'undefined') {
            DawnPhone.handleDisconnect();
         }
         // Clear the composer working-indicator for the same reason: a socket drop
         // mid-tool-loop would otherwise leave a lit "Using Tools" reactor pinned to
         // the composer until a fresh state arrives on reconnect.
         updateComposerBusy('idle');
      }
   }

   function updateState(state, detail, tools) {
      const previousState = DawnState.getAppState();

      // If server sends "idle" but audio is still playing, defer the transition
      // This prevents the jarring "IDLE" status while TTS is still speaking
      if (state === 'idle' && DawnAudioPlayback.isPlaying()) {
         pendingIdleState = true;
         console.log('Server sent idle but audio still playing, deferring state change');
         // Keep showing "speaking" until audio finishes
         state = 'speaking';
         detail = null;
      } else if (state !== 'idle') {
         // Clear pending idle if we get a non-idle state
         pendingIdleState = false;
      }

      DawnState.setAppState(state);

      // Update status indicator
      DawnElements.statusDot.className = state;

      // Show state with optional detail (e.g., "THINKING · Fetching URL...")
      if (detail) {
         DawnElements.statusText.textContent = state.toUpperCase() + ' · ' + detail;
      } else {
         DawnElements.statusText.textContent = state.toUpperCase();
      }

      // Update tools tray (expanded visualizer)
      updateToolsTray(tools);

      // Sync mini status bar (when visualizer collapsed)
      if (DawnElements.miniStatusDot) {
         DawnElements.miniStatusDot.className = 'status-dot ' + state;
      }
      if (DawnElements.miniStatusText) {
         // For mini bar: show tool count or tool names depending on count
         const miniText = formatMiniStatusText(state, detail, tools);
         DawnElements.miniStatusText.innerHTML = miniText;
      }

      // Sync the bottom composer "working" indicator (always-visible arc reactor).
      updateComposerBusy(state, tools);

      // Update ring container - preserve fft-active class if present
      const hasFftActive = DawnElements.ringContainer.classList.contains('fft-active');
      DawnElements.ringContainer.classList.remove(previousState);
      DawnElements.ringContainer.classList.add(state);
      // Re-add fft-active if it was there (in case it got removed)
      if (hasFftActive) {
         DawnElements.ringContainer.classList.add('fft-active');
      }

      // Update unified action button state (cancel/send/mic/listen)
      if (typeof DawnAlwaysOn !== 'undefined') {
         DawnAlwaysOn.resolveButtonState();
      }

      // Emit state event for decoupled modules
      if (typeof DawnEvents !== 'undefined') {
         DawnEvents.emit('state', { state, previousState, detail, tools });
      }
   }

   /**
    * Show/hide the bottom composer "working" indicator (arc reactor) and set its
    * label. Mirrors the codebase busy set (thinking | processing | speaking); any
    * other state (idle/listening/recording/error) hides it. When tools are actively
    * executing the label becomes "Using Tools" (takes precedence over the generic
    * busy word). Shares updateState()'s single source of truth so it can't drift
    * from the top/mini status bars.
    * @param {string} state - Current app state string.
    * @param {Array} [tools] - Active tool descriptors ({name,...}); non-empty while
    *                          the tool loop is running.
    */
   function updateComposerBusy(state, tools) {
      const el = DawnElements.composerBusy;
      if (!el) return;

      const LABELS = { thinking: 'Thinking', processing: 'Working', speaking: 'Speaking' };
      let label = LABELS[state];
      // Tool execution is the more informative signal — surface it over "Working".
      if (label && tools && tools.length > 0) {
         label = 'Using Tools';
      }

      if (label) {
         const labelEl = DawnElements.composerBusyLabel;
         // Guard so aria-live doesn't re-announce on unrelated state churn.
         if (labelEl && labelEl.textContent !== label) {
            labelEl.textContent = label;
         }
         el.classList.add('is-visible');
         el.setAttribute('aria-hidden', 'false');
      } else {
         el.classList.remove('is-visible');
         el.setAttribute('aria-hidden', 'true');
      }
   }

   /**
    * Format mini status bar text with tools indicator
    */
   function formatMiniStatusText(state, detail, tools) {
      const stateUpper = state.toUpperCase();

      if (!tools || tools.length === 0) {
         // No tools - show detail if present
         return detail ? `${stateUpper} · ${DawnFormat.escapeHtml(detail)}` : stateUpper;
      }

      if (tools.length === 1) {
         // Single tool - show name (escaped to prevent XSS)
         return `${stateUpper} · ${DawnFormat.escapeHtml(tools[0].name)}`;
      }

      if (tools.length === 2) {
         // Two tools - show both names (escaped to prevent XSS)
         return `${stateUpper} · ${DawnFormat.escapeHtml(tools[0].name)}, ${DawnFormat.escapeHtml(tools[1].name)}`;
      }

      // 3+ tools - show animated progress dots with ARIA label for accessibility
      // Use CSS custom property for delay so prefers-reduced-motion can override
      const dots = tools
         .map((_, i) => `<span class="mini-tool-dot" style="--delay-index: ${i}"></span>`)
         .join('');
      const toolNames = tools.map((t) => t.name).join(', ');
      return `${stateUpper} <span class="mini-tool-dots" role="status" aria-label="${tools.length} tools running: ${DawnFormat.escapeHtml(toolNames)}">${dots}</span>`;
   }

   /**
    * Update the tools tray in expanded visualizer
    */
   function updateToolsTray(tools) {
      // Get or create tools tray container
      let tray = document.getElementById('tools-tray');
      if (!tray) {
         // Create tray after status text
         const status = document.getElementById('status');
         if (status) {
            tray = document.createElement('div');
            tray.id = 'tools-tray';
            tray.className = 'tools-tray';
            tray.setAttribute('role', 'status');
            tray.setAttribute('aria-live', 'polite');
            tray.setAttribute('aria-label', 'Active tools');
            status.appendChild(tray);
         }
      }

      if (!tray) return;

      // Clear if no tools
      if (!tools || tools.length === 0) {
         tray.innerHTML = '';
         tray.classList.remove('visible');
         return;
      }

      // Build pills for each tool (escaped to prevent XSS)
      // Use CSS custom property for delay so prefers-reduced-motion can override
      const pills = tools
         .map((tool, index) => {
            const statusClass = tool.status === 'complete' ? 'complete' : 'running';
            const icon = tool.status === 'complete' ? '✓' : '';
            const safeName = DawnFormat.escapeHtml(tool.name);
            return `<span class="tool-pill ${statusClass}" style="--delay-index: ${index}">
        ${safeName}${icon ? `<span class="tool-pill-icon" aria-hidden="true">${icon}</span>` : ''}
      </span>`;
         })
         .join('');

      tray.innerHTML = pills;
      tray.classList.add('visible');
   }

   // =============================================================================
   // Event Handlers
   // =============================================================================
   // Composer recall: ArrowUp/ArrowDown cycle the CURRENT conversation's user
   // turns, read live from the transcript DOM — the single client-side source of
   // truth (there is no in-memory message model).  Switching conversations
   // rebuilds that DOM, so recall always matches the conversation on screen; no
   // separate/persisted history is kept.  historyNavIndex === null means "not
   // navigating" (the composer holds the user's live draft); recallSnapshot is
   // the user-turn list captured at the start of a navigation session and held
   // stable while cycling.
   let recallSnapshot = [];
   let historyNavIndex = null;

   // Ordered (oldest→newest) user-turn texts of the current conversation, read
   // from the transcript DOM.  Uses the un-rendered `data-raw-text` source and
   // skips image-only / document-only turns (empty raw text).
   // ASSUMPTION: the full conversation is rendered in the DOM.  If transcript
   // rendering ever windows/virtualizes long conversations, recall would silently
   // miss the un-rendered older turns — revisit this reader if that lands.
   function getConversationUserMessages() {
      const transcript = DawnElements.transcript || document.getElementById('transcript');
      if (!transcript) return [];
      const out = [];
      transcript.querySelectorAll('.transcript-entry.user').forEach((entry) => {
         let combined = '';
         entry.querySelectorAll('.text').forEach((t) => {
            const raw = t.getAttribute('data-raw-text') || '';
            if (raw) combined += (combined ? '\n' : '') + raw;
         });
         combined = combined.trim();
         if (combined) out.push(combined);
      });
      return out;
   }

   // Auto-grow the composer to fit its content.  Height only — shared by the
   // input listener and history recall; callers handle text-override/focus/caret.
   function resizeComposer(el) {
      el.style.height = 'auto';
      el.style.height = Math.min(el.scrollHeight, 150) + 'px';
      // Only show scrollbar when content exceeds max height
      el.style.overflowY = el.scrollHeight > 150 ? 'auto' : 'hidden';
   }

   // Place a recalled entry into the composer: fill, resize, focus, caret-to-end,
   // and sync the voice-mode "Send" override.  Programmatic .value assignment does
   // NOT fire an 'input' event, so this never resets historyNavIndex itself.
   function setComposerValue(text) {
      const input = DawnElements.textInput;
      input.value = text;
      resizeComposer(input);
      input.focus();
      input.setSelectionRange(input.value.length, input.value.length);
      if (typeof DawnAlwaysOn !== 'undefined') {
         DawnAlwaysOn.setTextOverride(input.value.trim().length > 0);
      }
   }

   function handleSend() {
      const text = DawnElements.textInput.value.trim();
      if (text) {
         if (sendTextMessage(text)) {
            // Exit recall navigation; the sent turn re-enters the list via the
            // server's transcript echo, so the next ArrowUp re-snapshots it.
            historyNavIndex = null;
            DawnElements.textInput.value = '';
            DawnElements.textInput.style.height = 'auto';
            // Reset text override after sending
            if (typeof DawnAlwaysOn !== 'undefined') {
               DawnAlwaysOn.setTextOverride(false);
            }
         }
      }
   }

   function handleKeydown(event) {
      // Enter sends — but never mid-IME-composition (CJK etc.).
      if (event.key === 'Enter' && !event.shiftKey && !event.isComposing) {
         event.preventDefault();
         handleSend();
         return;
      }

      const plainKey =
         !event.shiftKey && !event.ctrlKey && !event.metaKey && !event.altKey && !event.isComposing;

      // ArrowUp walks OLDER through this conversation's user turns.  Navigation
      // begins only from an empty composer (so it never fights caret movement in
      // a live multi-line draft) and snapshots the turns at that moment; once
      // navigating, each ArrowUp steps further back through the snapshot.
      if (event.key === 'ArrowUp' && plainKey) {
         const input = DawnElements.textInput;
         if (historyNavIndex === null) {
            if (input.value === '') {
               recallSnapshot = getConversationUserMessages();
               if (recallSnapshot.length > 0) {
                  event.preventDefault();
                  historyNavIndex = recallSnapshot.length - 1;
                  setComposerValue(recallSnapshot[historyNavIndex]);
               }
            }
         } else if (historyNavIndex > 0) {
            event.preventDefault();
            historyNavIndex--;
            setComposerValue(recallSnapshot[historyNavIndex]);
         } else {
            // Already at the oldest — swallow so the caret doesn't jump.
            event.preventDefault();
         }
         return;
      }

      // ArrowDown walks NEWER while navigating; stepping past the newest clears
      // the composer and exits navigation.
      if (event.key === 'ArrowDown' && plainKey && historyNavIndex !== null) {
         event.preventDefault();
         if (historyNavIndex < recallSnapshot.length - 1) {
            historyNavIndex++;
            setComposerValue(recallSnapshot[historyNavIndex]);
         } else {
            historyNavIndex = null;
            recallSnapshot = [];
            const input = DawnElements.textInput;
            input.value = '';
            resizeComposer(input);
            if (typeof DawnAlwaysOn !== 'undefined') {
               DawnAlwaysOn.setTextOverride(false);
            }
         }
         return;
      }
   }

   function handleCancel() {
      console.log('Cancel button clicked - sending cancel');
      DawnWS.send({ type: 'cancel' });
      // Finalize any active stream immediately on the client side
      DawnStreaming.finalize();
   }

   // =============================================================================
   // Opus Worker Initialization
   // =============================================================================

   /**
    * Initialize the Opus Web Worker for audio encoding/decoding
    */
   function initOpusWorker() {
      try {
         opusWorker = new Worker('js/opus-worker.js');

         opusWorker.onmessage = function (e) {
            const msg = e.data;

            switch (msg.type) {
               case 'ready':
                  console.log('Opus worker: WASM loaded');
                  // Initialize encoder/decoder
                  opusWorker.postMessage({ type: 'init' });
                  break;

               case 'init_done':
                  if (msg.webcodecs) {
                     console.log('Opus worker: WebCodecs initialized successfully');
                     opusReady = true;
                     // If already connected, send capability update to enable Opus
                     if (DawnWS.isConnected()) {
                        console.log('Sending Opus capability update');
                        DawnWS.send({
                           type: 'capabilities_update',
                           payload: {
                              capabilities: { audio_codecs: ['opus', 'pcm'] },
                           },
                        });
                     }
                  } else {
                     console.log('Opus worker: WebCodecs not available, using PCM only');
                     opusReady = false;
                  }
                  break;

               case 'encoded':
                  // Send encoded Opus data to server
                  if (msg.data && msg.data.length > 0) {
                     sendOpusData(msg.data);
                  }
                  break;

               case 'decoded':
                  // Queue decoded PCM for playback
                  if (msg.data && msg.data.length > 0) {
                     queueDecodedAudio(msg.data);
                  }
                  break;

               case 'error':
                  console.error('Opus worker error:', msg.error);
                  break;
            }
         };

         opusWorker.onerror = function (e) {
            console.error('Opus worker failed:', e.message);
            opusWorker = null;
            opusReady = false;
         };
      } catch (e) {
         console.warn('Failed to create Opus worker:', e);
         opusWorker = null;
         opusReady = false;
      }

      // Cleanup worker on page unload to prevent memory leaks
      window.addEventListener('beforeunload', function () {
         if (opusWorker) {
            opusWorker.terminate();
            opusWorker = null;
         }
      });
   }

   /**
    * Send encoded Opus data to server
    */
   function sendOpusData(opusData) {
      if (!DawnWS.isConnected()) {
         return;
      }

      // Create message: [type byte][Opus data]
      const payload = new Uint8Array(1 + opusData.length);
      payload[0] = DawnConfig.WS_BIN_AUDIO_IN;
      payload.set(opusData, 1);

      DawnWS.sendBinary(payload.buffer);
   }

   /**
    * Queue decoded audio samples for playback
    */
   function queueDecodedAudio(pcmData) {
      // Skip queueing if TTS was disabled (race condition: server may still be sending)
      if (!DawnTts.isEnabled()) {
         return;
      }

      // Convert Int16 to Uint8 for existing playback pipeline
      const bytes = new Uint8Array(pcmData.buffer, pcmData.byteOffset, pcmData.byteLength);
      DawnAudioPlayback.queueAudio(bytes);

      // If we were waiting for decode to complete, trigger playback now
      if (pendingDecodePlayback) {
         pendingDecodePlayback = false;
         DawnAudioPlayback.play();
      }
   }

   // =============================================================================
   // Visualizer Collapse Toggle
   // =============================================================================
   function toggleVisualizerCollapse() {
      visualizerCollapsed = !visualizerCollapsed;

      if (visualizerCollapsed) {
         DawnElements.visualizer.classList.add('collapsed');
         DawnElements.visualizerMini.classList.remove('hidden');
         if (DawnElements.visualizerCollapseToggle) {
            DawnElements.visualizerCollapseToggle.setAttribute('aria-expanded', 'false');
         }
         if (DawnElements.visualizerMini) {
            DawnElements.visualizerMini.setAttribute('aria-expanded', 'false');
         }
      } else {
         DawnElements.visualizer.classList.remove('collapsed');
         DawnElements.visualizerMini.classList.add('hidden');
         if (DawnElements.visualizerCollapseToggle) {
            DawnElements.visualizerCollapseToggle.setAttribute('aria-expanded', 'true');
         }
         if (DawnElements.visualizerMini) {
            DawnElements.visualizerMini.setAttribute('aria-expanded', 'true');
         }
      }

      // Persist state
      localStorage.setItem('dawn_visualizer_collapsed', visualizerCollapsed ? 'true' : 'false');
   }

   // =============================================================================
   // LLM Controls Collapse Toggle
   // =============================================================================
   let llmControlsCollapsed = false;

   function updateLlmMiniSummary() {
      const modeEl = document.getElementById('llm-mini-mode');
      const modelEl = document.getElementById('llm-mini-model');
      if (!modeEl || !modelEl) return;

      const typeSelect = document.getElementById('llm-type-select');
      const modelSelect = document.getElementById('llm-model-select');

      modeEl.textContent = typeSelect
         ? typeSelect.options[typeSelect.selectedIndex]?.text || 'Local'
         : 'Local';
      modelEl.textContent = modelSelect
         ? modelSelect.options[modelSelect.selectedIndex]?.text || ''
         : '';
   }

   function toggleLlmControlsCollapse() {
      llmControlsCollapsed = !llmControlsCollapsed;
      const grid = document.getElementById('llm-controls-grid');
      const mini = document.getElementById('llm-controls-mini');
      if (!grid || !mini) return;

      const collapseToggle = document.getElementById('llm-controls-collapse');

      if (llmControlsCollapsed) {
         updateLlmMiniSummary();
         grid.classList.add('collapsed');
         mini.classList.remove('hidden');
         mini.setAttribute('aria-expanded', 'false');
         if (collapseToggle) collapseToggle.setAttribute('aria-expanded', 'false');
      } else {
         grid.classList.remove('collapsed');
         mini.classList.add('hidden');
         mini.setAttribute('aria-expanded', 'true');
         if (collapseToggle) collapseToggle.setAttribute('aria-expanded', 'true');
      }

      localStorage.setItem('dawn_llm_controls_collapsed', llmControlsCollapsed ? 'true' : 'false');
   }

   // =============================================================================
   // Color Theme - Moved to /js/ui/theme.js (DawnTheme module)
   // TTS Toggle - Moved to /js/ui/tts.js (DawnTts module)
   // =============================================================================

   // =============================================================================
   // Initialization
   // =============================================================================
   async function init() {
      // Initialize DOM element cache first
      DawnElements.init();

      // Initialize Opus worker first (before connect)
      initOpusWorker();

      // Initialize color theme (via DawnTheme module)
      DawnTheme.init();

      // Event listeners
      DawnElements.textInput.addEventListener('keydown', handleKeydown);

      // Auto-resize textarea as user types + smart text override
      DawnElements.textInput.addEventListener('input', function () {
         // Editing exits history navigation so further Arrow keys do caret nav,
         // not recall (programmatic recall uses .value and doesn't fire 'input').
         historyNavIndex = null;
         resizeComposer(this);

         // Smart typing override: when text present in voice mode, show "Send"
         if (typeof DawnAlwaysOn !== 'undefined') {
            DawnAlwaysOn.setTextOverride(this.value.trim().length > 0);
         }
      });

      // Unified action button: mode determines behavior
      if (DawnElements.actionBtn) {
         // Click handler — delegates based on mode + state
         DawnElements.actionBtn.addEventListener('click', function (e) {
            var appState = DawnState.getAppState ? DawnState.getAppState() : 'idle';
            var isCancelState =
               appState === 'processing' || appState === 'speaking' || appState === 'thinking';

            // Cancel takes priority
            if (isCancelState) {
               handleCancel();
               return;
            }

            var mode =
               typeof DawnAlwaysOn !== 'undefined' ? DawnAlwaysOn.getSelectedMode() : 'send';
            var hasTextOverride =
               typeof DawnAlwaysOn !== 'undefined' && DawnAlwaysOn.isTextOverrideActive();

            // Text override: send text regardless of mode
            if (hasTextOverride) {
               handleSend();
               DawnAlwaysOn.setTextOverride(false);
               return;
            }

            if (mode === 'send') {
               handleSend();
            } else if (mode === 'continuous') {
               DawnAlwaysOn.toggle();
            }
            // PTT mode: click is a no-op (mousedown/mouseup handles it)
         });

         // PTT: mousedown starts, mouseup/mouseleave stops
         DawnElements.actionBtn.addEventListener('mousedown', function (e) {
            var mode =
               typeof DawnAlwaysOn !== 'undefined' ? DawnAlwaysOn.getSelectedMode() : 'send';
            if (mode !== 'push-to-talk') return;

            // Suppress PTT during cancel state or text override
            var appState = DawnState.getAppState ? DawnState.getAppState() : 'idle';
            var isCancelState =
               appState === 'processing' || appState === 'speaking' || appState === 'thinking';
            if (isCancelState) return;
            if (typeof DawnAlwaysOn !== 'undefined' && DawnAlwaysOn.isTextOverrideActive()) return;

            e.preventDefault();
            if (!DawnState.getIsRecording() && DawnState.getAudioSupported()) {
               DawnAudioCapture.start();
            }
         });

         DawnElements.actionBtn.addEventListener('mouseup', function (e) {
            var mode =
               typeof DawnAlwaysOn !== 'undefined' ? DawnAlwaysOn.getSelectedMode() : 'send';
            if (mode !== 'push-to-talk') return;
            e.preventDefault();
            if (DawnState.getIsRecording()) {
               DawnAudioCapture.stop();
            }
         });

         DawnElements.actionBtn.addEventListener('mouseleave', function () {
            var mode =
               typeof DawnAlwaysOn !== 'undefined' ? DawnAlwaysOn.getSelectedMode() : 'send';
            if (mode !== 'push-to-talk') return;
            if (DawnState.getIsRecording()) {
               DawnAudioCapture.stop();
            }
         });

         // Touch events for mobile PTT
         DawnElements.actionBtn.addEventListener('touchstart', function (e) {
            var mode =
               typeof DawnAlwaysOn !== 'undefined' ? DawnAlwaysOn.getSelectedMode() : 'send';
            if (mode !== 'push-to-talk') return;

            var appState = DawnState.getAppState ? DawnState.getAppState() : 'idle';
            var isCancelState =
               appState === 'processing' || appState === 'speaking' || appState === 'thinking';
            if (isCancelState) return;
            if (typeof DawnAlwaysOn !== 'undefined' && DawnAlwaysOn.isTextOverrideActive()) return;

            e.preventDefault();
            if (!DawnState.getIsRecording() && DawnState.getAudioSupported()) {
               DawnAudioCapture.start();
            }
         });

         DawnElements.actionBtn.addEventListener('touchend', function (e) {
            var mode =
               typeof DawnAlwaysOn !== 'undefined' ? DawnAlwaysOn.getSelectedMode() : 'send';
            if (mode !== 'push-to-talk') return;
            e.preventDefault();
            if (DawnState.getIsRecording()) {
               DawnAudioCapture.stop();
            }
         });
      }

      // TTS toggle button (via DawnTts module)
      DawnTts.init();

      // Always-on voice mode
      if (typeof DawnAlwaysOn !== 'undefined') {
         DawnAlwaysOn.init();
      }

      // Debug mode toggle
      DawnElements.debugBtn.addEventListener('click', function () {
         DawnState.setDebugMode(!DawnState.getDebugMode());
         this.classList.toggle('active', DawnState.getDebugMode());
         // Toggle body class for CSS-based visibility control
         document.body.classList.toggle('debug-mode', DawnState.getDebugMode());
         console.log('Debug mode:', DawnState.getDebugMode() ? 'enabled' : 'disabled');

         // Request system prompt when debug is enabled
         if (DawnState.getDebugMode() && DawnWS.isConnected()) {
            DawnWS.send({ type: 'get_system_prompt' });
         }

         // Scroll to bottom to see newly visible entries
         DawnElements.transcript.scrollTop = DawnElements.transcript.scrollHeight;
      });

      // Restore persisted debug mode across hard refreshes. Tool-call/result entries are
      // already in the DOM, CSS-gated by body.debug-mode, so applying the class reveals
      // them; the system prompt is (re-)requested here and on (re)connect (see
      // updateConnectionStatus).
      if (DawnState.getDebugMode()) {
         DawnElements.debugBtn.classList.add('active');
         document.body.classList.add('debug-mode');
         if (DawnWS.isConnected()) {
            DawnWS.send({ type: 'get_system_prompt' });
         }
      }

      // Visualizer collapse/expand setup
      // Restore state from localStorage (mobile defaults to collapsed on first visit)
      const savedCollapsed = localStorage.getItem('dawn_visualizer_collapsed');
      const isMobile = window.innerWidth <= 600;

      if (savedCollapsed === 'true' || (savedCollapsed === null && isMobile)) {
         visualizerCollapsed = true;
         if (DawnElements.visualizer) {
            DawnElements.visualizer.classList.add('collapsed');
         }
         if (DawnElements.visualizerMini) {
            DawnElements.visualizerMini.classList.remove('hidden');
            DawnElements.visualizerMini.setAttribute('aria-expanded', 'false');
         }
         if (DawnElements.visualizerCollapseToggle) {
            DawnElements.visualizerCollapseToggle.setAttribute('aria-expanded', 'false');
         }
      }

      // Collapse toggle (in visualizer) - click and keyboard
      if (DawnElements.visualizerCollapseToggle) {
         DawnElements.visualizerCollapseToggle.addEventListener('click', toggleVisualizerCollapse);
         DawnElements.visualizerCollapseToggle.addEventListener('keydown', function (e) {
            if (e.key === 'Enter' || e.key === ' ') {
               e.preventDefault();
               toggleVisualizerCollapse();
            }
         });
      }

      // Mini bar (expand) - click and keyboard (entire bar is clickable)
      if (DawnElements.visualizerMini) {
         DawnElements.visualizerMini.addEventListener('click', toggleVisualizerCollapse);
         DawnElements.visualizerMini.addEventListener('keydown', function (e) {
            if (e.key === 'Enter' || e.key === ' ') {
               e.preventDefault();
               toggleVisualizerCollapse();
            }
         });
      }

      // LLM controls collapse/expand setup
      const llmMini = document.getElementById('llm-controls-mini');
      const llmGrid = document.getElementById('llm-controls-grid');
      if (llmMini && llmGrid) {
         const savedLlmCollapsed = localStorage.getItem('dawn_llm_controls_collapsed');
         if (savedLlmCollapsed === 'true' || (savedLlmCollapsed === null && isMobile)) {
            llmControlsCollapsed = true;
            llmGrid.classList.add('collapsed');
            updateLlmMiniSummary();
            llmMini.classList.remove('hidden');
            llmMini.setAttribute('aria-expanded', 'false');
            const llmCollapseInit = document.getElementById('llm-controls-collapse');
            if (llmCollapseInit) llmCollapseInit.setAttribute('aria-expanded', 'false');
         }

         llmMini.addEventListener('click', toggleLlmControlsCollapse);
         llmMini.addEventListener('keydown', function (e) {
            if (e.key === 'Enter' || e.key === ' ') {
               e.preventDefault();
               toggleLlmControlsCollapse();
            }
         });

         // Update mini summary when selects change
         ['llm-type-select', 'llm-model-select'].forEach((id) => {
            const sel = document.getElementById(id);
            if (sel) sel.addEventListener('change', updateLlmMiniSummary);
         });

         // Collapse toggle inside the grid
         const llmCollapseToggle = document.getElementById('llm-controls-collapse');
         if (llmCollapseToggle) {
            llmCollapseToggle.addEventListener('click', toggleLlmControlsCollapse);
            llmCollapseToggle.addEventListener('keydown', function (e) {
               if (e.key === 'Enter' || e.key === ' ') {
                  e.preventDefault();
                  toggleLlmControlsCollapse();
               }
            });
         }
      }

      // Event delegation for transcript (handles dynamically added elements)
      if (DawnElements.transcript) {
         DawnElements.transcript.addEventListener('click', function (e) {
            // Handle continuation banner toggle
            const bannerHeader = e.target.closest('.continuation-header');
            if (bannerHeader) {
               e.preventDefault();
               const content = bannerHeader.parentElement.querySelector('.continuation-content');
               const toggle = bannerHeader.querySelector('.continuation-toggle');
               if (content && toggle) {
                  content.classList.toggle('collapsed');
                  toggle.textContent = content.classList.contains('collapsed') ? '▼' : '▲';
               }
               return;
            }

            // Handle continuation link (View Continuation button)
            const continuationLink = e.target.closest('.continuation-link');
            if (continuationLink) {
               e.preventDefault();
               const convId = continuationLink.dataset.convId;
               if (convId) {
                  requestLoadConversation(parseInt(convId, 10));
               }
               return;
            }
         });
      }

      // Initialize audio capture module
      DawnAudioCapture.setCallbacks({
         onMicButton: updateMicButton,
         onError: function (msg) {
            DawnTranscript.addEntry('system', msg);
         },
         getOpusEncoder: function () {
            return { ready: opusReady, worker: opusWorker };
         },
      });

      // Initialize audio playback module
      DawnAudioPlayback.setCallbacks({
         onPlaybackStart: function () {
            DawnVisualization.startFFT();
            // Pause always-on mic capture while we speak (echo prevention).
            // Playback-driven so a turn with no TTS never mutes.
            if (typeof DawnAlwaysOn !== 'undefined') {
               DawnAlwaysOn.onPlaybackStart();
            }
         },
         onPlaybackEnd: function () {
            DawnVisualization.stopFFT();
            // If server sent "idle" while we were playing, apply it now
            if (pendingIdleState) {
               pendingIdleState = false;
               console.log('Audio playback finished, applying deferred idle state');
               updateState('idle', null, null);
            }
            // Resume always-on mic capture after TTS finishes
            if (typeof DawnAlwaysOn !== 'undefined') {
               DawnAlwaysOn.onPlaybackEnd();
            }
         },
      });

      // Initialize streaming module
      DawnStreaming.setCallbacks({
         onStateChange: updateState,
         onSaveMessage: DawnHistory.saveMessage,
         getPendingVisuals: function () {
            var visuals = pendingVisualsForSave.slice();
            pendingVisualsForSave = [];
            return visuals;
         },
      });

      const audioResult = await DawnAudioCapture.init();
      if (!audioResult.supported) {
         // Disable voice mode dropdown items (Send still works)
         if (typeof DawnAlwaysOn !== 'undefined') {
            DawnAlwaysOn.disableAudioModes();
         }
         console.warn('Audio disabled:', audioResult.reason);
      }

      // Initialize visualization (rings, bars, default waveform)
      DawnVisualization.init();
      DawnVisualization.drawDefault();

      // Initialize context pressure gauge
      DawnContextGauge.init();

      // Initialize settings panel (includes modals, auth visibility, LLM controls)
      DawnSettings.setCallbacks({
         getAuthState: () => DawnState.authState,
         setAuthState: (state) => {
            DawnState.authState.authenticated = state.authenticated;
            DawnState.authState.isAdmin = state.isAdmin;
            DawnState.authState.username = state.username;
         },
         updateHistoryButtonVisibility: DawnHistory.updateButtonVisibility,
         updateMemoryButtonVisibility: DawnMemory.updateVisibility,
         updateSchedulerButtonVisibility: () => {
            if (window.DawnSchedulerQueue) {
               DawnSchedulerQueue.updateVisibility(DawnState.authState);
               if (DawnState.authState && DawnState.authState.authenticated) {
                  DawnSchedulerQueue.refresh();
               }
            }
            // Coding popover shares the same auth-gated reveal lifecycle.
            if (window.DawnCodeProjects) {
               DawnCodeProjects.updateVisibility(DawnState.authState);
            }
         },
         restoreHistorySidebarState: DawnHistory.restoreSidebarState,
      });
      DawnSettings.init();
      DawnSettings.initConversationLlmControls();
      DawnTools.init();
      DawnMetricsPanel.init();
      if (typeof DawnJobsActivity !== 'undefined') {
         DawnJobsActivity.init(
            typeof DawnHistory !== 'undefined' ? DawnHistory.getActiveConversationId : null
         );
      }
      DawnUserBadge.init({
         openSection: DawnSettings.openSection,
         metricsToggle: typeof DawnMetricsPanel !== 'undefined' ? DawnMetricsPanel.toggle : null,
      });
      DawnUsers.setCallbacks({
         trapFocus: DawnSettings.trapFocus,
         getAuthState: () => DawnState.authState,
      });
      DawnUsers.init();
      DawnSatellites.setCallbacks({
         trapFocus: DawnSettings.trapFocus,
      });
      DawnMySettings.setCallbacks({
         setTheme: DawnTheme.set,
         getAuthState: () => DawnState.authState,
      });
      DawnMySettings.init();
      DawnMySessions.setCallbacks({
         getAuthState: () => DawnState.authState,
      });
      DawnMySessions.init();
      if (typeof DawnCalendarAccounts !== 'undefined') {
         DawnCalendarAccounts.setCallbacks({
            getAuthState: () => DawnState.authState,
         });
         DawnCalendarAccounts.init();
      }
      if (typeof DawnEmailAccounts !== 'undefined') {
         DawnEmailAccounts.setCallbacks({
            getAuthState: () => DawnState.authState,
         });
         DawnEmailAccounts.init();
      }
      DawnHistory.setCallbacks({
         trapFocus: DawnSettings.trapFocus,
         getAuthState: () => DawnState.authState,
      });
      DawnHistory.init();

      // Initialize memory module (memory panel for viewing/managing user memories)
      DawnMemory.init({
         trapFocus: DawnSettings.trapFocus,
         getAuthState: () => DawnState.authState,
      });

      // Initialize watches panel (SAGE proactive attention — a tab inside the
      // scheduler popover, so init it before the scheduler queue below).
      if (typeof DawnWatches !== 'undefined') {
         DawnWatches.init();
      }

      // Initialize scheduler queue panel
      if (window.DawnSchedulerQueue) {
         DawnSchedulerQueue.init();
         DawnSchedulerQueue.updateVisibility(DawnState.authState);
      }

      // Initialize vision module (paste, camera, image processing)
      DawnVision.init();

      // Initialize visual renderer (inline SVG/HTML diagrams)
      if (typeof DawnVisualRender !== 'undefined') {
         DawnVisualRender.init();
      }

      // Initialize document upload module (chip UI, upload)
      if (typeof DawnDocuments !== 'undefined') {
         DawnDocuments.init();
      }

      // Initialize unified attach module (file picker, drag-drop, counter)
      if (typeof DawnAttach !== 'undefined') {
         DawnAttach.init();
      }

      // Initialize document library (RAG)
      if (typeof DawnDocLibrary !== 'undefined') {
         DawnDocLibrary.init({
            trapFocus: DawnSettings.trapFocus,
         });
      }

      // Initialize code projects (coding harness)
      if (typeof DawnCodeProjects !== 'undefined') {
         DawnCodeProjects.init({
            trapFocus: DawnSettings.trapFocus,
         });
      }

      // Initialize music player UI
      if (typeof DawnMusicUI !== 'undefined') {
         DawnMusicUI.init();
      }

      // Set up WebSocket callbacks and connect
      initWebSocketCallbacks();
      DawnWS.connect();

      // Allow manual reconnect by clicking connection status
      DawnElements.connectionStatus.addEventListener('click', () => {
         if (!DawnWS.isConnected()) {
            DawnWS.forceReconnect();
         }
      });
      DawnElements.connectionStatus.style.cursor = 'pointer';

      // Reconnect on visibility change
      document.addEventListener('visibilitychange', function () {
         if (!document.hidden && !DawnWS.isConnected()) {
            DawnWS.forceReconnect();
         }
      });

      // Fetch and display version info in footer
      fetchVersionInfo();

      // Claim document focus on load so the first click lands on a control
      // rather than just "activating" the page (after a browser-chrome reload
      // the document has no focus until clicked). Cursor-ready in the composer
      // is also nicer UX. Skip on touch (pointer: coarse) so we don't force the
      // on-screen keyboard up; preventScroll avoids a jump if it's below the fold.
      if (DawnElements.textInput && window.matchMedia('(pointer: fine)').matches) {
         DawnElements.textInput.focus({ preventScroll: true });
      }

      console.log(
         'DAWN WebUI initialized (audio:',
         audioResult.supported ? 'enabled' : 'disabled',
         ')'
      );
   }

   /**
    * Fetch version info from /health endpoint and update footer
    */
   async function fetchVersionInfo() {
      try {
         const response = await fetch('/health');
         if (response.ok) {
            const data = await response.json();
            const footerVersion = document.getElementById('footer-version');
            if (footerVersion && data.version && data.git_sha) {
               footerVersion.textContent = `Dawn WebUI v${data.version}: ${data.git_sha}`;
            }
         }
      } catch (e) {
         console.warn('Failed to fetch version info:', e);
      }
   }

   // =============================================================================
   // NOTE: Settings Panel moved to /js/ui/settings.js (DawnSettings module)
   // NOTE: User Badge Dropdown moved to /js/ui/user-badge.js (DawnUserBadge module)
   // =============================================================================

   // Start when DOM is ready
   if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', init);
   } else {
      init();
   }

   // Expose visualization toggle for testing/debugging
   // Usage: DAWN.toggleVisualization()
   window.DAWN = window.DAWN || {};
   window.DAWN.toggleVisualization = DawnVisualization.toggleMode;
   window.DAWN.getVisualizationMode = DawnVisualization.getMode;
   window.DAWN.toggleFFTDebug = DawnVisualization.toggleFFTDebug;
   window.DAWN.updateLlmMiniSummary = updateLlmMiniSummary;

   // Test helper for console access
   // Usage: DAWN.send({type: 'get_my_settings'})
   window.DAWN.send = function (msg) {
      if (DawnWS.isConnected()) {
         DawnWS.send(msg);
         console.log('Sent:', msg);
      } else {
         console.error('WebSocket not connected');
      }
   };
})();
