/**
 * Music Audio Worklet Processor
 *
 * Runs on the dedicated audio render thread for glitch-free playback. Receives
 * decoded stereo samples via port messages and drains a local ring in process().
 *
 * Dual-mode: audio may arrive either on the node port (main thread — the fallback
 * path) OR on a MessagePort transferred from the music-decode worker (the primary
 * path — decode is off the main thread). If a `workerPort` has been handed over,
 * audio/clear are taken on it and buffer reports go back on it (to the worker,
 * which combines them with decodeQueueSize and reports to the server); otherwise
 * everything uses the node port. Delivery on the worker port is ordered and on the
 * render thread, so a 'clear' always precedes later 'audio' — no ack needed.
 */

class MusicProcessor extends AudioWorkletProcessor {
   constructor() {
      super();

      // Ring buffer for audio samples (10 seconds at 48kHz stereo)
      this.bufferSize = 48000 * 10;
      this.leftBuffer = new Float32Array(this.bufferSize);
      this.rightBuffer = new Float32Array(this.bufferSize);
      this.writePos = 0;
      this.readPos = 0;
      this.samplesAvailable = 0;

      // Buffer status reporting. process() runs per 128-sample render quantum
      // (~2.67ms at 48kHz), so 12 calls ≈ 32ms — the cadence at which the server's
      // flow-control pacer receives fresh depth reports.
      this.reportInterval = 12;
      this.reportCounter = 0;

      // Set once the decode worker hands over its MessagePort (primary path).
      this.workerPort = null;

      this.port.onmessage = (e) => {
         const d = e.data;
         if (d && d.type === 'workerPort') {
            // One-time handoff: take audio/clear + send reports on the worker port.
            // Ignore a second handoff — one worker owns the data path, by invariant.
            if (this.workerPort) return;
            this.workerPort = d.port;
            this.workerPort.onmessage = (ev) => this.handleData(ev.data);
         } else if (!this.workerPort) {
            // Node port carries audio/clear only on the fallback (no-worker) path;
            // once a worker port is bound, the worker is the sole data source.
            this.handleData(d);
         }
      };
   }

   // Whichever channel is active — worker port (primary) or node port (fallback).
   reportPort() {
      return this.workerPort || this.port;
   }

   handleData(d) {
      if (!d) return;
      if (d.type === 'audio') {
         this.addSamples(d.left, d.right);
      } else if (d.type === 'clear') {
         this.writePos = 0;
         this.readPos = 0;
         this.samplesAvailable = 0;
         // Immediate 0-report so the server refills fast after a flush.
         this.reportPort().postMessage({ type: 'buffer', percent: 0, bufferedMs: 0 });
      }
   }

   addSamples(left, right) {
      const samplesToAdd = left.length;

      // Check for buffer overflow
      if (this.samplesAvailable + samplesToAdd > this.bufferSize) {
         // Buffer would overflow - skip oldest samples by advancing read position
         const overflow = this.samplesAvailable + samplesToAdd - this.bufferSize;
         this.readPos = (this.readPos + overflow) % this.bufferSize;
         this.samplesAvailable -= overflow;
      }

      for (let i = 0; i < samplesToAdd; i++) {
         this.leftBuffer[this.writePos] = left[i];
         this.rightBuffer[this.writePos] = right[i];
         this.writePos = (this.writePos + 1) % this.bufferSize;
      }

      this.samplesAvailable += samplesToAdd;
   }

   process(inputs, outputs, parameters) {
      const output = outputs[0];
      if (!output || output.length < 2) return true;

      const outputL = output[0];
      const outputR = output[1];
      const framesToProcess = outputL.length;

      for (let i = 0; i < framesToProcess; i++) {
         if (this.samplesAvailable > 0) {
            outputL[i] = this.leftBuffer[this.readPos];
            outputR[i] = this.rightBuffer[this.readPos];
            this.readPos = (this.readPos + 1) % this.bufferSize;
            this.samplesAvailable--;
         } else {
            // Buffer underrun - output silence
            outputL[i] = 0;
            outputR[i] = 0;
         }
      }

      // Report buffer status periodically. percent (of the 10s ring) drives the UI;
      // bufferedMs is the absolute depth the server's flow-control pacer needs
      // (percent quantizes to ~100ms steps, too coarse for a 2s target).
      this.reportCounter++;
      if (this.reportCounter >= this.reportInterval) {
         this.reportCounter = 0;
         const percent = Math.round((this.samplesAvailable / this.bufferSize) * 100);
         const bufferedMs = Math.round((this.samplesAvailable / sampleRate) * 1000);
         this.reportPort().postMessage({
            type: 'buffer',
            percent: percent,
            bufferedMs: bufferedMs,
         });
      }

      return true;
   }
}

registerProcessor('music-processor', MusicProcessor);
