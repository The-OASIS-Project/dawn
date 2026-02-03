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
   let pendingThumbnailsForSave = []; // Thumbnails to attach when saving user message

   // Opus codec state
   let opusWorker = null;
   let opusReady = false;
   let pendingDecodes = [];
   let pendingOpusData = [];
   let pendingDecodePlayback = false;

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
   function handleTextMessage(data) {
      try {
         const msg = JSON.parse(data);
         console.log('Received:', msg);

         switch (msg.type) {
            case 'state':
               updateState(msg.payload.state, msg.payload.detail, msg.payload.tools);
               break;
            case 'force_logout':
               // Session was revoked - force immediate logout
               console.warn('Force logout received:', msg.payload.reason);
               DawnToast.show(msg.payload.reason || 'Session revoked', 'error');
               localStorage.removeItem('dawn_session_token');
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
               } else if (msg.payload.role === 'tool') {
                  // Tool execution debug messages - display only, don't save to history
                  // This prevents polluting history with internal tool call formatting
                  DawnTranscript.addEntry(msg.payload.role, msg.payload.text);
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

                  DawnTranscript.addEntry(msg.payload.role, displayContent);

                  // Save to conversation history (auto-creates conversation on first message)
                  // Skip replay messages - these are history replay on reconnect, already in DB
                  if (!msg.payload.replay) {
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
                  localStorage.removeItem('dawn_session_token');
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
               } else {
                  DawnTranscript.addEntry('system', `Error: ${msg.payload.message}`);
               }
               break;
            case 'session':
               console.log('Session token received');
               localStorage.setItem('dawn_session_token', msg.payload.token);
               // Server has processed our init/reconnect with capabilities
               DawnWS.setCapabilitiesSynced(true);
               // Auth state is now included in session response (avoids extra config fetch)
               if (msg.payload.authenticated !== undefined) {
                  DawnState.authState.authenticated = msg.payload.authenticated;
                  DawnState.authState.isAdmin = msg.payload.is_admin || false;
                  DawnState.authState.username = msg.payload.username || '';
                  DawnSettings.updateAuthVisibility();
               }
               // Request full config to populate LLM controls
               DawnSettings.requestConfig();
               // Restore active conversation context (backend session may have lost it on restart)
               // This loads the conversation history into the LLM context so subsequent
               // messages have proper context
               const savedConvId = DawnHistory.getActiveConversationId();
               if (savedConvId && DawnState.authState.authenticated) {
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
            case 'get_config_response':
               DawnSettings.handleGetConfigResponse(msg.payload);
               // Update vision limits from server (single source of truth)
               if (msg.payload.vision_limits) {
                  DawnVision.updateLimits(msg.payload.vision_limits);
               }
               // Update vision button state based on model capabilities
               if (msg.payload.config) {
                  DawnVision.checkVisionSupport(msg.payload.config);
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
            case 'smartthings_status_response':
               DawnSmartThings.handleStatusResponse(msg.payload);
               break;
            case 'smartthings_auth_url_response':
               DawnSmartThings.handleAuthUrlResponse(msg.payload);
               break;
            case 'smartthings_exchange_code_response':
               DawnSmartThings.handleExchangeCodeResponse(msg.payload);
               break;
            case 'smartthings_disconnect_response':
               DawnSmartThings.handleDisconnectResponse(msg.payload);
               break;
            case 'smartthings_devices_response':
               DawnSmartThings.handleDevicesResponse(msg.payload);
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
            case 'context':
               DawnContextGauge.updateDisplay(msg.payload, DawnMetrics.updatePanel);
               // Save context to active conversation for restore on reload
               const activeConvId = DawnHistory.getActiveConversationId();
               if (activeConvId && msg.payload.current && msg.payload.max) {
                  DawnHistory.requestUpdateContext(
                     activeConvId,
                     msg.payload.current,
                     msg.payload.max
                  );
               }
               break;
            case 'context_compacted':
               DawnHistory.handleContextCompacted(msg.payload);
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
            case 'metrics_update':
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
            case 'delete_all_memories_response':
               DawnMemory.handleDeleteAllResponse(msg.payload);
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

                  if (opusReady && opusWorker) {
                     // Decode complete Opus stream via worker
                     // Set flag so playback triggers when decode completes
                     pendingDecodePlayback = true;
                     opusWorker.postMessage({ type: 'decode', data: combined }, [combined.buffer]);
                  } else {
                     // Raw PCM: 16-bit signed, 16kHz, mono
                     DawnAudioPlayback.queueAudio(combined);
                     DawnAudioPlayback.play();
                  }
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
    * Update mic button visual state
    */
   function updateMicButton(recording) {
      if (DawnElements.micBtn) {
         DawnElements.micBtn.classList.toggle('recording', recording);
         DawnElements.micBtn.textContent = recording ? 'Stop' : 'Mic';
         DawnElements.micBtn.title = recording ? 'Stop recording' : 'Start recording';
      }
   }

   function sendTextMessage(text) {
      if (!DawnWS.isConnected()) {
         console.error('WebSocket not connected');
         return false;
      }

      const msg = {
         type: 'text',
         payload: { text: text },
      };

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
      DawnElements.connectionStatus.className = status;
      DawnElements.connectionStatus.title = ''; // Clear any previous tooltip
      if (status === 'connected') {
         DawnElements.connectionStatus.textContent = 'Connected';
      } else if (status === 'connecting') {
         DawnElements.connectionStatus.textContent = 'Connecting...';
      } else {
         // Show disconnect reason if available (truncate for display)
         DawnElements.connectionStatus.textContent = reason
            ? 'Disconnected: ' + (reason.length > 30 ? reason.substring(0, 30) + '...' : reason)
            : 'Disconnected';
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

      // Update ring container - preserve fft-active class if present
      const hasFftActive = DawnElements.ringContainer.classList.contains('fft-active');
      DawnElements.ringContainer.classList.remove(previousState);
      DawnElements.ringContainer.classList.add(state);
      // Re-add fft-active if it was there (in case it got removed)
      if (hasFftActive) {
         DawnElements.ringContainer.classList.add('fft-active');
      }

      // Toggle send/stop button based on state
      // Show stop button during processing, speaking, or thinking states
      const showStop = state === 'processing' || state === 'speaking' || state === 'thinking';
      if (DawnElements.sendBtn) {
         DawnElements.sendBtn.classList.toggle('hidden', showStop);
      }
      if (DawnElements.stopBtn) {
         DawnElements.stopBtn.classList.toggle('hidden', !showStop);
      }

      // Emit state event for decoupled modules
      if (typeof DawnEvents !== 'undefined') {
         DawnEvents.emit('state', { state, previousState, detail, tools });
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
   function handleSend() {
      const text = DawnElements.textInput.value.trim();
      if (text) {
         if (sendTextMessage(text)) {
            DawnElements.textInput.value = '';
            DawnElements.textInput.style.height = 'auto';
         }
      }
   }

   function handleKeydown(event) {
      if (event.key === 'Enter' && !event.shiftKey) {
         event.preventDefault();
         handleSend();
      }
   }

   function handleStop() {
      console.log('Stop button clicked - sending cancel');
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
      DawnElements.sendBtn.addEventListener('click', handleSend);
      DawnElements.stopBtn.addEventListener('click', handleStop);
      DawnElements.textInput.addEventListener('keydown', handleKeydown);

      // Auto-resize textarea as user types
      DawnElements.textInput.addEventListener('input', function () {
         this.style.height = 'auto';
         const newHeight = Math.min(this.scrollHeight, 150);
         this.style.height = newHeight + 'px';
         // Only show scrollbar when content exceeds max height
         this.style.overflowY = this.scrollHeight > 150 ? 'auto' : 'hidden';
      });

      // Mic button - push to talk
      if (DawnElements.micBtn) {
         DawnElements.micBtn.addEventListener('mousedown', function (e) {
            e.preventDefault();
            if (!DawnState.getIsRecording() && DawnState.getAudioSupported()) {
               DawnAudioCapture.start();
            }
         });

         DawnElements.micBtn.addEventListener('mouseup', function (e) {
            e.preventDefault();
            if (DawnState.getIsRecording()) {
               DawnAudioCapture.stop();
            }
         });

         DawnElements.micBtn.addEventListener('mouseleave', function (e) {
            // Stop recording if mouse leaves button while pressed
            if (DawnState.getIsRecording()) {
               DawnAudioCapture.stop();
            }
         });

         // Touch events for mobile
         DawnElements.micBtn.addEventListener('touchstart', function (e) {
            e.preventDefault();
            if (!DawnState.getIsRecording() && DawnState.getAudioSupported()) {
               DawnAudioCapture.start();
            }
         });

         DawnElements.micBtn.addEventListener('touchend', function (e) {
            e.preventDefault();
            if (DawnState.getIsRecording()) {
               DawnAudioCapture.stop();
            }
         });
      }

      // TTS toggle button (via DawnTts module)
      DawnTts.init();

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
         onPlaybackStart: DawnVisualization.startFFT,
         onPlaybackEnd: function () {
            DawnVisualization.stopFFT();
            // If server sent "idle" while we were playing, apply it now
            if (pendingIdleState) {
               pendingIdleState = false;
               console.log('Audio playback finished, applying deferred idle state');
               updateState('idle', null, null);
            }
         },
      });

      // Initialize streaming module
      DawnStreaming.setCallbacks({
         onStateChange: updateState,
         onSaveMessage: DawnHistory.saveMessage,
      });

      const audioResult = await DawnAudioCapture.init();
      if (audioResult.supported && DawnElements.micBtn) {
         DawnElements.micBtn.disabled = false;
         DawnElements.micBtn.title = 'Hold to speak';
      } else if (DawnElements.micBtn) {
         // Show why audio is disabled
         DawnElements.micBtn.title = audioResult.reason || 'Audio not available';
         console.warn('Mic disabled:', audioResult.reason);
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
         restoreHistorySidebarState: DawnHistory.restoreSidebarState,
      });
      DawnSettings.init();
      DawnSettings.initConversationLlmControls();
      DawnTools.init();
      DawnMetricsPanel.init();
      initUserBadgeDropdown();
      DawnUsers.setCallbacks({
         trapFocus: DawnSettings.trapFocus,
         showConfirmModal: DawnSettings.showConfirmModal,
         getAuthState: () => DawnState.authState,
      });
      DawnUsers.init();
      DawnMySettings.setCallbacks({
         showConfirmModal: DawnSettings.showConfirmModal,
         setTheme: DawnTheme.set,
         getAuthState: () => DawnState.authState,
      });
      DawnMySettings.init();
      DawnMySessions.setCallbacks({
         showConfirmModal: DawnSettings.showConfirmModal,
         getAuthState: () => DawnState.authState,
      });
      DawnMySessions.init();
      DawnHistory.setCallbacks({
         trapFocus: DawnSettings.trapFocus,
         showConfirmModal: DawnSettings.showConfirmModal,
         showInputModal: DawnSettings.showInputModal,
         getAuthState: () => DawnState.authState,
      });
      DawnHistory.init();

      // Initialize memory module (memory panel for viewing/managing user memories)
      DawnMemory.init({
         showConfirmModal: DawnSettings.showConfirmModal,
         showInputModal: DawnSettings.showInputModal,
         trapFocus: DawnSettings.trapFocus,
         getAuthState: () => DawnState.authState,
      });

      // Initialize vision module (image upload/paste/drag-drop)
      DawnVision.init();

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
   // =============================================================================

   // =============================================================================
   // User Badge Dropdown
   // =============================================================================

   function initUserBadgeDropdown() {
      const badge = document.getElementById('user-badge');
      const dropdown = document.getElementById('user-badge-dropdown');

      if (!badge || !dropdown) return;

      // Toggle dropdown on badge click
      badge.addEventListener('click', function (e) {
         e.stopPropagation();
         const isOpen = dropdown.classList.contains('open');
         if (isOpen) {
            closeUserDropdown();
         } else {
            openUserDropdown();
         }
      });

      // Handle keyboard navigation
      badge.addEventListener('keydown', function (e) {
         if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            badge.click();
         } else if (e.key === 'Escape') {
            closeUserDropdown();
         }
      });

      // Handle keyboard navigation in dropdown (H11)
      dropdown.addEventListener('keydown', function (e) {
         const items = Array.from(
            dropdown.querySelectorAll('.dropdown-item:not(.dropdown-divider)')
         );
         const currentIndex = items.indexOf(document.activeElement);

         switch (e.key) {
            case 'Escape':
               closeUserDropdown();
               badge.focus();
               break;
            case 'ArrowDown':
               e.preventDefault();
               const nextIndex = currentIndex < items.length - 1 ? currentIndex + 1 : 0;
               items[nextIndex]?.focus();
               break;
            case 'ArrowUp':
               e.preventDefault();
               const prevIndex = currentIndex > 0 ? currentIndex - 1 : items.length - 1;
               items[prevIndex]?.focus();
               break;
            case 'Home':
               e.preventDefault();
               items[0]?.focus();
               break;
            case 'End':
               e.preventDefault();
               items[items.length - 1]?.focus();
               break;
         }
      });

      // Handle dropdown item clicks
      dropdown.addEventListener('click', function (e) {
         const item = e.target.closest('.dropdown-item');
         if (!item) return;

         const action = item.dataset.action;
         closeUserDropdown();

         switch (action) {
            case 'my-settings':
               DawnSettings.openSection('my-settings-section');
               break;
            case 'my-sessions':
               DawnSettings.openSection('my-sessions-section');
               break;
            case 'logout':
               handleLogout();
               break;
         }
      });

      // Close dropdown when clicking outside
      document.addEventListener('click', function (e) {
         if (!e.target.closest('.user-badge-container')) {
            closeUserDropdown();
         }
      });
   }

   function openUserDropdown() {
      const badge = document.getElementById('user-badge');
      const dropdown = document.getElementById('user-badge-dropdown');
      if (!badge || !dropdown) return;

      badge.setAttribute('aria-expanded', 'true');
      dropdown.classList.add('open');

      // Focus first menu item for keyboard users
      const firstItem = dropdown.querySelector('.dropdown-item');
      if (firstItem) {
         setTimeout(() => firstItem.focus(), 50);
      }
   }

   function closeUserDropdown() {
      const badge = document.getElementById('user-badge');
      const dropdown = document.getElementById('user-badge-dropdown');
      if (!badge || !dropdown) return;

      badge.setAttribute('aria-expanded', 'false');
      dropdown.classList.remove('open');
   }

   async function handleLogout() {
      // Clear WebSocket session token from localStorage
      localStorage.removeItem('dawn_session_token');

      try {
         // Use GET - logout has no request body
         const response = await fetch('/api/auth/logout', {
            credentials: 'same-origin',
         });

         // Always redirect to login page regardless of response
         window.location.href = '/login.html';
      } catch (err) {
         console.error('Logout error:', err);
         // Redirect anyway on network error
         window.location.href = '/login.html';
      }
   }

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
