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

   // Liveness heartbeat state (see DawnConfig.PING_*). Detects a half-open socket
   // (readyState OPEN but the server has stopped answering). Started by dawn.js on
   // an authed `session` frame (never before auth — a ping then draws
   // UNAUTHORIZED); stopped in onclose/disconnect.
   let heartbeatTimer = null;
   let pongTimer = null;
   let pingSeq = 0; // increments per ping
   let pendingPingSeq = 0; // seq of an outstanding ping (0 = none in flight)
   let missedPongs = 0; // consecutive unanswered pings, once pong is known-supported
   let pongSupported = false; // server answered a ping at least once (feature gate)
   let unsupportedProbes = 0; // unanswered pings before any pong (older server ignoring ping)
   let lastInboundAt = 0; // performance.now() of the last inbound frame

   // Cross-tab session-claim coordination (BroadcastChannel). The live owner posts
   // a claim (token + time) on each authed session frame and each pong; a sibling
   // tab about to do a NON-GESTURE reconnect checks for a recent same-token claim
   // and defers instead of stealing the session back (the half-open steal-back
   // hazard). A deliberate gesture (forceReconnect) bypasses this.
   let sessionChannel = null;
   let lastSiblingClaimAt = 0;
   let lastSiblingClaimToken = null;

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
      // Bail if a socket is already OPEN or mid-handshake (CONNECTING) — a
      // pageshow/online event during initial load could otherwise spawn a
      // duplicate socket and orphan the first (whose stale onopen would reset
      // shared flags).
      if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
         return;
      }

      initSessionChannel();

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
                     // "Always on" hint — the authoritative keepalive state is the
                     // server-persisted flag set via session_keepalive_enable; this
                     // just re-advertises intent on each (re)connect.
                     session_keepalive: isAlwaysOn(),
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
                     session_keepalive: isAlwaysOn(),
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
         stopHeartbeat();

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
         lastInboundAt = performance.now(); // any inbound frame proves the link is alive
         // Inbound traffic IS liveness: clear a false-suspect state (a single lost
         // pong during a streaming turn) so the watchdog can't stick in
         // 'stale'/isLive()=false on a healthy link, and cancel a now-moot in-flight
         // ping. Only cancel the ping once the server is known to answer pings, so
         // an as-yet-unconfirmed arming probe still runs to establish pongSupported.
         if (missedPongs > 0) {
            missedPongs = 0;
            if (callbacks.onStatus) callbacks.onStatus('connected');
         }
         if (pendingPingSeq !== 0 && pongSupported) {
            pendingPingSeq = 0;
            if (pongTimer) {
               clearTimeout(pongTimer);
               pongTimer = null;
            }
         }
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
      // Don't steal the session back from a live sibling tab (half-open steal-back).
      if (hasRecentSiblingClaim()) {
         console.log('Sibling tab owns the session — not reconnecting');
         deferToSibling();
         return;
      }
      // Default policy: a hidden tab defers its reconnect to foreground — it has no
      // business re-establishing/stealing a session while not visible. An "always
      // on" browser overrides this and reconnects in the background.
      if (typeof document !== 'undefined' && document.hidden && !isAlwaysOn()) {
         // The visibilitychange→probeNow listener reconnects on foreground.
         console.log('Tab hidden — deferring reconnect until foreground');
         return;
      }

      // Indefinite retry with FULL jitter: rand(0, min(MAX, BASE*2^n)). An uncapped
      // exponential at a fixed max resyncs many tabs/devices into a herd; full
      // jitter spreads the reconnects. (No attempt cap — a browser left open should
      // keep trying to reconnect for as long as the user wants to be connected.)
      const ceiling = Math.min(
         DawnConfig.RECONNECT_BASE_DELAY * Math.pow(2, reconnectAttempts),
         DawnConfig.RECONNECT_MAX_DELAY
      );
      const wait = Math.random() * ceiling;
      console.log(`Reconnecting in ${Math.round(wait)}ms... (attempt ${reconnectAttempts + 1})`);
      reconnectAttempts++;
      reconnectTimeoutId = setTimeout(connect, wait);
   }

   /**
    * Disconnect from WebSocket server
    */
   function disconnect() {
      stopHeartbeat();
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

   /* =========================================================================
    * Liveness heartbeat — detect a half-open socket (OPEN but the server has
    * stopped answering) and force a reconnect. Ported from Aurora's watchdog.
    * The server answers an authed `ping` with a seq-echoing `pong`; dawn.js
    * routes that pong to handlePong(). Started by dawn.js on an authed `session`
    * frame; stopped in onclose/disconnect.
    * ========================================================================= */

   function startHeartbeat() {
      stopHeartbeat();
      lastInboundAt = performance.now();
      missedPongs = 0;
      pendingPingSeq = 0;
      heartbeatTimer = setInterval(heartbeatTick, DawnConfig.PING_INTERVAL_MS);
      // Probe once now, while the link is healthy, so `pongSupported` is
      // established even on a connection that always carries traffic closer than
      // PING_IDLE_MS apart — which would otherwise never fire the idle-gated ping,
      // leaving the watchdog unarmed and a later half-open misread as an
      // older-server "unsupported" rather than a dead link.
      sendPing();
   }

   function stopHeartbeat() {
      if (heartbeatTimer) {
         clearInterval(heartbeatTimer);
         heartbeatTimer = null;
      }
      if (pongTimer) {
         clearTimeout(pongTimer);
         pongTimer = null;
      }
      pendingPingSeq = 0;
   }

   function heartbeatTick() {
      // Refresh our session-ownership claim every tick, decoupled from ping/pong,
      // so a sibling always sees a live owner within the claim window — on both
      // idle AND busy links. (handlePong can't be the claim source: a busy link
      // idle-gates its pings, and on an idle link the onmessage pong-clear makes
      // handlePong early-return before it could post.)
      if (ws && ws.readyState === WebSocket.OPEN) postSessionClaim();
      if (pendingPingSeq !== 0) return; // still waiting on a pong; the timeout owns that
      if (performance.now() - lastInboundAt < DawnConfig.PING_IDLE_MS) return; // recent traffic proves life
      sendPing();
   }

   function sendPing() {
      if (!ws || ws.readyState !== WebSocket.OPEN) return;
      pendingPingSeq = ++pingSeq;
      send({ type: 'ping', payload: { seq: pendingPingSeq } });
      if (pongTimer) clearTimeout(pongTimer);
      const seq = pendingPingSeq;
      pongTimer = setTimeout(function () {
         onPongTimeout(seq);
      }, DawnConfig.PONG_TIMEOUT_MS);
   }

   /**
    * Consume a `pong` frame (routed here by dawn.js's message handler). Matches
    * the echoed seq to the outstanding ping.
    */
   function handlePong(seq) {
      if (seq !== pendingPingSeq) return; // stale/duplicate
      const wasSuspect = missedPongs > 0; // recovered from a "stale" state
      pongSupported = true; // feature gate: the server answers pings
      unsupportedProbes = 0;
      missedPongs = 0;
      pendingPingSeq = 0;
      if (pongTimer) {
         clearTimeout(pongTimer);
         pongTimer = null;
      }
      // Only re-assert "connected" if we had told the UI the link went stale; a
      // healthy heartbeat stays quiet so the status chip doesn't churn every 10s.
      if (wasSuspect && callbacks.onStatus) {
         callbacks.onStatus('connected');
      }
      // (Session-ownership claims are posted on the heartbeat tick, not here — an
      // idle-link pong is cleared by onmessage before this runs.)
   }

   function onPongTimeout(seq) {
      if (seq !== pendingPingSeq) return; // already answered by a later frame
      pendingPingSeq = 0;
      if (!pongSupported) {
         unsupportedProbes++;
         if (unsupportedProbes >= DawnConfig.MAX_UNSUPPORTED_PROBES) {
            // Truly no answer after several tries — likely an older server that
            // ignores `ping`; fall back to onclose-only liveness (no false-dead).
            stopHeartbeat();
         } else {
            // Not yet confirmed the server answers pings and the arming probe was
            // lost. Retry promptly rather than waiting for the idle gate (steady
            // inbound traffic would suppress it) — otherwise the watchdog never
            // arms and a later real half-open is misread as "unsupported".
            sendPing();
         }
         return;
      }
      missedPongs++;
      if (missedPongs >= DawnConfig.MAX_MISSED_PONGS) {
         deadLink();
      } else if (callbacks.onStatus) {
         // First miss: unstable but not yet dead — tell the UI now instead of
         // waiting the full timeout for the watchdog to reconnect.
         callbacks.onStatus('stale', 'Connection unstable…');
      }
   }

   function deadLink() {
      // Half-open: the server stopped answering though readyState is OPEN. Force
      // the socket closed so onclose runs the normal reconnect+backoff (self-heal)
      // and the status chip stops falsely reading "Connected".
      console.warn('[dawn] link watchdog: no pong, forcing reconnect');
      stopHeartbeat();
      if (ws) ws.close(); // -> onclose -> scheduleReconnect (not superseded)
   }

   /**
    * True only when the link is confirmed usable right now: socket open, caps
    * synced, and no heartbeat ping currently overdue. Kept SEPARATE from
    * isConnected() (a pure transport check with 122 call sites): the connection-
    * status click gates on this so a half-open "Connected" pill still forces a
    * reconnect. (Future: user-write paths could gate on this too, to surface a
    * notice instead of a send vanishing into a half-open socket.)
    */
   function isLive() {
      if (!ws || ws.readyState !== WebSocket.OPEN || !capabilitiesSynced) return false;
      if (pongSupported && missedPongs >= 1) return false; // a ping went unanswered: suspect
      return true;
   }

   /* =========================================================================
    * Cross-tab claim + foreground-probe reconnect policy
    * ========================================================================= */

   // Per-browser "Always on": when set, this tab stays a live surface and
   // reconnects even while hidden (overriding the defer-to-foreground default).
   function isAlwaysOn() {
      try {
         return DawnStore.getBool(DawnStore.KEYS.SESSION_KEEPALIVE, false);
      } catch (e) {
         return false;
      }
   }

   function initSessionChannel() {
      if (sessionChannel || typeof BroadcastChannel === 'undefined') return;
      try {
         sessionChannel = new BroadcastChannel('dawn-session');
         sessionChannel.onmessage = function (ev) {
            const d = ev && ev.data;
            if (!d || d.type !== 'claim') return;
            lastSiblingClaimAt = performance.now();
            lastSiblingClaimToken = d.token || null;
         };
      } catch (e) {
         sessionChannel = null;
      }
   }

   // Announce that THIS tab currently owns the session (called on the authed
   // session frame and on each pong), so a sibling tab won't steal it back.
   function postSessionClaim() {
      if (!sessionChannel) return;
      try {
         sessionChannel.postMessage({
            type: 'claim',
            token: DawnStore.get(DawnStore.KEYS.SESSION_TOKEN, null),
            at: Date.now(),
         });
      } catch (e) {
         /* channel closed */
      }
   }

   // Has a sibling tab claimed OUR session within the last ~2 heartbeat intervals?
   function hasRecentSiblingClaim() {
      if (lastSiblingClaimAt === 0) return false;
      if (performance.now() - lastSiblingClaimAt > 2 * DawnConfig.PING_INTERVAL_MS) return false;
      const token = DawnStore.get(DawnStore.KEYS.SESSION_TOKEN, null);
      return !!token && lastSiblingClaimToken === token;
   }

   // A sibling tab claims the session — don't steal it back. But a claim only
   // proves the sibling was live within the last ~2 heartbeat intervals, not that
   // it still is, so do NOT latch `superseded` (which needs a user gesture to
   // clear and could wedge the LAST surviving tab if the sibling has since died).
   // Instead surface the state and re-evaluate after the claim window: if the
   // sibling re-claimed it's still live (defer again); if it's gone, reconnect.
   // Self-healing, so a stale claim can't permanently strand the only tab.
   function deferToSibling() {
      // A sibling was recently active on this session, so if we DO end up taking it
      // (user clicks "Use DAWN here", or the defer self-heals into a reconnect), do
      // a full transcript reload — the sibling may have advanced the conversation.
      // Set the reclaim flag, NOT `superseded` (the latter would block the
      // self-heal in probeNow/scheduleReconnect).
      reclaiming = true;
      if (callbacks.onStatus) {
         callbacks.onStatus('superseded', 'Active in another tab');
      }
      if (reconnectTimeoutId) {
         clearTimeout(reconnectTimeoutId);
      }
      reconnectTimeoutId = setTimeout(
         function () {
            reconnectTimeoutId = null;
            if (superseded) return; // a real server eviction happened meanwhile — respect it
            if (hasRecentSiblingClaim()) {
               deferToSibling(); // sibling re-claimed → still live → keep deferring
            } else {
               reconnectAttempts = 0;
               connect();
            }
         },
         2 * DawnConfig.PING_INTERVAL_MS + 1000
      );
   }

   // Foreground/online/wake probe. Hidden-tab timers are throttled (a half-open
   // may be minutes-stale on wake, and a hidden-deferred reconnect pending), so on
   // regaining foreground/connectivity act immediately instead of waiting out a
   // throttled tick or a jittered backoff. Called from dawn.js on
   // visibilitychange→visible, window 'online', and 'pageshow'.
   function probeNow() {
      if (superseded) return; // reclaim is an explicit click only (forceReconnect)
      const down = !ws || ws.readyState === WebSocket.CLOSED || ws.readyState === WebSocket.CLOSING;
      if (down) {
         if (hasRecentSiblingClaim()) {
            deferToSibling();
            return;
         }
         // Respect the defer-to-foreground default: an online/pageshow event must
         // not reconnect a hidden non-always-on tab (visibilitychange→visible has
         // document.hidden === false, so it always passes here).
         if (typeof document !== 'undefined' && document.hidden && !isAlwaysOn()) {
            return;
         }
         if (reconnectTimeoutId) {
            clearTimeout(reconnectTimeoutId);
            reconnectTimeoutId = null;
         }
         reconnectAttempts = 0;
         connect();
      } else if (ws.readyState === WebSocket.OPEN) {
         // Open but possibly stale after throttling — probe now so a wake-from-
         // sleep half-open is caught fast rather than on the next throttled tick.
         sendPing();
      }
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
      isLive: isLive,
      startHeartbeat: startHeartbeat,
      handlePong: handlePong,
      probeNow: probeNow,
      postSessionClaim: postSessionClaim,
      setCallbacks: setCallbacks,
      setMaxClientsReached: setMaxClientsReached,
      setCapabilitiesSynced: setCapabilitiesSynced,
      getCapabilitiesSynced: getCapabilitiesSynced,
      getSessionToken: getSessionToken,
   };
})(window);
