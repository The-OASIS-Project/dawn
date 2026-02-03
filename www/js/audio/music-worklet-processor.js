/**
 * Music Audio Worklet Processor
 *
 * Runs on dedicated audio thread for glitch-free playback.
 * Receives decoded stereo audio samples via port messages.
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

      // Buffer status reporting (every ~250ms)
      this.reportInterval = 12; // ~12 process calls at 128 samples = 256ms at 48kHz
      this.reportCounter = 0;

      // Handle incoming audio data
      this.port.onmessage = (e) => {
         if (e.data.type === 'audio') {
            this.addSamples(e.data.left, e.data.right);
         } else if (e.data.type === 'clear') {
            this.writePos = 0;
            this.readPos = 0;
            this.samplesAvailable = 0;
            // Report buffer cleared immediately
            this.port.postMessage({ type: 'buffer', percent: 0 });
         }
      };
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

      // Report buffer status periodically
      this.reportCounter++;
      if (this.reportCounter >= this.reportInterval) {
         this.reportCounter = 0;
         const percent = Math.round((this.samplesAvailable / this.bufferSize) * 100);
         this.port.postMessage({ type: 'buffer', percent: percent });
      }

      return true;
   }
}

registerProcessor('music-processor', MusicProcessor);
