/**
 * DAWN WebUI JavaScript
 * Phase 4: Voice input and audio playback
 */

(function() {
  'use strict';

  // =============================================================================
  // Configuration
  // =============================================================================
  const WS_SUBPROTOCOL = 'dawn-1.0';
  const RECONNECT_BASE_DELAY = 1000;
  const RECONNECT_MAX_DELAY = 30000;

  // Audio configuration
  const AUDIO_SAMPLE_RATE = 16000;  // 16kHz for ASR
  const AUDIO_CHANNELS = 1;         // Mono
  let audioChunkMs = 200;           // Send audio every N ms (updated from server config)

  // Binary message types (match server)
  const WS_BIN_AUDIO_IN = 0x01;
  const WS_BIN_AUDIO_IN_END = 0x02;
  const WS_BIN_AUDIO_OUT = 0x11;
  const WS_BIN_AUDIO_OUT_END = 0x12;

  // =============================================================================
  // State
  // =============================================================================
  let ws = null;
  let reconnectAttempts = 0;
  let currentState = 'idle';
  let debugMode = false;

  // Audio state
  let audioContext = null;
  let mediaStream = null;
  let audioProcessor = null;
  let isRecording = false;
  let audioSupported = false;

  // Audio playback state
  let playbackContext = null;
  let audioChunks = [];
  let isPlayingAudio = false;

  // FFT visualization state (for playback - gives DAWN "life" when speaking)
  let playbackAnalyser = null;
  let fftAnimationId = null;
  let fftDataArray = null;
  let waveformHistory = [];  // Store recent waveform paths for trail effect
  let frameCount = 0;        // Frame counter for trail sampling
  const TRAIL_LENGTH = 5;    // Number of trailing echoes
  const TRAIL_SAMPLE_RATE = 10;  // Store trail every N frames for visible separation

  // =============================================================================
  // DOM Elements
  // =============================================================================
  const elements = {
    connectionStatus: document.getElementById('connection-status'),
    ringContainer: document.getElementById('ring-container'),
    ringOuter: document.getElementById('ring-outer'),
    ringInner: document.getElementById('ring-inner'),
    waveformPath: document.getElementById('waveform-path'),
    waveformTrails: [
      document.getElementById('waveform-trail-1'),
      document.getElementById('waveform-trail-2'),
      document.getElementById('waveform-trail-3'),
      document.getElementById('waveform-trail-4'),
      document.getElementById('waveform-trail-5'),
    ],
    statusDot: document.getElementById('status-dot'),
    statusText: document.getElementById('status-text'),
    transcript: document.getElementById('transcript'),
    textInput: document.getElementById('text-input'),
    sendBtn: document.getElementById('send-btn'),
    micBtn: document.getElementById('mic-btn'),
    debugCheckbox: document.getElementById('debug-mode'),
  };

  // Waveform configuration (viewBox is 240x240)
  const WAVEFORM_CENTER = 120;  // Center of SVG viewBox
  const WAVEFORM_BASE_RADIUS = 85;  // Base circle radius
  const WAVEFORM_SPIKE_HEIGHT = 30;  // Max spike height (max radius = 115, within 120 center)
  const WAVEFORM_POINTS = 64;  // Number of points around the circle (should be even for symmetry)

  // =============================================================================
  // WebSocket Connection
  // =============================================================================
  function getWebSocketUrl() {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    return `${protocol}//${window.location.host}/ws`;
  }

  function connect() {
    if (ws && ws.readyState === WebSocket.OPEN) {
      return;
    }

    updateConnectionStatus('connecting');

    try {
      ws = new WebSocket(getWebSocketUrl(), WS_SUBPROTOCOL);
    } catch (e) {
      console.error('Failed to create WebSocket:', e);
      scheduleReconnect();
      return;
    }

    ws.binaryType = 'arraybuffer';

    ws.onopen = function() {
      console.log('WebSocket connected');
      reconnectAttempts = 0;
      updateConnectionStatus('connected');

      // Try to reconnect with existing session token
      const savedToken = localStorage.getItem('dawn_session_token');
      if (savedToken) {
        console.log('Attempting session reconnect with token:', savedToken.substring(0, 8) + '...');
        ws.send(JSON.stringify({
          type: 'reconnect',
          payload: { token: savedToken }
        }));
      }
      // Server will send state update after processing reconnect or creating new session
    };

    ws.onclose = function(event) {
      console.log('WebSocket closed:', event.code, event.reason);
      updateConnectionStatus('disconnected');
      scheduleReconnect();
    };

    ws.onerror = function(error) {
      console.error('WebSocket error:', error);
    };

    ws.onmessage = function(event) {
      if (event.data instanceof ArrayBuffer) {
        handleBinaryMessage(event.data);
      } else {
        handleTextMessage(event.data);
      }
    };
  }

  function scheduleReconnect() {
    const delay = Math.min(
      RECONNECT_BASE_DELAY * Math.pow(2, reconnectAttempts),
      RECONNECT_MAX_DELAY
    );
    const jitter = Math.random() * 500;

    console.log(`Reconnecting in ${Math.round(delay + jitter)}ms...`);
    reconnectAttempts++;

    setTimeout(connect, delay + jitter);
  }

  function disconnect() {
    if (ws) {
      ws.close();
      ws = null;
    }
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
          updateState(msg.payload.state);
          break;
        case 'transcript':
          addTranscriptEntry(msg.payload.role, msg.payload.text);
          break;
        case 'error':
          console.error('Server error:', msg.payload);
          addTranscriptEntry('system', `Error: ${msg.payload.message}`);
          break;
        case 'session':
          console.log('Session token received');
          localStorage.setItem('dawn_session_token', msg.payload.token);
          break;
        case 'config':
          console.log('Config received:', msg.payload);
          if (msg.payload.audio_chunk_ms) {
            audioChunkMs = msg.payload.audio_chunk_ms;
            console.log('Audio chunk size set to:', audioChunkMs, 'ms');
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

      switch (msgType) {
        case WS_BIN_AUDIO_OUT:
          // TTS audio chunk (raw PCM: 16-bit signed, 16kHz, mono)
          if (bytes.length > 1) {
            // Copy payload bytes (skip type byte)
            const payload = bytes.slice(1);
            audioChunks.push(payload);
            console.log('Received audio chunk:', payload.length, 'bytes (total chunks:', audioChunks.length, ')');
          }
          break;

        case WS_BIN_AUDIO_OUT_END:
          // End of TTS audio - play accumulated chunks
          console.log('Audio stream ended, playing', audioChunks.length, 'chunks');
          if (audioChunks.length > 0) {
            playAccumulatedAudio();
          }
          break;

        default:
          console.log('Unknown binary message type:', '0x' + msgType.toString(16));
      }
    } catch (e) {
      console.error('Error handling binary message:', e);
    }
  }

  // Audio playback queue - stores complete audio buffers waiting to play
  let audioPlaybackQueue = [];

  /**
   * Play accumulated PCM audio chunks via Web Audio API
   * Format: 16-bit signed integer PCM, 16kHz, mono
   * If audio is currently playing, queues the new audio for later
   */
  async function playAccumulatedAudio() {
    if (audioChunks.length === 0) {
      console.log('playAccumulatedAudio: no chunks to play');
      return;
    }

    // Concatenate chunks into a single buffer
    const totalLength = audioChunks.reduce((sum, chunk) => sum + chunk.length, 0);
    console.log('Total audio bytes:', totalLength);

    // Ensure even number of bytes for Int16 alignment
    const alignedLength = totalLength - (totalLength % 2);
    if (alignedLength !== totalLength) {
      console.warn('Audio length not aligned, truncating', totalLength - alignedLength, 'bytes');
    }

    const combinedBuffer = new Uint8Array(alignedLength);
    let offset = 0;
    for (const chunk of audioChunks) {
      const bytesToCopy = Math.min(chunk.length, alignedLength - offset);
      if (bytesToCopy > 0) {
        combinedBuffer.set(chunk.subarray(0, bytesToCopy), offset);
        offset += bytesToCopy;
      }
    }
    audioChunks = [];  // Clear for next audio stream

    if (isPlayingAudio) {
      // Queue this audio to play after current finishes
      console.log('Audio playing, queuing', alignedLength, 'bytes for later (queue size:', audioPlaybackQueue.length + 1, ')');
      audioPlaybackQueue.push(combinedBuffer);
      return;
    }

    // Play immediately
    await playAudioBuffer(combinedBuffer);
  }

  /**
   * Play a raw PCM buffer
   */
  async function playAudioBuffer(buffer) {
    isPlayingAudio = true;
    const alignedLength = buffer.length;

    try {
      // Convert bytes to Int16 samples (little-endian)
      const numSamples = alignedLength / 2;
      const int16Data = new Int16Array(numSamples);
      const dataView = new DataView(buffer.buffer);
      for (let i = 0; i < numSamples; i++) {
        int16Data[i] = dataView.getInt16(i * 2, true);  // true = little-endian
      }

      console.log('Playing', numSamples, 'samples (', (numSamples / AUDIO_SAMPLE_RATE).toFixed(2), 'seconds)');

      // Create playback AudioContext if needed
      const AudioContext = window.AudioContext || window.webkitAudioContext;
      if (!playbackContext || playbackContext.state === 'closed') {
        playbackContext = new AudioContext({ sampleRate: AUDIO_SAMPLE_RATE });

        // Create analyser for FFT visualization (gives DAWN "life" when speaking)
        playbackAnalyser = playbackContext.createAnalyser();
        playbackAnalyser.fftSize = 256;  // More frequency bins for detail
        playbackAnalyser.smoothingTimeConstant = 0.4;  // Lower = more responsive, higher = smoother
        fftDataArray = new Uint8Array(playbackAnalyser.frequencyBinCount);
      }

      // Resume context if suspended (browser autoplay policy)
      if (playbackContext.state === 'suspended') {
        console.log('Resuming suspended audio context...');
        await playbackContext.resume();
      }

      // Create AudioBuffer
      const audioBuffer = playbackContext.createBuffer(1, numSamples, AUDIO_SAMPLE_RATE);
      const channelData = audioBuffer.getChannelData(0);

      // Convert Int16 (-32768 to 32767) to Float32 (-1.0 to 1.0)
      for (let i = 0; i < numSamples; i++) {
        channelData[i] = int16Data[i] / 32768.0;
      }

      // Create source node and connect through analyser for FFT visualization
      const source = playbackContext.createBufferSource();
      source.buffer = audioBuffer;
      source.connect(playbackAnalyser);
      playbackAnalyser.connect(playbackContext.destination);

      // Start FFT visualization if not already running
      if (!fftAnimationId) {
        startFFTVisualization();
      }

      source.onended = function() {
        console.log('Audio chunk finished');

        // Check if there's queued audio to play next
        if (audioPlaybackQueue.length > 0) {
          const nextBuffer = audioPlaybackQueue.shift();
          console.log('Playing next queued audio (remaining in queue:', audioPlaybackQueue.length, ')');
          playAudioBuffer(nextBuffer);
        } else {
          // No more audio - stop FFT and mark playback complete
          console.log('All audio playback finished');
          isPlayingAudio = false;
          stopFFTVisualization();
        }
      };

      console.log('Starting audio playback...');
      source.start(0);

    } catch (e) {
      console.error('Failed to play audio:', e);
      isPlayingAudio = false;
      stopFFTVisualization();
      // Try to play next in queue even on error
      if (audioPlaybackQueue.length > 0) {
        const nextBuffer = audioPlaybackQueue.shift();
        playAudioBuffer(nextBuffer);
      }
    }
  }

  // =============================================================================
  // Audio Capture
  // =============================================================================

  /**
   * Initialize audio context and check for microphone support
   */
  async function initAudio() {
    try {
      // Check for secure context (required for getUserMedia)
      if (!window.isSecureContext) {
        console.warn('Audio requires secure context (HTTPS or localhost)');
        console.warn('Current origin:', window.location.origin);
        return { supported: false, reason: 'Requires HTTPS or localhost access' };
      }

      // Check for Web Audio API support
      const AudioContext = window.AudioContext || window.webkitAudioContext;
      if (!AudioContext) {
        console.warn('Web Audio API not supported');
        return { supported: false, reason: 'Web Audio API not supported' };
      }

      // Check for getUserMedia support
      if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
        console.warn('getUserMedia not supported');
        return { supported: false, reason: 'getUserMedia not supported' };
      }

      audioSupported = true;
      console.log('Audio capture supported');
      return { supported: true, reason: null };
    } catch (e) {
      console.error('Audio init failed:', e);
      return { supported: false, reason: e.message };
    }
  }

  /**
   * Start recording audio from microphone
   */
  async function startRecording() {
    if (isRecording) {
      console.warn('Already recording');
      return;
    }

    if (!audioSupported) {
      console.error('Audio not supported');
      return;
    }

    try {
      // Request microphone access
      mediaStream = await navigator.mediaDevices.getUserMedia({
        audio: {
          sampleRate: AUDIO_SAMPLE_RATE,
          channelCount: AUDIO_CHANNELS,
          echoCancellation: true,
          noiseSuppression: true,
          autoGainControl: true
        }
      });

      // Create audio context at target sample rate
      const AudioContext = window.AudioContext || window.webkitAudioContext;
      audioContext = new AudioContext({ sampleRate: AUDIO_SAMPLE_RATE });

      // Create source from microphone stream
      const source = audioContext.createMediaStreamSource(mediaStream);

      // Create ScriptProcessorNode for audio processing
      // Note: ScriptProcessorNode is deprecated but AudioWorklet requires HTTPS
      // Buffer size must be power of two, calculate from configured chunk ms
      const desiredSamples = Math.floor(AUDIO_SAMPLE_RATE * audioChunkMs / 1000);
      const bufferSize = Math.pow(2, Math.ceil(Math.log2(desiredSamples)));
      console.log('Audio buffer size:', bufferSize, 'samples (', bufferSize / AUDIO_SAMPLE_RATE * 1000, 'ms)');
      audioProcessor = audioContext.createScriptProcessor(bufferSize, 1, 1);

      audioProcessor.onaudioprocess = function(e) {
        // Clear output buffer to prevent feedback/garbage
        const outputData = e.outputBuffer.getChannelData(0);
        outputData.fill(0);

        if (!isRecording) return;

        // Get input audio data (Float32Array)
        const inputData = e.inputBuffer.getChannelData(0);

        // Convert Float32 (-1 to 1) to Int16 (-32768 to 32767)
        const pcmData = new Int16Array(inputData.length);
        for (let i = 0; i < inputData.length; i++) {
          const s = Math.max(-1, Math.min(1, inputData[i]));
          pcmData[i] = s < 0 ? s * 0x8000 : s * 0x7FFF;
        }

        // Send audio chunk to server
        sendAudioChunk(pcmData.buffer);
      };

      // Connect nodes: source -> processor -> destination
      source.connect(audioProcessor);
      audioProcessor.connect(audioContext.destination);

      isRecording = true;
      updateMicButton(true);
      console.log('Recording started');

    } catch (e) {
      console.error('Failed to start recording:', e);
      if (e.name === 'NotAllowedError') {
        addTranscriptEntry('system', 'Microphone access denied. Please allow microphone access.');
      } else {
        addTranscriptEntry('system', 'Failed to access microphone: ' + e.message);
      }
    }
  }

  /**
   * Stop recording and send end marker
   */
  function stopRecording() {
    if (!isRecording) {
      return;
    }

    isRecording = false;

    // Stop audio processing
    if (audioProcessor) {
      audioProcessor.disconnect();
      audioProcessor = null;
    }

    // Close audio context
    if (audioContext && audioContext.state !== 'closed') {
      audioContext.close();
      audioContext = null;
    }

    // Stop microphone stream
    if (mediaStream) {
      mediaStream.getTracks().forEach(track => track.stop());
      mediaStream = null;
    }

    // Send end marker to server
    sendAudioEnd();

    updateMicButton(false);
    console.log('Recording stopped');
  }

  /**
   * Send audio chunk to server
   */
  function sendAudioChunk(pcmBuffer) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }

    // Create message: [type byte][PCM data]
    const payload = new Uint8Array(1 + pcmBuffer.byteLength);
    payload[0] = WS_BIN_AUDIO_IN;
    payload.set(new Uint8Array(pcmBuffer), 1);

    ws.send(payload.buffer);
  }

  /**
   * Send audio end marker to server
   */
  function sendAudioEnd() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }

    const payload = new Uint8Array([WS_BIN_AUDIO_IN_END]);
    ws.send(payload.buffer);
    console.log('Sent audio end marker');
  }

  // =============================================================================
  // FFT Visualization (animates rings when DAWN speaks)
  // =============================================================================

  /**
   * Process FFT data for visualization
   * - Uses logarithmic frequency scaling (speech is in low frequencies)
   * - Normalizes to maximize dynamic range
   * - Creates symmetrical output for pleasing visuals
   * @param {Uint8Array} fftData - Raw FFT frequency data
   * @returns {Float32Array} Processed values (0-1) for each waveform point
   */
  function processFFTData(fftData) {
    const halfPoints = WAVEFORM_POINTS / 2;
    const processed = new Float32Array(WAVEFORM_POINTS);

    if (!fftData || fftData.length === 0) {
      return processed;  // All zeros
    }

    // Use only the useful frequency range (skip DC, focus on speech frequencies)
    const usableBins = Math.floor(fftData.length * 0.8);  // Use 80% of bins
    const startBin = 1;  // Skip DC component

    // Sample FFT bins with logarithmic distribution for half the points
    const halfValues = new Float32Array(halfPoints);
    let maxVal = 0;

    for (let i = 0; i < halfPoints; i++) {
      // Logarithmic mapping: more points for low frequencies
      const t = i / halfPoints;
      const logT = Math.pow(t, 0.6);  // Compress high frequencies
      const binIndex = startBin + Math.floor(logT * (usableBins - startBin));

      // Average a few neighboring bins for smoother result
      let sum = 0;
      let count = 0;
      for (let j = -1; j <= 1; j++) {
        const idx = binIndex + j;
        if (idx >= 0 && idx < fftData.length) {
          sum += fftData[idx];
          count++;
        }
      }
      halfValues[i] = count > 0 ? sum / count : 0;
      if (halfValues[i] > maxVal) maxVal = halfValues[i];
    }

    // Normalize to 0-1 range with some minimum threshold
    const threshold = 10;  // Minimum value to consider as signal
    if (maxVal > threshold) {
      for (let i = 0; i < halfPoints; i++) {
        halfValues[i] = Math.max(0, (halfValues[i] - threshold)) / (maxVal - threshold);
        // Apply slight curve for more dramatic spikes
        halfValues[i] = Math.pow(halfValues[i], 0.7);
      }
    }

    // Create symmetrical waveform (mirror around the circle)
    for (let i = 0; i < halfPoints; i++) {
      processed[i] = halfValues[i];
      processed[WAVEFORM_POINTS - 1 - i] = halfValues[i];
    }

    return processed;
  }

  /**
   * Generate SVG path for circular waveform
   * @param {Float32Array|null} processedData - Processed FFT values (0-1) or null for default circle
   * @param {number} scale - Overall scale multiplier
   * @returns {string} SVG path d attribute
   */
  function generateWaveformPath(processedData, scale = 1.0) {
    const points = [];
    const numPoints = WAVEFORM_POINTS;

    for (let i = 0; i < numPoints; i++) {
      const angle = (i / numPoints) * Math.PI * 2 - Math.PI / 2;  // Start at top

      // Get processed value for this point (or 0 if no data)
      const value = (processedData && processedData.length > i) ? processedData[i] : 0;

      // Calculate radius with spike based on value
      const spikeHeight = value * WAVEFORM_SPIKE_HEIGHT * scale;
      const radius = (WAVEFORM_BASE_RADIUS + spikeHeight) * scale;

      // Convert polar to cartesian
      const x = WAVEFORM_CENTER + Math.cos(angle) * radius;
      const y = WAVEFORM_CENTER + Math.sin(angle) * radius;

      points.push({ x, y });
    }

    // Build SVG path - use smooth curves for nicer look
    if (points.length === 0) return '';

    let path = `M ${points[0].x.toFixed(1)} ${points[0].y.toFixed(1)}`;

    // Use quadratic curves for smooth spikes
    for (let i = 1; i < points.length; i++) {
      const curr = points[i];
      const prev = points[i - 1];
      // Control point at midpoint
      const cpX = (prev.x + curr.x) / 2;
      const cpY = (prev.y + curr.y) / 2;
      path += ` Q ${prev.x.toFixed(1)} ${prev.y.toFixed(1)} ${cpX.toFixed(1)} ${cpY.toFixed(1)}`;
    }

    // Close the path smoothly
    const last = points[points.length - 1];
    const first = points[0];
    const cpX = (last.x + first.x) / 2;
    const cpY = (last.y + first.y) / 2;
    path += ` Q ${last.x.toFixed(1)} ${last.y.toFixed(1)} ${cpX.toFixed(1)} ${cpY.toFixed(1)}`;
    path += ' Z';

    return path;
  }

  /**
   * Draw default circular waveform (idle state)
   */
  function drawDefaultWaveform() {
    const path = generateWaveformPath(null, 1.0);
    if (elements.waveformPath) {
      elements.waveformPath.setAttribute('d', path);
    }
    // Clear trails
    for (const trail of elements.waveformTrails) {
      if (trail) {
        trail.setAttribute('d', path);  // Set to same circle for clean look
      }
    }
    waveformHistory = [];
    frameCount = 0;
  }

  /**
   * Start FFT visualization animation loop
   */
  function startFFTVisualization() {
    if (!elements.ringContainer || !playbackAnalyser || !fftDataArray) {
      console.warn('FFT visualization: missing requirements');
      return;
    }

    console.log('Starting FFT visualization for DAWN speech');

    // Add class to pause CSS animations and enable JS control
    elements.ringContainer.classList.add('fft-active');

    // Animation loop
    function animate() {
      if (!playbackAnalyser || !fftDataArray || !isPlayingAudio) {
        return;
      }

      // Get frequency data
      playbackAnalyser.getByteFrequencyData(fftDataArray);

      // Process FFT data for visualization
      const processedData = processFFTData(fftDataArray);

      // Calculate average volume for inner ring scaling
      let sum = 0;
      for (let i = 0; i < fftDataArray.length; i++) {
        sum += fftDataArray[i];
      }
      const average = sum / fftDataArray.length;
      const normalizedLevel = average / 255;

      // Generate waveform path from processed FFT data
      if (elements.waveformPath) {
        const path = generateWaveformPath(processedData, 1.0);
        elements.waveformPath.setAttribute('d', path);

        // Update trail history every N frames for visible separation
        frameCount++;
        if (frameCount >= TRAIL_SAMPLE_RATE) {
          frameCount = 0;
          waveformHistory.unshift(path);
          if (waveformHistory.length > TRAIL_LENGTH) {
            waveformHistory.pop();
          }
        }

        // Update trail paths with historical data
        for (let i = 0; i < elements.waveformTrails.length; i++) {
          if (elements.waveformTrails[i] && waveformHistory[i]) {
            elements.waveformTrails[i].setAttribute('d', waveformHistory[i]);
          }
        }
      }

      // Time-based animations
      const time = Date.now() / 1000;

      // Pulse the glowing core based on audio intensity
      const coreScale = 1.0 + normalizedLevel * 0.25;
      const coreOpacity = 0.6 + normalizedLevel * 0.4;  // 0.6 to 1.0
      if (elements.ringInner) {
        elements.ringInner.style.transform = `scale(${coreScale.toFixed(3)})`;
        elements.ringInner.style.opacity = coreOpacity.toFixed(2);
      }

      // Rotate outer ring for circular motion effect
      const outerRotation = (time * 45) % 360;  // 45 deg/sec rotation
      if (elements.ringOuter) {
        elements.ringOuter.style.transform = `rotate(${outerRotation.toFixed(1)}deg)`;
      }

      fftAnimationId = requestAnimationFrame(animate);
    }

    fftAnimationId = requestAnimationFrame(animate);
  }

  /**
   * Stop FFT visualization and restore CSS animations
   */
  function stopFFTVisualization() {
    if (fftAnimationId) {
      cancelAnimationFrame(fftAnimationId);
      fftAnimationId = null;
    }

    // Reset to default circular waveform
    drawDefaultWaveform();

    // Reset ring transforms and remove fft-active class
    if (elements.ringOuter) {
      elements.ringOuter.style.transform = '';
    }
    if (elements.ringInner) {
      elements.ringInner.style.transform = '';
      elements.ringInner.style.opacity = '';  // Reset to CSS default
    }
    if (elements.ringContainer) {
      elements.ringContainer.classList.remove('fft-active');
    }
    console.log('Stopped FFT visualization');
  }

  /**
   * Update mic button visual state
   */
  function updateMicButton(recording) {
    if (elements.micBtn) {
      elements.micBtn.classList.toggle('recording', recording);
      elements.micBtn.textContent = recording ? 'Stop' : 'Mic';
      elements.micBtn.title = recording ? 'Stop recording' : 'Start recording';
    }
  }

  function sendTextMessage(text) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      console.error('WebSocket not connected');
      return false;
    }

    const msg = {
      type: 'text',
      payload: { text: text }
    };

    ws.send(JSON.stringify(msg));
    // Note: Server echoes user message back as transcript, so no local entry needed
    // State update also comes from server
    return true;
  }

  // =============================================================================
  // UI Updates
  // =============================================================================
  function updateConnectionStatus(status) {
    elements.connectionStatus.className = status;
    elements.connectionStatus.textContent =
      status === 'connected' ? 'Connected' :
      status === 'connecting' ? 'Connecting...' :
      'Disconnected';
  }

  function updateState(state) {
    const previousState = currentState;
    currentState = state;

    // Update status indicator
    elements.statusDot.className = state;
    elements.statusText.textContent = state.toUpperCase();

    // Update ring container - preserve fft-active class if present
    const hasFftActive = elements.ringContainer.classList.contains('fft-active');
    elements.ringContainer.classList.remove(previousState);
    elements.ringContainer.classList.add(state);
    // Re-add fft-active if it was there (in case it got removed)
    if (hasFftActive) {
      elements.ringContainer.classList.add('fft-active');
    }
  }

  /**
   * Check if text contains command tags
   */
  function containsCommandTags(text) {
    return text.includes('<command>') || text.includes('[Tool Result:');
  }

  /**
   * Check if text is ONLY debug content (no user-facing text)
   */
  function isOnlyDebugContent(text) {
    // Remove all command tags and tool results, see if anything meaningful remains
    const stripped = text
      .replace(/<command>[\s\S]*?<\/command>/g, '')
      .replace(/\[Tool Result:[\s\S]*?\]/g, '')
      .trim();
    return stripped.length === 0;
  }

  /**
   * Extract non-command text from a mixed message
   */
  function extractUserFacingText(text) {
    return text
      .replace(/<command>[\s\S]*?<\/command>/g, '')
      .replace(/\[Tool Result:[\s\S]*?\]/g, '')
      .trim();
  }

  /**
   * Extract command/debug portions from a message
   */
  function extractDebugContent(text) {
    const commands = [];
    const toolResults = [];

    // Extract command tags
    const cmdRegex = /<command>([\s\S]*?)<\/command>/g;
    let match;
    while ((match = cmdRegex.exec(text)) !== null) {
      commands.push(match[0]);
    }

    // Extract tool results
    const toolRegex = /\[Tool Result:[\s\S]*?\]/g;
    while ((match = toolRegex.exec(text)) !== null) {
      toolResults.push(match[0]);
    }

    return { commands, toolResults };
  }

  /**
   * Format command text for debug display
   */
  function formatCommandText(text) {
    // Highlight <command> tags in cyan, tool results in green
    return text
      .replace(/(<command>)/g, '<span style="color:#22d3ee">$1</span>')
      .replace(/(<\/command>)/g, '<span style="color:#22d3ee">$1</span>')
      .replace(/(\[Tool Result:[^\]]*\])/g, '<span style="color:#22c55e">$1</span>');
  }

  /**
   * Add a debug entry to the transcript
   */
  function addDebugEntry(label, content) {
    const entry = document.createElement('div');

    // Determine specific debug class based on label
    let debugClass = 'debug';
    if (label === 'command') {
      debugClass = 'debug command';
    } else if (label === 'tool result') {
      debugClass = 'debug tool-result';
    }

    entry.className = `transcript-entry ${debugClass}`;
    entry.innerHTML = `
      <div class="role">${escapeHtml(label)}</div>
      <div class="text">${formatCommandText(escapeHtml(content))}</div>
    `;
    if (!debugMode) {
      entry.style.display = 'none';
    }
    elements.transcript.appendChild(entry);
  }

  /**
   * Add a normal entry to the transcript
   */
  function addNormalEntry(role, text) {
    const entry = document.createElement('div');
    entry.className = `transcript-entry ${role}`;
    entry.innerHTML = `
      <div class="role">${escapeHtml(role)}</div>
      <div class="text">${escapeHtml(text)}</div>
    `;
    elements.transcript.appendChild(entry);
  }

  function addTranscriptEntry(role, text) {
    // Remove placeholder if present
    const placeholder = elements.transcript.querySelector('.transcript-placeholder');
    if (placeholder) {
      placeholder.remove();
    }

    // Special case: Tool results are sent as complete messages starting with [Tool Result:
    // These can contain ] characters in the content, so don't try to parse with regex
    if (text.startsWith('[Tool Result:')) {
      addDebugEntry('tool result', text);
      elements.transcript.scrollTop = elements.transcript.scrollHeight;
      return;
    }

    const hasDebugContent = containsCommandTags(text);

    if (!hasDebugContent) {
      // Pure user-facing message - show normally
      addNormalEntry(role, text);
    } else if (isOnlyDebugContent(text)) {
      // Pure debug message (only commands/tool results) - debug only
      addDebugEntry(`debug (${role})`, text);
    } else {
      // Mixed message - show user-facing text normally AND debug content separately
      const userText = extractUserFacingText(text);
      const { commands, toolResults } = extractDebugContent(text);

      // Add debug entries for commands
      commands.forEach(cmd => {
        addDebugEntry('command', cmd);
      });

      // Add debug entries for tool results
      toolResults.forEach(result => {
        addDebugEntry('tool result', result);
      });

      // Add user-facing text if any
      if (userText.length > 0) {
        addNormalEntry(role, userText);
      }
    }

    elements.transcript.scrollTop = elements.transcript.scrollHeight;
  }

  function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
  }

  // =============================================================================
  // Event Handlers
  // =============================================================================
  function handleSend() {
    const text = elements.textInput.value.trim();
    if (text) {
      if (sendTextMessage(text)) {
        elements.textInput.value = '';
      }
    }
  }

  function handleKeydown(event) {
    if (event.key === 'Enter' && !event.shiftKey) {
      event.preventDefault();
      handleSend();
    }
  }

  // =============================================================================
  // Initialization
  // =============================================================================
  async function init() {
    // Event listeners
    elements.sendBtn.addEventListener('click', handleSend);
    elements.textInput.addEventListener('keydown', handleKeydown);

    // Mic button - push to talk
    if (elements.micBtn) {
      elements.micBtn.addEventListener('mousedown', function(e) {
        e.preventDefault();
        if (!isRecording && audioSupported) {
          startRecording();
        }
      });

      elements.micBtn.addEventListener('mouseup', function(e) {
        e.preventDefault();
        if (isRecording) {
          stopRecording();
        }
      });

      elements.micBtn.addEventListener('mouseleave', function(e) {
        // Stop recording if mouse leaves button while pressed
        if (isRecording) {
          stopRecording();
        }
      });

      // Touch events for mobile
      elements.micBtn.addEventListener('touchstart', function(e) {
        e.preventDefault();
        if (!isRecording && audioSupported) {
          startRecording();
        }
      });

      elements.micBtn.addEventListener('touchend', function(e) {
        e.preventDefault();
        if (isRecording) {
          stopRecording();
        }
      });
    }

    // Debug mode toggle
    elements.debugCheckbox.addEventListener('change', function() {
      debugMode = this.checked;
      console.log('Debug mode:', debugMode ? 'enabled' : 'disabled');

      // Toggle visibility of existing debug entries
      const debugEntries = elements.transcript.querySelectorAll('.transcript-entry.debug');
      debugEntries.forEach(entry => {
        entry.style.display = debugMode ? 'block' : 'none';
      });

      // Scroll to bottom to see newly visible entries
      elements.transcript.scrollTop = elements.transcript.scrollHeight;
    });

    // Initialize audio support
    const audioResult = await initAudio();
    if (audioResult.supported && elements.micBtn) {
      elements.micBtn.disabled = false;
      elements.micBtn.title = 'Hold to speak';
    } else if (elements.micBtn) {
      // Show why audio is disabled
      elements.micBtn.title = audioResult.reason || 'Audio not available';
      console.warn('Mic disabled:', audioResult.reason);
    }

    // Draw default waveform shape (circle) on page load
    drawDefaultWaveform();

    // Connect to WebSocket
    connect();

    // Reconnect on visibility change
    document.addEventListener('visibilitychange', function() {
      if (!document.hidden && (!ws || ws.readyState !== WebSocket.OPEN)) {
        connect();
      }
    });

    console.log('DAWN WebUI initialized (audio:', audioResult.supported ? 'enabled' : 'disabled', ')');
  }

  // Start when DOM is ready
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }

})();
