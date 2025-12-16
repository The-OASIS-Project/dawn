/**
 * DAWN WebUI JavaScript
 * Phase 1: Basic WebSocket connection and text input
 */

(function() {
  'use strict';

  // =============================================================================
  // Configuration
  // =============================================================================
  const WS_SUBPROTOCOL = 'dawn-1.0';
  const RECONNECT_BASE_DELAY = 1000;
  const RECONNECT_MAX_DELAY = 30000;

  // =============================================================================
  // State
  // =============================================================================
  let ws = null;
  let reconnectAttempts = 0;
  let currentState = 'idle';
  let debugMode = false;

  // =============================================================================
  // DOM Elements
  // =============================================================================
  const elements = {
    connectionStatus: document.getElementById('connection-status'),
    ringContainer: document.getElementById('ring-container'),
    statusDot: document.getElementById('status-dot'),
    statusText: document.getElementById('status-text'),
    transcript: document.getElementById('transcript'),
    textInput: document.getElementById('text-input'),
    sendBtn: document.getElementById('send-btn'),
    micBtn: document.getElementById('mic-btn'),
    debugCheckbox: document.getElementById('debug-mode'),
  };

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
        default:
          console.log('Unknown message type:', msg.type);
      }
    } catch (e) {
      console.error('Failed to parse message:', e, data);
    }
  }

  function handleBinaryMessage(data) {
    // Phase 4: Audio data handling
    console.log('Received binary message:', data.byteLength, 'bytes');
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
    currentState = state;

    // Update status indicator
    elements.statusDot.className = state;
    elements.statusText.textContent = state.toUpperCase();

    // Update ring container
    elements.ringContainer.className = state;
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
  function init() {
    // Event listeners
    elements.sendBtn.addEventListener('click', handleSend);
    elements.textInput.addEventListener('keydown', handleKeydown);

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

    // Connect to WebSocket
    connect();

    // Reconnect on visibility change
    document.addEventListener('visibilitychange', function() {
      if (!document.hidden && (!ws || ws.readyState !== WebSocket.OPEN)) {
        connect();
      }
    });

    console.log('DAWN WebUI initialized');
  }

  // Start when DOM is ready
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }

})();
