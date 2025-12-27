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
    debugBtn: document.getElementById('debug-btn'),
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

      // Try to reconnect with existing session token, or request new session
      const savedToken = localStorage.getItem('dawn_session_token');
      if (savedToken) {
        console.log('Attempting session reconnect with token:', savedToken.substring(0, 8) + '...');
        ws.send(JSON.stringify({
          type: 'reconnect',
          payload: { token: savedToken }
        }));
      } else {
        // No saved token - request a new session
        console.log('No saved token, requesting new session');
        ws.send(JSON.stringify({
          type: 'init',
          payload: {}
        }));
      }
    };

    ws.onclose = function(event) {
      console.log('WebSocket closed:', event.code, event.reason);
      // Show close reason if available (e.g., "max clients reached")
      if (event.reason) {
        updateConnectionStatus('disconnected', event.reason);
      } else {
        updateConnectionStatus('disconnected');
      }
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
          // Check for special LLM state update (sent with role '__llm_state__')
          if (msg.payload.role === '__llm_state__') {
            try {
              const stateMsg = JSON.parse(msg.payload.text);
              if (stateMsg.type === 'llm_state_update' && stateMsg.payload) {
                console.log('LLM state update received:', stateMsg.payload);
                updateLlmControls(stateMsg.payload);
              }
            } catch (e) {
              console.error('Failed to parse LLM state update:', e);
            }
          } else {
            addTranscriptEntry(msg.payload.role, msg.payload.text);
          }
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
        case 'get_config_response':
          handleGetConfigResponse(msg.payload);
          break;
        case 'set_config_response':
          handleSetConfigResponse(msg.payload);
          break;
        case 'set_secrets_response':
          handleSetSecretsResponse(msg.payload);
          break;
        case 'get_audio_devices_response':
          handleGetAudioDevicesResponse(msg.payload);
          break;
        case 'list_models_response':
          handleModelsListResponse(msg.payload);
          break;
        case 'list_interfaces_response':
          handleInterfacesListResponse(msg.payload);
          break;
        case 'restart_response':
          handleRestartResponse(msg.payload);
          break;
        case 'set_session_llm_response':
          handleSetSessionLlmResponse(msg.payload);
          break;
        case 'smartthings_status_response':
          handleSmartThingsStatusResponse(msg.payload);
          break;
        case 'smartthings_auth_url_response':
          handleSmartThingsAuthUrlResponse(msg.payload);
          break;
        case 'smartthings_exchange_code_response':
          handleSmartThingsExchangeCodeResponse(msg.payload);
          break;
        case 'smartthings_disconnect_response':
          handleSmartThingsDisconnectResponse(msg.payload);
          break;
        case 'smartthings_devices_response':
          handleSmartThingsDevicesResponse(msg.payload);
          break;
        case 'system_prompt_response':
          handleSystemPromptResponse(msg.payload);
          break;
        case 'get_tools_config_response':
          handleGetToolsConfigResponse(msg.payload);
          break;
        case 'set_tools_config_response':
          handleSetToolsConfigResponse(msg.payload);
          break;
        case 'context':
          updateContextDisplay(msg.payload);
          break;
        case 'get_metrics_response':
          handleMetricsResponse(msg.payload);
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
  function updateConnectionStatus(status, reason) {
    elements.connectionStatus.className = status;
    if (status === 'connected') {
      elements.connectionStatus.textContent = 'Connected';
    } else if (status === 'connecting') {
      elements.connectionStatus.textContent = 'Connecting...';
    } else {
      // Show disconnect reason if available (truncate for display)
      elements.connectionStatus.textContent = reason
        ? 'Disconnected: ' + (reason.length > 30 ? reason.substring(0, 30) + '...' : reason)
        : 'Disconnected';
    }
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
   * Update context/token usage display
   * @param {Object} payload - Context info {current, max, usage, threshold}
   */
  function updateContextDisplay(payload) {
    const contextDisplay = document.getElementById('context-display');
    const contextBar = document.getElementById('context-bar');
    const contextThreshold = document.getElementById('context-threshold');
    const contextText = document.getElementById('context-text');

    if (!contextDisplay || !contextBar || !contextText) {
      console.warn('Context display elements not found');
      return;
    }

    const { current, max, usage, threshold } = payload;

    // Show the display
    contextDisplay.classList.remove('hidden');

    // Update the progress bar
    const usagePercent = Math.min(usage, 100);
    contextBar.style.width = `${usagePercent}%`;

    // Set color based on usage level
    contextBar.classList.remove('warning', 'danger');
    if (usage >= threshold) {
      contextBar.classList.add('danger');
    } else if (usage >= threshold * 0.75) {
      contextBar.classList.add('warning');
    }

    // Position the threshold marker
    if (contextThreshold) {
      contextThreshold.style.left = `${threshold}%`;
    }

    // Update text
    const currentK = (current / 1000).toFixed(1);
    const maxK = (max / 1000).toFixed(0);
    contextText.textContent = `${currentK}k/${maxK}k (${usage.toFixed(0)}%)`;

    console.log(`Context update: ${current}/${max} tokens (${usage.toFixed(1)}%), threshold: ${threshold}%`);
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
    } else if (label === 'tool call') {
      debugClass = 'debug tool-call';
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

    // Special case: Tool calls/results are sent as complete messages
    // These can contain ] characters in the content, so don't try to parse with regex
    if (text.startsWith('[Tool Result:')) {
      addDebugEntry('tool result', text);
      elements.transcript.scrollTop = elements.transcript.scrollHeight;
      return;
    }
    if (text.startsWith('[Tool Call:')) {
      addDebugEntry('tool call', text);
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
    elements.debugBtn.addEventListener('click', function() {
      debugMode = !debugMode;
      this.classList.toggle('active', debugMode);
      console.log('Debug mode:', debugMode ? 'enabled' : 'disabled');

      // Toggle visibility of existing debug entries
      const debugEntries = elements.transcript.querySelectorAll('.transcript-entry.debug');
      debugEntries.forEach(entry => {
        entry.style.display = debugMode ? 'block' : 'none';
      });

      // Request system prompt when debug is enabled
      if (debugMode && ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ type: 'get_system_prompt' }));
      }

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

    // Initialize settings panel
    initSettingsElements();
    initSettingsListeners();
    initToolsSection();

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

  // =============================================================================
  // Settings Panel
  // =============================================================================

  // Settings state
  let currentConfig = null;
  let currentSecrets = null;
  let restartRequiredFields = [];
  let changedFields = new Set();
  let dynamicOptions = { asr_models: [], tts_voices: [], bind_addresses: [] };

  // Settings DOM elements (added after init)
  const settingsElements = {};

  // Settings schema for dynamic form generation
  const SETTINGS_SCHEMA = {
    general: {
      label: 'General',
      icon: '&#x2699;',
      fields: {
        ai_name: { type: 'text', label: 'AI Name / Wake Word', hint: 'Wake word to activate voice input' },
        log_file: { type: 'text', label: 'Log File Path', placeholder: 'Leave empty for stdout', hint: 'Path to log file, or empty for console output' }
      }
    },
    persona: {
      label: 'Persona',
      icon: '&#x1F464;',
      fields: {
        description: { type: 'textarea', label: 'AI Description', rows: 3, placeholder: 'Custom personality description', hint: 'Personality and behavior instructions for the AI. Changes apply to new conversations only.' }
      }
    },
    localization: {
      label: 'Localization',
      icon: '&#x1F30D;',
      fields: {
        location: { type: 'text', label: 'Location', placeholder: 'e.g., San Francisco, CA', hint: 'Default location for weather and local queries' },
        timezone: {
          type: 'select',
          label: 'Timezone',
          hint: 'IANA timezone for time-related responses',
          options: [
            '', // System default
            'America/New_York',
            'America/Chicago',
            'America/Denver',
            'America/Phoenix',
            'America/Los_Angeles',
            'America/Anchorage',
            'America/Honolulu',
            'America/Toronto',
            'America/Vancouver',
            'America/Mexico_City',
            'America/Sao_Paulo',
            'America/Buenos_Aires',
            'Europe/London',
            'Europe/Paris',
            'Europe/Berlin',
            'Europe/Rome',
            'Europe/Madrid',
            'Europe/Amsterdam',
            'Europe/Moscow',
            'Asia/Tokyo',
            'Asia/Shanghai',
            'Asia/Hong_Kong',
            'Asia/Singapore',
            'Asia/Seoul',
            'Asia/Mumbai',
            'Asia/Dubai',
            'Asia/Bangkok',
            'Australia/Sydney',
            'Australia/Melbourne',
            'Australia/Perth',
            'Pacific/Auckland',
            'Pacific/Fiji',
            'UTC'
          ]
        },
        units: { type: 'select', label: 'Units', options: ['imperial', 'metric'], hint: 'Measurement system for weather and calculations' }
      }
    },
    audio: {
      label: 'Audio',
      icon: '&#x1F50A;',
      fields: {
        backend: { type: 'select', label: 'Backend', options: ['auto', 'pulse', 'alsa'], restart: true, hint: 'Audio system: auto detects PulseAudio or falls back to ALSA' },
        capture_device: { type: 'text', label: 'Capture Device', restart: true, hint: 'Microphone device name (e.g., default, hw:0,0)' },
        playback_device: { type: 'text', label: 'Playback Device', restart: true, hint: 'Speaker device name (e.g., default, hw:0,0)' },
        bargein: {
          type: 'group',
          label: 'Barge-In',
          fields: {
            enabled: { type: 'checkbox', label: 'Enable Barge-In', hint: 'Allow interrupting AI speech with new voice input' },
            cooldown_ms: { type: 'number', label: 'Cooldown (ms)', min: 0, hint: 'Minimum time between barge-in events' },
            startup_cooldown_ms: { type: 'number', label: 'Startup Cooldown (ms)', min: 0, hint: 'Block barge-in when TTS first starts speaking' }
          }
        }
      }
    },
    commands: {
      label: 'Commands',
      icon: '&#x2328;',
      fields: {
        processing_mode: { type: 'select', label: 'Processing Mode', options: ['direct_only', 'llm_only', 'direct_first'], restart: true, hint: 'direct_only: pattern matching only, llm_only: AI interprets all commands, direct_first: try patterns then AI' }
      }
    },
    vad: {
      label: 'Voice Activity Detection',
      icon: '&#x1F3A4;',
      fields: {
        speech_threshold: { type: 'number', label: 'Speech Threshold', min: 0, max: 1, step: 0.05, hint: 'VAD confidence to start listening (higher = less sensitive)' },
        speech_threshold_tts: { type: 'number', label: 'Speech Threshold (TTS)', min: 0, max: 1, step: 0.05, hint: 'VAD threshold during TTS playback (higher to ignore echo)' },
        silence_threshold: { type: 'number', label: 'Silence Threshold', min: 0, max: 1, step: 0.05, hint: 'VAD confidence to detect end of speech' },
        end_of_speech_duration: { type: 'number', label: 'End of Speech (sec)', min: 0, step: 0.1, hint: 'Silence duration before processing speech' },
        max_recording_duration: { type: 'number', label: 'Max Recording (sec)', min: 1, step: 1, hint: 'Maximum single utterance length' },
        preroll_ms: { type: 'number', label: 'Preroll (ms)', min: 0, hint: 'Audio captured before VAD trigger (catches word beginnings)' }
      }
    },
    asr: {
      label: 'Speech Recognition',
      icon: '&#x1F4DD;',
      fields: {
        models_path: { type: 'text', label: 'Models Path', restart: true, hint: 'Directory containing Whisper model files' },
        model: { type: 'dynamic_select', label: 'Model', restart: true, hint: 'Whisper model size', dynamicKey: 'asr_models', allowCustom: true }
      }
    },
    tts: {
      label: 'Text-to-Speech',
      icon: '&#x1F5E3;',
      fields: {
        models_path: { type: 'text', label: 'Models Path', restart: true, hint: 'Directory containing Piper voice model files' },
        voice_model: { type: 'dynamic_select', label: 'Voice Model', restart: true, hint: 'Piper voice model', dynamicKey: 'tts_voices', allowCustom: true },
        length_scale: { type: 'number', label: 'Speed (0.5-2.0)', min: 0.5, max: 2.0, step: 0.05, hint: 'Speaking rate: <1.0 = faster, >1.0 = slower' }
      }
    },
    llm: {
      label: 'Language Model',
      icon: '&#x1F916;',
      fields: {
        type: { type: 'select', label: 'Type', options: ['cloud', 'local'], hint: 'Use cloud APIs or local llama-server' },
        max_tokens: { type: 'number', label: 'Max Tokens', min: 100, hint: 'Maximum tokens in LLM response' },
        cloud: {
          type: 'group',
          label: 'Cloud Settings',
          fields: {
            provider: { type: 'select', label: 'Provider', options: ['openai', 'claude'], hint: 'Cloud LLM provider' },
            openai_model: { type: 'text', label: 'OpenAI Model', hint: 'e.g., gpt-4o, gpt-4-turbo, gpt-4o-mini' },
            claude_model: { type: 'text', label: 'Claude Model', hint: 'e.g., claude-sonnet-4-20250514, claude-opus-4-20250514' },
            endpoint: { type: 'text', label: 'Custom Endpoint', placeholder: 'Leave empty for default', hint: 'Override API endpoint (for proxies or compatible APIs)' },
            vision_enabled: { type: 'checkbox', label: 'Enable Vision', hint: 'Allow image analysis with vision-capable models' }
          }
        },
        local: {
          type: 'group',
          label: 'Local Settings',
          fields: {
            endpoint: { type: 'text', label: 'Endpoint', hint: 'llama-server URL (e.g., http://127.0.0.1:8080)' },
            model: { type: 'text', label: 'Model', placeholder: 'Leave empty for server default', hint: 'Model name if server hosts multiple models' },
            vision_enabled: { type: 'checkbox', label: 'Enable Vision', hint: 'Enable for multimodal models like LLaVA' }
          }
        },
        tools: {
          type: 'group',
          label: 'Tool Calling',
          fields: {
            native_enabled: { type: 'checkbox', label: 'Enable Native Tools', restart: true, hint: 'Use native function/tool calling instead of <command> tags (requires compatible LLM)' }
          }
        }
      }
    },
    search: {
      label: 'Web Search',
      icon: '&#x1F50D;',
      fields: {
        engine: { type: 'select', label: 'Engine', options: ['searxng', 'disabled'], hint: 'Search engine for web queries (SearXNG is privacy-focused)' },
        endpoint: { type: 'text', label: 'Endpoint', hint: 'SearXNG instance URL (e.g., http://localhost:8888)' },
        summarizer: {
          type: 'group',
          label: 'Result Summarizer',
          fields: {
            backend: { type: 'select', label: 'Backend', options: ['disabled', 'local', 'default'], hint: 'disabled: no summarization, local: use local LLM, default: use active LLM' },
            threshold_bytes: { type: 'number', label: 'Threshold (bytes)', min: 0, step: 512, hint: 'Summarize results larger than this (0 = always summarize)' },
            target_words: { type: 'number', label: 'Target Words', min: 50, step: 50, hint: 'Target word count for summarized output' }
          }
        }
      }
    },
    url_fetcher: {
      label: 'URL Fetcher',
      icon: '&#x1F310;',
      fields: {
        flaresolverr: {
          type: 'group',
          label: 'FlareSolverr',
          fields: {
            enabled: { type: 'checkbox', label: 'Enable FlareSolverr', hint: 'Auto-fallback for sites with Cloudflare protection (requires FlareSolverr service)' },
            endpoint: { type: 'text', label: 'Endpoint', hint: 'FlareSolverr API URL (e.g., http://localhost:8191/v1)' },
            timeout_sec: { type: 'number', label: 'Timeout (sec)', min: 1, max: 120, hint: 'Request timeout for FlareSolverr' },
            max_response_bytes: { type: 'number', label: 'Max Response (bytes)', min: 1024, step: 1024, hint: 'Maximum response size to accept' }
          }
        }
      }
    },
    mqtt: {
      label: 'MQTT',
      icon: '&#x1F4E1;',
      fields: {
        enabled: { type: 'checkbox', label: 'Enable MQTT', hint: 'Connect to MQTT broker for smart home control' },
        broker: { type: 'text', label: 'Broker Address', hint: 'MQTT broker hostname or IP address' },
        port: { type: 'number', label: 'Port', min: 1, max: 65535, hint: 'MQTT broker port (default: 1883)' }
      }
    },
    network: {
      label: 'Network (DAP)',
      description: 'Dawn Audio Protocol server for ESP32 and other remote voice clients',
      icon: '&#x1F4F6;',
      fields: {
        enabled: { type: 'checkbox', label: 'Enable DAP Server', restart: true, hint: 'Accept connections from remote voice clients' },
        host: { type: 'dynamic_select', label: 'Bind Address', restart: true, hint: 'Network interface to listen on', dynamicKey: 'bind_addresses' },
        port: { type: 'number', label: 'Port', min: 1, max: 65535, restart: true, hint: 'TCP port for DAP connections (default: 5000)' },
        workers: { type: 'number', label: 'Workers', min: 1, max: 8, restart: true, hint: 'Concurrent client processing threads' }
      }
    },
    webui: {
      label: 'WebUI',
      icon: '&#x1F310;',
      fields: {
        enabled: { type: 'checkbox', label: 'Enable WebUI', hint: 'Browser-based interface for voice interaction' },
        port: { type: 'number', label: 'Port', min: 1, max: 65535, restart: true, hint: 'HTTP/WebSocket port (default: 3000)' },
        max_clients: { type: 'number', label: 'Max Clients', min: 1, restart: true, hint: 'Maximum concurrent browser connections' },
        workers: { type: 'number', label: 'ASR Workers', min: 1, max: 8, restart: true, hint: 'Parallel speech recognition threads' },
        bind_address: { type: 'dynamic_select', label: 'Bind Address', restart: true, hint: 'Network interface to listen on', dynamicKey: 'bind_addresses' },
        https: { type: 'checkbox', label: 'Enable HTTPS', restart: true, hint: 'Required for microphone access on remote connections' }
      }
    },
    shutdown: {
      label: 'Shutdown',
      icon: '&#x1F512;',
      fields: {
        enabled: { type: 'checkbox', label: 'Enable Voice Shutdown', hint: 'Allow system shutdown via voice command (disabled by default for security)' },
        passphrase: { type: 'text', label: 'Passphrase', hint: 'Secret phrase required to authorize shutdown (leave empty for no passphrase)' }
      }
    },
    debug: {
      label: 'Debug',
      icon: '&#x1F41B;',
      fields: {
        mic_record: { type: 'checkbox', label: 'Record Microphone' },
        asr_record: { type: 'checkbox', label: 'Record ASR Input' },
        aec_record: { type: 'checkbox', label: 'Record AEC' },
        record_path: { type: 'text', label: 'Recording Path' }
      }
    },
    tui: {
      label: 'Terminal UI',
      icon: '&#x1F5A5;',
      fields: {
        enabled: { type: 'checkbox', label: 'Enable TUI', restart: true, hint: 'Show terminal dashboard with real-time metrics' }
      }
    },
    paths: {
      label: 'Paths',
      icon: '&#x1F4C1;',
      fields: {
        music_dir: { type: 'text', label: 'Music Directory', hint: 'Path to music library for playback commands' },
        commands_config: { type: 'text', label: 'Commands Config', restart: true, hint: 'Path to device/command mappings JSON file' }
      }
    }
  };

  /**
   * Initialize settings panel elements
   */
  function initSettingsElements() {
    settingsElements.panel = document.getElementById('settings-panel');
    settingsElements.overlay = document.getElementById('settings-overlay');
    settingsElements.closeBtn = document.getElementById('settings-close');
    settingsElements.openBtn = document.getElementById('settings-btn');
    settingsElements.configPath = document.getElementById('config-path-display');
    settingsElements.secretsPath = document.getElementById('secrets-path-display');
    settingsElements.sectionsContainer = document.getElementById('settings-sections');
    settingsElements.saveConfigBtn = document.getElementById('save-config-btn');
    settingsElements.saveSecretsBtn = document.getElementById('save-secrets-btn');
    settingsElements.resetBtn = document.getElementById('reset-config-btn');
    settingsElements.restartNotice = document.getElementById('restart-notice');

    // Secret inputs
    settingsElements.secretOpenai = document.getElementById('secret-openai');
    settingsElements.secretClaude = document.getElementById('secret-claude');
    settingsElements.secretMqttUser = document.getElementById('secret-mqtt-user');
    settingsElements.secretMqttPass = document.getElementById('secret-mqtt-pass');

    // Secret status indicators
    settingsElements.statusOpenai = document.getElementById('status-openai');
    settingsElements.statusClaude = document.getElementById('status-claude');
    settingsElements.statusMqttUser = document.getElementById('status-mqtt-user');
    settingsElements.statusMqttPass = document.getElementById('status-mqtt-pass');

    // SmartThings elements
    settingsElements.stStatusIndicator = document.getElementById('st-status-indicator');
    settingsElements.stStatusText = settingsElements.stStatusIndicator?.querySelector('.st-status-text');
    settingsElements.stDevicesCountRow = document.getElementById('st-devices-count-row');
    settingsElements.stDevicesCount = document.getElementById('st-devices-count');
    settingsElements.stConnectBtn = document.getElementById('st-connect-btn');
    settingsElements.stRefreshBtn = document.getElementById('st-refresh-btn');
    settingsElements.stDisconnectBtn = document.getElementById('st-disconnect-btn');
    settingsElements.stDevicesList = document.getElementById('st-devices-list');
    settingsElements.stDevicesContainer = document.getElementById('st-devices-container');
    settingsElements.stNotConfigured = document.getElementById('st-not-configured');
  }

  /**
   * Open settings panel and request config
   */
  function openSettings() {
    if (!settingsElements.panel) return;

    settingsElements.panel.classList.remove('hidden');
    settingsElements.overlay.classList.remove('hidden');

    // Request config, models, and interfaces from server
    requestConfig();
    requestModelsList();
    requestInterfacesList();
    requestSmartThingsStatus();
  }

  /**
   * Close settings panel
   */
  function closeSettings() {
    if (!settingsElements.panel) return;

    settingsElements.panel.classList.add('hidden');
    settingsElements.overlay.classList.add('hidden');
  }

  /**
   * Request current configuration from server
   */
  function requestConfig() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      console.error('WebSocket not connected');
      return;
    }

    ws.send(JSON.stringify({ type: 'get_config' }));
    console.log('Requested configuration from server');
  }

  /**
   * Request available ASR/TTS models list from server
   */
  function requestModelsList() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }

    ws.send(JSON.stringify({ type: 'list_models' }));
    console.log('Requested models list from server');
  }

  /**
   * Handle models list response from server
   */
  function handleModelsListResponse(payload) {
    if (payload.asr_models) {
      dynamicOptions.asr_models = payload.asr_models;
    }
    if (payload.tts_voices) {
      dynamicOptions.tts_voices = payload.tts_voices;
    }
    console.log('Received models list:', dynamicOptions);

    // Update any already-rendered dynamic selects
    updateDynamicSelects();
  }

  /**
   * Request available network interfaces from server
   */
  function requestInterfacesList() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }

    ws.send(JSON.stringify({ type: 'list_interfaces' }));
    console.log('Requested interfaces list from server');
  }

  /**
   * Handle interfaces list response from server
   */
  function handleInterfacesListResponse(payload) {
    if (payload.addresses) {
      dynamicOptions.bind_addresses = payload.addresses;
    }
    console.log('Received interfaces list:', dynamicOptions.bind_addresses);

    // Update any already-rendered dynamic selects
    updateDynamicSelects();
  }

  /**
   * Update dynamic select dropdowns with current options
   */
  function updateDynamicSelects() {
    document.querySelectorAll('select[data-dynamic-key]').forEach(select => {
      const key = select.dataset.dynamicKey;
      const options = dynamicOptions[key] || [];
      const currentValue = select.value;

      // Clear existing options except the first (current value placeholder)
      while (select.options.length > 1) {
        select.remove(1);
      }

      // Add options from server (skip current value to avoid duplicate)
      options.forEach(opt => {
        if (opt === currentValue) return; // Skip - already in first option
        const option = document.createElement('option');
        option.value = opt;
        option.textContent = opt;
        select.appendChild(option);
      });

      // If current value isn't in options, mark it as "(current)"
      if (currentValue && !options.includes(currentValue)) {
        select.options[0].textContent = currentValue + ' (current)';
      }
    });
  }

  /**
   * Handle config response from server
   */
  function handleGetConfigResponse(payload) {
    console.log('Received config:', payload);

    currentConfig = payload.config;
    currentSecrets = payload.secrets;
    restartRequiredFields = payload.requires_restart || [];
    changedFields.clear();

    // Update path displays
    if (settingsElements.configPath) {
      settingsElements.configPath.textContent = payload.config_path || 'Unknown';
    }
    if (settingsElements.secretsPath) {
      settingsElements.secretsPath.textContent = payload.secrets_path || 'Unknown';
    }

    // Render settings sections
    renderSettingsSections();

    // Update secrets status
    updateSecretsStatus(currentSecrets);

    // Hide restart notice initially
    if (settingsElements.restartNotice) {
      settingsElements.restartNotice.classList.add('hidden');
    }

    // Initialize audio backend state (grey out or request devices)
    const backendSelect = document.getElementById('setting-audio-backend');
    if (backendSelect) {
      // Add backend change listener
      backendSelect.addEventListener('change', () => {
        updateAudioBackendState(backendSelect.value);
      });

      // Initialize state based on current value
      updateAudioBackendState(backendSelect.value);
    }

    // Update LLM runtime controls
    if (payload.llm_runtime) {
      updateLlmControls(payload.llm_runtime);
    }
  }

  /**
   * Render settings sections dynamically from schema
   */
  function renderSettingsSections() {
    if (!settingsElements.sectionsContainer || !currentConfig) return;

    settingsElements.sectionsContainer.innerHTML = '';

    for (const [sectionKey, sectionDef] of Object.entries(SETTINGS_SCHEMA)) {
      const configSection = currentConfig[sectionKey] || {};
      const sectionEl = createSettingsSection(sectionKey, sectionDef, configSection);
      settingsElements.sectionsContainer.appendChild(sectionEl);
    }
  }

  /**
   * Create a settings section element
   */
  function createSettingsSection(key, def, configData) {
    const section = document.createElement('div');
    section.className = 'settings-section';
    section.dataset.section = key;

    // Header
    const header = document.createElement('h3');
    header.className = 'section-header';
    header.innerHTML = `
      <span class="section-icon">${def.icon}</span>
      ${def.label}
      <span class="section-toggle">&#9660;</span>
    `;
    header.addEventListener('click', () => {
      header.classList.toggle('collapsed');
      content.classList.toggle('collapsed');
    });

    // Content
    const content = document.createElement('div');
    content.className = 'section-content';

    // Add section description if present
    if (def.description) {
      const desc = document.createElement('p');
      desc.className = 'section-description';
      desc.textContent = def.description;
      content.appendChild(desc);
    }

    // Render fields
    for (const [fieldKey, fieldDef] of Object.entries(def.fields)) {
      const value = configData[fieldKey];
      const fieldEl = createSettingField(key, fieldKey, fieldDef, value);
      content.appendChild(fieldEl);
    }

    section.appendChild(header);
    section.appendChild(content);

    return section;
  }

  /**
   * Create a setting field element
   */
  function createSettingField(sectionKey, fieldKey, def, value) {
    const fullKey = `${sectionKey}.${fieldKey}`;

    // Handle nested groups
    if (def.type === 'group') {
      const groupEl = document.createElement('div');
      groupEl.className = 'setting-group';
      groupEl.innerHTML = `<div class="group-label">${def.label}</div>`;

      for (const [subKey, subDef] of Object.entries(def.fields)) {
        const subValue = value ? value[subKey] : undefined;
        const fieldEl = createSettingField(`${sectionKey}.${fieldKey}`, subKey, subDef, subValue);
        groupEl.appendChild(fieldEl);
      }

      return groupEl;
    }

    const item = document.createElement('div');
    item.className = def.type === 'checkbox' ? 'setting-item setting-item-row' : 'setting-item';

    // Add tooltip hint if defined
    if (def.hint) {
      item.title = def.hint;
    }

    const needsRestart = def.restart || restartRequiredFields.includes(fullKey);
    const restartBadge = needsRestart ? '<span class="restart-badge">restart</span>' : '';

    let inputHtml = '';
    const inputId = `setting-${sectionKey}-${fieldKey}`.replace(/\./g, '-');

    switch (def.type) {
      case 'text':
        inputHtml = `<input type="text" id="${inputId}" value="${escapeAttr(value || '')}" placeholder="${def.placeholder || ''}" data-key="${fullKey}">`;
        break;
      case 'number':
        const numAttrs = [
          def.min !== undefined ? `min="${def.min}"` : '',
          def.max !== undefined ? `max="${def.max}"` : '',
          def.step !== undefined ? `step="${def.step}"` : ''
        ].filter(Boolean).join(' ');
        inputHtml = `<input type="number" id="${inputId}" value="${formatNumber(value)}" ${numAttrs} data-key="${fullKey}">`;
        break;
      case 'checkbox':
        inputHtml = `<input type="checkbox" id="${inputId}" ${value ? 'checked' : ''} data-key="${fullKey}">`;
        break;
      case 'select':
        const options = def.options.map(opt => {
          const label = opt === '' ? '(System default)' : opt;
          return `<option value="${opt}" ${value === opt ? 'selected' : ''}>${label}</option>`;
        }).join('');
        inputHtml = `<select id="${inputId}" data-key="${fullKey}">${options}</select>`;
        break;
      case 'dynamic_select':
        // Dynamic select that gets options from server (e.g., model lists)
        const dynKey = def.dynamicKey;
        const dynOptions = dynamicOptions[dynKey] || [];
        const currentVal = value || '';

        // Start with current value as first option
        let dynOptionsHtml = `<option value="${escapeAttr(currentVal)}" selected>${escapeHtml(currentVal) || '(none)'}</option>`;

        // Add options from server if available
        dynOptions.forEach(opt => {
          if (opt !== currentVal) {
            dynOptionsHtml += `<option value="${escapeAttr(opt)}">${escapeHtml(opt)}</option>`;
          }
        });

        inputHtml = `<select id="${inputId}" data-key="${fullKey}" data-dynamic-key="${dynKey}">${dynOptionsHtml}</select>`;
        break;
      case 'textarea':
        inputHtml = `<textarea id="${inputId}" rows="${def.rows || 3}" placeholder="${def.placeholder || ''}" data-key="${fullKey}">${escapeHtml(value || '')}</textarea>`;
        break;
    }

    if (def.type === 'checkbox') {
      item.innerHTML = `
        ${inputHtml}
        <label for="${inputId}">${def.label} ${restartBadge}</label>
      `;
    } else {
      item.innerHTML = `
        <label for="${inputId}">${def.label} ${restartBadge}</label>
        ${inputHtml}
      `;
    }

    // Add change listener
    const input = item.querySelector('input, select, textarea');
    if (input) {
      input.addEventListener('change', () => handleSettingChange(fullKey, input));
      input.addEventListener('input', () => handleSettingChange(fullKey, input));
    }

    return item;
  }

  /**
   * Handle setting value change
   */
  function handleSettingChange(key, input) {
    changedFields.add(key);

    // Check if this field requires restart
    if (restartRequiredFields.includes(key)) {
      if (settingsElements.restartNotice) {
        settingsElements.restartNotice.classList.remove('hidden');
      }
    }
  }

  /**
   * Update secrets status indicators
   */
  function updateSecretsStatus(secrets) {
    if (!secrets) return;

    const updateStatus = (el, isSet) => {
      if (!el) return;
      el.textContent = isSet ? 'Set' : 'Not set';
      el.className = `secret-status ${isSet ? 'is-set' : 'not-set'}`;
    };

    updateStatus(settingsElements.statusOpenai, secrets.openai_api_key);
    updateStatus(settingsElements.statusClaude, secrets.claude_api_key);
    updateStatus(settingsElements.statusMqttUser, secrets.mqtt_username);
    updateStatus(settingsElements.statusMqttPass, secrets.mqtt_password);
  }

  /**
   * Collect all config values from form
   */
  function collectConfigValues() {
    const config = {};
    const inputs = settingsElements.sectionsContainer.querySelectorAll('[data-key]');

    inputs.forEach(input => {
      const key = input.dataset.key;
      const parts = key.split('.');

      let value;
      if (input.type === 'checkbox') {
        value = input.checked;
      } else if (input.type === 'number') {
        value = input.value !== '' ? parseFloat(input.value) : null;
      } else {
        value = input.value;
      }

      // Build nested structure
      let obj = config;
      for (let i = 0; i < parts.length - 1; i++) {
        if (!obj[parts[i]]) obj[parts[i]] = {};
        obj = obj[parts[i]];
      }
      obj[parts[parts.length - 1]] = value;
    });

    // AI name should always be lowercase (for wake word detection)
    if (config.general && config.general.ai_name) {
      config.general.ai_name = config.general.ai_name.toLowerCase();
    }

    return config;
  }

  /**
   * Save configuration to server
   */
  function saveConfig() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      console.error('WebSocket not connected');
      alert('Cannot save: Not connected to server');
      return;
    }

    const config = collectConfigValues();
    console.log('Saving config:', config);

    ws.send(JSON.stringify({
      type: 'set_config',
      payload: config
    }));
  }

  /**
   * Save secrets to server
   */
  function saveSecrets() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      console.error('WebSocket not connected');
      alert('Cannot save: Not connected to server');
      return;
    }

    const secrets = {};

    // Only send non-empty values (don't overwrite with empty)
    if (settingsElements.secretOpenai && settingsElements.secretOpenai.value) {
      secrets.openai_api_key = settingsElements.secretOpenai.value;
    }
    if (settingsElements.secretClaude && settingsElements.secretClaude.value) {
      secrets.claude_api_key = settingsElements.secretClaude.value;
    }
    if (settingsElements.secretMqttUser && settingsElements.secretMqttUser.value) {
      secrets.mqtt_username = settingsElements.secretMqttUser.value;
    }
    if (settingsElements.secretMqttPass && settingsElements.secretMqttPass.value) {
      secrets.mqtt_password = settingsElements.secretMqttPass.value;
    }

    if (Object.keys(secrets).length === 0) {
      alert('No secrets entered to save');
      return;
    }

    console.log('Saving secrets (keys only):', Object.keys(secrets));

    ws.send(JSON.stringify({
      type: 'set_secrets',
      payload: secrets
    }));

    // Clear inputs after sending
    if (settingsElements.secretOpenai) settingsElements.secretOpenai.value = '';
    if (settingsElements.secretClaude) settingsElements.secretClaude.value = '';
    if (settingsElements.secretMqttUser) settingsElements.secretMqttUser.value = '';
    if (settingsElements.secretMqttPass) settingsElements.secretMqttPass.value = '';
  }

  /**
   * Handle set_config response
   */
  function handleSetConfigResponse(payload) {
    if (payload.success) {
      console.log('Config saved successfully');

      // Check if any changed fields require restart
      const restartFields = getChangedRestartRequiredFields();
      if (restartFields.length > 0) {
        console.log('Restart required for fields:', restartFields);
        showRestartConfirmation(restartFields);
      } else {
        alert('Configuration saved successfully!');
      }

      // Clear changed fields tracking
      changedFields.clear();
    } else {
      console.error('Failed to save config:', payload.error);
      alert('Failed to save configuration: ' + (payload.error || 'Unknown error'));
    }
  }

  /**
   * Get list of changed fields that require restart
   */
  function getChangedRestartRequiredFields() {
    const restartFields = [];
    for (const field of changedFields) {
      if (restartRequiredFields.includes(field)) {
        restartFields.push(field);
      }
    }
    return restartFields;
  }

  /**
   * Show restart confirmation dialog
   */
  function showRestartConfirmation(changedRestartFields) {
    const fieldList = changedRestartFields.map(f => '  • ' + f).join('\n');
    const message = 'Configuration saved successfully!\n\n' +
      'The following changes require a restart to take effect:\n' +
      fieldList + '\n\n' +
      'Do you want to restart DAWN now?';

    if (confirm(message)) {
      requestRestart();
    }
  }

  /**
   * Request application restart
   */
  function requestRestart() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      console.error('WebSocket not connected');
      alert('Cannot restart: Not connected to server');
      return;
    }

    console.log('Requesting application restart');
    ws.send(JSON.stringify({ type: 'restart' }));
  }

  /**
   * Handle restart response
   */
  function handleRestartResponse(payload) {
    if (payload.success) {
      console.log('Restart initiated:', payload.message);
      alert('DAWN is restarting. The page will attempt to reconnect automatically.');
    } else {
      console.error('Restart failed:', payload.error);
      alert('Failed to restart: ' + (payload.error || 'Unknown error'));
    }
  }

  /**
   * Handle system prompt response - display at top of transcript in debug mode
   */
  function handleSystemPromptResponse(payload) {
    if (!payload.success) {
      console.warn('Failed to get system prompt:', payload.error);
      return;
    }

    // Remove any existing system prompt entry
    const existing = document.getElementById('system-prompt-entry');
    if (existing) {
      existing.remove();
    }

    // Create collapsible system prompt entry
    const entry = document.createElement('div');
    entry.id = 'system-prompt-entry';
    entry.className = 'transcript-entry debug system-prompt';

    const promptLength = payload.length || payload.prompt.length;
    const tokenEstimate = Math.round(promptLength / 4); // Rough estimate

    entry.innerHTML = `
      <div class="system-prompt-header">
        <span class="system-prompt-icon">&#x2699;</span>
        <span class="system-prompt-title">System Prompt</span>
        <span class="system-prompt-stats">${promptLength.toLocaleString()} chars (~${tokenEstimate.toLocaleString()} tokens)</span>
        <span class="system-prompt-toggle">&#x25BC;</span>
      </div>
      <div class="system-prompt-content">
        <pre>${escapeHtml(payload.prompt)}</pre>
      </div>
    `;

    // Add click handler for expand/collapse
    const header = entry.querySelector('.system-prompt-header');
    header.addEventListener('click', function() {
      entry.classList.toggle('expanded');
    });

    // Insert at the top of the transcript (after placeholder if present)
    const placeholder = elements.transcript.querySelector('.transcript-placeholder');
    if (placeholder) {
      placeholder.after(entry);
    } else {
      elements.transcript.prepend(entry);
    }

    // Show only if debug mode is on
    if (!debugMode) {
      entry.style.display = 'none';
    }

    console.log(`System prompt loaded: ${promptLength} chars (~${tokenEstimate} tokens)`);
  }

  /**
   * Handle set_secrets response
   */
  function handleSetSecretsResponse(payload) {
    if (payload.success) {
      console.log('Secrets saved successfully');
      alert('Secrets saved successfully!');

      // Update status indicators
      if (payload.secrets) {
        updateSecretsStatus(payload.secrets);
      }
    } else {
      console.error('Failed to save secrets:', payload.error);
      alert('Failed to save secrets: ' + (payload.error || 'Unknown error'));
    }
  }

  // Audio device state
  let audioDevicesCache = { capture: [], playback: [] };

  /**
   * Request audio devices from server
   */
  function requestAudioDevices(backend) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      console.error('WebSocket not connected');
      return;
    }

    console.log('Requesting audio devices for backend:', backend);
    ws.send(JSON.stringify({
      type: 'get_audio_devices',
      payload: { backend: backend }
    }));
  }

  /**
   * Handle audio devices response
   */
  function handleGetAudioDevicesResponse(payload) {
    console.log('Received audio devices:', payload);

    audioDevicesCache.capture = payload.capture_devices || [];
    audioDevicesCache.playback = payload.playback_devices || [];

    // Update capture device field
    updateAudioDeviceField('capture_device', audioDevicesCache.capture);
    updateAudioDeviceField('playback_device', audioDevicesCache.playback);
  }

  /**
   * Update an audio device field to a select with options
   */
  function updateAudioDeviceField(fieldName, devices) {
    // ID matches how createSettingField generates it (underscores preserved)
    const inputId = `setting-audio-${fieldName}`;
    const input = document.getElementById(inputId);
    console.log('updateAudioDeviceField:', fieldName, 'inputId:', inputId, 'input:', input, 'devices:', devices);
    if (!input) return;

    const currentValue = input.value || '';
    const dataKey = input.dataset.key;
    const parent = input.parentElement;

    // Replace input with select
    const select = document.createElement('select');
    select.id = inputId;
    select.dataset.key = dataKey;

    // Add default option
    const defaultOpt = document.createElement('option');
    defaultOpt.value = '';
    defaultOpt.textContent = 'Default';
    select.appendChild(defaultOpt);

    // Add device options (devices can be strings or {id, name} objects)
    devices.forEach(device => {
      const opt = document.createElement('option');
      // Handle both string devices and object devices
      const deviceId = typeof device === 'string' ? device : device.id;
      const deviceName = typeof device === 'string' ? device : (device.name || device.id);
      opt.value = deviceId;
      opt.textContent = deviceName;
      if (deviceId === currentValue) {
        opt.selected = true;
      }
      select.appendChild(opt);
    });

    // Add change listener
    select.addEventListener('change', () => handleSettingChange(dataKey, select));

    // Replace input with select
    parent.replaceChild(select, input);
  }

  /**
   * Update audio device fields based on backend selection
   */
  function updateAudioBackendState(backend) {
    // IDs match how createSettingField generates them (underscores preserved)
    const captureInput = document.getElementById('setting-audio-capture_device');
    const playbackInput = document.getElementById('setting-audio-playback_device');

    console.log('updateAudioBackendState:', backend, 'captureInput:', captureInput, 'playbackInput:', playbackInput);

    if (backend === 'auto') {
      // Grey out device fields when auto
      if (captureInput) {
        captureInput.disabled = true;
        captureInput.title = 'Device is auto-detected when backend is "auto"';
      }
      if (playbackInput) {
        playbackInput.disabled = true;
        playbackInput.title = 'Device is auto-detected when backend is "auto"';
      }
    } else {
      // Enable device fields and request available devices
      if (captureInput) {
        captureInput.disabled = false;
        captureInput.title = '';
      }
      if (playbackInput) {
        playbackInput.disabled = false;
        playbackInput.title = '';
      }

      // Request available devices for this backend
      requestAudioDevices(backend);
    }
  }

  /**
   * Toggle secret visibility
   */
  function toggleSecretVisibility(targetId) {
    const input = document.getElementById(targetId);
    if (!input) return;

    input.type = input.type === 'password' ? 'text' : 'password';
  }

  /**
   * Escape attribute value
   */
  function escapeAttr(str) {
    return String(str)
      .replace(/&/g, '&amp;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;');
  }

  /**
   * Format a number for display, removing floating-point precision artifacts.
   * Rounds to 6 significant digits to clean up values like 0.9200000166893005 → 0.92
   */
  function formatNumber(value) {
    if (value === undefined || value === null || value === '') return '';
    const num = Number(value);
    if (isNaN(num)) return '';
    // Use toPrecision to limit significant digits, then parseFloat to remove trailing zeros
    return parseFloat(num.toPrecision(6));
  }

  /**
   * Initialize settings event listeners
   */
  function initSettingsListeners() {
    // Open button
    if (settingsElements.openBtn) {
      settingsElements.openBtn.addEventListener('click', openSettings);
    }

    // Close button
    if (settingsElements.closeBtn) {
      settingsElements.closeBtn.addEventListener('click', closeSettings);
    }

    // Overlay click to close
    if (settingsElements.overlay) {
      settingsElements.overlay.addEventListener('click', closeSettings);
    }

    // Save config button
    if (settingsElements.saveConfigBtn) {
      settingsElements.saveConfigBtn.addEventListener('click', saveConfig);
    }

    // Save secrets button
    if (settingsElements.saveSecretsBtn) {
      settingsElements.saveSecretsBtn.addEventListener('click', saveSecrets);
    }

    // Reset button
    if (settingsElements.resetBtn) {
      settingsElements.resetBtn.addEventListener('click', () => {
        if (confirm('Reset all settings to defaults? This will reload the current configuration.')) {
          requestConfig();
        }
      });
    }

    // Secret toggle buttons
    document.querySelectorAll('.secret-toggle').forEach(btn => {
      btn.addEventListener('click', () => {
        const targetId = btn.dataset.target;
        if (targetId) {
          toggleSecretVisibility(targetId);
        }
      });
    });

    // Section header toggle
    document.querySelectorAll('.section-header').forEach(header => {
      header.addEventListener('click', () => {
        header.classList.toggle('collapsed');
        const content = header.nextElementSibling;
        if (content) {
          content.classList.toggle('collapsed');
        }
      });
    });

    // Escape key to close
    document.addEventListener('keydown', (e) => {
      if (e.key === 'Escape' && !settingsElements.panel.classList.contains('hidden')) {
        closeSettings();
      }
    });

    // LLM quick controls event listeners
    initLlmControls();

    // SmartThings button listeners
    if (settingsElements.stConnectBtn) {
      settingsElements.stConnectBtn.addEventListener('click', startSmartThingsOAuth);
    }
    if (settingsElements.stRefreshBtn) {
      settingsElements.stRefreshBtn.addEventListener('click', refreshSmartThingsDevices);
    }
    if (settingsElements.stDisconnectBtn) {
      settingsElements.stDisconnectBtn.addEventListener('click', disconnectSmartThings);
    }
  }

  // =============================================================================
  // LLM Runtime Controls
  // =============================================================================

  // Current LLM runtime state
  let llmRuntimeState = {
    type: 'cloud',
    provider: 'openai',
    model: '',
    openai_available: false,
    claude_available: false
  };

  /**
   * Initialize LLM quick controls
   */
  function initLlmControls() {
    const typeSelect = document.getElementById('llm-type-select');
    const providerSelect = document.getElementById('llm-provider-select');

    if (typeSelect) {
      typeSelect.addEventListener('change', () => {
        setSessionLlm({ type: typeSelect.value });
      });
    }

    if (providerSelect) {
      providerSelect.addEventListener('change', () => {
        setSessionLlm({ provider: providerSelect.value });
      });
    }
  }

  /**
   * Update LLM controls from server state
   */
  function updateLlmControls(runtime) {
    llmRuntimeState = { ...llmRuntimeState, ...runtime };

    const typeSelect = document.getElementById('llm-type-select');
    const providerSelect = document.getElementById('llm-provider-select');
    const providerGroup = document.getElementById('provider-group');
    const modelDisplay = document.getElementById('llm-model-display');

    if (typeSelect) {
      typeSelect.value = runtime.type || 'cloud';
    }

    if (providerSelect) {
      // Update available options based on API key availability
      providerSelect.innerHTML = '';

      if (runtime.openai_available) {
        const opt = document.createElement('option');
        opt.value = 'openai';
        opt.textContent = 'OpenAI';
        providerSelect.appendChild(opt);
      }

      if (runtime.claude_available) {
        const opt = document.createElement('option');
        opt.value = 'claude';
        opt.textContent = 'Claude';
        providerSelect.appendChild(opt);
      }

      // If no providers available, show disabled message
      if (!runtime.openai_available && !runtime.claude_available) {
        const opt = document.createElement('option');
        opt.value = '';
        opt.textContent = 'No API keys configured';
        opt.disabled = true;
        providerSelect.appendChild(opt);
        providerSelect.disabled = true;
      } else {
        providerSelect.disabled = false;
        // Set current value
        const currentProvider = runtime.provider?.toLowerCase() || 'openai';
        if (providerSelect.querySelector(`option[value="${currentProvider}"]`)) {
          providerSelect.value = currentProvider;
        }
      }
    }

    // Enable/disable provider selector based on LLM type
    // (Always visible, but disabled when local is selected)
    if (providerSelect) {
      if (runtime.type === 'local') {
        providerSelect.disabled = true;
        providerSelect.title = 'Provider selection not available for local LLM';
      } else if (runtime.openai_available || runtime.claude_available) {
        providerSelect.disabled = false;
        providerSelect.title = 'Switch cloud provider';
      }
    }

    // Update model display
    if (modelDisplay) {
      modelDisplay.textContent = runtime.model ? `(${runtime.model})` : '';
    }

    console.log('LLM controls updated:', runtime);
  }

  /**
   * Send per-session LLM config change to server
   */
  function setSessionLlm(changes) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      console.error('WebSocket not connected');
      return;
    }

    console.log('Setting session LLM:', changes);
    ws.send(JSON.stringify({
      type: 'set_session_llm',
      payload: changes
    }));
  }

  /**
   * Handle per-session LLM config change response
   */
  function handleSetSessionLlmResponse(payload) {
    if (payload.success) {
      console.log('Session LLM updated:', payload);
      updateLlmControls(payload);
    } else {
      console.error('Failed to update session LLM:', payload.error);
      alert('Failed to switch LLM: ' + (payload.error || 'Unknown error'));

      // Revert controls to actual state
      if (llmRuntimeState) {
        updateLlmControls(llmRuntimeState);
      }
    }
  }

  // =============================================================================
  // SmartThings Integration
  // =============================================================================

  // SmartThings state
  let smartThingsState = {
    configured: false,
    authenticated: false,
    devices_count: 0,
    devices: [],
    auth_mode: 'none' // 'none', 'pat', 'oauth2'
  };

  /**
   * Request SmartThings status from server
   */
  function requestSmartThingsStatus() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }
    ws.send(JSON.stringify({ type: 'smartthings_status' }));
    console.log('Requested SmartThings status');
  }

  /**
   * Handle SmartThings status response
   */
  function handleSmartThingsStatusResponse(payload) {
    console.log('SmartThings status:', payload);
    smartThingsState.configured = payload.configured || false;
    smartThingsState.authenticated = payload.authenticated || false;
    smartThingsState.devices_count = payload.devices_count || 0;
    smartThingsState.auth_mode = payload.auth_mode || 'none';

    updateSmartThingsUI();

    // If authenticated, also get device list
    if (smartThingsState.authenticated) {
      requestSmartThingsDevices();
    }
  }

  /**
   * Helper to show/hide elements using CSS classes (CSP-compliant)
   */
  function stShow(el, displayClass) {
    if (!el) return;
    el.classList.remove('st-hidden', 'st-visible', 'st-visible-flex', 'st-visible-block');
    el.classList.add(displayClass || 'st-visible');
  }

  function stHide(el) {
    if (!el) return;
    el.classList.remove('st-visible', 'st-visible-flex', 'st-visible-block');
    el.classList.add('st-hidden');
  }

  /**
   * Update SmartThings UI based on current state
   */
  function updateSmartThingsUI() {
    const indicator = settingsElements.stStatusIndicator;
    const statusText = settingsElements.stStatusText;

    if (!indicator || !statusText) return;

    // Remove all status classes
    indicator.classList.remove('not-configured', 'configured', 'connected');

    if (!smartThingsState.configured) {
      // Not configured - show setup instructions
      indicator.classList.add('not-configured');
      statusText.textContent = 'Not Configured';

      stShow(settingsElements.stNotConfigured, 'st-visible-block');
      stHide(settingsElements.stConnectBtn);
      stHide(settingsElements.stRefreshBtn);
      stHide(settingsElements.stDisconnectBtn);
      stHide(settingsElements.stDevicesCountRow);
      stHide(settingsElements.stDevicesList);

    } else if (!smartThingsState.authenticated) {
      // Configured but not authenticated
      indicator.classList.add('configured');

      if (smartThingsState.auth_mode === 'oauth2') {
        // OAuth2 mode - show connect button
        statusText.textContent = 'Not Connected';
        stShow(settingsElements.stConnectBtn);
      } else {
        // PAT mode but not working - show error
        statusText.textContent = 'Token Invalid';
        stHide(settingsElements.stConnectBtn);
      }

      stHide(settingsElements.stNotConfigured);
      stHide(settingsElements.stRefreshBtn);
      stHide(settingsElements.stDisconnectBtn);
      stHide(settingsElements.stDevicesCountRow);
      stHide(settingsElements.stDevicesList);

    } else {
      // Authenticated - show connected state and devices
      indicator.classList.add('connected');

      if (smartThingsState.auth_mode === 'pat') {
        statusText.textContent = 'Connected (PAT)';
        // PAT is configured in secrets.toml, can't disconnect via UI
        stHide(settingsElements.stDisconnectBtn);
      } else {
        statusText.textContent = 'Connected (OAuth2)';
        stShow(settingsElements.stDisconnectBtn);
      }

      stHide(settingsElements.stNotConfigured);
      stHide(settingsElements.stConnectBtn);
      stShow(settingsElements.stRefreshBtn);
      stShow(settingsElements.stDevicesCountRow, 'st-visible-flex');
      settingsElements.stDevicesCount.textContent = smartThingsState.devices_count + ' device' + (smartThingsState.devices_count !== 1 ? 's' : '');

      if (smartThingsState.devices.length > 0) {
        stShow(settingsElements.stDevicesList, 'st-visible-block');
      }
    }
  }

  /**
   * Start SmartThings OAuth flow
   */
  function startSmartThingsOAuth() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      alert('Not connected to server');
      return;
    }

    // Get auth URL from server
    const redirectUri = window.location.origin + '/smartthings/callback';
    ws.send(JSON.stringify({
      type: 'smartthings_get_auth_url',
      payload: { redirect_uri: redirectUri }
    }));
    console.log('Requesting SmartThings auth URL');
  }

  /**
   * Handle SmartThings auth URL response
   */
  function handleSmartThingsAuthUrlResponse(payload) {
    if (payload.error) {
      alert('Failed to get auth URL: ' + payload.error);
      return;
    }

    if (payload.auth_url) {
      console.log('Opening SmartThings authorization URL');
      // Open in new window for OAuth flow
      const authWindow = window.open(payload.auth_url, 'smartthings_auth', 'width=600,height=700');

      // Listen for OAuth callback
      window.addEventListener('message', function handleOAuthCallback(event) {
        if (event.origin !== window.location.origin) return;
        if (event.data && event.data.type === 'smartthings_oauth_callback') {
          window.removeEventListener('message', handleOAuthCallback);
          if (authWindow) authWindow.close();

          if (event.data.code) {
            // Exchange code for tokens (include state for CSRF protection)
            exchangeSmartThingsCode(event.data.code, event.data.state);
          } else if (event.data.error) {
            alert('OAuth failed: ' + event.data.error);
          }
        }
      });
    }
  }

  /**
   * Exchange OAuth code for tokens
   * @param {string} code - Authorization code from OAuth callback
   * @param {string} state - State parameter for CSRF protection
   */
  function exchangeSmartThingsCode(code, state) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }

    const redirectUri = window.location.origin + '/smartthings/callback';
    const payload = { code: code, redirect_uri: redirectUri };
    if (state) {
      payload.state = state;
    }
    ws.send(JSON.stringify({
      type: 'smartthings_exchange_code',
      payload: payload
    }));
    console.log('Exchanging SmartThings auth code with CSRF state');
  }

  /**
   * Handle SmartThings code exchange response
   */
  function handleSmartThingsExchangeCodeResponse(payload) {
    if (payload.success) {
      console.log('SmartThings connected successfully');
      // Refresh status to show connected state
      requestSmartThingsStatus();
    } else {
      alert('Failed to connect SmartThings: ' + (payload.error || 'Unknown error'));
    }
  }

  /**
   * Request SmartThings devices list
   */
  function requestSmartThingsDevices() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }
    ws.send(JSON.stringify({ type: 'smartthings_list_devices' }));
    console.log('Requesting SmartThings devices');
  }

  /**
   * Refresh SmartThings devices (force refresh from API)
   */
  function refreshSmartThingsDevices() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }
    settingsElements.stRefreshBtn.disabled = true;
    settingsElements.stRefreshBtn.textContent = 'Refreshing...';

    ws.send(JSON.stringify({ type: 'smartthings_refresh_devices' }));
    console.log('Refreshing SmartThings devices');
  }

  /**
   * Handle SmartThings devices response
   */
  function handleSmartThingsDevicesResponse(payload) {
    // Re-enable refresh button
    if (settingsElements.stRefreshBtn) {
      settingsElements.stRefreshBtn.disabled = false;
      settingsElements.stRefreshBtn.textContent = 'Refresh Devices';
    }

    if (payload.error) {
      console.error('SmartThings devices error:', payload.error);
      return;
    }

    if (payload.devices) {
      smartThingsState.devices = payload.devices;
      smartThingsState.devices_count = payload.devices.length;
      renderSmartThingsDevices(payload.devices);
      updateSmartThingsUI();
    }
  }

  /**
   * Render SmartThings devices list
   */
  function renderSmartThingsDevices(devices) {
    const container = settingsElements.stDevicesContainer;
    if (!container) return;

    container.innerHTML = '';

    if (devices.length === 0) {
      container.innerHTML = '<div class="st-no-devices">No devices found</div>';
      return;
    }

    devices.forEach(device => {
      const deviceEl = document.createElement('div');
      deviceEl.className = 'st-device-item';

      // Get capability icons
      const caps = getCapabilityIcons(device.capabilities);

      deviceEl.innerHTML = `
        <div class="st-device-name">${escapeHtml(device.label || device.name)}</div>
        <div class="st-device-info">
          <span class="st-device-room">${escapeHtml(device.room || 'No room')}</span>
          <span class="st-device-caps">${caps}</span>
        </div>
      `;
      container.appendChild(deviceEl);
    });

    settingsElements.stDevicesList.style.display = 'block';
  }

  /**
   * Get capability icons for a device
   */
  function getCapabilityIcons(capabilities) {
    const icons = [];
    const capBits = capabilities || 0;

    if (capBits & 0x0001) icons.push('&#x1F4A1;'); // Switch (lightbulb)
    if (capBits & 0x0002) icons.push('&#x1F506;'); // Dimmer (brightness)
    if (capBits & 0x0004) icons.push('&#x1F308;'); // Color
    if (capBits & 0x0010) icons.push('&#x1F321;'); // Thermostat
    if (capBits & 0x0020) icons.push('&#x1F510;'); // Lock
    if (capBits & 0x0040) icons.push('&#x1F3C3;'); // Motion
    if (capBits & 0x0080) icons.push('&#x1F6AA;'); // Contact (door)
    if (capBits & 0x0100) icons.push('&#x1F321;'); // Temperature sensor
    if (capBits & 0x0200) icons.push('&#x1F4A7;'); // Humidity
    if (capBits & 0x2000) icons.push('&#x1FA9F;'); // Window shade

    return icons.join(' ') || '&#x2699;'; // Default: gear
  }

  /**
   * Disconnect SmartThings
   */
  function disconnectSmartThings() {
    if (!confirm('Disconnect SmartThings? This will remove your saved tokens.')) {
      return;
    }

    if (!ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }

    ws.send(JSON.stringify({ type: 'smartthings_disconnect' }));
    console.log('Disconnecting SmartThings');
  }

  /**
   * Handle SmartThings disconnect response
   */
  function handleSmartThingsDisconnectResponse(payload) {
    if (payload.success) {
      console.log('SmartThings disconnected');
      smartThingsState.authenticated = false;
      smartThingsState.devices = [];
      smartThingsState.devices_count = 0;
      updateSmartThingsUI();
    } else {
      alert('Failed to disconnect: ' + (payload.error || 'Unknown error'));
    }
  }

  /* =============================================================================
   * LLM Tools Configuration
   * ============================================================================= */

  let toolsConfig = [];

  /**
   * Request tools configuration from server
   */
  function requestToolsConfig() {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'get_tools_config' }));
      console.log('Requesting tools config');
    }
  }

  /**
   * Handle get_tools_config response
   */
  function handleGetToolsConfigResponse(payload) {
    console.log('Tools config received:', payload);

    if (!payload.tools) {
      console.warn('No tools in response');
      return;
    }

    // Sort tools: non-armor first, then armor tools at bottom, alphabetically within each group
    toolsConfig = payload.tools.sort((a, b) => {
      const aArmor = a.armor_feature ? 1 : 0;
      const bArmor = b.armor_feature ? 1 : 0;
      if (aArmor !== bArmor) return aArmor - bArmor;
      return a.name.localeCompare(b.name);
    });
    renderToolsList();
    updateToolsTokenEstimates(payload.token_estimate);
  }

  /**
   * Handle set_tools_config response
   */
  function handleSetToolsConfigResponse(payload) {
    console.log('Set tools config response:', payload);

    const saveBtn = document.getElementById('save-tools-btn');
    if (payload.success) {
      saveBtn.textContent = 'Saved!';
      saveBtn.classList.add('saved');
      setTimeout(() => {
        saveBtn.textContent = 'Save Tool Settings';
        saveBtn.classList.remove('saved');
      }, 2000);

      // Update token estimates with new values
      if (payload.token_estimate) {
        updateToolsTokenEstimates(payload.token_estimate);
      }
    } else {
      saveBtn.textContent = 'Error!';
      setTimeout(() => {
        saveBtn.textContent = 'Save Tool Settings';
      }, 2000);
      console.error('Failed to save tools config:', payload.error);
    }
  }

  /**
   * Render the tools list with checkboxes
   */
  function renderToolsList() {
    const container = document.getElementById('tools-list');
    if (!container) return;

    if (toolsConfig.length === 0) {
      container.innerHTML = '<div class="tools-loading">No tools available</div>';
      return;
    }

    container.innerHTML = toolsConfig.map(tool => {
      const disabledClass = !tool.available ? 'disabled' : '';
      const disabledAttr = !tool.available ? 'disabled' : '';
      // Arc reactor SVG for armor features - circle with inverted triangle
      // Equilateral triangle inscribed in circle (center 12,12, radius 10)
      const armorIcon = tool.armor_feature ? `<span class="armor-icon" title="OASIS armor feature">
        <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.5">
          <circle cx="12" cy="12" r="10"/>
          <polygon points="12,22 3.34,7 20.66,7" stroke-linejoin="round"/>
        </svg>
      </span>` : '';

      return `
        <div class="tool-item ${disabledClass}" data-tool="${tool.name}" title="${tool.description}">
          <div class="tool-info">
            <div class="tool-name">${tool.name}${armorIcon}</div>
          </div>
          <div class="tool-checkbox">
            <input type="checkbox" id="tool-local-${tool.name}"
                   ${tool.local ? 'checked' : ''} ${disabledAttr}
                   data-tool="${tool.name}" data-type="local">
          </div>
          <div class="tool-checkbox">
            <input type="checkbox" id="tool-remote-${tool.name}"
                   ${tool.remote ? 'checked' : ''} ${disabledAttr}
                   data-tool="${tool.name}" data-type="remote">
          </div>
        </div>
      `;
    }).join('');
  }

  /**
   * Update the token estimate display
   */
  function updateToolsTokenEstimates(estimates) {
    if (!estimates) return;

    const localEl = document.getElementById('tools-tokens-local');
    const remoteEl = document.getElementById('tools-tokens-remote');

    if (localEl) {
      localEl.textContent = `Local: ${estimates.local || 0} tokens`;
    }
    if (remoteEl) {
      remoteEl.textContent = `Remote: ${estimates.remote || 0} tokens`;
    }
  }

  /**
   * Save tools configuration
   */
  function saveToolsConfig() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      console.error('WebSocket not connected');
      return;
    }

    const tools = [];
    const container = document.getElementById('tools-list');
    const items = container.querySelectorAll('.tool-item');

    items.forEach(item => {
      const name = item.dataset.tool;
      const localCb = item.querySelector(`input[data-type="local"]`);
      const remoteCb = item.querySelector(`input[data-type="remote"]`);

      tools.push({
        name: name,
        local: localCb ? localCb.checked : false,
        remote: remoteCb ? remoteCb.checked : false
      });
    });

    ws.send(JSON.stringify({
      type: 'set_tools_config',
      payload: { tools: tools }
    }));

    console.log('Saving tools config:', tools);
  }

  /**
   * Initialize tools section
   */
  function initToolsSection() {
    const saveBtn = document.getElementById('save-tools-btn');
    if (saveBtn) {
      saveBtn.addEventListener('click', saveToolsConfig);
    }

    // Request tools config when settings panel opens
    const settingsBtn = document.getElementById('settings-btn');
    if (settingsBtn) {
      settingsBtn.addEventListener('click', () => {
        // Small delay to let other things initialize
        setTimeout(requestToolsConfig, 100);
      });
    }

    // Initialize metrics panel
    initMetricsPanel();
  }

  // =============================================================================
  // Metrics Panel
  // =============================================================================

  let metricsInterval = null;
  let metricsVisible = false;

  function initMetricsPanel() {
    const metricsBtn = document.getElementById('metrics-btn');
    const metricsClose = document.getElementById('metrics-close');
    const metricsPanel = document.getElementById('metrics-panel');

    if (metricsBtn) {
      metricsBtn.addEventListener('click', toggleMetricsPanel);
    }
    if (metricsClose) {
      metricsClose.addEventListener('click', hideMetricsPanel);
    }
  }

  function toggleMetricsPanel() {
    const panel = document.getElementById('metrics-panel');
    const btn = document.getElementById('metrics-btn');
    if (panel.classList.contains('hidden')) {
      showMetricsPanel();
    } else {
      hideMetricsPanel();
    }
  }

  function showMetricsPanel() {
    const panel = document.getElementById('metrics-panel');
    const btn = document.getElementById('metrics-btn');
    panel.classList.remove('hidden');
    btn.classList.add('active');
    metricsVisible = true;
    requestMetrics();
    // Refresh every 2 seconds while visible
    metricsInterval = setInterval(requestMetrics, 2000);
  }

  function hideMetricsPanel() {
    const panel = document.getElementById('metrics-panel');
    const btn = document.getElementById('metrics-btn');
    panel.classList.add('hidden');
    btn.classList.remove('active');
    metricsVisible = false;
    if (metricsInterval) {
      clearInterval(metricsInterval);
      metricsInterval = null;
    }
  }

  function requestMetrics() {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'get_metrics' }));
    }
  }

  function handleMetricsResponse(payload) {
    if (!metricsVisible) return;

    // Session stats
    const uptime = payload.session?.uptime_seconds || 0;
    const hours = Math.floor(uptime / 3600);
    const mins = Math.floor((uptime % 3600) / 60);
    const uptimeStr = hours > 0 ? `${hours}h ${mins}m` : `${mins}m`;
    setText('m-uptime', uptimeStr);

    const queries = payload.session?.queries_total || 0;
    const cloud = payload.session?.queries_cloud || 0;
    const local = payload.session?.queries_local || 0;
    setText('m-queries', `${queries} (${cloud}c/${local}l)`);
    setText('m-errors', payload.session?.errors || 0);
    setText('m-bargeins', payload.session?.bargeins || 0);

    // Tokens
    const tc = payload.tokens || {};
    setText('m-tokens-cloud', `${formatNum(tc.cloud_input)}/${formatNum(tc.cloud_output)}`);
    setText('m-tokens-local', `${formatNum(tc.local_input)}/${formatNum(tc.local_output)}`);
    setText('m-tokens-cached', formatNum(tc.cached));

    // Last pipeline
    const last = payload.last || {};
    setText('m-last-asr', formatMs(last.asr_ms));
    setText('m-last-llm', formatMs(last.llm_total_ms));
    setText('m-last-tts', formatMs(last.tts_ms));

    // Averages
    const avg = payload.averages || {};
    setText('m-avg-asr', formatMs(avg.asr_ms));
    setText('m-avg-llm', formatMs(avg.llm_total_ms));
    setText('m-avg-tts', formatMs(avg.tts_ms));

    // System
    const vad = payload.state?.vad_probability || 0;
    setText('m-vad', `${(vad * 100).toFixed(0)}%`);

    const aec = payload.aec || {};
    let aecStr = aec.enabled ? (aec.calibrated ? `${aec.delay_ms}ms` : 'uncal') : 'off';
    setText('m-aec', aecStr);
  }

  function setText(id, value) {
    const el = document.getElementById(id);
    if (el) el.textContent = value;
  }

  function formatMs(ms) {
    if (ms === undefined || ms === null || ms === 0) return '--';
    if (ms < 1000) return `${Math.round(ms)}ms`;
    return `${(ms / 1000).toFixed(1)}s`;
  }

  function formatNum(n) {
    if (n === undefined || n === null) return '--';
    if (n >= 1000000) return `${(n / 1000000).toFixed(1)}M`;
    if (n >= 1000) return `${(n / 1000).toFixed(1)}K`;
    return String(n);
  }

  // Start when DOM is ready
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }

})();
