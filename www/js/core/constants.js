/**
 * DAWN Core Constants Module
 * Shared configuration values for WebSocket, audio, and protocol
 *
 * Usage:
 *   DawnConfig.WS_SUBPROTOCOL
 *   DawnConfig.AUDIO_SAMPLE_RATE
 *   DawnConfig.WS_BIN_AUDIO_IN
 */
(function (global) {
   'use strict';

   // WebSocket configuration
   const WS_SUBPROTOCOL = 'dawn-1.0';

   // Reconnection settings
   const RECONNECT_BASE_DELAY = 1000;
   const RECONNECT_MAX_DELAY = 30000;
   const RECONNECT_MAX_ATTEMPTS = 5;

   // Audio configuration
   // 48kHz is Opus native rate - server resamples to 16kHz for ASR
   const AUDIO_SAMPLE_RATE = 48000;
   const AUDIO_CHANNELS = 1; // Mono

   // Binary message types (match server)
   const WS_BIN_AUDIO_IN = 0x01;
   const WS_BIN_AUDIO_IN_END = 0x02;
   const WS_BIN_AUDIO_OUT = 0x11;
   const WS_BIN_AUDIO_SEGMENT_END = 0x12; // Play accumulated audio segment now

   // Music streaming binary types (0x20-0x2F range)
   const WS_BIN_MUSIC_DATA = 0x20; // Server -> Client: Opus music audio chunk
   const WS_BIN_MUSIC_SEGMENT_END = 0x21; // Server -> Client: End of buffered segment

   // Context/token limits
   // Default context window size - used when model-specific limit is unknown
   // TODO: Make this dynamic based on selected model (GPT-4o: 128k, Claude: 200k, Gemini: 1M+)
   const DEFAULT_CONTEXT_MAX = 128000;

   // Expose globally
   global.DawnConfig = {
      // WebSocket
      WS_SUBPROTOCOL: WS_SUBPROTOCOL,

      // Reconnection
      RECONNECT_BASE_DELAY: RECONNECT_BASE_DELAY,
      RECONNECT_MAX_DELAY: RECONNECT_MAX_DELAY,
      RECONNECT_MAX_ATTEMPTS: RECONNECT_MAX_ATTEMPTS,

      // Audio
      AUDIO_SAMPLE_RATE: AUDIO_SAMPLE_RATE,
      AUDIO_CHANNELS: AUDIO_CHANNELS,

      // Binary message types
      WS_BIN_AUDIO_IN: WS_BIN_AUDIO_IN,
      WS_BIN_AUDIO_IN_END: WS_BIN_AUDIO_IN_END,
      WS_BIN_AUDIO_OUT: WS_BIN_AUDIO_OUT,
      WS_BIN_AUDIO_SEGMENT_END: WS_BIN_AUDIO_SEGMENT_END,

      // Music streaming binary types
      WS_BIN_MUSIC_DATA: WS_BIN_MUSIC_DATA,
      WS_BIN_MUSIC_SEGMENT_END: WS_BIN_MUSIC_SEGMENT_END,

      // Context limits
      DEFAULT_CONTEXT_MAX: DEFAULT_CONTEXT_MAX,
   };
})(window);
