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
   };

   // Audio playback
   let audioContext = null;
   let analyser = null;
   let fftDataArray = null;
   let opusDecoder = null;
   let gainNode = null;
   let workletNode = null;
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

      /**
       * Connect to the dedicated music streaming server
       */
      connect() {
         if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            return; // Already connected
         }

         const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
         const host = window.location.hostname;
         const mainPort = parseInt(window.location.port || '8080', 10);
         const musicPort = mainPort + 1;
         const url = `${protocol}//${host}:${musicPort}`;

         console.log('Music stream: Connecting to', url);

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
       * Handle connection close - attempt reconnect
       */
      handleClose() {
         this.connected = false;
         this.authenticated = false;
         console.log('Music stream: Disconnected');

         // Attempt reconnect after 2 seconds if main socket is connected
         if (DawnWS.isConnected && DawnWS.isConnected()) {
            this.reconnectTimer = setTimeout(() => {
               console.log('Music stream: Attempting reconnect...');
               this.connect();
            }, 2000);
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
                  console.log('Music stream: Authenticated');
               } else if (msg.type === 'auth_failed') {
                  console.error('Music stream: Auth failed:', msg.reason);
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

         // Create stereo Opus decoder using WebCodecs
         if (typeof AudioDecoder === 'undefined') {
            console.error('Music playback: WebCodecs AudioDecoder not available');
            return;
         }

         // Opus is always encoded at 48kHz
         const decoderConfig = {
            codec: 'opus',
            sampleRate: OPUS_SAMPLE_RATE,
            numberOfChannels: 2, // Stereo
         };

         const support = await AudioDecoder.isConfigSupported(decoderConfig);
         if (!support.supported) {
            console.error('Music playback: Stereo Opus decoding at 48kHz not supported');
            return;
         }

         opusDecoder = new AudioDecoder({
            output: handleDecodedAudio,
            error: (e) => {
               console.error('Music decoder error:', e);
            },
         });

         opusDecoder.configure(decoderConfig);

         // Create AudioWorkletNode for glitch-free playback on audio thread
         try {
            await audioContext.audioWorklet.addModule('js/audio/music-worklet-processor.js');
            workletNode = new AudioWorkletNode(audioContext, 'music-processor', {
               outputChannelCount: [2],
            });
            workletNode.connect(gainNode);

            // Receive buffer status updates from worklet
            workletNode.port.onmessage = (e) => {
               if (e.data.type === 'buffer') {
                  state.bufferPercent = e.data.percent;
                  if (callbacks.onBufferUpdate) {
                     callbacks.onBufferUpdate(e.data.percent);
                  }
               }
            };

            console.log('Music playback: Using AudioWorklet for playback');
         } catch (workletError) {
            console.error('Music playback: AudioWorklet failed:', workletError);
            return;
         }

         isDecoderReady = true;
         console.log('Music playback: Stereo Opus decoder initialized at 48kHz');
      } catch (e) {
         console.error('Music playback: Failed to initialize decoder:', e);
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

      // Connect to dedicated music streaming server
      MusicStreamConnection.connect();

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
      // Disconnect from dedicated music server
      MusicStreamConnection.disconnect();

      DawnWS.send({ type: 'music_unsubscribe' });

      state.subscribed = false;
      state.playing = false;
      state.paused = false;
      state.track = null;
      decodeTimestamp = 0;

      // Clear worklet buffer
      if (workletNode) {
         workletNode.port.postMessage({ type: 'clear' });
      }

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
      // This makes the UI feel more responsive
      const bufferClearActions = ['seek', 'next', 'previous', 'play_index', 'stop'];
      if (bufferClearActions.includes(action) && workletNode) {
         workletNode.port.postMessage({ type: 'clear' });
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

      if (state.track) {
         state.durationSec = state.track.duration_sec || 0;
      }

      // Detect track change or seek (position jump > 2 seconds)
      const trackChanged = state.queueIndex !== prevQueueIndex;
      const seeked = Math.abs(state.positionSec - prevPosition) > 2;

      // Clear audio buffer on track change or seek for instant response
      if ((trackChanged || seeked) && workletNode) {
         workletNode.port.postMessage({ type: 'clear' });
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
            if (workletNode) {
               workletNode.port.postMessage({ type: 'clear' });
            }
            // Restore volume when resuming
            gainNode.gain.setValueAtTime(state.volume, audioContext.currentTime);
         }
      }

      // Clear buffer when stopped (not paused)
      if (!state.playing && workletNode) {
         workletNode.port.postMessage({ type: 'clear' });
      }

      notifyStateChange();
   }

   /**
    * Handle position update from server
    * @param {object} payload - Position payload
    */
   function handlePositionUpdate(payload) {
      state.positionSec = payload.position_sec || 0;
      state.durationSec = payload.duration_sec || state.durationSec;

      if (callbacks.onPositionUpdate) {
         callbacks.onPositionUpdate(state.positionSec, state.durationSec);
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
   };
})(window);
