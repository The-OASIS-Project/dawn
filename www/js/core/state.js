/**
 * DAWN State Management Module
 * Centralized state with getters/setters for controlled access
 *
 * Usage:
 *   DawnState.getAppState()           // Get current app state
 *   DawnState.setAppState('speaking') // Set app state
 *   DawnState.streamingState.active   // Access complex state objects
 *   DawnState.authState.isAdmin       // Check auth state
 */
(function (global) {
   'use strict';

   // =============================================================================
   // Simple State Variables
   // =============================================================================
   let currentState = 'idle';
   // Persisted across hard refreshes (localStorage); see setDebugMode.
   let debugMode = DawnStore.getBool(DawnStore.KEYS.DEBUG_MODE, false);
   let isRecording = false;
   let audioSupported = false;

   // =============================================================================
   // Complex State Objects
   // =============================================================================

   /**
    * LLM streaming state (ChatGPT-style real-time text)
    */
   const streamingState = {
      active: false,
      streamId: null,
      entryElement: null,
      textElement: null,
      content: '',
      lastRenderMs: 0,
      renderDebounceMs: 100,
      pendingRender: false,
      reasoningTokens: 0, // OpenAI o-series reasoning token count
   };

   /**
    * Real-time metrics state (for multi-ring visualization)
    */
   const metricsState = {
      state: 'idle',
      ttft_ms: 0,
      token_rate: 0,
      context_percent: 0,
      lastUpdate: 0,
      last_ttft_ms: 0,
      last_token_rate: 0,
      last_thinking_tokens: 0, // Preserved reasoning tokens for telemetry display
   };

   /**
    * Hesitation ring state (token timing variance)
    */
   const hesitationState = {
      dtWindow: [],
      windowSize: 16,
      tPrevMs: 0,
      loadSmooth: 0,
      lastTokenMs: 0,
      animationId: null,
      runningSum: 0,
      runningSumSq: 0,
   };

   /**
    * Extended thinking state (Claude/llama.cpp reasoning display)
    */
   const thinkingState = {
      active: false,
      streamId: null,
      provider: null,
      entryElement: null,
      contentElement: null,
      content: '',
      collapsed: true,
      startTime: 0,
      lastRenderMs: 0,
      renderDebounceMs: 100,
      pendingRender: false,
      // Finalized values for saving with the assistant message
      finalizedContent: '',
      finalizedDuration: '0',
      finalizedProvider: null,
   };

   /**
    * Auth state (populated from session response)
    */
   const authState = {
      authenticated: false,
      isAdmin: false,
      username: '',
      sessionId: undefined, // this connection's session (session frame's session_id)
   };

   /**
    * Document state (for text document upload and attachment)
    * Defaults are overridden by server config.documents on connect.
    */
   const documentState = {
      pendingDocuments: [], // [{filename, content, size, type}]
      maxDocuments: 5, // Overridden by config.documents.max_documents
      maxFileSize: 512 * 1024, // Overridden by config.documents.max_file_size_kb * 1024
   };

   /**
    * Vision state (for image upload and preview)
    * Defaults are overridden by server config.vision on connect.
    */
   /**
    * Unified attachment limits (images + documents combined)
    */
   const attachmentState = {
      maxAttachments: 8, // Unified limit, overridden by server config
   };

   const visionState = {
      pendingImages: [], // Array of { data: base64, mimeType: string, thumbnail: string }
      maxImages: 5, // Per-type sub-limit for images (LLM vision API cap)
      maxSize: 4 * 1024 * 1024, // Overridden by config.vision.max_image_size_kb * 1024
      maxDimension: 1024, // Overridden by config.vision.max_dimension
      isProcessing: false, // Loading indicator state
      visionEnabled: false, // Whether current model supports vision
   };

   // =============================================================================
   // Expose globally
   // =============================================================================
   global.DawnState = {
      // Simple state getters/setters
      getAppState: () => currentState,
      setAppState: (state) => {
         currentState = state;
      },

      getDebugMode: () => debugMode,
      setDebugMode: (mode) => {
         debugMode = mode;
         DawnStore.setBool(DawnStore.KEYS.DEBUG_MODE, mode);
      },

      getIsRecording: () => isRecording,
      setIsRecording: (recording) => {
         isRecording = recording;
      },

      getAudioSupported: () => audioSupported,
      setAudioSupported: (supported) => {
         audioSupported = supported;
      },

      // Complex state objects (direct access)
      streamingState: streamingState,
      metricsState: metricsState,
      hesitationState: hesitationState,
      thinkingState: thinkingState,
      authState: authState,
      visionState: visionState,
      documentState: documentState,
      attachmentState: attachmentState,
   };
})(window);
