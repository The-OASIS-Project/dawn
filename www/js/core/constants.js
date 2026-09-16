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

   // Reconnection settings. No attempt cap: a browser left open reconnects
   // indefinitely with full-jitter backoff (see websocket.js scheduleReconnect).
   const RECONNECT_BASE_DELAY = 1000;
   const RECONNECT_MAX_DELAY = 30000;

   // Liveness heartbeat (app-level ping/pong). The server answers an authed
   // `ping` with a seq-echoing `pong`; these detect a half-open socket that
   // readyState still reports as OPEN. Values match Aurora's proven watchdog and
   // sit well under the server's ~1800s idle expiry and typical proxy timeouts.
   const PING_IDLE_MS = 8000; // only ping after this much inbound silence (traffic already proves life)
   const PING_INTERVAL_MS = 10000; // heartbeat tick cadence
   const PONG_TIMEOUT_MS = 6000; // a ping unanswered this long counts as one miss
   // Hysteresis: an isolated missed pong (a single late one on a marginal link)
   // must NOT flash the "unstable" pill. Only surface it after this many
   // CONSECUTIVE misses; a recovery (pong or any inbound frame) resets the count.
   const PONG_STALE_MISSES = 2; // consecutive misses => show "unstable" (not yet dead)
   const MAX_MISSED_PONGS = 3; // consecutive misses => link dead, force reconnect
   const MAX_UNSUPPORTED_PROBES = 3; // no pong EVER => assume an older server, stop probing (no false dead)

   // Audio configuration
   // 48kHz is Opus native rate - server resamples to 16kHz for ASR
   const AUDIO_SAMPLE_RATE = 48000;
   const AUDIO_CHANNELS = 1; // Mono

   // Binary message types (match server)
   const WS_BIN_AUDIO_IN = 0x01;
   const WS_BIN_AUDIO_IN_END = 0x02;
   const WS_BIN_AUDIO_OUT = 0x11;
   const WS_BIN_AUDIO_SEGMENT_END = 0x12; // Play accumulated audio segment now

   // Context/token limits
   // Default context window size - used as ultimate fallback when server hasn't sent model info yet.
   // Server sends dynamic context_max based on actual model via WebSocket 'context' messages.
   const DEFAULT_CONTEXT_MAX = 128000;

   // Expose globally
   global.DawnConfig = {
      // WebSocket
      WS_SUBPROTOCOL: WS_SUBPROTOCOL,

      // Reconnection
      RECONNECT_BASE_DELAY: RECONNECT_BASE_DELAY,
      RECONNECT_MAX_DELAY: RECONNECT_MAX_DELAY,

      // Liveness heartbeat
      PING_IDLE_MS: PING_IDLE_MS,
      PING_INTERVAL_MS: PING_INTERVAL_MS,
      PONG_TIMEOUT_MS: PONG_TIMEOUT_MS,
      PONG_STALE_MISSES: PONG_STALE_MISSES,
      MAX_MISSED_PONGS: MAX_MISSED_PONGS,
      MAX_UNSUPPORTED_PROBES: MAX_UNSUPPORTED_PROBES,

      // Audio
      AUDIO_SAMPLE_RATE: AUDIO_SAMPLE_RATE,
      AUDIO_CHANNELS: AUDIO_CHANNELS,

      // Binary message types
      WS_BIN_AUDIO_IN: WS_BIN_AUDIO_IN,
      WS_BIN_AUDIO_IN_END: WS_BIN_AUDIO_IN_END,
      WS_BIN_AUDIO_OUT: WS_BIN_AUDIO_OUT,
      WS_BIN_AUDIO_SEGMENT_END: WS_BIN_AUDIO_SEGMENT_END,

      // Context limits
      DEFAULT_CONTEXT_MAX: DEFAULT_CONTEXT_MAX,
   };
})(window);
