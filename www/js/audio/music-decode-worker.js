/**
 * Music decode worker.
 *
 * Owns the entire music audio DATA path off the browser main thread: the dedicated
 * music WebSocket (port 3001), WebCodecs Opus decode, and delivery of decoded PCM
 * straight to the AudioWorkletProcessor over a transferred MessagePort. The main
 * thread never touches music audio bytes — so concurrent TTS / UI load on the main
 * thread can no longer starve music decode (the residual jitter the server-side
 * flow-control could not fix).
 *
 * Ports:
 *   - self (main <-> worker): control only — token, server config, subscribe,
 *     clear, and periodic buffered-depth status back to main for the UI/position.
 *   - workletPort (worker <-> worklet): 'audio'/'clear' down; 'buffer' (ring depth)
 *     up. MessagePort delivery is ordered and on the audio render thread (verified),
 *     so a 'clear' always precedes later audio with no ack handshake needed.
 *
 * Flow control (parity with the shipped main-thread path): total buffered =
 * worklet ring depth + AudioDecoder.decodeQueueSize; the worker sends
 * {type:'music_buffer', buffered_ms} up the WS, clamped to [0, CLIENT_BUFFER_MAX_MS],
 * with an immediate 0-report on clear so the server refills fast after a flush.
 */

'use strict';

const OPUS_SAMPLE_RATE = 48000;
const FRAME_US = 20000; // 20ms per Opus frame, microseconds
const FRAME_MS = 20;
const CLIENT_BUFFER_MAX_MS = 10000; // mirror WEBUI_MUSIC_CLIENT_BUFFER_MAX_MS (10s worklet ring)
const MAX_RETRIES = 5;

let ws = null;
let connected = false;
let authenticated = false;
let authFailed = false;
let retryCount = 0;
let reconnectTimer = null;
let subscribed = false;

let token = null;
let serverPort = null; // advertised by main's `config` frame
let serverEnabled = null;

let workletPort = null;
let decoder = null;
let decoderReady = false;
let decodeTimestamp = 0;
let workletBufferedMs = 0;
let statusPostCounter = 0;
const STATUS_POST_EVERY = 3; // ~96ms: UI buffer-bar cadence (the WS report stays at ~32ms)

/* ---- Music WebSocket (ported from MusicStreamConnection; self.location works in a worker) ---- */

function musicUrl() {
   const protocol = self.location.protocol === 'https:' ? 'wss:' : 'ws:';
   const host = self.location.hostname;
   const mainPort = parseInt(self.location.port || '8080', 10);
   const port = serverPort || mainPort + 1;
   return `${protocol}//${host}:${port}`;
}

function connect() {
   if (ws && ws.readyState === WebSocket.OPEN) return;
   if (authFailed) return; // wait for a fresh token
   if (serverEnabled === false) return; // music server off

   try {
      ws = new WebSocket(musicUrl(), 'dawn-music');
      ws.binaryType = 'arraybuffer';
      ws.onopen = onOpen;
      ws.onclose = onClose;
      ws.onmessage = onMessage;
      ws.onerror = (e) => console.error('Music worker: WS error', e);
   } catch (e) {
      console.error('Music worker: failed to create WebSocket', e);
   }
}

function disconnect() {
   if (reconnectTimer) {
      clearTimeout(reconnectTimer);
      reconnectTimer = null;
   }
   if (ws) {
      try {
         ws.close();
      } catch (e) {
         /* ignore */
      }
      ws = null;
   }
   connected = false;
   authenticated = false;
}

function onOpen() {
   connected = true;
   if (token) {
      ws.send(JSON.stringify({ type: 'auth', token }));
   } else {
      console.warn('Music worker: no token available for auth');
   }
}

function onClose() {
   connected = false;
   authenticated = false;
   if (authFailed) return;
   if (retryCount < MAX_RETRIES) {
      const delay = Math.min(2000 * Math.pow(2, retryCount), 30000);
      retryCount++;
      reconnectTimer = setTimeout(connect, delay);
   }
}

