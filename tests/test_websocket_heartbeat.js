/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s).
 *
 * Unit test for the WebUI liveness-heartbeat state machine (www/js/core/websocket.js):
 * verifies the missed-pong hysteresis (single transient miss is invisible; "stale"
 * only after PONG_STALE_MISSES consecutive; deadLink at MAX_MISSED_PONGS) and that
 * inbound traffic / a recovered pong clears the suspect state.
 *
 * Dependency-free: loads the real constants.js + websocket.js IIFEs in a vm context
 * with a deterministic fake clock and mock WebSocket, then drives the actual code.
 * Run: node tests/test_websocket_heartbeat.js
 */
'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const WWW = path.join(__dirname, '..', 'www', 'js', 'core');

/* ---- deterministic fake clock (setTimeout/setInterval + performance.now) ---- */
function makeClock() {
   let now = 0;
   let seq = 1;
   const timers = new Map();
   const schedule = (fn, ms, repeat) => {
      const id = seq++;
      timers.set(id, { at: now + (ms || 0), fn, ms: ms || 0, repeat });
      return id;
   };
   return {
      get now() {
         return now;
      },
      setTimeout: (fn, ms) => schedule(fn, ms, false),
      setInterval: (fn, ms) => schedule(fn, ms, true),
      clearTimeout: (id) => timers.delete(id),
      clearInterval: (id) => timers.delete(id),
      advance(ms) {
         const target = now + ms;
         for (;;) {
            let next = null;
            for (const [id, t] of timers) {
               if (t.at <= target && (!next || t.at < next.t.at)) next = { id, t };
            }
            if (!next) break;
            now = next.t.at;
            if (next.t.repeat) next.t.at = now + next.t.ms;
            else timers.delete(next.id);
            next.t.fn();
         }
         now = target;
      },
   };
}

/* ---- mock WebSocket (always OPEN; records sent frames; close() → onclose) ---- */
function makeMockWS() {
   class MockWS {
      constructor() {
         this.readyState = MockWS.OPEN;
         this.sent = [];
         MockWS.instances.push(this);
      }
      send(data) {
         this.sent.push(data);
      }
      close() {
         this.readyState = MockWS.CLOSED;
         if (this.onclose) this.onclose({ code: 1006, reason: 'test' });
      }
   }
   MockWS.CONNECTING = 0;
   MockWS.OPEN = 1;
   MockWS.CLOSING = 2;
   MockWS.CLOSED = 3;
   MockWS.instances = [];
   return MockWS;
}

/* Load a fresh, isolated DawnWS with all globals mocked. */
function makeEnv() {
   const clock = makeClock();
   const MockWS = makeMockWS();
   const statuses = [];
   const sandbox = {
      window: { location: { protocol: 'https:', host: 'test' } },
      console: { log() {}, warn() {}, error() {} },
      performance: {
         now() {
            return clock.now;
         },
      },
      setTimeout: clock.setTimeout,
      clearTimeout: clock.clearTimeout,
      setInterval: clock.setInterval,
      clearInterval: clock.clearInterval,
      WebSocket: MockWS,
      BroadcastChannel: class {
         postMessage() {}
         close() {}
      },
      document: { hidden: false },
      DawnStore: {
         KEYS: { SESSION_TOKEN: 'st', TTS_ENABLED: 'tts', SESSION_KEEPALIVE: 'ka' },
         get: () => null,
         getBool: () => false,
      },
      Date: Date,
      JSON: JSON,
      Math: Math,
   };
   vm.createContext(sandbox);
   vm.runInContext(fs.readFileSync(path.join(WWW, 'constants.js'), 'utf8'), sandbox);
   sandbox.DawnConfig = sandbox.window.DawnConfig; // websocket.js reads the bare global
   vm.runInContext(fs.readFileSync(path.join(WWW, 'websocket.js'), 'utf8'), sandbox);

   const DawnWS = sandbox.window.DawnWS;
   DawnWS.setCallbacks({
      onStatus: (s) => statuses.push(s),
      onTextMessage: () => {},
      getOpusReady: () => false,
      getTtsEnabled: () => false,
   });

   // Bring a session up and arm the heartbeat exactly as dawn.js does on the
   // authed `session` frame: connect → open → caps synced → startHeartbeat, then
   // answer the arming ping so pongSupported is established.
   DawnWS.connect();
   const ws = MockWS.instances[0];
   ws.onopen();
   DawnWS.setCapabilitiesSynced(true);
   DawnWS.startHeartbeat();
   DawnWS.handlePong(lastPingSeq(ws));
   statuses.length = 0; // ignore the connect/connected noise; assert only what follows

   const cfg = sandbox.window.DawnConfig;
   return { DawnWS, ws, statuses, clock, cfg };
}

