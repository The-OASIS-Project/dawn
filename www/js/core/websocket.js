/**
 * DAWN WebSocket Core Module
 * Connection lifecycle management with reconnection logic
 *
 * Usage:
 *   DawnWS.connect()                    // Connect to server
 *   DawnWS.disconnect()                 // Close connection
 *   DawnWS.send({ type: 'foo' })        // Send JSON message
 *   DawnWS.sendBinary(data)             // Send binary data
 *   DawnWS.isConnected()                // Check connection state
 *   DawnWS.forceReconnect()             // Manual reconnect
 *   DawnWS.setCallbacks({ onStatus, onTextMessage, onBinaryMessage })
 */
(function (global) {
   'use strict';

   // Connection state
   let ws = null;
   let reconnectAttempts = 0;
   let reconnectTimeoutId = null;
   let maxClientsReached = false;
   let capabilitiesSynced = false;

   // Callbacks (set by dawn.js)
   let callbacks = {
      onStatus: null, // (status, detail) => void
      onTextMessage: null, // (data) => void
      onBinaryMessage: null, // (data) => void
      getOpusReady: null, // () => boolean
      getTtsEnabled: null, // () => boolean
   };

   /**
    * Get WebSocket URL based on current page protocol
    */
   function getWebSocketUrl() {
      const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
      return `${protocol}//${window.location.host}/ws`;
   }

   /**
    * Connect to WebSocket server
    */
   function connect() {
      if (ws && ws.readyState === WebSocket.OPEN) {
         return;
      }

      if (callbacks.onStatus) {
         callbacks.onStatus('connecting');
      }

      try {
         ws = new WebSocket(getWebSocketUrl(), DawnConfig.WS_SUBPROTOCOL);
      } catch (e) {
         console.error('Failed to create WebSocket:', e);
         scheduleReconnect();
         return;
      }

      ws.binaryType = 'arraybuffer';

      ws.onopen = function () {
         console.log('WebSocket connected');
         reconnectAttempts = 0;
         maxClientsReached = false;

         if (callbacks.onStatus) {
            callbacks.onStatus('connected');
         }

         // Build capabilities for audio codec negotiation
         const opusReady = callbacks.getOpusReady ? callbacks.getOpusReady() : false;
         const capabilities = {
            audio_codecs: opusReady ? ['opus', 'pcm'] : ['pcm'],
         };

         // Get TTS preference from callback or localStorage
         const ttsEnabled = callbacks.getTtsEnabled
            ? callbacks.getTtsEnabled()
            : localStorage.getItem('ttsEnabled') === 'true';

         // Try to reconnect with existing session token, or request new session
         const savedToken = localStorage.getItem('dawn_session_token');
         if (savedToken) {
            console.log('Attempting session reconnect with saved token');
            ws.send(
               JSON.stringify({
                  type: 'reconnect',
                  payload: {
                     token: savedToken,
                     capabilities: capabilities,
                     tts_enabled: ttsEnabled,
                  },
               })
            );
         } else {
            console.log('No saved token, requesting new session');
            ws.send(
               JSON.stringify({
                  type: 'init',
                  payload: { capabilities: capabilities, tts_enabled: ttsEnabled },
               })
            );
         }
         console.log('Audio codecs:', capabilities.audio_codecs, 'TTS:', ttsEnabled ? 'on' : 'off');
      };

      ws.onclose = function (event) {
         console.log('WebSocket closed:', event.code, event.reason);
         capabilitiesSynced = false;

         if (maxClientsReached) {
            console.log('Server at capacity, not auto-reconnecting');
            return;
         }

         if (callbacks.onStatus) {
            callbacks.onStatus('disconnected', event.reason || null);
         }
         scheduleReconnect();
      };

      ws.onerror = function (error) {
         console.error('WebSocket error:', error);
      };

      ws.onmessage = function (event) {
         if (event.data instanceof ArrayBuffer) {
            if (callbacks.onBinaryMessage) {
               callbacks.onBinaryMessage(event.data);
            }
         } else {
            if (callbacks.onTextMessage) {
               callbacks.onTextMessage(event.data);
            }
         }
      };
   }

   /**
    * Schedule reconnection with exponential backoff
    */
   function scheduleReconnect() {
      if (reconnectAttempts >= DawnConfig.RECONNECT_MAX_ATTEMPTS) {
         console.log('Max reconnection attempts reached, stopping');
         if (callbacks.onStatus) {
            callbacks.onStatus('disconnected', 'Connection failed - click to retry');
         }
         return;
      }

      const delay = Math.min(
         DawnConfig.RECONNECT_BASE_DELAY * Math.pow(2, reconnectAttempts),
         DawnConfig.RECONNECT_MAX_DELAY
      );
      const jitter = Math.random() * 500;

      console.log(
         `Reconnecting in ${Math.round(delay + jitter)}ms... (attempt ${reconnectAttempts + 1}/${DawnConfig.RECONNECT_MAX_ATTEMPTS})`
      );
      reconnectAttempts++;

      reconnectTimeoutId = setTimeout(connect, delay + jitter);
   }

   /**
    * Disconnect from WebSocket server
    */
   function disconnect() {
      if (reconnectTimeoutId) {
         clearTimeout(reconnectTimeoutId);
         reconnectTimeoutId = null;
      }
      if (ws) {
         ws.close();
         ws = null;
      }
   }

   /**
    * Force reconnection (user-initiated retry)
    */
   function forceReconnect() {
      reconnectAttempts = 0;
      maxClientsReached = false;
      disconnect();
      connect();
   }

   /**
    * Send a JSON message
    */
   function send(msg) {
      if (ws && ws.readyState === WebSocket.OPEN) {
         ws.send(JSON.stringify(msg));
      } else {
         console.warn('WebSocket not connected, cannot send:', msg.type || msg);
      }
   }

   /**
    * Send binary data
    */
   function sendBinary(data) {
      if (ws && ws.readyState === WebSocket.OPEN) {
         ws.send(data);
      } else {
         console.warn('WebSocket not connected, cannot send binary');
      }
   }

   /**
    * Check if connected
    */
   function isConnected() {
      return ws && ws.readyState === WebSocket.OPEN;
   }

   /**
    * Set max clients reached flag (called from message handler)
    */
   function setMaxClientsReached(reached) {
      maxClientsReached = reached;
   }

   /**
    * Set capabilities synced flag
    */
   function setCapabilitiesSynced(synced) {
      capabilitiesSynced = synced;
   }

   /**
    * Get capabilities synced state
    */
   function getCapabilitiesSynced() {
      return capabilitiesSynced;
   }

   /**
    * Set callbacks for message handling
    */
   function setCallbacks(cbs) {
      if (cbs.onStatus) callbacks.onStatus = cbs.onStatus;
      if (cbs.onTextMessage) callbacks.onTextMessage = cbs.onTextMessage;
      if (cbs.onBinaryMessage) callbacks.onBinaryMessage = cbs.onBinaryMessage;
      if (cbs.getOpusReady) callbacks.getOpusReady = cbs.getOpusReady;
      if (cbs.getTtsEnabled) callbacks.getTtsEnabled = cbs.getTtsEnabled;
   }

   /**
    * Get the current session token
    * Used by dedicated music server for authentication
    * @returns {string|null} Session token or null if not available
    */
   function getSessionToken() {
      return localStorage.getItem('dawn_session_token');
   }

   // Expose globally
   global.DawnWS = {
      connect: connect,
      disconnect: disconnect,
      forceReconnect: forceReconnect,
      send: send,
      sendBinary: sendBinary,
      isConnected: isConnected,
      setCallbacks: setCallbacks,
      setMaxClientsReached: setMaxClientsReached,
      setCapabilitiesSynced: setCapabilitiesSynced,
      getCapabilitiesSynced: getCapabilitiesSynced,
      getSessionToken: getSessionToken,
   };
})(window);