function onMessage(event) {
   if (event.data instanceof ArrayBuffer) {
      handleMusicData(event.data);
      return;
   }
   try {
      const msg = JSON.parse(event.data);
      if (msg.type === 'auth_ok') {
         authenticated = true;
         retryCount = 0;
         authFailed = false;
      } else if (msg.type === 'auth_failed') {
         console.error('Music worker: auth failed:', msg.reason);
         authFailed = true;
         if (ws) ws.close();
      }
   } catch (e) {
      /* ignore malformed JSON */
   }
}

function wsReady() {
   return connected && authenticated && ws && ws.readyState === WebSocket.OPEN;
}

function sendUp(obj) {
   if (!wsReady()) return;
   try {
      ws.send(JSON.stringify(obj));
   } catch (e) {
      /* socket tearing down — reports are periodic */
   }
}

/* ---- WebCodecs decode (ported from initOpusDecoder + handleDecodedAudio) ---- */

let initPromise = null;

function initDecoder() {
   if (decoderReady) return Promise.resolve();
   if (initPromise) return initPromise; // in flight — WS frames arrive concurrently
   initPromise = (async () => {
      if (typeof AudioDecoder === 'undefined') {
         console.error('Music worker: WebCodecs AudioDecoder unavailable');
         return;
      }
      const config = { codec: 'opus', sampleRate: OPUS_SAMPLE_RATE, numberOfChannels: 2 };
      const support = await AudioDecoder.isConfigSupported(config);
      if (!support.supported) {
         console.error('Music worker: stereo Opus @48kHz not supported');
         return;
      }
      decoder = new AudioDecoder({
         output: handleDecodedAudio,
         error: (e) => {
            console.error('Music worker: decoder error', e);
            // A fatal error closes the decoder. Drop it so the next frame re-inits,
            // instead of the closed-state guard silently dropping audio forever.
            if (decoder && decoder.state === 'closed') {
               decoder = null;
               decoderReady = false;
               initPromise = null;
            }
         },
      });
      decoder.configure(config);
      decoderReady = true;
   })().finally(() => {
      if (!decoderReady) initPromise = null; // allow retry on failure
   });
   return initPromise;
}

function handleDecodedAudio(audioData) {
   try {
      const numFrames = audioData.numberOfFrames;
      const numChannels = audioData.numberOfChannels;
      const format = audioData.format;
      let left, right;

      if (format === 'f32-planar') {
         left = new Float32Array(numFrames);
         right = new Float32Array(numFrames);
         audioData.copyTo(left, { planeIndex: 0 });
         if (numChannels > 1) audioData.copyTo(right, { planeIndex: 1 });
         else right.set(left);
      } else if (format === 'f32') {
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
            for (let i = 0; i < numFrames; i++) {
               left[i] = interleaved[i];
               right[i] = interleaved[i];
            }
         }
      } else {
         const bytesPerSample = format.startsWith('s16') ? 2 : 4;
         const totalSamples = numFrames * numChannels;
         const buffer = new ArrayBuffer(totalSamples * bytesPerSample);
         audioData.copyTo(buffer, { planeIndex: 0 });
         left = new Float32Array(numFrames);
         right = new Float32Array(numFrames);
         if (format === 's16') {
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
            const int16 = new Int16Array(buffer);
            for (let i = 0; i < numFrames; i++) left[i] = int16[i] / 32768.0;
            if (numChannels > 1) {
               audioData.copyTo(buffer, { planeIndex: 1 });
               const int16Right = new Int16Array(buffer);
               for (let i = 0; i < numFrames; i++) right[i] = int16Right[i] / 32768.0;
            } else {
               right.set(left);
            }
         } else {
            console.warn('Music worker: unsupported format', format);
            return; // finally still closes audioData
         }
      }

      if (workletPort) {
         workletPort.postMessage({ type: 'audio', left, right }, [left.buffer, right.buffer]);
      }
   } catch (e) {
      console.error('Music worker: error handling decoded audio', e, audioData && audioData.format);
   } finally {
      // Close on every path (normal, unsupported-format return, or a throw mid-copy) —
      // a leaked AudioData wraps decoded PCM the GC won't reclaim promptly.
      audioData.close();
   }
}