/* Seq of the most recent `ping` frame the client sent. */
function lastPingSeq(ws) {
   for (let i = ws.sent.length - 1; i >= 0; i--) {
      const m = JSON.parse(ws.sent[i]);
      if (m.type === 'ping') return m.payload.seq;
   }
   return null;
}

/* Advance to the next heartbeat-tick boundary; the tick sends a ping (idle-gated,
 * which holds here since recovery pongs don't refresh lastInboundAt). Returns the
 * ping seq just sent. Lands exactly on the boundary so the pong window is fully
 * ahead of us — no accidental extra miss. */
function tick(env) {
   const P = env.cfg.PING_INTERVAL_MS;
   const boundary = Math.floor(env.clock.now / P) * P + P; // strictly next multiple
   env.clock.advance(boundary - env.clock.now);
   return lastPingSeq(env.ws);
}

/* One consecutive missed pong: send a ping, then let its pong window elapse
 * unanswered (PONG_TIMEOUT < PING_INTERVAL, so this can't reach the next tick). */
function missOnce(env) {
   tick(env);
   env.clock.advance(env.cfg.PONG_TIMEOUT_MS);
}

/* One recovery: send a ping and answer it BEFORE its timeout (genuine flapping —
 * a late-but-answered pong), which resets the consecutive-miss count. */
function recoverOnce(env) {
   const seq = tick(env);
   env.DawnWS.handlePong(seq);
}

/* ------------------------------ assertions ------------------------------ */
let failures = 0;
function check(name, cond) {
   if (cond) {
      console.log('  ok   - ' + name);
   } else {
      console.log('  FAIL - ' + name);
      failures++;
   }
}

/* T1 — a single transient miss is invisible (no flicker). */
(function singleMissInvisible() {
   console.log('single missed pong does not surface "stale"');
   const env = makeEnv();
   missOnce(env);
   check('no "stale" status after 1 miss', !env.statuses.includes('stale'));
   check('isLive() still true after 1 miss', env.DawnWS.isLive() === true);
})();

/* T2 — "stale" only at the hysteresis threshold (2 consecutive misses). */
(function staleAtThreshold() {
   console.log('two consecutive missed pongs surface "stale"');
   const env = makeEnv();
   missOnce(env);
   missOnce(env);
   check('"stale" emitted after 2 consecutive misses', env.statuses.includes('stale'));
   check('isLive() false while stale', env.DawnWS.isLive() === false);
})();

/* T3 — deadLink (forced reconnect) at MAX_MISSED_PONGS consecutive misses. */
(function deadAtMax() {
   console.log('three consecutive missed pongs force a reconnect (deadLink)');
   const env = makeEnv();
   missOnce(env);
   missOnce(env);
   missOnce(env);
   check('socket was force-closed (deadLink)', env.ws.readyState === env.ws.constructor.CLOSED);
   check("onclose surfaced 'disconnected'", env.statuses.includes('disconnected'));
})();

/* T4 — a recovery resets the count, so a FLAPPING link never shows "stale". */
(function flappingNeverStale() {
   console.log('a flapping link (miss, recover, miss, recover) never shows "stale"');
   const env = makeEnv();
   for (let cycle = 0; cycle < 4; cycle++) {
      missOnce(env); // one miss (count -> 1)
      recoverOnce(env); // late-but-answered pong resets the count before a 2nd miss
   }
   check('no "stale" across sustained flapping', !env.statuses.includes('stale'));
   check('isLive() true after each recovery', env.DawnWS.isLive() === true);
})();

/* T5 — inbound traffic clears an already-shown "stale" (re-asserts connected). */
(function inboundClearsStale() {
   console.log('inbound traffic clears a shown "stale" and re-asserts connected');
   const env = makeEnv();
   missOnce(env);
   missOnce(env);
   check('precondition: "stale" is shown', env.statuses.includes('stale'));
   env.ws.onmessage({ data: JSON.stringify({ type: 'state' }) }); // any inbound frame
   check(
      're-asserted "connected" after inbound',
      env.statuses[env.statuses.length - 1] === 'connected'
   );
   check('isLive() true again', env.DawnWS.isLive() === true);
})();

/* --------------------------------- result --------------------------------- */
if (failures === 0) {
   console.log('\nPASS - all heartbeat hysteresis assertions held');
   process.exit(0);
} else {
   console.log('\nFAIL - ' + failures + ' assertion(s) failed');
   process.exit(1);
}
