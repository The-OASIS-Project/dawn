# Home Assistant Subsystem

Source: `src/tools/homeassistant_service.c`, `src/tools/homeassistant_tool.c`, `src/webui/webui_homeassistant.c`

Part of the [D.A.W.N. architecture](../../../ARCHITECTURE.md) — see the main doc for layer rules, threading model, and lock ordering.

---

**Purpose**: Smart home control via Home Assistant REST API — lights, climate, locks, covers, media players, scenes, scripts, automations.

## Architecture: Service + Tool + WebUI Admin

```
┌───────────────────────────────────────────────────────────────────────┐
│                     LLM TOOL INTERFACE                                │
│  homeassistant_tool.c                                                │
│  16 actions: get_state, turn_on, turn_off, toggle, set_brightness,   │
│  set_color, set_color_temp, set_climate, lock, unlock, open_cover,   │
│  close_cover, media_play, activate_scene, trigger_script,            │
│  trigger_automation                                                  │
├───────────────────────────────────────────────────────────────────────┤
│                     SERVICE LAYER                                     │
│  homeassistant_service.c                                             │
│  → REST API via libcurl with Long-Lived Access Token                 │
│  → Entity cache with periodic refresh                                │
│  → Fuzzy name matching (Levenshtein + token overlap)                 │
│  → Generic call_service() dispatcher                                 │
├───────────────────────────────────────────────────────────────────────┤
│                     WEBUI ADMIN                                       │
│  webui_homeassistant.c + homeassistant.js                            │
│  → Entity browser, connection status, URL/token config               │
│  → Compile-time feature guard (DAWN_ENABLE_HOMEASSISTANT_TOOL)       │
│  → server_features WS message + CSS feature-flag visibility          │
└───────────────────────────────────────────────────────────────────────┘
```

## Key Design Points

- **Fuzzy matching, never a guess** (`homeassistant_match.c`, pure, `tests/test_homeassistant_match.c`):
  "Turn on the living room light" works even if the HA entity name differs slightly, but two
  entities tied for the best match are listed for the user to pick ("could be: …"), never the
  first one. A whole name wins over names that contain it. A switch tied with a same-named
  entity of another domain (its "change device type" replacement) drops out of the tie. The matched entity is copied
  out under the cache lock.
