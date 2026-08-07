/**
 * DAWN Music Playback Module
 * Streams and plays music from WebUI server using Opus codec
 *
 * Usage:
 *   DawnMusicPlayback.subscribe(quality)  // Start receiving music stream
 *   DawnMusicPlayback.unsubscribe()       // Stop receiving stream
 *   DawnMusicPlayback.control(action, opts)  // Control playback
 *   DawnMusicPlayback.search(query)       // Search music library
 *   DawnMusicPlayback.getState()          // Get current playback state
 *   DawnMusicPlayback.setCallbacks({ onStateChange, onPositionUpdate, onError })
 */
(function (global) {
   'use strict';

   /**
    * @typedef {Object} DawnMusicTrack
    * @property {string} [path]          - File path (server-relative).
    * @property {string} [title]         - Track title; may be empty for files without metadata.
    * @property {string} [artist]        - Track artist; may be empty.
    * @property {string} [album]         - Track album; may be empty.
    * @property {number} [duration_sec]  - Track duration in seconds (server-reported).
    */

   /**
    * @typedef {Object} DawnMusicPlaybackState
    * Snapshot returned by getState() and passed to onStateChange.  Stable
    * shape consumed across the WebUI (music.js, media-session.js, etc.) —
    * field renames break those consumers silently; if you change any of
    * these names, update every reader.
    *
    * @property {boolean} playing         - True while actively playing audio.
    * @property {boolean} paused          - True while paused mid-track.
    * @property {boolean} buffering       - True while initial buffer fills.
    * @property {DawnMusicTrack|null} track - Current track (or null when none).
    * @property {number} positionSec      - Current playback position (seconds).
    * @property {number} durationSec      - Track duration (seconds).
    * @property {number} queueLength      - Total tracks queued.
    * @property {number} queueIndex       - Index of current track in queue.
    * @property {string} sourceFormat     - Source codec/container (e.g., 'flac').
    * @property {number} sourceRate       - Source sample rate (Hz).
    * @property {string} quality          - Streaming quality preset.
    * @property {number} bitrate          - Streaming bitrate (bps).
    * @property {string} bitrateMode      - 'cbr' / 'vbr' / etc.
    * @property {boolean} shuffle         - Shuffle mode on/off.
    * @property {number} repeatMode       - 0=none, 1=all, 2=one.
    * @property {boolean} subscribed      - Subscribed to the music stream socket.
    * @property {number} volume           - 0..1 client-side gain.
    * @property {number} bufferPercent    - 0..100.
    * @property {number} bufferedMs        - Total client-side buffered audio (worklet + decode queue), ms.
    */

   // Playback state
   const state = {
      // Playback
      playing: false,
      paused: false,
      buffering: false,

      // Current track
      track: null, // { path, title, artist, album, duration_sec }
      positionSec: 0,
      durationSec: 0,

      // Queue
      queueLength: 0,
      queueIndex: 0,

      // Source info
      sourceFormat: '--',
      sourceRate: 0,

      // Quality info
      quality: 'standard',
      bitrate: 96000,
      bitrateMode: 'vbr',

      // Playback modes
      shuffle: false,
      repeatMode: 0, // 0=none, 1=all, 2=one

      // Subscription state
      subscribed: false,

      // Audio settings
      volume: 0.8,
      bufferPercent: 0,
      bufferedMs: 0,
   };

   // Audio playback
   let audioContext = null;
   let analyser = null;
   let fftDataArray = null;
   let opusDecoder = null;
   let gainNode = null;
   let workletNode = null;

   // ms of audio per Opus frame (960 samples @ 48kHz). Used to convert the
   // WebCodecs decode-queue backlog into a duration for buffer reporting.
   const MUSIC_FRAME_MS = 20;

   // Primary path: a Web Worker owns the music WebSocket + Opus decode and posts
   // PCM straight to the worklet over a MessagePort, so decode is off the main
   // thread (immune to TTS/UI contention). Falls back to the in-file main-thread
   // decode path when a Worker can't be created. Decided once in initOpusDecoder().
   let useWorkerPath = false;
   let decodeWorker = null;
   // Server-advertised music-stream config, relayed to the worker (mirrors the
   // fields on MusicStreamConnection used by the fallback path).
   const serverCfg = { port: null, enabled: null };
   let isDecoderReady = false;
   let decodeTimestamp = 0;

   // Callbacks
   let callbacks = {
      onStateChange: null, // (state) => void
      onPositionUpdate: null, // (positionSec, durationSec) => void
      onBufferUpdate: null, // (percent) => void
      onError: null, // (code, message) => void
      onSearchResults: null, // (results) => void
      onLibraryResponse: null, // (data) => void
      onQueueResponse: null, // (queue) => void
   };

   // ==========================================================================
   // Dedicated Music Stream Connection
   // ==========================================================================

   /**
    * Dedicated WebSocket connection for music audio streaming.
    * Runs on separate port (main+1) to isolate high-bandwidth audio
    * from control messages on the main WebSocket.
    */
   const MusicStreamConnection = {
      ws: null,
      connected: false,
      authenticated: false,
      reconnectTimer: null,
      retryCount: 0,
      maxRetries: 5,
      authFailed: false, // True when server rejects token — stop retrying
      // Advertised by the server's `config` frame. serverPort null = not yet
      // advertised (fall back to main+1); serverEnabled false = music server off.
      serverPort: null,
      serverEnabled: null,

      /**
       * Connect to the dedicated music streaming server
       */
      connect() {
         if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            return; // Already connected
         }

         // Don't retry if auth was rejected — wait for fresh token
         if (this.authFailed) {
            console.log('Music stream: Auth failed previously, waiting for new session token');
            return;
         }

         // Skip the dedicated socket entirely when the server says music is off.
         if (this.serverEnabled === false) {
            console.log('Music stream: server reports music disabled; not connecting');
            return;
         }

         const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
         const host = window.location.hostname;
         const mainPort = parseInt(window.location.port || '8080', 10);
         // Prefer the server-advertised port; fall back to main+1 for older servers.
         const musicPort = this.serverPort || mainPort + 1;
         const url = `${protocol}//${host}:${musicPort}`;

         console.log('Music stream: Connecting to', url, '(attempt', this.retryCount + 1 + ')');

         try {
            this.ws = new WebSocket(url, 'dawn-music');
            this.ws.binaryType = 'arraybuffer';

            this.ws.onopen = () => this.handleOpen();
            this.ws.onclose = () => this.handleClose();
            this.ws.onmessage = (e) => this.handleMessage(e);
            this.ws.onerror = (e) => this.handleError(e);
         } catch (e) {
            console.error('Music stream: Failed to create WebSocket:', e);
         }
      },

      /**
       * Disconnect from music server
       */
      disconnect() {
         if (this.reconnectTimer) {
            clearTimeout(this.reconnectTimer);
            this.reconnectTimer = null;
         }
         if (this.ws) {
            this.ws.close();
            this.ws = null;
         }
         this.connected = false;
         this.authenticated = false;
      },

      /**
       * Reconnect with a fresh session token (called when main WS gets new token)
       */
      reconnectWithFreshToken() {
         this.disconnect();
         this.retryCount = 0;
         this.authFailed = false;
         if (state.subscribed) {
            this.connect();
         }
      },

      /**
       * Handle connection open - send authentication
       */
      handleOpen() {
         this.connected = true;
         console.log('Music stream: Connected');

         // Get session token from main WebSocket
         const token = DawnWS.getSessionToken ? DawnWS.getSessionToken() : null;
         if (token) {
            this.ws.send(JSON.stringify({ type: 'auth', token: token }));
         } else {
            console.warn('Music stream: No session token available for auth');
         }
      },

      /**
       * Handle connection close - attempt reconnect with backoff
       */
      handleClose() {
         this.connected = false;
         this.authenticated = false;
         console.log('Music stream: Disconnected');

         // Don't reconnect if auth was explicitly rejected
         if (this.authFailed) {
            return;
         }

         // Exponential backoff: 2s, 4s, 8s, 16s, 30s cap
         if (this.retryCount < this.maxRetries && DawnWS.isConnected && DawnWS.isConnected()) {
            const delay = Math.min(2000 * Math.pow(2, this.retryCount), 30000);
            this.retryCount++;
            console.log(
               'Music stream: Reconnecting in',
               delay + 'ms',
               '(retry ' + this.retryCount + '/' + this.maxRetries + ')'
            );
            this.reconnectTimer = setTimeout(() => {
               this.connect();
            }, delay);
         } else if (this.retryCount >= this.maxRetries) {
            console.warn('Music stream: Max retries reached, waiting for new session token');
         }
      },

      /**
       * Handle incoming message (auth response or binary audio)
       */
      handleMessage(event) {
         if (event.data instanceof ArrayBuffer) {
            // Binary audio data - route to decoder
            handleMusicData(event.data);
         } else {
            // JSON message (auth response)
            try {
               const msg = JSON.parse(event.data);
               if (msg.type === 'auth_ok') {
                  this.authenticated = true;
                  this.retryCount = 0; // Reset on successful auth
                  this.authFailed = false;
                  console.log('Music stream: Authenticated');
               } else if (msg.type === 'auth_failed') {
                  console.error('Music stream: Auth failed:', msg.reason);
                  this.authFailed = true; // Stop retrying with stale token
                  this.ws.close();
               }
            } catch (e) {
               console.warn('Music stream: Invalid JSON:', e);
            }
         }
      },

      /**
       * Handle connection error
       */
      handleError(event) {
         console.error('Music stream: WebSocket error', event);
      },

      /**
       * Check if connected and authenticated
       */
      isReady() {
         return this.connected && this.authenticated;
      },

      /**
       * Send a JSON control message up the music socket (e.g. buffer reports for
       * closed-loop flow control). No-op unless authenticated and the socket is open.
       */
      send(obj) {
         if (!this.isReady() || !this.ws || this.ws.readyState !== WebSocket.OPEN) {
            return;
         }
         try {
            this.ws.send(JSON.stringify(obj));
         } catch (e) {
            // Socket may be tearing down; drop silently — reports are periodic.
         }
      },
   };

   // Opus always encodes at 48kHz
   const OPUS_SAMPLE_RATE = 48000;

   /**
    * Initialize Opus decoder for stereo music streaming
    */
   async function initOpusDecoder() {
      if (isDecoderReady) return;

      try {
         // Create audio context at 48kHz to match Opus native rate
         const AudioContext = window.AudioContext || window.webkitAudioContext;
         audioContext = new AudioContext({ sampleRate: OPUS_SAMPLE_RATE });

         // Check if browser honored our sample rate request
         if (audioContext.sampleRate !== OPUS_SAMPLE_RATE) {
            console.warn(
               'Music playback: Requested 48kHz but got',
               audioContext.sampleRate,
               '- audio may play at wrong speed'
            );
         }
         console.log('Music playback: AudioContext sample rate:', audioContext.sampleRate);

         // Create gain node for volume control
         gainNode = audioContext.createGain();
         gainNode.gain.value = 0.8;

         // Create analyser for visualization
         analyser = audioContext.createAnalyser();
         analyser.fftSize = 2048; // Higher resolution for better frequency detail
         analyser.smoothingTimeConstant = 0.6;
         analyser.minDecibels = -70; // Capture quieter signals
         analyser.maxDecibels = -10;
         fftDataArray = new Uint8Array(analyser.frequencyBinCount);

         // Connect: gain -> analyser -> destination
         gainNode.connect(analyser);
         analyser.connect(audioContext.destination);

         // Create the AudioWorklet node first — both paths render through it.
         try {
            await audioContext.audioWorklet.addModule('js/audio/music-worklet-processor.js');
            workletNode = new AudioWorkletNode(audioContext, 'music-processor', {
               outputChannelCount: [2],
            });
            workletNode.connect(gainNode);
         } catch (workletError) {
            console.error('Music playback: AudioWorklet failed:', workletError);
            return;
         }

         // Prefer the worker decode path (decode off the main thread); fall back to
         // in-file main-thread decode if a Worker can't be created.
         useWorkerPath = typeof Worker !== 'undefined';
         if (useWorkerPath) {
            try {
               setupWorkerPath();
            } catch (e) {
               console.warn('Music playback: worker decode path unavailable, using main-thread', e);
               useWorkerPath = false;
               if (decodeWorker) {
                  try {
                     decodeWorker.terminate();
                  } catch (_) {
                     /* ignore */
                  }
                  decodeWorker = null;
               }
            }
         }
         if (!useWorkerPath) {
            if (!(await setupMainDecoder())) return; // unsupported codec
         }

         isDecoderReady = true;
         console.log(
            'Music playback:',
            useWorkerPath ? 'worker decode path' : 'main-thread decode'
         );
      } catch (e) {
         console.error('Music playback: Failed to initialize decoder:', e);
      }
   }

   /**
    * Worker decode path: hand a MessagePort to the worklet (via its node port), spin
    * up the decode worker with the other end, and seed it with server config + token.
    * The worklet takes audio + sends buffer reports on the worker port; the worker
    * combines them with decodeQueueSize and reports to the server. Main only receives
    * a periodic buffered-depth status (for the UI bar + position correction).
    */
   function setupWorkerPath() {
      // Construct the Worker FIRST. If it throws (CSP worker-src, SecurityError — none
      // caught by the typeof Worker gate), we fall back to main-thread decode with the
      // worklet untouched. Handing the port to the worklet before this would latch a
      // dead reportPort, silently breaking flow control on the very fallback it exists for.
      decodeWorker = new Worker('js/audio/music-decode-worker.js');
      decodeWorker.onerror = (e) => {
         console.error('Music playback: decode worker error', (e && e.message) || e);
      };
      decodeWorker.onmessage = (e) => {
         const d = e.data;
         if (d && d.type === 'status' && typeof d.bufferedMs === 'number') {
            state.bufferedMs = d.bufferedMs;
            // bufferPercent here is derived from bufferedMs (10s ring == 100%). The
            // fallback path derives it from the worklet's samplesAvailable/bufferSize.
            // Both are keyed to the same 10s CLIENT_BUFFER ring, so they coincide — keep
            // the ring size and this /100 divisor in sync if either changes.
            state.bufferPercent = Math.min(100, Math.round(d.bufferedMs / 100));
            if (callbacks.onBufferUpdate) callbacks.onBufferUpdate(state.bufferPercent);
         }
      };

      const ch = new MessageChannel();
      // processorOptions can't carry a Transferable, so hand p2 over via the node port.
      workletNode.port.postMessage({ type: 'workerPort', port: ch.port2 }, [ch.port2]);
      decodeWorker.postMessage({ type: 'workletPort', port: ch.port1 }, [ch.port1]);
      decodeWorker.postMessage({
         type: 'setServer',
         port: serverCfg.port,
         enabled: serverCfg.enabled,
      });
      const tok = DawnWS.getSessionToken ? DawnWS.getSessionToken() : null;
      decodeWorker.postMessage({ type: 'setToken', token: tok });
   }

   /**
    * Fallback main-thread decode path: WebCodecs AudioDecoder on the main thread,
    * worklet reports back to main which adds decodeQueueSize and reports to the server.
    * @returns {Promise<boolean>} false if the codec is unsupported
    */
   async function setupMainDecoder() {
      if (typeof AudioDecoder === 'undefined') {
         console.error('Music playback: WebCodecs AudioDecoder not available');
         return false;
      }
      const decoderConfig = { codec: 'opus', sampleRate: OPUS_SAMPLE_RATE, numberOfChannels: 2 };
      const support = await AudioDecoder.isConfigSupported(decoderConfig);
      if (!support.supported) {
         console.error('Music playback: Stereo Opus decoding at 48kHz not supported');
         return false;
      }
      opusDecoder = new AudioDecoder({
         output: handleDecodedAudio,
         error: (e) => console.error('Music decoder error:', e),
      });
      opusDecoder.configure(decoderConfig);

      // Buffer report (main path): worklet -> main; add decodeQueueSize; report to server.
      workletNode.port.onmessage = (e) => {
         if (e.data.type === 'buffer') {
            state.bufferPercent = e.data.percent;
            if (typeof e.data.bufferedMs === 'number') {
               const decodeBacklogMs =
                  opusDecoder && opusDecoder.decodeQueueSize
                     ? opusDecoder.decodeQueueSize * MUSIC_FRAME_MS
                     : 0;
               state.bufferedMs = e.data.bufferedMs + decodeBacklogMs;
               MusicStreamConnection.send({ type: 'music_buffer', buffered_ms: state.bufferedMs });
            }
            if (callbacks.onBufferUpdate) callbacks.onBufferUpdate(e.data.percent);
         }
      };
      return true;
   }

   /**
    * Flush the client audio buffer on a playback discontinuity. Worker path: tell the
    * worker (resets its decoder + drops pending + clears the worklet, all ordered).
    * Fallback: clear the worklet directly.
    */
   function flushWorklet() {
      if (useWorkerPath && decodeWorker) {
         decodeWorker.postMessage({ type: 'clear' });
      } else if (workletNode) {
         workletNode.port.postMessage({ type: 'clear' });
      }
   }

   /** Resume a suspended AudioContext (call on the play/control user gesture — the
    *  worker path never sees the audio data, so resume can't ride on data arrival). */
   function resumeContext() {
      if (audioContext && audioContext.state === 'suspended') {
         audioContext.resume().catch(() => {});
      }
   }

   // Debug: count decoded frames
   let decodedFrameCount = 0;

   /**
    * Handle decoded audio from WebCodecs
    */
   function handleDecodedAudio(audioData) {
      try {
         const numFrames = audioData.numberOfFrames;
         const numChannels = audioData.numberOfChannels;
         const format = audioData.format;

         // Log first few decodes to check format
         if (decodedFrameCount < 5) {
            console.log(
               'Music decode:',
               'frames=' + numFrames,
               'channels=' + numChannels,
               'format=' + format,
               'sampleRate=' + audioData.sampleRate
            );
         }
         decodedFrameCount++;

         let left, right;

         if (format === 'f32-planar') {
            // Planar format: each channel is a separate plane
            left = new Float32Array(numFrames);
            right = new Float32Array(numFrames);

            audioData.copyTo(left, { planeIndex: 0 });
            if (numChannels > 1) {
               audioData.copyTo(right, { planeIndex: 1 });
            } else {
               right.set(left);
            }
         } else if (format === 'f32') {
            // Interleaved format: L R L R L R ...
            const interleaved = new Float32Array(numFrames * numChannels);
            audioData.copyTo(interleaved, { planeIndex: 0 });

            left = new Float32Array(numFrames);
            right = new Float32Array(numFrames);

            if (numChannels >= 2) {
               for (let i = 0; i < numFrames; i++) {
                  left[i] = interleaved[i * numChannels];
                  right[i] = interleaved[i * numChannels + 1];
               }
            } else {
               // Mono interleaved
               for (let i = 0; i < numFrames; i++) {
                  left[i] = interleaved[i];
                  right[i] = interleaved[i];
               }
            }
         } else {
            // Handle s16 or other formats by converting
            const bytesPerSample = format.startsWith('s16') ? 2 : 4;
            const totalSamples = numFrames * numChannels;
            const buffer = new ArrayBuffer(totalSamples * bytesPerSample);

            audioData.copyTo(buffer, { planeIndex: 0 });

            left = new Float32Array(numFrames);
            right = new Float32Array(numFrames);

            if (format === 's16') {
               // Interleaved signed 16-bit
               const int16 = new Int16Array(buffer);
               if (numChannels >= 2) {
                  for (let i = 0; i < numFrames; i++) {
                     left[i] = int16[i * numChannels] / 32768.0;
                     right[i] = int16[i * numChannels + 1] / 32768.0;
                  }
               } else {
                  for (let i = 0; i < numFrames; i++) {
                     left[i] = int16[i] / 32768.0;
                     right[i] = left[i];
                  }
               }
            } else if (format === 's16-planar') {
               // Planar signed 16-bit
               const int16 = new Int16Array(buffer);
               for (let i = 0; i < numFrames; i++) {
                  left[i] = int16[i] / 32768.0;
               }
               if (numChannels > 1) {
                  audioData.copyTo(buffer, { planeIndex: 1 });
                  const int16Right = new Int16Array(buffer);
                  for (let i = 0; i < numFrames; i++) {
                     right[i] = int16Right[i] / 32768.0;
                  }
               } else {
                  right.set(left);
               }
            } else {
               console.warn('Music playback: Unsupported format:', format);
               audioData.close();
               return;
            }
         }

         // Send to AudioWorklet for playback using transferable objects
         if (workletNode) {
            workletNode.port.postMessage({ type: 'audio', left, right }, [
               left.buffer,
               right.buffer,
            ]);
         }
         audioData.close();
      } catch (e) {
         console.error(
            'Music playback: Error handling decoded audio:',
            e,
            'format:',
            audioData?.format
         );
      }
   }

   /**
    * Handle incoming music audio data
    * @param {ArrayBuffer} data - Binary message with Opus data
    */
   async function handleMusicData(data) {
      // Initialize decoder if needed
      if (!isDecoderReady) {
         await initOpusDecoder();
      }

      // Resume context if suspended (browser autoplay policy)
      if (audioContext && audioContext.state === 'suspended') {
         await audioContext.resume();
      }

      if (!opusDecoder || opusDecoder.state === 'closed') {
         return;
      }

      const view = new DataView(data);

      // Skip first byte (message type)
      let offset = 1;

      // Parse length-prefixed Opus frames and decode each one
      while (offset + 2 <= data.byteLength) {
         const frameLen = view.getUint16(offset, true); // little-endian
         offset += 2;

         if (frameLen === 0 || frameLen > 1500 || offset + frameLen > data.byteLength) {
            console.warn('Music playback: Invalid frame length', frameLen);
            break;
         }

         const opusFrame = new Uint8Array(data, offset, frameLen);
         offset += frameLen;

         try {
            // Create EncodedAudioChunk and decode
            const chunk = new EncodedAudioChunk({
               type: 'key',
               timestamp: decodeTimestamp,
               data: opusFrame,
            });

            opusDecoder.decode(chunk);
            decodeTimestamp += 20000; // 20ms in microseconds
         } catch (e) {
            console.warn('Music playback: Decode error:', e);
         }
      }

      state.buffering = false;
   }

   /**
    * Handle music segment end (not used for streaming, kept for compatibility)
    */
   function handleMusicSegmentEnd() {
      // For streaming playback, we don't need segment markers
      // Audio is played continuously as it's decoded
   }

   /**
    * Subscribe to music stream
    * @param {string} quality - Quality tier (voice, standard, high, hifi)
    * @param {string} bitrateMode - Bitrate mode (vbr, cbr)
    */
   async function subscribe(quality, bitrateMode) {
      await initOpusDecoder();
      // Resume the context on this user gesture (worker path never sees data arrival).
      resumeContext();

      // Connect to the dedicated music streaming server (worker owns the WS in the
      // worker path; the main-thread MusicStreamConnection owns it in the fallback).
      if (useWorkerPath && decodeWorker) {
         const tok = DawnWS.getSessionToken ? DawnWS.getSessionToken() : null;
         decodeWorker.postMessage({ type: 'setToken', token: tok });
         decodeWorker.postMessage({ type: 'subscribe' });
      } else {
         MusicStreamConnection.connect();
      }

      // Build payload - omit fields to use server defaults
      const payload = {};
      if (quality) payload.quality = quality;
      if (bitrateMode) payload.bitrate_mode = bitrateMode;

      // Send subscribe to main socket (for control/state messages)
      DawnWS.send({
         type: 'music_subscribe',
         payload: payload,
      });

      state.subscribed = true;
      if (quality) state.quality = quality;
      console.log(
         'Music playback: Subscribed',
         quality ? `(quality: ${quality})` : '(server defaults)'
      );
   }

   /**
    * Set playback volume
    * @param {number} volume - Volume level 0.0 to 1.0
    */
   function setVolume(volume) {
      state.volume = Math.max(0, Math.min(1, volume));
      if (gainNode && state.playing && !state.paused) {
         // Only apply volume if actively playing (not muted due to pause)
         gainNode.gain.value = state.volume;
      }
   }

   /**
    * Update streaming quality and/or bitrate mode
    * Sends a new subscribe message with the updated settings.
    * @param {string} quality - Quality tier (voice, standard, high, hifi)
    * @param {string} bitrateMode - Bitrate mode (vbr, cbr)
    */
   function updateQuality(quality, bitrateMode) {
      if (!state.subscribed) return;

      const payload = {};
      if (quality) {
         payload.quality = quality;
         state.quality = quality;
      }
      if (bitrateMode) payload.bitrate_mode = bitrateMode;

      DawnWS.send({
         type: 'music_subscribe',
         payload: payload,
      });
      console.log(
         'Music playback: Settings updated',
         quality ? `quality: ${quality}` : '',
         bitrateMode ? `bitrate: ${bitrateMode}` : ''
      );
   }

   /**
    * Unsubscribe from music stream
    */
   function unsubscribe() {
      // Disconnect from the dedicated music server (worker or main-thread owner).
      if (useWorkerPath && decodeWorker) {
         decodeWorker.postMessage({ type: 'unsubscribe' });
      } else {
         MusicStreamConnection.disconnect();
      }

      DawnWS.send({ type: 'music_unsubscribe' });

      state.subscribed = false;
      state.playing = false;
      state.paused = false;
      state.track = null;
      decodeTimestamp = 0;

      flushWorklet();

      notifyStateChange();
      console.log('Music playback: Unsubscribed');
   }

   /**
    * Send playback control command
    * @param {string} action - Control action (play, pause, stop, next, previous, seek)
    * @param {object} opts - Additional options (query for play, position_sec for seek)
    */
   function control(action, opts = {}) {
      // Clear audio buffer immediately for actions that change playback position
      // (optimistic — before the server echoes music_state) so the UI feels responsive.
      const bufferClearActions = ['seek', 'next', 'previous', 'play_index', 'stop'];
      if (bufferClearActions.includes(action)) {
         flushWorklet();
      }

      const payload = { action: action, ...opts };
      DawnWS.send({ type: 'music_control', payload: payload });
   }

   /**
    * Search music library
    * @param {string} query - Search query
    * @param {number} limit - Maximum results (default 50)
    */
   function search(query, limit = 50) {
      DawnWS.send({
         type: 'music_search',
         payload: { query: query, limit: limit },
      });
   }

   /**
    * Browse music library
    * @param {string} browseType - Browse type (stats, artists, albums)
    */
   function browseLibrary(browseType = 'stats') {
      DawnWS.send({
         type: 'music_library',
         payload: { type: browseType },
      });
   }

   /**
    * Manage playback queue
    * @param {string} action - Queue action (list, add, remove, clear)
    * @param {object} opts - Action options
    */
   function queue(action, opts = {}) {
      DawnWS.send({
         type: 'music_queue',
         payload: { action: action, ...opts },
      });
   }

   /**
    * Handle music state update from server
    * @param {object} payload - State payload
    */
   function handleStateUpdate(payload) {
      const wasPlaying = state.playing && !state.paused;
      const prevQueueIndex = state.queueIndex;
      const prevPosition = state.positionSec;

      state.playing = payload.playing || false;
      state.paused = payload.paused || false;
      state.track = payload.track || null;
      state.positionSec = payload.position_sec || 0;
      state.queueLength = payload.queue_length || 0;
      state.queueIndex = payload.queue_index || 0;
      state.sourceFormat = payload.source_format || '--';
      state.sourceRate = payload.source_rate || 0;
      state.quality = payload.quality || 'standard';
      state.bitrate = payload.bitrate || 96000;
      state.bitrateMode = payload.bitrate_mode || 'vbr';
      state.shuffle = payload.shuffle || false;
      state.repeatMode = payload.repeat_mode || 0;

      // Sync volume from server if present
      if (payload.volume !== undefined) {
         state.volume = payload.volume;
      }

      if (state.track) {
         state.durationSec = state.track.duration_sec || 0;
      }

      // Detect track change or seek (position jump > 2 seconds)
      const trackChanged = state.queueIndex !== prevQueueIndex;
      const seeked = Math.abs(state.positionSec - prevPosition) > 2;

      // A server-tagged natural auto-advance is gapless: the server keeps streaming
      // the next track into our existing buffered lead, and both the index change
      // and the position jump to ~0 are expected. Suppress the flush so we don't
      // truncate the ~2s of already-buffered audio (which includes the start of the
      // next track). User-initiated skips/seeks are NOT tagged, so they still flush.
      const autoAdvance = payload.advance === 'auto';

      // Clear audio buffer on track change or seek for instant response
      if ((trackChanged || seeked) && !autoAdvance) {
         flushWorklet();
      }

      // Handle audio output based on playback state
      const isPlaying = state.playing && !state.paused;
      if (gainNode) {
         if (!isPlaying && wasPlaying) {
            // Mute immediately when pausing/stopping
            gainNode.gain.setValueAtTime(0, audioContext.currentTime);
         } else if (isPlaying && !wasPlaying) {
            // Clear stale audio from ring buffer before resuming —
            // buffer holds pre-pause audio at the wrong position
            flushWorklet();
            // Resume the context (worker path can't resume on data arrival) + volume.
            resumeContext();
            gainNode.gain.setValueAtTime(state.volume, audioContext.currentTime);
         }
      }

      // Clear buffer when stopped (not paused)
      if (!state.playing) {
         flushWorklet();
      }

      notifyStateChange();
   }

   /**
    * Handle position update from server
    * @param {object} payload - Position payload
    */
   function handlePositionUpdate(payload) {
      // Keep the raw decode position in state (used for seek detection); surface an
      // audible-corrected position to the UI. The server streams a ~2s lead, so its
      // decode position runs ahead of what the user hears — subtract our own buffered
      // depth (ground truth, from the worklet) so the progress bar tracks playback.
      state.positionSec = payload.position_sec || 0;
      state.durationSec = payload.duration_sec || state.durationSec;

      if (callbacks.onPositionUpdate) {
         const bufferedSec = (state.bufferedMs || 0) / 1000;
         const audibleSec = Math.max(0, state.positionSec - bufferedSec);
         callbacks.onPositionUpdate(audibleSec, state.durationSec);
      }
   }

   /**
    * Handle error from server
    * @param {object} payload - Error payload
    */
   function handleError(payload) {
      console.error('Music playback error:', payload.code, payload.message);

      if (callbacks.onError) {
         callbacks.onError(payload.code, payload.message);
      }
   }

   /**
    * Handle search results from server
    * @param {object} payload - Search results payload
    */
   function handleSearchResults(payload) {
      if (callbacks.onSearchResults) {
         callbacks.onSearchResults(payload);
      }
   }

   /**
    * Handle library response from server
    * @param {object} payload - Library response payload
    */
   function handleLibraryResponse(payload) {
      if (callbacks.onLibraryResponse) {
         callbacks.onLibraryResponse(payload);
      }
   }

   /**
    * Handle queue response from server
    * @param {object} payload - Queue response payload
    */
   function handleQueueResponse(payload) {
      if (callbacks.onQueueResponse) {
         callbacks.onQueueResponse(payload);
      }
   }

   /**
    * Notify state change callback
    */
   function notifyStateChange() {
      if (callbacks.onStateChange) {
         callbacks.onStateChange({ ...state });
      }
   }

   /**
    * Get current state
    * @returns {object} Current playback state
    */
   function getState() {
      return { ...state };
   }

   /**
    * Check if subscribed to music stream
    * @returns {boolean}
    */
   function isSubscribed() {
      return state.subscribed;
   }

   /**
    * Check if music is playing
    * @returns {boolean}
    */
   function isPlaying() {
      return state.playing && !state.paused;
   }

   /**
    * Get analyser node for visualization
    * @returns {AnalyserNode|null}
    */
   function getAnalyser() {
      return analyser;
   }

   /**
    * Get FFT data array for visualization
    * @returns {Uint8Array|null}
    */
   function getFFTData() {
      return fftDataArray;
   }

   /**
    * Set callbacks
    * @param {object} cbs - Callback functions
    */
   function setCallbacks(cbs) {
      if (cbs.onStateChange) callbacks.onStateChange = cbs.onStateChange;
      if (cbs.onPositionUpdate) callbacks.onPositionUpdate = cbs.onPositionUpdate;
      if (cbs.onError) callbacks.onError = cbs.onError;
      if (cbs.onSearchResults) callbacks.onSearchResults = cbs.onSearchResults;
      if (cbs.onLibraryResponse) callbacks.onLibraryResponse = cbs.onLibraryResponse;
      if (cbs.onQueueResponse) callbacks.onQueueResponse = cbs.onQueueResponse;
   }

   /**
    * Handle binary message from WebSocket
    * Called from dawn.js message router
    * @param {ArrayBuffer} data - Binary message data
    */
   function handleBinaryMessage(data) {
      const view = new Uint8Array(data);
      if (view.length === 0) return;

      const messageType = view[0];

      switch (messageType) {
         case DawnConfig.WS_BIN_MUSIC_DATA:
            handleMusicData(data);
            break;
         case DawnConfig.WS_BIN_MUSIC_SEGMENT_END:
            handleMusicSegmentEnd();
            break;
      }
   }

   /**
    * Handle JSON message from WebSocket
    * Called from dawn.js message router
    * @param {object} message - Parsed JSON message
    */
   function handleJsonMessage(message) {
      switch (message.type) {
         case 'music_state':
            handleStateUpdate(message.payload);
            break;
         case 'music_position':
            handlePositionUpdate(message.payload);
            break;
         case 'music_error':
            handleError(message.payload);
            break;
         case 'music_search_response':
            handleSearchResults(message.payload);
            break;
         case 'music_library_response':
            handleLibraryResponse(message.payload);
            break;
         case 'music_queue_response':
            handleQueueResponse(message.payload);
            break;
      }
   }

   /**
    * Reconnect music stream with fresh token (called when main WS gets new session)
    */
   function reconnectMusicStream() {
      if (useWorkerPath && decodeWorker) {
         // Relay the fresh token; the worker resets its auth latch and reconnects.
         const tok = DawnWS.getSessionToken ? DawnWS.getSessionToken() : null;
         decodeWorker.postMessage({ type: 'setToken', token: tok });
      } else {
         MusicStreamConnection.reconnectWithFreshToken();
      }
   }

   // Expose globally
   global.DawnMusicPlayback = {
      subscribe: subscribe,
      unsubscribe: unsubscribe,
      control: control,
      search: search,
      browseLibrary: browseLibrary,
      queue: queue,
      getState: getState,
      isSubscribed: isSubscribed,
      isPlaying: isPlaying,
      getAnalyser: getAnalyser,
      getFFTData: getFFTData,
      setCallbacks: setCallbacks,
      setVolume: setVolume,
      updateQuality: updateQuality,
      handleBinaryMessage: handleBinaryMessage,
      handleJsonMessage: handleJsonMessage,
      reconnectMusicStream: reconnectMusicStream,
      setServerConfig: setServerConfig,
   };

   /**
    * Record the music-stream server details advertised in the `config` frame so
    * the dedicated socket targets the real port instead of assuming main+1, and
    * can skip connecting entirely when the server has music disabled.
    * @param {number} port - Advertised music-stream port
    * @param {boolean} enabled - Whether the music server is running
    */
   function setServerConfig(port, enabled) {
      if (typeof port === 'number' && port > 0) {
         MusicStreamConnection.serverPort = port;
         serverCfg.port = port;
      }
      if (typeof enabled === 'boolean') {
         MusicStreamConnection.serverEnabled = enabled;
         serverCfg.enabled = enabled;
      }
      // Relay to the worker if the worker path is live.
      if (decodeWorker) {
         decodeWorker.postMessage({
            type: 'setServer',
            port: serverCfg.port,
            enabled: serverCfg.enabled,
         });
      }
   }
})(window);
