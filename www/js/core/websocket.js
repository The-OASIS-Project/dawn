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
   // Set when the server closes us with WS code 4001 ("superseded") — another
   // connection (tab/device) reconnected and took over this session. While set, we
   // do NOT auto-reconnect (neither the onclose backoff nor the visibilitychange
   // path), so the two tabs don't ping-pong stealing the session. Cleared only by a
   // deliberate user gesture (forceReconnect) or a fresh successful connect.
   let superseded = false;
   // Set when forceReconnect() is a RECLAIM (fired while superseded) — another tab
   // held the session and may have advanced the conversation behind this tab's back.
   // The next `session` frame consumes it to force a full transcript re-render
   // (load_conversation) instead of the lightweight re-anchor a normal reconnect uses.
   let reclaiming = false;

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

      // Capture THIS socket so its late/stale onclose can't mutate the live
      // connection's state. On a fast reconnect (disconnect→connect) the server
      // can evict the old socket with 4001 AFTER a newer socket already opened;
      // without this guard that stale close would latch `superseded` on the
      // healthy connection. onclose below early-returns when `thisWs !== ws`.
      const thisWs = ws;

      ws.binaryType = 'arraybuffer';

      ws.onopen = function () {
         console.log('WebSocket connected');
         reconnectAttempts = 0;
         maxClientsReached = false;
         superseded = false;

         if (callbacks.onStatus) {
            callbacks.onStatus('connected');
         }

         // Build capabilities for audio codec negotiation
         const opusReady = callbacks.getOpusReady ? callbacks.getOpusReady() : false;
         const capabilities = {
            audio_codecs: opusReady ? ['opus', 'pcm'] : ['pcm'],
         };

         // Get TTS preference from callback or storage
         const ttsEnabled = callbacks.getTtsEnabled
            ? callbacks.getTtsEnabled()
            : DawnStore.getBool(DawnStore.KEYS.TTS_ENABLED, false);

         // Try to reconnect with existing session token, or request new session
         const savedToken = DawnStore.get(DawnStore.KEYS.SESSION_TOKEN, null);
         if (savedToken) {
            console.log('Attempting session reconnect with saved token');
            ws.send(
               JSON.stringify({
                  type: 'reconnect',
                  payload: {
                     token: savedToken,
                     capabilities: capabilities,
                     tts_enabled: ttsEnabled,
                     // Living tool pills: we render our OWN turn's tool steps from the
                     // tool_step frame, so ask the server to include us in its fan.
                     tool_step_origin: true,
                  },
               })
            );
         } else {
            console.log('No saved token, requesting new session');
            ws.send(
               JSON.stringify({
                  type: 'init',
                  payload: {
                     capabilities: capabilities,
                     tts_enabled: ttsEnabled,
                     tool_step_origin: true,
                  },
               })
            );
         }
         console.log('Audio codecs:', capabilities.audio_codecs, 'TTS:', ttsEnabled ? 'on' : 'off');
      };

      ws.onclose = function (event) {
         // Ignore a close on a socket that a newer connect() already replaced —
         // otherwise a stale/evicted socket's late 4001 would latch `superseded`
         // (or schedule a reconnect) on the live connection. Guard on `ws &&` so we
         // only skip when a DIFFERENT socket is currently live (the fast-reconnect
         // race); when `ws` is null (disconnect() pre-nulls it during teardown) or
         // `ws === thisWs` (a normal drop), run the handler normally.
         if (ws && thisWs !== ws) {
            console.log('Ignoring close on stale/superseded socket (code', event.code, ')');
            return;
         }
         console.log('WebSocket closed:', event.code, event.reason);
         capabilitiesSynced = false;

         // Superseded: another connection reconnected and took over this session.
         // Do NOT auto-reconnect — re-stealing it would ping-pong the two tabs.
         // Two signals, because a reverse proxy STRIPS the 4001 close code (a
         // proxied browser sees a generic 1006): the same-origin path reads
         // event.code === 4001; the proxy path relies on the `session_superseded`
         // DATA frame that arrives just before this close and already set
         // `superseded` (via markSuperseded). Either one means back off.
         if (event.code === 4001 || superseded) {
            console.log('Session superseded by another connection — not auto-reconnecting');
            superseded = true;
            if (callbacks.onStatus) {
               callbacks.onStatus('superseded', event.reason || 'Another tab or device is active');
            }
            return;
         }

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
      // A gesture fired while superseded IS a reclaim: another tab held the session,
      // so the next session frame must fully reload the transcript (it may be stale).
      if (superseded) {
         reclaiming = true;
      }
      superseded = false; // deliberate user gesture — reclaim the session (evicts the other tab)
      disconnect();
      connect();
   }

   /**
    * Consume the one-shot "this reconnect is a reclaim after takeover" flag.
    * dawn.js calls it in the `session`-frame handler to decide full transcript
    * reload (reclaim, or a fresh session) vs the lightweight re-anchor (normal
    * reconnect where the display is already current). Returns then clears.
    */
   function consumeReclaiming() {
      const r = reclaiming;
      reclaiming = false;
      return r;
   }

   /**
    * Whether the server superseded this connection (another tab/device took over).
    * dawn.js uses it to suppress the visibilitychange auto-reconnect so a focus
    * change doesn't silently re-steal the session.
    */
   function isSuperseded() {
      return superseded;
   }

   /**
    * Mark this connection superseded from a `session_superseded` DATA frame
    * (the proxy-robust takeover signal — see onclose). Sets the flag so the
    * close that follows the frame backs off instead of auto-reconnecting, even
    * when a proxy stripped the 4001 code down to 1006. Cleared by a deliberate
    * forceReconnect or a fresh successful open.
    */
   function markSuperseded() {
      superseded = true;
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
      return DawnStore.get(DawnStore.KEYS.SESSION_TOKEN, null);
   }

   // Expose globally
   global.DawnWS = {
      connect: connect,
      disconnect: disconnect,
      forceReconnect: forceReconnect,
      isSuperseded: isSuperseded,
      markSuperseded: markSuperseded,
      consumeReclaiming: consumeReclaiming,
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