- **Doors need their name and a yes**: an entity that opens a door is a lock; a cover whose
  device class is garage, gate or door, or whose name says so (blinds and awnings aren't); or
  a switch, scene, script or automation with garage, gate, door or unlock as a whole word of its
  name or entity_id (`homeassistant_opens_door`). It matches only by its whole name or one
  containing what was said, and unlock, open, on, toggle, or activating/running/triggering it
  stage a preview; `confirm` with its pending_id carries it out in the user's next turn
  (`core/pending_slots`). Opening blinds or an awning acts directly (`ha_classify_call` tells
  the gate it's an action, so a text needs its reply code). Lock, close and off act directly.
  Unlock can't be scheduled; a scheduled open of a door is refused when it fires.
- **Area-aware**: a satellite mapped to an area tells the model, with the turn's standing directions, to use that area for requests that don't name a room.
- **Feature guard**: `DAWN_ENABLE_HOMEASSISTANT_TOOL` CMake option. (DAWN's standalone SmartThings tool was removed in May 2026 — its upstream OAuth flow was permanently broken by an AWS WAF rule. Install Home Assistant via `docs/HOMEASSISTANT_SETUP.md` and use HA's own SmartThings integration if you need SmartThings device coverage.)
- **Entity cache**: avoids per-request API calls; refreshed on configurable interval.

## WebUI WebSocket verbs (`webui_homeassistant.c`)

All `ha_*` verbs are **admin-only** (`conn_require_admin`). Read verbs (`ha_status`,
`ha_test_connection`, `ha_list_entities`, `ha_refresh_entities`) return an
`ha_entities_response`; the write verb (`ha_call_service`) returns
`ha_call_service_response`. The browser-facing wire contract (request/response shapes,
error strings) is documented for the consumer in `dawn-nextgen/docs/DAWN_UI_SIGNAL_MAP.md
§9.4`.

- **Rich entity attributes (#7)**: `serialize_entity_list()` emits a per-entity
  `attributes` object, **switched on the entity's domain** (`serialize_entity_attributes()`)
  — only the keys that domain's widget needs (light→`brightness`, fan→`percentage`,
  cover→`current_position`, climate→`hvac_mode`/`hvac_modes`/temps, sensor→`unit_of_measurement`/
  `device_class`). Additive: an older client ignores it, absent keys fall back to a plain
  toggle. New `ha_entity_t` fields back the sensor/fan/climate-list values.

- **Interactive control (#8) — `ha_call_service`**: one general verb
  (`entity_id`/`domain`/`service`/`data`) over the public `homeassistant_call_service()`
  wrapper on the service layer's `call_service_json`. `data` is passed to HA verbatim; the
  effective domain is derived from the `entity_id` prefix when omitted.

- **Write allowlist** (`HA_BOARD_SERVICES[]`): the browser write path may invoke **only**
  the exact `(domain, service)` pairs the board's widgets send (the 18-pair table). Anything
  else HA exposes (`shell_command.*`, `python_script.*`, arbitrary `automation.trigger`, …)
  is refused with `"Service not permitted"` before any call — a deliberate blast-radius
  control on the admin-cookie surface. **The LLM tool path is unaffected** (it uses the
  dedicated `homeassistant_turn_on`/etc. functions, not this verb), so the assistant keeps
  full service reach. ⚠️ Adding a controllable domain to the board means editing three
  co-located sites in one change: this table, `serialize_entity_attributes()`, and the
  client widget map (§9.4) — a boxed comment on the table names all three.

- **Realtime WS ingest** (`homeassistant_ws.c`, `[home_assistant] realtime`, default on):
  a persistent listener thread on HA's own `/api/websocket` — a hybrid split where this
  READ/EVENT plane rides the socket while commands stay on REST (per-call independence, so a
  WS reconnect backoff never blocks control). Handshake: `auth` → `subscribe_events`
  (state_changed) → `config/{area,entity,device}_registry/list` (the area join, retiring the
  `/api/template` scrape on this path) → `get_states` seed → LIVE. Self-clocked ping/pong
  liveness; `ws_reconnect` backoff; `s_ws_connected` is kept SEPARATE from the REST
  `connected` flag. Live `state_changed` events are coalesced (dirty-set, ~200 ms) and
  batch-applied under one write lock (`homeassistant_cache_apply_batch`), then pushed as an
  `ha_state_changed` **delta** to admin browsers (`ha_broadcast_state_changed` weak hook →
  `broadcast_json_to_admins`). A `ha_observe_state_changed` weak hook is the (unbuilt) SAGE
  seam. See `docs/WEBSOCKET_PROTOCOL.md` (`ha_state_changed`) + `DAWN_UI_SIGNAL_MAP.md §9.4`.

- **Server-authoritative reconcile**: on a successful `ha_call_service`, the handler
  re-polls HA (entity-only, via `homeassistant_snapshot_entities(force_refresh=true)`) and
  broadcasts a fresh `ha_entities_response` to the acting admin's browser sessions
  (`webui_broadcast_json_to_user`, browsers-only, user-scoped) — but ONLY when realtime is
  off/disconnected; when the WS is live the `state_changed` event delta is the reconcile.
  The UI renders the truth it receives rather than orchestrating its own re-poll.

- **Race-safe reads**: all three entity serializers copy the cache into caller memory under
  `s_ha.rwlock` (`homeassistant_snapshot_entities`) rather than serializing the live struct
  while a worker thread rewrites it. `homeassistant_refresh_entities` (areas + entities)
  remains as the areas-inclusive primitive; the snapshot path is entity-only so a
  per-control reconcile doesn't trigger a wasted area re-poll.

> **Security (hardened 2026-08-01)**: the WS upgrade is now CSRF-origin-checked
> (`webui_is_same_origin_request` at `LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION` in
> `webui_server.c`) — a cross-origin browser can no longer open an authed WS to ride the
> admin cookie into the allowlisted `lock`/`cover` writes. Browser-shaped origins are matched
> against the Host; `null`/opaque origins are rejected; native clients (satellites, CLI) that
> send no browser Origin are allowed. Backed by the `SameSite=Strict` session cookie.