async function handleMusicData(data) {
   if (!decoderReady) await initDecoder();
   if (!decoder || decoder.state === 'closed') return;

   const view = new DataView(data);
   let offset = 1; // skip message-type byte
   while (offset + 2 <= data.byteLength) {
      const frameLen = view.getUint16(offset, true);
      offset += 2;
      if (frameLen === 0 || frameLen > 1500 || offset + frameLen > data.byteLength) {
         console.warn('Music worker: invalid frame length', frameLen);
         break;
      }
      const opusFrame = new Uint8Array(data, offset, frameLen);
      offset += frameLen;
      try {
         decoder.decode(
            new EncodedAudioChunk({ type: 'key', timestamp: decodeTimestamp, data: opusFrame })
         );
         decodeTimestamp += FRAME_US;
      } catch (e) {
         console.warn('Music worker: decode error', e);
      }
   }
}

/* ---- Flush: reset decoder + drop pending, then tell the worklet to clear (ordered) ---- */

function flush() {
   if (decoder && decoder.state === 'configured') {
      try {
         decoder.reset();
         decoder.configure({ codec: 'opus', sampleRate: OPUS_SAMPLE_RATE, numberOfChannels: 2 });
      } catch (e) {
         /* ignore */
      }
   }
   decodeTimestamp = 0;
   workletBufferedMs = 0;
   if (workletPort) workletPort.postMessage({ type: 'clear' });
   // Immediate 0-report so the server refills fast after a flush.
   sendUp({ type: 'music_buffer', buffered_ms: 0 });
   postMessage({ type: 'status', bufferedMs: 0 });
}

/* ---- Worklet -> worker: ring depth report; combine with decodeQueueSize and report up ---- */

function onWorkletMessage(e) {
   const d = e.data;
   if (!d) return;
   if (d.type === 'buffer' && typeof d.bufferedMs === 'number') {
      workletBufferedMs = d.bufferedMs;
      const backlogMs = decoder && decoder.decodeQueueSize ? decoder.decodeQueueSize * FRAME_MS : 0;
      let total = workletBufferedMs + backlogMs;
      if (total < 0) total = 0;
      else if (total > CLIENT_BUFFER_MAX_MS) total = CLIENT_BUFFER_MAX_MS;
      // The server flow-control pacer needs every report (~32ms).
      sendUp({ type: 'music_buffer', buffered_ms: total });
      // The UI buffer bar does not — throttle to ~100ms so the worker isn't waking
      // the main thread ~31x/sec (the very contention this move exists to remove).
      if (++statusPostCounter >= STATUS_POST_EVERY) {
         statusPostCounter = 0;
         postMessage({ type: 'status', bufferedMs: total });
      }
   }
}

/* ---- Main -> worker control ---- */

self.onmessage = (e) => {
   const d = e.data;
   if (!d) return;
   switch (d.type) {
      case 'workletPort':
         workletPort = d.port;
         workletPort.onmessage = onWorkletMessage;
         break;
      case 'setToken': {
         const changed = d.token && d.token !== token;
         token = d.token || null;
         if (token && authFailed) {
            // fresh token after an auth failure — reset the latch and reconnect
            authFailed = false;
            retryCount = 0;
            if (subscribed) {
               disconnect();
               connect();
            }
         } else if (changed && subscribed && wsReady()) {
            // token rotated on a live, authed socket — re-auth so a revoked session
            // can't persist (matches the fallback's reconnectWithFreshToken()).
            disconnect();
            connect();
         }
         break;
      }
      case 'setServer':
         serverPort = d.port != null ? d.port : serverPort;
         serverEnabled = d.enabled != null ? d.enabled : serverEnabled;
         break;
      case 'subscribe':
         subscribed = true;
         retryCount = 0;
         connect();
         break;
      case 'unsubscribe':
         subscribed = false;
         flush();
         disconnect();
         break;
      case 'clear':
         flush();
         break;
      default:
         break;
   }
};
