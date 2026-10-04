# Phone Call & SMS System — Implementation Plan

## Context

Add phone call and SMS support to the OASIS ecosystem via the Waveshare SIM7600G-H 4G modem (USB, SIMCom chipset, US Mobile SIM). The modem is connected to the Jetson with 5 serial ports (`/dev/ttyUSB0-4`) and provides tertiary internet via RNDIS (`usb0`, route-metric 20100). Call audio is carried as **raw PCM over the modem's USB audio port** (`/dev/ttyUSB4`, 16 kHz S16LE mono) — verified end-to-end on a live call 2026-07-06 (see [Hardware Notes](#hardware-notes)). **No analog crossover cable or USB sound card is required** — the earlier "3.5 mm jack only" plan was based on a false negative (`AT+CPCMREG=1` is a per-call command that returns `ERROR` at idle, exactly like `AT+CECM=1`; tested inside an active call it succeeds). During calls, DAWN's main mic continues wake-word listening so the user can say "Friday, hang up" etc.

**Scope**: Voice calls, SMS, call/SMS history DB, HUD notifications, LLM tool, TTS announcements for incoming events.

> **Call-audio quality (WebRTC APM — uplink + downlink, live tuning, WebUI settings)** is a distinct shipped sub-design: see [atlas/dawn/archive/PHONE_CALL_AUDIO_APM_DESIGN.md](https://github.com/The-OASIS-Project/atlas/blob/main/dawn/archive/PHONE_CALL_AUDIO_APM_DESIGN.md). This doc covers the transport, call/SMS state machine, and ECHO daemon.

---

## Architecture — Two-Daemon Design

The modem logic is split between a standalone daemon (**ECHO**) and DAWN, following the OASIS pattern of dedicated daemons per hardware domain with MQTT as the bus. This keeps DAWN focused on AI/voice/tools and avoids burdening it with serial I/O and AT command parsing.

```
DAWN (AI assistant)                     ECHO (modem daemon)
  phone_tool.c  ── MQTT ──────────────>  oasis-echo
    |                echo/cmd               |
  phone_service.c <── MQTT ─────────────  AT commands
    |                echo/events            |
    |                echo/telemetry         |
  phone_db.c                            SIM7600G-H
    |                                   /dev/ttyUSB2 (AT — ECHO owns)
  audio bridge <── raw PCM 16k S16LE ── /dev/ttyUSB4 (USB audio — DAWN owns)
```

**Audio plane / control plane split** (verified 2026-07-06): ECHO owns the **control plane** — `/dev/ttyUSB2` (AT, flock-exclusive) — and sends the per-call `AT+CPCMREG=1` / `AT+CPCMFRM=1` in its existing `modem_call_audio_setup()`. DAWN owns the **audio plane** — it opens `/dev/ttyUSB4` (a distinct device node, no flock contention) and streams raw PCM. DAWN never touches the AT port.

**ECHO** (`~/code/The-OASIS-Project/echo/`): Owns the serial port, handles all AT command traffic, publishes telemetry and events, receives commands via MQTT. ~1,850 lines of C, modeled after STAT.

**DAWN**: Thin `phone_tool.c` (LLM tool interface), `phone_service.c` (call state machine, TTS, HUD, audio bridge), `phone_db.c` (SQLite call/SMS logs). ~800 lines total added.

**MIRAGE**: Subscribes directly to `echo/telemetry` for LTE signal bars on the HUD. No DAWN relay needed.


---

## ECHO Daemon (Standalone)

**Repo**: `~/code/The-OASIS-Project/echo/`
**Binary**: `oasis-echo`
**Template**: STAT (`~/code/The-OASIS-Project/stat/`)

### Responsibilities

- Own `/dev/ttyUSB2` exclusively (flock)
- AT command send/receive with response parsing
- URC reader thread (RING, +CLIP, +CMTI, NO CARRIER, +CREG)
- Signal strength polling (AT+CSQ every 10s)
- Phone number validation (`^[+*#0-9]{1,20}$`)
- SMS body sanitization (strip bytes 0x00-0x1F except newline, reject Ctrl-Z/ESC)
- Hard rate limiting (5 calls/hour, 20 SMS/hour — defense in depth)
- Health monitoring (AT heartbeat, reset on timeout, reconnect on USB disconnect)
- RNDIS data path setup (bundles `sim7600-rndis-up.sh`, previously untracked)

### File Structure

```
echo/
  CMakeLists.txt
  LICENSE, README.md, CLAUDE.md, .clang-format
  config/
    oasis-echo.service
    sim7600-rndis.service           (bundled from /etc/systemd, previously untracked)
    echo.conf
  scripts/
    sim7600-rndis-up.sh             (bundled from /usr/local/sbin, previously untracked)
  install.sh
  include/
    at_command.h                    AT command send/receive API
    logging.h                       Copy from STAT
    modem.h                         Modem state, init, shutdown, signal polling
    mqtt_comms.h                    MQTT init, publish, subscribe, command dispatch
    echo.h                          Global types, version, config struct
    sms.h                           SMS text-mode helpers, phone number validation
    urc_handler.h                   URC reader thread, event classification
  src/
    at_command.c       (~350 lines)
    logging.c          (copy from STAT)
    modem.c            (~400 lines)
    mqtt_comms.c       (~300 lines)
    oasis-echo.c       (~350 lines)
    sms.c              (~200 lines)
    urc_handler.c      (~250 lines)
  tests/
    unity/
      unity.h                       Unity test framework (vendored, MIT license)
      unity.c
      unity_internals.h
    test_at_command.c               AT response parsing, timeout handling
    test_sms.c                      Phone number validation, SMS body sanitization
    test_urc_handler.c              URC classification, RING+CLIP merge, state machine
    test_mqtt_messages.c            OCP message format, JSON construction/parsing
```

### MQTT Topics

| Topic | Dir | QoS | Retain | Content |
|-------|-----|-----|--------|---------|
| `echo/telemetry` | out | 0 | true | Signal, network state, call state (every 10s) |
| `echo/events` | out | 1 | false | Incoming call, SMS received, call ended |
| `echo/response` | out | 1 | false | Command responses with `request_id` |
| `echo/status` | out | 1 | true | Component status online/offline (LWT) |
| `echo/cmd` | in | — | — | Commands from DAWN |

All messages conform to OCP v1.4 (`docs/OASIS_COMMUNICATIONS_PROTOCOL.md`).

### Message Formats

**Telemetry** (`echo/telemetry`, every 10s):
```json
{
  "device": "echo",
  "signal_dbm": -67,
  "signal_bars": 4,
  "csq": 18,
  "registration": "registered_home",
  "operator": "US Mobile",
  "network_type": "LTE",
  "call_state": "idle",
  "sim_status": "ready",
  "timestamp": 1744300000
}
```

**Events** (`echo/events`):
```json
{"device":"echo","event":"incoming_call","number":"+15551234567","timestamp":1744300000}
{"device":"echo","event":"call_connected","timestamp":1744300020}
{"device":"echo","event":"call_ended","reason":"remote_hangup","duration_sec":45,"timestamp":1744300065}
{"device":"echo","event":"sms_received","index":3,"sender":"+15551234567","body":"Hey are you free?","timestamp":1744300000}
{"device":"echo","event":"modem_lost","timestamp":1744300000}
{"device":"echo","event":"modem_reconnected","timestamp":1744300030}
```

**Commands** (`echo/cmd`) — OCP format:
```json
{"device":"echo","action":"dial","value":"+15551234567","request_id":"worker_0_42","timestamp":1744300000}
{"device":"echo","action":"answer","request_id":"worker_0_43","timestamp":1744300000}
{"device":"echo","action":"hangup","request_id":"worker_0_44","timestamp":1744300000}
{"device":"echo","action":"send_sms","value":"+15551234567","data":{"type":"text/plain","encoding":"utf8","content":"On my way"},"request_id":"worker_0_45","timestamp":1744300000}
{"device":"echo","action":"read_sms","value":"3","request_id":"worker_0_46","timestamp":1744300000}
{"device":"echo","action":"delete_sms","value":"3","request_id":"worker_0_47","timestamp":1744300000}
{"device":"echo","action":"signal","request_id":"worker_0_48","timestamp":1744300000}
{"device":"echo","action":"dtmf","value":"1","request_id":"worker_0_49","timestamp":1744300000}
{"device":"echo","action":"query_call","request_id":"worker_0_50","timestamp":1744300000}
```

**Responses** (`echo/response`) — OCP format:
```json
{"device":"echo","action":"dial","request_id":"worker_0_42","status":"success","timestamp":1744300000}
{"device":"echo","action":"dial","request_id":"worker_0_42","status":"error","error":{"code":"NO_CARRIER","message":"Call failed: no carrier"},"timestamp":1744300005}
{"device":"echo","action":"signal","request_id":"worker_0_48","status":"success","value":"{\"signal_dbm\":-67,\"csq\":18}","timestamp":1744300000}
```

### Threading Model

Two threads (plus mosquitto's background thread):

```
Main Thread                         URC Reader Thread
    |                                     |
    |-- main loop (while g_running)       |-- blocking read() on serial fd
    |   - poll telemetry (every 10s)      |   - parse complete lines
    |   - MQTT on_message callback        |   - classify: URC vs AT response
    |     dispatches commands via          |   - URC → mqtt_publish(echo/events)
    |     at_command_send()               |   - AT response → signal cond_var
    |   - sleep(interval)                 |     back to main thread
    |                                     |
```

**AT command serialization**: A mutex + condition variable. Two command types:
- **Sync** (`AT`, `AT+CSQ`, `AT+CMGR`, etc.): `at_command_send()` acquires the mutex, writes the command, `cond_wait`s for OK/ERROR. URC reader signals the condvar with the result. Timeout: 1-2s for most, 60s for AT+CMGS.
- **Async** (`ATD`, `ATA`): `at_command_send_async()` acquires the mutex, writes the command, releases the mutex immediately. Returns "ok, dialing" to the caller. The URC reader handles the eventual result (`CONNECT`, `NO CARRIER`, `BUSY`) as events published to MQTT.

The URC reader thread owns ALL serial reads and maintains a state machine: it knows which command (if any) is pending and what response terminators to expect. URCs that arrive mid-response are queued and dispatched after the response completes. This single-reader design avoids the race condition of two threads reading the same fd.

**AT+CMGS two-phase protocol**: SMS send requires `>` prompt → body → Ctrl-Z (0x1A). A variant of `at_command_send()` handles this multi-step interaction separately.

### Modem Init Sequence

```
AT              — verify communication
ATE0            — disable echo
AT+CMEE=2       — verbose error messages
AT+CSMP=17,167,0,8  — SMS params: UCS2 DCS (enables emoji)
AT+CLIP=1       — enable caller ID
AT+CMGF=1       — SMS text mode
AT+CPMS="ME","ME","ME" — SMS storage to modem memory
AT+CNMI=2,1,0,0,0  — SMS notification via URC
AT+CREG=1       — network registration URCs
AT+CSDVC=1      — audio to headset jack
AT+CLVL=3       — volume mid-level
AT+CSQ          — initial signal read
AT+COPS?        — initial operator query
```

Note: Modem kept in default UCS2 charset (no `AT+CSCS="GSM"`). Phone numbers and SMS bodies are UCS2 hex-encoded for `AT+CMGS` and decoded from `AT+CMGR`. Echo cancellation (`AT+CECM=1`) is sent per-call on `VOICE CALL: BEGIN`, not at init.

### Existing Network Configuration (DO NOT BREAK)

The modem provides tertiary internet via two NetworkManager connections:

| Interface | Connection | Metric | Priority | Manager |
|-----------|-----------|--------|----------|---------|
| `enP8p1s0` (wired) | Wired connection 1 | 100 | Primary | NM |
| `wlP1p1s0` (WiFi) | TORTUGA | 1000 | Secondary | NM |
| `usb0` (RNDIS) | simcom-rndis | 20100 | Tertiary | NM (no ModemManager) |

1. **`simcom-rndis`** — RNDIS ethernet on `usb0`. autoconnect=yes, route-metric=20100 (lowest priority), MTU 1420. Managed by NetworkManager directly — **does NOT use ModemManager**.

2. **`US Mobile`** — GSM connection (autoconnect=no, APN=`pwg`, SIM=`8901260...`). Uses ModemManager for PPP/QMI. Already disabled. Will stop working when ModemManager is disabled — no impact.

### RNDIS Data Path (Bundled)

A systemd oneshot service `sim7600-rndis.service` runs at boot to activate the data connection:
- **Service**: `/etc/systemd/system/sim7600-rndis.service` (enabled, runs after network-online.target)
- **Script**: `/usr/local/sbin/sim7600-rndis-up.sh`

The script does:
1. Waits for `usb0` interface (30s)
2. Creates/updates `simcom-rndis` NM profile (route-metric 20100, MTU 1420)
3. Brings up the NM connection
4. Waits for AT port (`/dev/ttyUSB2` or `/dev/ttyUSB3`, 40s)
5. Sends AT commands to activate RNDIS data: `AT+CGATT=1`, `AT+CGDCONT=1,"IPV4V6","fast.t-mobile.com"`, `AT+NETOPEN`
6. Runs `dhclient` on `usb0`, sets MTU and DNS

These were previously untracked (manually installed to `/etc/systemd/system/` and `/usr/local/sbin/`). Now bundled in the ECHO repo under `config/` and `scripts/`.

**ECHO must coordinate with this script** — it uses AT commands on the same serial port at boot:
- **Phase 1** (simplest): ECHO starts after the RNDIS script completes (`After=sim7600-rndis.service`). ECHO then takes exclusive ownership via flock.
- **Future**: Absorb RNDIS setup into ECHO's init sequence. ECHO sends `AT+NETOPEN` as part of its own modem init. Retire the standalone script.

**Known issue** (2026-04-11): `simcom-rndis` is not active — `usb0` has a link-local address (`169.254.x.x`) and the NM connection shows no device. The route table has a fallback `default dev usb0 scope link metric 1015` but no real gateway. The boot script may be failing silently or the modem data path drops after init. **Verify and fix RNDIS data connectivity before or during Phase 1.** This is another argument for ECHO absorbing the setup — it can monitor the connection and re-establish it if it drops.

**Safe to disable ModemManager**: RNDIS uses plain ethernet on `usb0` via NetworkManager. ModemManager is not involved.

### Config and Deployment

**`config/echo.conf`**:
```conf
MQTT_HOST=localhost
MQTT_PORT=8883
MQTT_USERNAME=echo
MQTT_PASSWORD=changeme
MQTT_TLS=true
MQTT_CA_CERT=/etc/mosquitto/certs/ca.crt
SERIAL_PORT=/dev/ttyUSB2
SERIAL_BAUD=115200
TELEMETRY_INTERVAL=10
RATE_LIMIT_CALLS_PER_HOUR=5
RATE_LIMIT_SMS_PER_HOUR=20
```

**`config/oasis-echo.service`**:
```ini
[Unit]
Description=OASIS ECHO - Enhanced Cellular Handling Operations
After=network.target mosquitto.service sim7600-rndis.service
Wants=mosquitto.service

[Service]
Type=simple
ExecStart=/usr/local/bin/oasis-echo --service \
  --mqtt-host ${MQTT_HOST} --mqtt-port ${MQTT_PORT} \
  --serial-port ${SERIAL_PORT} --serial-baud ${SERIAL_BAUD}
EnvironmentFile=/etc/oasis/echo.conf
Restart=on-failure
RestartSec=5s
User=oasis
Group=oasis
ProtectSystem=full
ProtectHome=true
PrivateTmp=true
NoNewPrivileges=true
SupplementaryGroups=dialout

[Install]
WantedBy=multi-user.target
```

---

## ECHO Testing (Unity Framework)

ECHO uses [Unity](https://github.com/ThrowTheSwitch/Unity) (MIT license) as its test framework — a pure C, single-file test harness designed for embedded systems. Two files (`unity.c`, `unity.h`) are vendored into `tests/unity/`. No external dependencies.

### Test Targets

Built via CMake `enable_testing()` + `add_test()`. Each test is a standalone binary:

```bash
# Build and run all tests
cmake -B build -DCMAKE_BUILD_TYPE=Debug && make -C build -j8
ctest --test-dir build --output-on-failure

# Run individual test
./build/tests/test_sms
```

### Test Modules

**`test_at_command.c`** — AT command layer (no hardware needed):
- Response parsing: extract data lines, detect OK/ERROR/+CME ERROR terminators
- Timeout handling: verify timeout returns correct status
- Command formatting: verify AT string construction
- Sync vs async command classification

**`test_sms.c`** — SMS and phone number validation:
- Valid numbers: `+15551234567`, `*67+15551234567`, `#31#`, `911`
- Invalid numbers: empty, too long (>20), letters, semicolons, backticks
- SMS body sanitization: Ctrl-Z (0x1A) stripped, ESC (0x1B) stripped, newlines preserved
- SMS body length enforcement (800 byte max)
- AT command injection attempts rejected

**`test_urc_handler.c`** — URC classification and state machine:
- Classify known URCs: RING, +CLIP, +CMTI, NO CARRIER, BUSY, +CREG
- RING + CLIP merge: CLIP arrives within 300ms → combined event with number
- RING without CLIP: timeout → event with empty number (blocked caller ID)
- URCs during pending AT command: queued, dispatched after response completes
- Unknown lines: ignored gracefully

**`test_mqtt_messages.c`** — OCP message format:
- Telemetry JSON construction: all required fields present, valid types
- Event JSON: `device`, `event`, `timestamp` fields
- Response JSON: `request_id` echoed, `status` field, `error` object format
- Command parsing: extract `action`, `value`, `request_id`, `data` from JSON

### CMake Integration

```cmake
# In CMakeLists.txt
option(BUILD_TESTS "Build unit tests" ON)

if(BUILD_TESTS)
   enable_testing()

   # Unity test framework (vendored)
   add_library(unity STATIC tests/unity/unity.c)
   target_include_directories(unity PUBLIC tests/unity)

   # Test executables
   foreach(test_name test_at_command test_sms test_urc_handler test_mqtt_messages)
      add_executable(${test_name} tests/${test_name}.c)
      target_link_libraries(${test_name} unity ${JSONC_LIBRARIES})
      target_include_directories(${test_name} PRIVATE include ${JSONC_INCLUDE_DIRS})
      add_test(NAME ${test_name} COMMAND ${test_name})
   endforeach()
endif()
```

Tests link against Unity and the specific source files they exercise (not the full daemon binary). This allows testing AT parsing, SMS validation, and URC classification without a serial port or MQTT broker.

---

## DAWN Integration (Phone Tool + Service)

### New Files (3 source + 1 test)

#### 1. `include/tools/phone_tool.h` + `src/tools/phone_tool.c` (~350 lines)

Single tool, multiple actions — follows `email_tool.c` pattern.

**Actions**: `call`, `confirm_call`, `answer`, `hang_up`, `send_sms`, `confirm_sms`, `read_sms`, `call_log`, `sms_log`, `status`

**Metadata**:
```c
.name = "phone",
.device_string = "phone",
.topic = "dawn",
.aliases = {"telephone", "call", "sms", "text"},
.capabilities = TOOL_CAP_NETWORK | TOOL_CAP_DANGEROUS,
.init = phone_tool_init,
.cleanup = phone_tool_cleanup,
.is_available = phone_tool_available,
```

**Configurable two-step confirmation**:
- `confirm_outbound = true` (default): `call` returns "About to call Mom at +1555..., say confirm." Then `confirm_call` publishes `dial` to ECHO. Same for SMS.
- `confirm_outbound = false`: Execute immediately.

**MQTT integration**:
- Publishes OCP commands to `echo/cmd` with `request_id` via `command_router`
- `phone_tool_init()` subscribes to `echo/events` and `echo/response`
- Event handler routes to `phone_service` for state machine processing

#### 2. `include/tools/phone_service.h` + `src/tools/phone_service.c` (~500 lines)

**State**: `PHONE_STATE_IDLE`, `DIALING`, `RINGING_IN`, `ACTIVE`, `HANGING_UP`

**Functions**:
- `phone_service_init()` / `phone_service_shutdown()` / `phone_service_available()`
- `phone_service_call(user_id, name_or_number)` — resolve contact, publish dial to ECHO
- `phone_service_answer(user_id)` / `phone_service_hangup(user_id)`
- `phone_service_send_sms(user_id, name_or_number, body)`
- `phone_service_get_unread_sms(user_id, out, max)`
- `phone_service_call_log(user_id, count, out, max)` / `phone_service_sms_log(...)`
- `phone_service_get_state()` / `phone_service_get_signal()`
- `phone_service_handle_event(event_json)` — called when ECHO publishes to `echo/events`

**Call state machine** protected by `state_mutex`.

**Event handling** (from ECHO via MQTT):
- `incoming_call` → set state=RINGING_IN, reverse-lookup contact, insert call log (missed initially), send HUD MQTT, TTS announce
- `call_connected` → set state=ACTIVE, send HUD MQTT (bridge not started yet — waits for `pcm_ready`)
- `pcm_ready` (after ECHO arms `AT+CPCMREG=1`) → open `/dev/ttyUSB4`, start audio bridge (bounded timeout guard if it never arrives)
- `call_ended` → calculate duration, update call log, stop audio bridge, set state=IDLE, send HUD MQTT
- `sms_received` → reverse-lookup sender, insert SMS log, publish `delete_sms` to ECHO (after DB commit), send HUD MQTT, TTS announce
- `modem_lost` → if call active, log as failed, stop audio bridge, set state=IDLE

**Contact resolution**: `contacts_find(user_id, input, "phone", results, 5)` from existing `contacts_db.c`.

**Reverse lookup for incoming**: Query contacts by phone number value to resolve display name.

**+CLIP sanitization**: Strip non-printable chars, validate number pattern, cap lengths.

**SMS content safety**: Prefix incoming SMS body with `[Incoming SMS - external content]` for LLM.

**TTS announcements**: `text_to_speech(strdup(text))` — suppress during active call.

**HUD MQTT dispatch** via `worker_pool_get_mosq()` to topic `"hud"`:
```json
{"device":"phone","action":"incoming_call","event_id":"ph-001","number":"+15551234","name":"Mom","initials":"M","ring_timeout":30,"timestamp":1710345600}
{"device":"phone","action":"call_active","event_id":"ph-001","number":"+15551234","name":"Mom","timestamp":1710345620}
{"device":"phone","action":"call_ended","event_id":"ph-001","number":"+15551234","name":"Mom","duration":142,"reason":"completed"}
{"device":"phone","action":"sms_received","event_id":"ph-003","number":"+15555678","name":"Jane","initials":"J","preview":"Hey are you free tom...","body_length":142,"priority":"normal","ttl":15}
```

**Rate limiting** (UX layer): Track last N outbound actions with timestamps. Defaults: max 5 SMS/minute, max 3 dial attempts/minute, max 30 SMS/day. Configurable. Refuse and log when exceeded. (ECHO has its own hard limits as defense in depth.)

**Cross-topic ordering**: Must handle `call_connected` event arriving before `dial` response (different MQTT topics, no ordering guarantee).

#### 3. `include/tools/phone_db.h` + `src/tools/phone_db.c` (~350 lines)

Uses Pattern A (shared `auth_db` handle, same as `calendar_db.c`).

**Tables**:
```sql
CREATE TABLE IF NOT EXISTS phone_call_log (
   id INTEGER PRIMARY KEY AUTOINCREMENT,
   user_id INTEGER NOT NULL,
   direction INTEGER NOT NULL,        -- 0=outgoing, 1=incoming
   number TEXT NOT NULL,
   contact_name TEXT DEFAULT '',
   duration_sec INTEGER DEFAULT 0,
   timestamp INTEGER NOT NULL,
   status INTEGER NOT NULL            -- 0=answered, 1=missed, 2=rejected, 3=failed
);
CREATE INDEX idx_phone_call_user_ts ON phone_call_log(user_id, timestamp DESC);

CREATE TABLE IF NOT EXISTS phone_sms_log (
   id INTEGER PRIMARY KEY AUTOINCREMENT,
   user_id INTEGER NOT NULL,
   direction INTEGER NOT NULL,
   number TEXT NOT NULL,
   contact_name TEXT DEFAULT '',
   body TEXT NOT NULL,
   timestamp INTEGER NOT NULL,
   read INTEGER DEFAULT 0
);
CREATE INDEX idx_phone_sms_user_ts ON phone_sms_log(user_id, timestamp DESC);
CREATE INDEX idx_phone_sms_unread ON phone_sms_log(user_id, read) WHERE read = 0;
```

Schema version bump (add to `auth_db_internal.h`).

#### 4. `tests/test_phone_db.c`

Unit tests for DB layer. Follow `test_scheduler` pattern.

### Files to Modify (5)

1. **`include/auth/auth_db_internal.h`** — prepared statements, schema version bump
2. **`src/auth/auth_db_core.c`** — migration: CREATE TABLE phone_call_log + phone_sms_log
3. **`include/config/dawn_config.h`** — add `phone_config_t` struct
4. **`cmake/DawnTools.cmake`** — add `DAWN_ENABLE_PHONE_TOOL` option
5. **`src/tools/tools_init.c`** — add registration call

### dawn.toml Addition

```toml
[phone]
enabled = true
confirm_outbound = true
pcm_port = "/dev/ttyUSB4"          # modem USB audio port (raw 16k S16LE PCM); no USB sound card
sms_retention_days = 90
call_log_retention_days = 90
rate_limit_sms_per_min = 10
rate_limit_calls_per_min = 3
rate_limit_sms_per_day = 30
```

---

## Phone Audio Bridge

### Overview

A dedicated `phone_audio_bridge` thread in `phone_service.c` handles bidirectional PCM streaming between the **modem's USB audio port** (`/dev/ttyUSB4`, raw 16 kHz S16LE mono) and the active client. There is no USB sound card and no analog path — the modem PCM port is a raw byte stream DAWN reads/writes directly.

### Audio Flow

```
  SIM7600G-H  ── raw PCM 16k S16LE ──>  /dev/ttyUSB4  ── read ──┐
  (far-end     <── raw PCM 16k S16LE ──               <─ write ─┤
   call audio)                                                  |
                                    phone_audio_bridge thread    |
                                     (open ttyUSB4 O_RDWR,       |
                                      resample if client≠16k,    |
                                      real-time-clocked writer)  |
                                              |
                          ┌───────────────────┼───────────────────┐
                          |                   |                   |
                    Local client         WebUI client        Satellite
                   (primary audio       (WebSocket Opus     (DAP2 audio
                    playback/capture)    0x11/0x12 out,      session)
                                         0x01/0x02 in)
```

Downlink at 16 kHz matches DAWN's ASR pipeline directly (no upsample for wake-word-during-call). CECM (far-end echo cancel, `AT+CECM=1`) is already set per-call by ECHO; near-end AEC is DAWN's existing AEC3 on the helmet path.

### Coordination & Handshake (race-free)

The control plane (ECHO) and audio plane (DAWN) hand off through an explicit ready signal so there is **no window of unknown state**:

1. ECHO detects `VOICE CALL: BEGIN`, sets state ACTIVE, publishes `call_connected`, and (in `modem_call_audio_setup()`) sends `AT+CPCMFRM=1` + `AT+CPCMREG=1` on ttyUSB2.
2. **Only after `CPCMREG=1` returns `OK`**, ECHO publishes a `pcm_ready` event (new). DAWN opens `/dev/ttyUSB4` and starts the bridge loop **on `pcm_ready`, not on `call_connected`** — this eliminates the startup race where ttyUSB4 has no data until PCM is armed.
3. DAWN's `pcm_ready` handler **guards on call state** (only arms the bridge when the machine is `ACTIVE`/`ANSWERING`) — `echo/events` is unauthenticated by default, and an unguarded handler would let a spoofed `pcm_ready` open the local mic onto the modem line (hot-mic). The handshake is **event-driven and non-blocking**: if `pcm_ready` never arrives, the bridge simply never starts (the call has no audio) and nothing blocks, so hang-up still works — no watchdog timer is needed.
4. On `call_ended`/`modem_lost`, DAWN stops the bridge and closes ttyUSB4 **before** returning; ECHO's `AT+CHUP` auto-stops modem PCM (no explicit `CPCMREG=0` needed). Teardown is bounded: the single bridge thread's cycle is paced by the blocking mic capture (one ~20 ms period), the ttyUSB4 fd is `O_NONBLOCK`, and a fatal error latches the run flag so the thread exits.

### Bridge Thread Lifecycle

1. **Start** (on `pcm_ready`, call-state-guarded):
   - Pause music **only if it was playing** (`getMusicPlay()` snapshot → `setMusicPlay(0)`), restored to the same state on teardown. TTS announcements are already suppressed during `ACTIVE` (`phone_service` announce guard); full TTS pause during a call is deferred — an open decision tied to whether Friday should be audible on the line (see "Friday-on-the-phone" future scope).
   - Open `/dev/ttyUSB4` (`O_RDWR | O_NONBLOCK | O_NOFOLLOW`, path allow-listed to `/dev/ttyUSB*`/`/dev/ttyACM*`, `S_ISCHR` checked, raw `cfmakeraw` termios, no flow control)
   - No resampler needed on the local-handset path: both audio streams are opened at 16 kHz mono and PulseAudio converts to/from the device

2. **Run** (single lockstep loop, per cycle): capture one mic frame (blocking ~20 ms — the real-time pace) → soft-gain + write it to ttyUSB4 (uplink) → read the matching amount of downlink from ttyUSB4 → soft-gain + write to local playback. The SIM7600 USB PCM is **synchronous full-duplex**, so read and write must stay balanced in one thread — two independent direction threads drift and the modem stops draining uplink (verified live). Both streams are 16 kHz mono (PulseAudio converts to/from the device — no resampler). Level is a `tanh` soft-limiter per direction (`PHONE_DOWNLINK_GAIN`/`PHONE_UPLINK_GAIN`): the modem's own `CRXVOL`/`CTXVOL` don't affect the USB PCM tap, so all gain is digital here.

3. **Stop** (on `call_ended`/`modem_lost`): signal the thread, join it, close ttyUSB4 + the audio streams; restore music if the call paused it.

### Client Routing

**Implemented today: local handset only.** WebUI/satellite routing is future scope (see Phase 8) — the bridge currently hard-binds the modem side to the local capture/playback devices.

- **Local** (helmet) — *implemented*: bridge ttyUSB4 modem PCM ↔ primary playback/capture device.
- **WebUI** (browser) — *Phase 8*: encode/decode via `webui_opus_encode_stream()`/`webui_opus_decode_stream()`, send as binary WebSocket messages. Needs a jitter buffer for the async browser audio since the lockstep loop can no longer pace off a local mic.
- **Satellite** — *Phase 8*: same binary audio path as WebUI.

### Wake Word During Calls

Primary mic capture continues independently during calls. The user's voice goes to both the call bridge AND the ASR path. "Friday, hang up" triggers wake word → ASR → LLM → `phone_tool action=hang_up`.

---

## Hardware Notes

- **Modem**: Waveshare SIM7600G-H 4G HAT, USB ID `1e0e:9011`
- **Serial ports**: `/dev/ttyUSB0-4` (all `option1` driver, USB IF02–IF06). ttyUSB2 = AT command port (primary); **ttyUSB4 = USB audio / raw PCM port**
- **Network**: RNDIS via `usb0` (route-metric 20100, MTU 1420, tertiary behind wired at 100 and WiFi at 1000)
- **Audio**: **USB PCM over `/dev/ttyUSB4`** — 16 kHz S16LE mono, full-duplex. Verified end-to-end on a live call 2026-07-06 (firmware `LE20B04SIM7600G22`): downlink transcribed cleanly by local Whisper; uplink played back at natural pitch/speed. The modem does **not** enumerate a USB Audio Class / ALSA card (`arecord -l` shows no modem card), so PCM is a raw byte stream, not an ALSA device.
  - **`AT+CPCMREG` and `AT+CPCMFRM` are per-call commands** — like `AT+CECM=1`, they return `ERROR` at idle and must be sent inside an active call. The original "no USB PCM" conclusion was a false negative from testing `CPCMREG=1` at idle.
  - Enable sequence (on the AT port, after `VOICE CALL: BEGIN`): `AT+CPCMFRM=1` (16 kHz; resets to 8 kHz on modem reset — set per call) then `AT+CPCMREG=1`. PCM then flows on ttyUSB4.
  - Teardown: `AT+CHUP` ends the call and auto-stops PCM. `AT+CPCMREG=0` during teardown returns `ERROR` (harmless) — prefer just `CHUP`.
  - **Synchronous full-duplex**: the modem exchanges one downlink and one uplink frame per period and expects the host to keep read and write balanced. The bridge does both in a **single lockstep thread paced by the blocking mic capture** (write uplink, read the matching downlink) — two independent threads drift and the modem stops draining uplink (observed live: all uplink writes `POLLOUT`-timeout mid-call). A pure sine test tone is a poor validator (the AMR voice codec distorts sustained tones — it *sounds* like a mid-tone pitch split); validate with speech, not tones.
  - **All call-audio gain is digital in the bridge.** The modem's gain controls (`CRXVOL`/`CTXVOL`/`COUTGAIN`/`CLVL`) act on the analog codec only, **not** the USB PCM tap — verified 2026-07-06 by sweeping `CRXVOL` across its whole `0x0000-0xFFFF` range with a constant far-end source (downlink level was flat). Also: aggressive AT gain probing that leaves an extreme value set (e.g. `CTXVOL=0xFFFF`) can wedge the modem's uplink until `AT+CFUN=1,1`.
- **SIM**: US Mobile (T-Mobile MVNO), APN `fast.t-mobile.com`

---

## Data Flows

**Outgoing call (with confirm)**: User says "call Mom" → LLM → phone_tool `action=call` → contact lookup → return "About to call Mom, confirm?" → User says "yes" → LLM → `action=confirm_call` → phone_service publishes `{"action":"dial","value":"+1555..."}` to `echo/cmd` → ECHO sends `ATD+1555...` → ECHO publishes `call_connected` on `echo/events` → phone_service starts audio bridge → HUD MQTT

**Incoming call**: ECHO URC reader detects RING + +CLIP → publishes `incoming_call` on `echo/events` → DAWN phone_service → reverse lookup → state=RINGING_IN → insert call log (missed initially) → HUD MQTT → TTS "Incoming call from Mom" → User says "answer" → LLM → phone_service publishes `{"action":"answer"}` to `echo/cmd` → ECHO sends `ATA` → ECHO publishes `call_connected` → phone_service starts audio bridge

**Incoming SMS**: ECHO detects +CMTI → auto-reads SMS → publishes `sms_received` with body on `echo/events` → DAWN phone_service → reverse lookup → sanitize body → `phone_db_sms_log_insert()` → on success: publish `delete_sms` to ECHO → HUD MQTT → TTS "New text from Jane"

---

## Implementation Order

### Phase 1: ECHO Skeleton + Serial ✓
Create repo, serial I/O, AT command/response. Verify: send `AT`, get `OK`.

### Phase 2: ECHO URC Thread + Modem Init ✓
URC reader, init sequence, signal polling. Verify: RING events in console.

### Phase 3: ECHO MQTT Integration ✓
MQTT connect/subscribe/publish, wire commands/events, SMS, telemetry. Verify: `mosquitto_pub` commands work.

### Phase 4: DAWN Integration + Deployment ✓
phone_tool.c, phone_service.c, phone_db.c, config, systemd. MIRAGE LTE signal bars. Verify: voice command "call Mom" flows end-to-end.

**Phase 4.5: Ring Events, Ringtone, LLM Context, Voicemail Detection** ✓ (April 2026)
- ECHO: ring broadcast on each RING URC, ring timeout watchdog (AT+CLCC poll after 10s), CMTI dedup, hangup publishes call_ended directly, ms timestamps in logging
- DAWN: Iron Man ringtone (NES pulse synthesis via audio_backend), ring event forwarding to HUD, LLM context injection for all phone events, SMS delete fire-and-forget (fixes MQTT callback self-deadlock), configurable phone service user_id
- MIRAGE: notification system (state machine, config-driven elements, base64 photo decode, fade in/out), contact photo display from MQTT

### Phase 5: Audio Bridge + End-to-End — **local handset FUNCTIONAL** 2026-07-06 (pending commit)
Audio bridge thread over `/dev/ttyUSB4` (raw 16 kHz S16LE USB PCM — **no crossover cable / USB sound card**; hardware path verified 2026-07-06), ECHO per-call `CPCMFRM=1`+`CPCMREG=1` + `pcm_ready` handshake.

**Shipped (local helmet handset):** `src/tools/phone_audio_bridge.c` — a **single lockstep thread** (the SIM7600 USB PCM is synchronous full-duplex; two independent threads drift and the modem stops draining uplink) bridging the local mic ↔ ttyUSB4 ↔ speaker at 16 kHz mono via PulseAudio; tanh soft-limiter gains for level (modem CRXVOL/CTXVOL don't affect the USB PCM tap, so gain is digital). Live-verified two-way on a real 3rd-party call (`up_short=0` across the whole call). Started on `pcm_ready` (call-state-guarded), stopped on `call_ended`/`modem_lost`.

**Remaining for full Phase 5:** (a) **audio quality** — uplink AGC/noise-gate + downlink level (uplink reads "microphone-y/inconsistent" = room mic + cellular AMR AGC/VAD; see TODO.md); (b) **WebUI/satellite routing** — the bridge currently targets only the local handset, not the browser/DAP2 Opus paths (the `webui_broadcast_phone_call` banner can *answer* a call but there's no browser call audio yet); (c) SMS end-to-end, rate limiting, edge cases already covered by earlier phases.

### Phase 5.5: PDU Mode + Concatenated SMS ✓ (April 2026)

Flipped ECHO from text mode (`AT+CMGF=1`) to PDU mode (`AT+CMGF=0`) so SMS send/receive handle messages of any length. Text mode had capped UCS2 at 70 characters (modem returned `CMS ERROR` for anything longer; verified 2026-04-15). PDU mode gives the daemon full control of the 3GPP TS 23.040 binary frame, including UDH-based concatenation across segments.

**Shipped (ECHO)**:
- `include/pdu.h` + `src/pdu.c` — UCS2 SMS-SUBMIT encoder with 8-bit-reference UDH concat; SMS-DELIVER decoder handling both UCS2 and GSM7 (most handsets default to GSM7 for plain ASCII, so decode has to support it even though outbound is UCS2-only in v1). Hardened bounds checks at every length prefix, hex-alphabet validation, and Unicode sanitization stripping NULs, bidi overrides (U+202A–U+202E, U+2066–U+2069), zero-width / formatting chars (U+200B–U+200F, U+2060, U+FEFF, U+061C, U+180E), variation selectors (U+FE00–U+FE0F, U+E0100–U+E01EF), and the Unicode Tag block (U+E0000–U+E007F — prompt-injection smuggling vector). `pdu_new_ref_id()` seeded from `getrandom()` so the outbound ref_id isn't predictable from passive observation.
- `include/sms_reassembly.h` + `src/sms_reassembly.c` — bounded inline store: 8 slots, 2-per-sender cap, 10-min TTL, LRU eviction weighted by fragment count (so a persistent attacker can't evict honest mid-message slots). Duplicate detection via bitmask check *before* copy so a double-send can't corrupt slot state.
- `include/sms_io.h` + `src/sms_io.c` — orchestrator extracted from `oasis-echo.c`; `sms_io_send_and_respond()` owns the error-code → MQTT-response mapping. Inter-segment pacing (150ms default, configurable) avoids SIM7600 wedging on back-to-back concat sends.
- New `at_command_send_pdu()` — two-phase `AT+CMGS=<octets>` + hex + Ctrl-Z with a re-validation of the hex alphabet before bytes hit the serial port (defence-in-depth).
- Rate-limiter rewrite: ring-buffer-with-64-entries → leaky-bucket counter. The old ring silently stopped enforcing at limits > 64, so the new 200/hr segment budget wouldn't have worked without this fix. `rate_bucket_take_n()` debits N tokens atomically for multi-segment sends.
- Concurrency improvement: CMTI/CALL_CONNECTED events pushed to a dedicated `g_deferred_queue` drained *after* MQTT commands, so a 10-segment inbound burst can't stall a concurrent hangup/dial. CMGR/CMGD also use a tight 500ms timeout (down from 2s) because they hit local modem storage, not the network. Combined effect: worst-case MQTT-command latency during an SMS burst dropped from ~40s to ~1s.
- CMGR failure path now issues CMGD fire-and-forget so a transient modem read failure doesn't pin an inbox slot forever.
- `mqtt_publish_response()` schema gains an optional pre-serialized `data_json` merged under the `"data"` key; for `send_sms` it carries `{segments_sent, segments_total}` so consumers can surface partial-send observability. Existing consumers ignore unknown fields.
- Modem init now takes `pdu_mode` flag; `--legacy-sms` / `PDU_MODE=0` env restores text-mode as a rollback. `AT+CSMP` only sent in text mode (PDU carries DCS per-frame).
- Tests: 19 PDU (encode, decode, UDH, malformed rejection, UCS2 sanitization, GSM7 round-trip with real iPhone-style fixture) + 8 reassembly (LRU, per-sender cap, duplicate, timeout, total-mismatch). All 6 suites pass.

**Shipped (DAWN)** — PR #9 earlier in the cycle widened `phone_sms_log.body` from 1024 → 2048 bytes and added a segment-hint helper in `phone_tool.c` so the send-confirmation prompt tells the user when a message will send as multiple parts.

**Validated on real SIM7600G-H (2026-04-22)**: single + multi-segment outbound (up to 5 segments), emoji and surrogate pairs round-trip intact, receiving phone shows one conversation bubble. Multi-segment inbound with 🤔🇨🇦😎 (including a compound flag emoji) reassembles cleanly. Message rate limit and segment rate limit both reject correctly. Voice calls unaffected. Legacy `--legacy-sms` rollback confirmed — long inbound messages fragment into multiple `sms_received` events as pre-PDU, single-segment send still works.

**Deferred to v2**: GSM7 *encode* for outbound (UCS2-only v1, so ASCII messages use 67 chars/segment instead of the 153 GSM7 packing would allow). Add once bandwidth metrics justify the packing code.

**v2 follow-ups tracked**:
- Phone routing to active user — currently incoming-call chimes fan out to every session equally, and the local speaker plays regardless of which satellite/browser the user is actually at. Paused `satellite_mappings` work in memory. Surfaced concrete failures 2026-04-22: WebUI refused to answer an actively-ringing call because that session's LLM context didn't reflect the ring state; satellite + local both tried `phone.answer` for the same call and raced. See `docs/TODO.md` High Impact.
- MIRAGE long-SMS overflow — HUD notification frame doesn't wrap/clip long bodies, and PDU mode suddenly makes long inbound common. See `docs/TODO.md` High Impact.

### Phase 6: MMS Support (Future)
Receive and send multimedia messages (images, video, contacts). The SIM7600G-H supports MMS via AT commands (`AT+CMMSINIT`, `AT+CMMSDOWN`, `AT+CMMSVIEW`, `AT+CMMSSEND`). MMS uses a WAP/HTTP data channel to the carrier's MMSC (Multimedia Messaging Service Center) — the image payload is not delivered inline like SMS text.

**Current behavior**: MMS notifications arrive as empty SMS (blank sender and body via `+CMTI` / `AT+CMGR`). ECHO handles this gracefully (no crashes), but the image content is lost. Verified 2026-04-14.

**Required work (ECHO side)**:
- MMS init: `AT+CMMSINIT` to start MMS service, configure MMSC URL and WAP gateway (carrier-specific: T-Mobile MMSC is `http://mms.msg.eng.t-mobile.com/mms/wapenc`)
- MMS receive: detect MMS notification in `+CMTI`, fetch from MMSC via `AT+CMMSDOWN`, parse MIME multipart body (extract image + text parts)
- MMS send: compose MIME multipart, upload via `AT+CMMSSEND`
- Image storage: save received images to filesystem, reference by path in MQTT events
- New MQTT event fields: `"media_type": "image/jpeg"`, `"media_path": "/var/lib/echo/mms/..."`, `"media_size": 45320`

**Required work (DAWN side)**:
- `phone_service.c`: handle `sms_received` events with `media_path` — download or reference the image
- `phone_tool.c`: display image info to LLM, optionally pass to vision API for description
- WebUI: display MMS images in conversation/notification

**Complexity**: High. MMS involves HTTP transactions over the modem's data connection, MIME parsing, carrier-specific MMSC configuration, and image storage lifecycle. Consider as a separate project phase after audio bridge is complete.

### Phase 7: Smart Contact Resolution (Future)

Improve contact resolution to support natural language references ("my daughter", "the electrician") and multi-result disambiguation.

**Current behavior**: `resolve_number()` does a direct `contacts_find()` LIKE search on entity name. Works for exact names ("Jon", "Mom") but fails for relational references ("my daughter") and silently picks the first result when multiple contacts match.

**Required work**:

1. **Memory-aware resolution**: When `contacts_find()` returns zero results, query the memory system (entity relations, facts) to resolve references like "my daughter" → entity "Sarah" → phone contact. This leverages existing memory_db relations (e.g., `Jon → has_daughter → Sarah`).

2. **Multi-result disambiguation**: When `contacts_find()` returns multiple phone contacts for an ambiguous query:
   - Return all matches to the LLM with an index: "Multiple contacts found: 1. Sarah Smith (+1555...) 2. Emily Smith (+1555...). Which one?"
   - Add a `select_contact` action or extend `confirm_call`/`confirm_sms` to accept an index
   - LLM asks the user and re-calls with the selected index

3. **Relation-based lookup flow**:
   ```
   User: "Call my daughter"
   → resolve_number("my daughter") → contacts_find returns 0
   → memory_db query: find entities related to user with relation "daughter"
   → returns [Sarah, Emily]
   → look up phone contacts for each entity
   → if 1 match: proceed to confirm
   → if 2+ matches: return disambiguation list to LLM
   → if 0 matches: "No phone number found for your daughter(s)"
   ```

4. **Fuzzy matching**: Consider matching on entity aliases, nicknames, and partial names (e.g., "Chris" matching "Christopher").

**Complexity**: Medium. The memory system and contacts DB already exist — this is primarily logic in `phone_service.c`'s `resolve_number()` and a new disambiguation response format in `phone_tool.c`.

### Phase 8: Browser call audio (WebUI phone calls) — Future

Route call audio to a **browser** so a WebUI user can take/place a call from the web interface, not just the local helmet handset. The `webui_broadcast_phone_call` banner already lets a browser **answer/reject/hang up** (shipped); this adds the actual two-way **audio**.

**What exists to reuse**: the WebUI already has a full-duplex Opus audio pipeline — `webui_opus_encode_stream()` / `webui_opus_decode_stream()` (`src/webui/webui_audio.c`), binary WS frames (server→client `0x11`/`0x12`, client→server `0x01`/`0x02`), 48 kHz browser ↔ 16 kHz server resamplers, and always-on voice mode. The modem PCM transport (`phone_audio_bridge.c`, ttyUSB4) is done.

**Required work**:
- Generalize the bridge's endpoint from "local pulse capture/playback" to a **routable sink/source** (an "active client" abstraction). Today `bridge_thread` hard-codes `getPcmCaptureDevice()`/`getPcmPlaybackDevice()`; a WebUI call instead binds the modem side to a WebUI session's Opus streams.
- Downlink: far-end PCM (ttyUSB4, 16 kHz) → `webui_opus_encode_stream()` → binary WS to the browser.
- Uplink: browser mic Opus → `webui_opus_decode_stream()` → 16 kHz → ttyUSB4 (keeping the lockstep balance — the WebUI audio arrives asynchronously, so it must feed a jitter buffer the lockstep loop drains, rather than pacing off a local mic).
- Route selection: `phone_service` picks the audio target based on which surface answered (local handset vs a specific WebUI session).
- Keep the local wake-word path working when the call is on the browser (or decide it's browser-only).

**Complexity**: Medium. Main design question is the pacing hand-off — the lockstep loop is currently paced by the *local blocking mic capture*; a browser call has no local mic to clock it, so it must pace off the modem read (or a fixed-rate timer) and drain a WS jitter buffer for uplink. Prerequisite for treating a phone call as "just another DAP2/WebUI audio client."

### Phase 9: Friday on the phone (AI converses on a call) — Future

The marquee capability: a caller talks to **Friday** (the AI) over the phone — she answers, listens, and responds — rather than the phone bridging to a human. This is **net-new**, not a tweak to the bridge: it routes the call audio through DAWN's **ASR → LLM → TTS** loop instead of to a human client. The PCM bridge (Phase 5) is the prerequisite transport; this is the assistant integration on top.

**Shape**:
- A new **phone-call session type** whose audio I/O "device" is the modem (ttyUSB4), analogous to how a DAP2/WebUI session's device is a WebSocket. Downlink far-end PCM → ASR; TTS → uplink PCM.
- **ASR over a phone line**: Whisper on narrowband, AGC-processed, noisy cellular audio — accuracy will be lower than the helmet mic; the audio-quality work (uplink AGC / cleanup, see TODO.md) feeds directly into this.
- **Endpointing / turn-taking**: VAD tuned for phone audio to decide when the caller finished speaking (harder than a close mic; the far-end AGC/comfort-noise complicates silence detection).
- **Barge-in**: caller talks over Friday → interrupt in-flight TTS (DAWN already has wake-word-interrupt machinery to borrow).
- **TTS-during-call becomes required** (resolves the deferred decision in Phase 5's Bridge Thread Lifecycle) — here Friday *must* be audible on the line, so no TTS suppression.
- **Initiation policy**: does Friday auto-answer (an "AI receptionist" mode), or is it an explicit handoff ("Friday, take this call")? Config-gated; auto-answering a real phone number has obvious safety/social implications.
- Optional: a distinct system prompt / persona for phone callers (they aren't the authenticated owner).

**Complexity**: High. It's a new session type + phone-tuned ASR endpointing + barge-in + a policy layer, all riding on the (now-working) PCM bridge. Highest-wow, biggest build. Independent of Phase 8 — either can come first; Phase 8 is breadth (same call on more surfaces), Phase 9 is depth (the AI *is* the surface).

---

## LLM Message Deletion — SHIPPED (April 2026)

The original tech-debt gap (no way for the LLM to delete SMS/call records) is resolved.

**Implementation**:

- `phone_db.c` / `phone_db.h`: new `PHONE_DB_SUCCESS`/`FAILURE`/`NOT_FOUND` return codes, seven new functions — `phone_db_sms_log_delete`, `_delete_by_number`, `_count_by_number`, `phone_db_call_log_delete`, `_delete_older_than`, `_count_older_than` — plus a shared `phone_number_normalize()` helper so LLM-supplied formats (`"+1-555-..."`, `"(555) ..."`, bare 10-digit US) match stored E.164 rows. Every DELETE is `user_id`-scoped.
- `phone_tool.c`: four new actions — `delete_sms`, `confirm_delete_sms`, `delete_call`, `confirm_delete_call`. Two-step confirmation with a **120-second TTL** on pending state plus a per-user delete rate limit (10/hour default). Matches the `send_sms`/`confirm_sms` and email-tool pattern — the LLM previews, the user says "confirm" on the next turn. TTL bounds replay windows; rate limit caps prompt-injection-coerced bulk deletes.
- `phone_sms_log_t.body` widened from `[1024]` → `[2048]` so multi-segment inbound SMS (from the ECHO PDU work, now shipped — see Phase 5.5 above) doesn't truncate. Stack cost held constant by capping recent-query arrays at 10 entries (down from 20) in `handle_read_sms`/`handle_sms_log`.
- SMS segment-hint added to the `send_sms` confirmation prompt — "This will send as N text messages" when the body exceeds one segment. Gated on `warn_on_multi_segment` config (default `true`).
- Delete criteria in v1: `id`, `number`, or `older_than_days` — exactly one required. No by-contact-name (LLM pre-resolves). Bulk older-than-days is wired for both call log and SMS log.
- **Send/call pending-state TTL backport (April 2026)**: applied the same 120s TTL from the delete path to the older `send_sms`/`confirm_sms` and `call`/`confirm_call` pairs. Previously those globals (`s_pending_call_*`, `s_pending_sms_*`) had no expiry — a stale "confirm" minutes later would still trigger. Now arm stamps `time(NULL)`, confirm checks against `PHONE_TOOL_PENDING_TTL_SEC`, clears state, and returns "Error: confirmation expired. Please retry the ... request." Live-validated on WebUI: `confirm_sms` called 135s after arm correctly rejected instead of sending.

**Still open** — tracked in `docs/TODO.md`:
- Migrate all `phone_tool` pending state from globals to session-scoped.
- Clean up `phone_db.c`'s legacy `-1` error returns across insert/update/query.
- SMS bulk delete by `older_than_days`.
- WebUI deletion parity (deferred — only build if a user asks).

---

## Verification Checklist

1. `oasis-echo` starts, opens `/dev/ttyUSB2`, runs init sequence
2. Signal strength appears in `echo/telemetry` every 10s
3. MIRAGE shows LTE signal bars on HUD
4. `mosquitto_pub` commands (dial, hangup, send_sms) work
5. Incoming call triggers `echo/events` with caller ID
6. DAWN voice command "call X" flows through MQTT to modem
7. Audio bridge works for calls (local speaker, WebUI, satellite)
8. SMS send/receive/log works end-to-end
9. Modem disconnect → `modem_lost` event → reconnect on replug
10. `systemctl start/stop/restart oasis-echo` works correctly
11. RNDIS data connectivity verified through modem route

---

## Security Considerations

- **MQTT auth mandatory**: TLS + username/password. Mosquitto ACL restricts `echo/cmd` to DAWN's identity only.
- **AT command injection**: Phone number validation (`^[+*#0-9]{1,20}$`) at both ECHO and DAWN.
- **SMS body Ctrl-Z injection**: ECHO strips bytes 0x00-0x1F (except newline), rejects 0x1A/0x1B. Max 800 bytes.
- **Rate limiting (two layers)**: DAWN limits for UX (5 SMS/min, 3 calls/min). ECHO limits for abuse prevention (5 calls/hour, 20 SMS/hour).
- **Serial port security**: `flock(LOCK_EX)` for exclusive access. Validate port path matches `/dev/ttyUSB*` or `/dev/ttyACM*`.
- **SMS content safety**: Incoming SMS body prefixed with `[Incoming SMS - external content]` for LLM.
- **CLIP sanitization**: Strip control chars, validate format, cap lengths before use in TTS/MQTT/DB.

---

## Key Reusable Code (DAWN)

| What | Where | How |
|------|-------|-----|
| Contact lookup | `contacts_find()` in `src/memory/contacts_db.c` | Resolve "Mom" → phone number |
| MQTT publish | `worker_pool_get_mosq()` in `include/core/worker_pool.h` | HUD events (NULL-check!) |
| Command router | `command_router_register/wait/deliver` in `include/core/command_router.h` | Sync MQTT request/response with ECHO |
| TTS announce | `text_to_speech(strdup(text))` | Announce incoming calls/SMS |
| Tool registration | `tool_registry_register()` | Standard tool pattern |
| DB shared handle | `AUTH_DB_*` macros in `include/auth/auth_db_internal.h` | Pattern A for phone_db |
| Modem PCM | raw `open("/dev/ttyUSB4", O_RDWR)` read/write (not audio_backend) | Modem-side call audio: raw 16 kHz S16LE stream |
| Audio streams | `audio_stream_capture/playback_open()` in `include/audio/audio_backend.h` | Client-side (local helmet) playback/capture |
| Resampler | `resampler_create/process()` in `include/audio/resampler.h` | Modem 16 kHz ↔ client rate (e.g. 48 kHz WebUI) |
| WebUI Opus | `webui_opus_encode/decode_stream()` in `src/webui/webui_audio.c` | Browser call audio |
| TTS pause | `tts_playback_state` in `include/tts/text_to_speech.h` | Suppress TTS during call |
| Music pause | `setMusicPlay(0/1)` in `src/audio/flac_playback.c` | Pause music during call |
| OCP helpers | `ocp_get_timestamp_ms()` in `include/core/ocp_helpers.h` | Timestamp generation |

---

## Review Findings Incorporated

### Architecture (2 High, 4 Medium)

| Severity | Finding | Resolution |
|----------|---------|------------|
| HIGH | AT response state machine — `ATD` returns nothing until `CONNECT`/`NO CARRIER` (30+ sec). URC reader needs to know which command is pending and what terminators to expect. | State machine in URC reader tracks pending command type. URCs mid-response queued for dispatch after response completes. |
| HIGH | Sync vs async AT commands — `ATD`/`ATA` must not hold mutex for call duration. | Split into sync (hold mutex, wait OK/ERROR) and async (write, release mutex, result arrives as URC event). |
| MEDIUM | RING + CLIP merge timing — +CLIP arrives after RING but timing not guaranteed. | 300ms timer after RING. If CLIP doesn't arrive, publish `incoming_call` with empty number. |
| MEDIUM | AT+CMGS two-phase protocol — SMS send requires `>` prompt → body → Ctrl-Z, not simple send/response. | Variant of `at_command_send()` for multi-step interaction. |
| MEDIUM | DTMF and call query missing from command set. | Added `dtmf` (`AT+VTS`) and `query_call` (`AT+CLCC`) actions. |
| MEDIUM | Cross-topic MQTT ordering — `call_connected` event may arrive before `dial` response. | DAWN's phone_service handles out-of-order events across `echo/response` and `echo/events`. |

### Security (2 Critical, 2 High, 2 Medium)

| Severity | Finding | Resolution |
|----------|---------|------------|
| CRITICAL | MQTT command channel has zero authentication — anyone on the network can dial premium numbers. | MQTT auth + TLS mandatory. Mosquitto ACL restricts `echo/cmd` publish to DAWN's identity only. |
| CRITICAL | SMS body Ctrl-Z injection — body containing `\x1A` terminates SMS and executes arbitrary AT commands. | `sms.c` strips bytes 0x00-0x1F except 0x0A (newline). Reject 0x1A (Ctrl-Z) and 0x1B (ESC). Max 800 bytes. |
| HIGH | No rate limiting in ECHO itself — DAWN's limits can be bypassed via direct MQTT. | Hard limits in ECHO: 5 calls/hour, 20 SMS/hour. Configurable in `echo.conf`. |
| HIGH | Response spoofing via request_id if MQTT unauthenticated. | Mitigated by MQTT ACLs. Also add request_id timeout/matching in DAWN's command_router. |
| MEDIUM | SMS body published to MQTT in cleartext. | Enforce TLS (same fix as auth). |
| MEDIUM | Serial port path from config not validated. | Validate path matches `/dev/ttyUSB*` or `/dev/ttyACM*`. Log warning otherwise. |
