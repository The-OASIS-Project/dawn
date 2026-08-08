/**
 * Shared Opus music decode helpers.
 *
 * Used by BOTH music-decode paths so the frame-parse and the WebCodecs
 * format-conversion matrix live in exactly one place:
 *   - the off-main-thread decode worker (music-decode-worker.js, via importScripts) —
 *     the primary path, decode off the main thread for the dedicated music socket;
 *   - the main-thread path (music-playback.js) — the consumer of the server's
 *     main-WS music fallback, used when the dedicated socket (port 3001) isn't
 *     connected (single-port / reverse-proxy deployments stream music over the
 *     main WS instead; see webui_music.c queue_music_direct()).
 *
 * Runs unchanged in a Window or a WorkerGlobalScope — it touches only ArrayBuffer,
 * DataView, typed arrays, and the WebCodecs AudioData handed in by the caller (no
 * DOM). Attaches its API to `self` (window on the main thread, the worker global in
 * a worker).
 */
(function (global) {
   'use strict';

   const MUSIC_FRAME_MAX = 1500; // sanity cap on a single Opus frame (bytes)

   /**
    * Iterate the length-prefixed Opus frames in a binary music message.
    * Layout: [type byte][u16 len][frame bytes][u16 len][frame bytes]...  (len LE).
    * The frame passed to onFrame is a Uint8Array VIEW into `data` — valid only for
    * the duration of the call; EncodedAudioChunk copies it synchronously, so decode
    * inside the callback.
    * @param {ArrayBuffer} data - Binary message, including the leading type byte.
    * @param {function(Uint8Array): void} onFrame - Called once per valid frame.
    */
   function forEachOpusFrame(data, onFrame) {
      const view = new DataView(data);
      let offset = 1; // skip the message-type byte
      while (offset + 2 <= data.byteLength) {
         const frameLen = view.getUint16(offset, true); // little-endian
         offset += 2;
         if (frameLen === 0 || frameLen > MUSIC_FRAME_MAX || offset + frameLen > data.byteLength) {
            console.warn('Music decode: invalid frame length', frameLen);
            break;
         }
         onFrame(new Uint8Array(data, offset, frameLen));
         offset += frameLen;
      }
   }

   /**
    * Convert a decoded WebCodecs AudioData to a planar stereo {left, right} pair of
    * Float32Array (the shape the worklet's addSamples() consumes). Handles every
    * format the Opus decoder may emit — f32-planar (the Chrome hot path), f32, s16,
    * s16-planar — and upmixes mono. Returns null for an unsupported format (caller
    * should skip the frame). Does NOT close audioData — the caller owns its lifecycle.
    * @param {AudioData} audioData
    * @returns {{left: Float32Array, right: Float32Array}|null}
    */
   function audioDataToStereo(audioData) {
      const numFrames = audioData.numberOfFrames;
      const numChannels = audioData.numberOfChannels;
      const format = audioData.format;
      const left = new Float32Array(numFrames);
      const right = new Float32Array(numFrames);

      if (format === 'f32-planar') {
         // Planar: each channel is its own plane.
         audioData.copyTo(left, { planeIndex: 0 });
         if (numChannels > 1) {
            audioData.copyTo(right, { planeIndex: 1 });
         } else {
            right.set(left);
         }
      } else if (format === 'f32') {
         // Interleaved: L R L R ...
         const interleaved = new Float32Array(numFrames * numChannels);
         audioData.copyTo(interleaved, { planeIndex: 0 });
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
      } else if (format === 's16' || format === 's16-planar') {
         const buffer = new ArrayBuffer(numFrames * numChannels * 2);
         audioData.copyTo(buffer, { planeIndex: 0 });
         const int16 = new Int16Array(buffer);
         if (format === 's16') {
            // Interleaved signed 16-bit.
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
         } else {
            // Planar signed 16-bit: plane 0 = left, plane 1 = right.
            for (let i = 0; i < numFrames; i++) left[i] = int16[i] / 32768.0;
            if (numChannels > 1) {
               audioData.copyTo(buffer, { planeIndex: 1 });
               const int16Right = new Int16Array(buffer);
               for (let i = 0; i < numFrames; i++) right[i] = int16Right[i] / 32768.0;
            } else {
               right.set(left);
            }
         }
      } else {
         console.warn('Music decode: unsupported format', format);
         return null;
      }

      return { left, right };
   }

   global.DawnMusicDecode = {
      forEachOpusFrame: forEachOpusFrame,
      audioDataToStereo: audioDataToStereo,
      MUSIC_FRAME_MAX: MUSIC_FRAME_MAX,
   };
})(typeof self !== 'undefined' ? self : this);
