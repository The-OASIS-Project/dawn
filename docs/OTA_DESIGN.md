# Server→Satellite OTA Update System — Design

**Status:** Phases 1–4 SHIPPED (server, Tier 1 RPi apply, Tier 2 ESP32 apply) and merged to `main`;
Phase 5 (hardening) partly shipped (`push all` canary rollout). See *Implementation status* below.
**Date:** 2026-06-06 (design); implementation status updated 2026-06-08. Reviewed by architecture /
embedded-efficiency / security agents (v2 folds in their criticals: sign-raw-bytes,
verify-before-parse, offline signing, ESP32 reboot-to-clean OTA, TOCTOU, server-push spine).

---

## Implementation status (2026-06-09)

Committed on branch `satellite_ota` (`a38c901` Phase 1, `9593abc` satellite build tooling,
`c266b75` manifest core + keytool, `1b3e45e` Phase 2 server engine, `5ab603c` status/WS docs,
`198dc08` #11 operator surfaces):

**DONE — server side, fully tested (CI 69/69) + live-verified on real hardware:**
- **Phase 1** — `firmware_version` + `ota` capability reported at registration; `ota_device_state`
  table (auth_db v59) + `src/core/ota_db.c`; WebUI admin shows per-device firmware. Validated on
  real hardware (dawn-kitchen Tier 1, Office Speaker Tier 2 both report 2.0.0).
- **Satellite build tooling** — CI `satellite-build` job + pre-push hook; `dawn_satellite/`
  Pi-target build container (`Dockerfile.pibase`/`.pibuild` + `build-pi.sh`, GHCR base
  `ghcr.io/the-oasis-project/dawn-satellite-build:trixie-arm64`); fixed two headless-build bugs.
- **Phase 2 #8 manifest core** — `src/core/ota_manifest.{c,h}` (fixed LE binary wire, Ed25519
  verify-before-parse + keyring, SHA-256, numeric semver, anti-rollback). 17 unit tests.
- **Phase 2 #9 offline keytool** — `tools/ota_keytool.c` (`make ota-keytool`): keygen / sign /
  verify / pubkey-header. Validated end-to-end.
- **Phase 2 #10 daemon glue** — `src/core/ota.{c,h}` (release store, resolve, begin_push, token
  download authz + realpath guard, finalize) + `ota_db` state machine (single-flight, one-time
  token, reconcile). `[ota]` config. 9 integration tests (`tests/test_ota.c`).
- **Phase 2 #11 transports** — HTTPS image route (`webui_http.c`, `/api/ota/..`, TLS+token+
  path-hardened), WS device handlers + server→device push (`src/webui/webui_ota.c`), admin-gated
  `ota_push`/`ota_list`, registration finalize hook. WS protocol documented (WEBSOCKET_PROTOCOL.md).
- **#11 operator surfaces** — (a) **dawn-admin CLI**: admin opcode band `0xC0–0xCF` reserved in
  `admin_socket.h`, `src/auth/admin_socket_ota.c` (`handle_ota_list_cmd`/`handle_ota_push_cmd`,
  push delegates to `webui_ota_push` so it shares the WebUI spine), `dawn-admin ota list` +
  `dawn-admin ota push --uuid <u> --version <v> [--allow-downgrade]` (client in
  `dawn-admin/socket_client.{c,h}`).  **`dawn-admin ota rescan`** (opcode `0xC2`) re-scans the release
  dir into the in-memory store so a freshly-staged release is pushable **without a daemon restart** —
  `ota-release.sh` auto-calls it after staging (best-effort), and `--allow-downgrade` is threaded
  through `ota-release.sh --push`.  (b) **WebUI admin panel**: per-online-device version picker +
  allow-downgrade + Push button in `www/js/admin/satellites.js` (fetches `ota_list`, sends
  `ota_push`, renders in-flight `ota_state`), dispatch in `dawn.js`, styles in
  `www/css/components/satellites.css`. Four-agent review applied (escapeAttr XSS fix incl. the
  pre-existing delete-button site; clamped snprintf accumulation; mobile touch targets; keep-in-sync
  note on the cross-layer forward decl). **Live-verified 2026-06-09** end-to-end on real hardware:
  CLI `ota list`/`push` + all error paths, token mint (ttl=120s), offer delivery over dawn-kitchen's
  WS, single-flight lock, and the WebUI panel (gating, confirm, push, busy-state).

**DONE — device apply (both tiers):**
- **Phase 3 (RPi apply)** — install-path migration, device-side `abi_tag` check, libcurl download +
  libsodium verify-before-commit (0700, no TOCTOU), atomic binary swap + self-restart, launcher-owned
  boot-count rollback. Distribution pipeline (.deb, `release.yml`, `ota-release.sh`), runtime keyring
  (`/etc/dawn/ota_pubkey`), offer fresh-read + finalize mismatch-resolution + platform-bound download
  token (auth_db v60), and host test suites (`test_ota_apply`/`_launch`/`_marker`). Live-verified
  2.0.0→2.2.0 on dawn-kitchen.
- **Phase 4 (ESP32 apply)** — see §8. `dawn_satellite_arduino/ota_apply.cpp`: offer verify (TweetNaCl
  Ed25519) → NVS hand-off → reboot → WiFi-only download via `esp_ota_ops` + SHA-256 → boot switch →
  NVS boot-count guard rollback, **plus a verify-boot `esp_timer` watchdog** so an image that boots but
  hangs in `setup()` (not just a reboot loop) still advances the guard to revert. **Live-verified
  end-to-end on the ESP32-S3 (Office Speaker) 2026-06-10**: clean 2.1.0→2.2.0 and 2.2.0→2.3.0 through to
  daemon `committed` + WebUI `success`, AND the watchdog rollback — a deliberately-broken (`PSRAM=opi`)
  push hung in `setup()` and self-reverted to the prior slot, no USB reflash. Residual caveat (§8): a
  crash in the *first* lines of `setup()` before `ota_boot_path()` runs (Serial/TFT init) still isn't
  counted, but those are fixed, allocation-free calls.

**REMAINING:**
- **Phase 5 (hardening)** — partial:
  - **`push all` canary-then-rollout — SHIPPED 2026-06-11** (`src/core/ota_rollout.c`, admin opcodes
    `0xC3`–`0xC5`, `dawn-admin ota push-all/rollout-status/rollout-abort`). One canary first; the rest
    fan out only after it re-registers on the new version; a canary revert or timeout halts the rollout.
    **v1 scope: online devices** of the platform/tier not already on the target (offline → skipped, not
    queued — offer-pending-on-reconnect is a future add). Single active rollout, in-memory.  **WebUI
    surface — SHIPPED**: a "Fleet Rollout" panel at the bottom of the satellite-management area
    (`www/js/admin/satellites.js` + `satellites.css`): satellite-type + version + allow-downgrade →
    Roll Out, with a polled status line + Abort (WS `ota_push_all`/`ota_rollout_status`/`ota_rollout_abort`
    in `webui_ota.c`).
  - **Still open:** audit log of OTA operations; anti-rollback enforcement audit; (deferred) signed
    `allow_downgrade` in the manifest (wire-format change); (optional) ESP32 native rollback via a
    custom IDF bootloader.

---

## Context

Rolling new code to satellites is manual today (RPi: `install.sh` stop/copy/restart; ESP32: USB
re-flash) — painful for one device, worse for several. Goal: an operator-triggered OTA system
following embedded-industry security/process standards, across both tiers.

Two tiers, different *apply*, one control plane:
- **Tier 1 (RPi):** Linux binary under systemd. Apply = atomic binary(+libs) swap + self-restart.
- **Tier 2 (ESP32-S3):** Arduino app image. Apply = native dual-partition OTA + bootloader rollback.

**Decided:** control plane on DAP2 WebSocket; **image via HTTPS pull** (auth-gated); **operator-
triggered** (dawn-admin / WebUI) to device/tier/all; **Tier 1 = binary(+libs) only** with rollback.

---

## Trust model

- **Offline signing.** The release **private key never touches the daemon.** Signing happens at
  build/release time off-box (a `dawn-admin ota sign` / standalone tool using libsodium
  `crypto_sign_detached`). The daemon only stores + serves the signed artifacts.
- **Sign raw bytes, not JSON.** The signed object is a **fixed length-prefixed binary manifest**
  (`magic‖fmt_ver‖platform‖tier‖version‖abi_tag‖image_size‖sha256[32]‖min_version`). Device does
  `crypto_sign_verify_detached` over the exact bytes **first**, parses only after it returns 0.
  `abi_tag` carries the build target's OS/ABI identity (see §10) so a device rejects an image not
  built for its runtime.
- **Two independent version gates live in the signed manifest, not the WS offer.** Do not conflate
  them (`ota_manifest_rollback_ok` enforces both):
  - **Anti-rollback (automatic):** the device refuses any candidate whose `version` is *older than
    what's installed* (`m.version < installed`). This is the real protection against pushing an old,
    known-vulnerable build, and it needs **no configuration** — it is always on unless the operator
    selects `allow_downgrade` at push time. `allow_downgrade` (unsigned, from the offer) only relaxes
    this one check.
  - **Anti-skip floor (`min_version`, optional):** the minimum version a device must *already be
    running* to accept the update (`installed >= m.min_version`). Use it ONLY to force sequential
    upgrades through a required-migration milestone (e.g. "don't jump straight to 3.0.0 without
    passing through the 2.5.0 schema migration"). It is **empty by default** (`ota-release.sh` omits
    `--min-version` unless asked) so a routine release installs on any older device — that is the
    whole point of OTA. An empty `min_version` does **not** weaken anti-rollback; the two checks are
    orthogonal.
  - **Known residual (sec-H1, accepted v1):** `allow_downgrade` is unsigned, so a forged/MITM'd
    trusted WSS session plus a genuinely-signed older image could land that older build. `min_version`
    does **not** bound this (the replayed old image carries its own low floor); the only real bound is
    that `allow_downgrade` defaults false and is operator-selected per push. **Phase 5 hardening:**
    move the downgrade authorization *into* the signed manifest (a signed flag) so only the offline
    signer can sanction a downgrade — deferred because it is a wire-format change to the shipped
    Phase 2 manifest core + keytool.
- **Root of trust = an operator-provisioned keyring** (current + next public key) per device, for
  rotation without bricking; dual-sign during overlap; **offline split-custody backup** of private
  keys. Signature compromise is NOT mitigated by rollback.
  - **Tier 1 (RPi): the keyring is a runtime file `/etc/dawn/ota_pubkey`** (root-owned, `0644`, one
    64-hex pubkey per line), NOT compiled into the binary. It lives **outside** the service's
    `ReadWritePaths`, so under `ProtectSystem=strict` the dawn-uid satellite can *read* it but never
    *modify* its own trust anchor — a stronger anchor than a key baked into the (necessarily dawn-
    writable, for self-swap) binary. Hardcoded path (not `satellite.toml`, which is dawn-writable).
    **Fail-closed:** no `/etc/dawn/ota_pubkey` → every OTA offer is rejected. This is also what lets
    a **prebuilt binary be distributed** (e.g. on GitHub) with no baked-in key — each operator runs
    `ota-keytool keygen` and provisions *their own* pubkey, then signs releases for *their own* fleet.
  - ESP32 has no secure-boot/flash-encryption (software-only RoT — a flash-access attacker can swap
    the pubkey; accepted v1, documented).
- **TLS mandatory for OTA.** Reject `ota_*` and `/api/ota/...` when `!lws_is_ssl(wsi)`; device
  verifies CA + hostname. The image is **integrity-protected, not confidential**; the download
  token is an anti-abuse/DoS gate, not secrecy.

---

## Components

### 1. Versioning + capability reporting (both tiers) — Phase 1 foundation
- Add `firmware_version` + an `ota` capability flag (or `ota_protocol_version`) to the
  `satellite_register` payload (`webui_satellite.c`; Tier 1 send existing `VERSION` `main.c:59`;
  Tier 2 add `#define FIRMWARE_VERSION`). ESP32 also reports running-partition label in status.
- **Server never offers to a device that didn't advertise OTA support;** unknown WS `type` must be
  ignored, not fatal.
- Persist version in a new `ota_device_state` table. Register handler writes `satellite_mappings` +
  `ota_device_state` **under one auth_db lock**.

### 2. Release store + keys (offline)
- Release dir `[ota] release_dir` (default `/var/lib/dawn/ota/<platform>/<version>/`) holding the
  image, signed binary manifest, detached `.sig`. Daemon validates manifests on load, serves them;
  does **not** sign.
- `dawn-admin ota keygen` (offline) emits the device keyring header (`ota_pubkey.h`, current+next)
  + writes the private key to operator-controlled storage; `dawn-admin ota sign` produces
  manifest+sig. Private keys split-custody, off the daemon.
  *As built (2026-11-27): keygen/sign/verify/pubkey-header live in the standalone `ota-keytool`
  (`tools/ota_keytool.c`, `make ota-keytool`), not in `dawn-admin`.*

### 3. Server module split (engine + adapters)
- **`src/core/ota_manifest.c`** — pure: build/verify the binary manifest (libsodium
  `crypto_sign_verify_detached` directly — already linked, `CMakeLists.txt:180`), SHA-256, numeric
  semver compare, anti-rollback. No globals/DB/config → host-unit-testable with zero stubs.
- **`src/core/ota.c`** (Layer 2) — daemon glue: release-store I/O, `ota_device_state` via the shared
  auth_db handle + `AUTH_DB_LOCK_*` (precedent: `src/core/missed_notifications_db.c`). **No `webui/`
  include**; server→device push via **callback inversion**.
- **`src/webui/webui_ota.c`** (Layer 4) — WS `ota_offer/ack/reject/status` + HTTPS download route +
  WebUI admin messages.
- **`src/auth/admin_socket_ota.c`** — dawn-admin transport (mirror `admin_socket_messaging.c`).
  Reserve admin opcode band **0xC0–0xCF** (avoid coding-harness 0xB0–0xB8).

### 4. Server→device push delivery (spine)
- `ota push` resolves uuid→session via `session_find_by_uuid` (server-build only; `#ifdef`-stubbed
  local), enqueues `ota_offer` on the session outbound queue, then `lws_callback_on_writable` +
  `lws_cancel_service`. Hold the session lock only to resolve+enqueue; **never hold the auth_db leaf
  lock while taking a session lock.** Offline → `state=offer_pending`, deliver on next register.

### 5. State machine — `ota_device_state(uuid PK, current_version, target_version, target_platform, state, last_error, token, token_expires, updated_at)`
- **DB = single source of truth.** States: `idle → offered → downloading → verifying → applying →
  rebooting → success | failed | offer_pending`.
- **Single-flight:** reject new push if state ∈ {offered…rebooting} unless `--force-abort`.
- **Restart reconciliation:** transient rows older than a timeout → `unknown`, reconciled on
  reconnect.
- **Register-time finalize / server-owned commit:** on reconnect, reported `firmware_version ==
  target` → `success`. Device does NOT self-validate purely on "I connected."
- **Register-time mismatch resolution:** if a device that had progressed past `offered` reconnects
  reporting a version ≠ target (rollback, crash-recovery, or an image whose firmware-version header
  was never bumped), the otherwise-stuck in-flight row is moved to `failed` so the panel unsticks and
  the release is re-pushable. `target_version` is **kept** so a genuine success register that follows
  (e.g. a brief same-process WS reconnect mid-apply) still finalizes via the commit branch above.
  Lifecycle of that kept target: it is cleared by the next push (`begin_offer` overwrites it) or a
  successful finalize; a device that permanently stays on the old version leaves a `failed` row with a
  stale `target_version` until the operator re-pushes — acceptable for v1 (re-push works; the row is
  out of the in-flight set so it doesn't block). `reconcile_stale` only touches in-flight rows, not
  `failed`, so it will not self-clear that target.

### 6. WS control messages + download route
- S→C `ota_offer {version, platform, size, sha256, url, token, allow_downgrade}` (advisory;
  authority is the signed manifest).
- C→S `ota_ack` / `ota_reject {reason}` / `ota_status {state, progress, error, running_partition?}`.
- `GET /api/ota/<platform>/<version>/{image,manifest,sig}` — **token is the primary auth gate:**
  one-time (server ledger), bound to authenticated session uuid + version **+ platform** (v60:
  `ota_device_state.target_platform`, set at `begin_offer`, matched in `consume_token`), short TTL,
  constant-time. The platform binding stops a token issued for `rpi/X` from fetching `esp32/X`'s image
  when both are staged. Path hardening: platform enum whitelist, semver regex, `realpath` prefix vs
  `release_dir`. IP rate-limiter is a coarse DoS guard only; cap/stagger concurrent downloads.

### 7. Tier 1 (RPi) apply
- **ABI check first.** Before download/apply the device verifies the manifest's `abi_tag` matches
  its own runtime (§10) and rejects (`ota_reject{reason:abi_mismatch}`) if not. The marker-commit
  rollback is the backstop if a mismatch slips through (binary fails to start → revert).
- Image = **gzipped** signed `dawn_satellite-<ver>-rpi.tar.gz` (binary + changed libs).
- **R1 install migration (Phase 1 prereq):** run from a **dawn-owned writable dir**
  `/var/lib/dawn-satellite/bin/dawn_satellite`; `bin/`, `ota/rollback/`, `ota/tmp/` under **one**
  `ReadWritePaths` mount (atomic `rename()` needs same fs). `LD_LIBRARY_PATH`/rpath to the dawn-owned
  lib dir — **skip `ldconfig`** (read-only under `ProtectSystem=strict`). Ship in `install.sh`.
- Download via **libcurl**; verify via **libsodium** (`pkg_check_modules(SODIUM REQUIRED libsodium)`).
- **No TOCTOU:** stage to **0700**, hash the exact bytes being committed (verify before final
  `rename`, no re-open). Free-space preflight (< 2.5× image → `no_space`); delete staged tarball
  after extract; one rollback copy.
- **Rollback:** `ota_pending` marker (boot-count) → `exit(0)`; `Restart=always` relaunches. Restore
  the rollback copy at the **top of startup** if marker persists and **boot-count ≥ threshold
  (< systemd `StartLimitBurst=5`)**. Server-owned commit clears the marker on confirmed reconnect.

### 8. Tier 2 (ESP32) apply — reboot-to-clean-state *(SHIPPED — Phase 4, 2026-06-10; `dawn_satellite_arduino/ota_apply.cpp`)*
As built (a few deliberate deviations from the original sketch below, all noted):
- **RAM is the gate, not flash.** A two-state NVS record (`'D'` download / `'V'` verify) drives it. At
  offer time (full heap, WS up) the manifest is **Ed25519-verified, cross-checked, and the authenticated
  metadata stored in NVS**, then `esp_restart()`. `ota_boot_path()` runs first in `setup()` — before
  WiFi/audio/WS — so the download has maximal free *internal* heap and only one mbedTLS session (no
  concurrent WS TLS).
- `partitions.csv`: two 0x1E0000 app slots + otadata, **NVS at stock 0x9000/0x5000** (preserves
  `uuid`/`recon_sec` across the bootstrap re-flash), no spiffs use. Runtime guard `OTA_MAX_IMAGE_BYTES`
  = slot − 64 KB (`static_assert`); build-time advisory: `.bin` < ~85% of slot.
- **Download path** (`'D'`): WiFi + **NTP sync first** (WiFiClientSecure cert dates need real time, else
  every download fails), then `HTTPClient` over `WiFiClientSecure(setCACert)`. Streams 4 KB chunks
  (non-blocking `read()`, 180 s wall-clock + 20 s stall caps) via **`esp_ota_ops` directly** (not the
  Arduino `Update` wrapper — so the boot-partition switch happens only *after* the hash check, not inside
  `Update.end`). The loop-task WDT is dropped for the download (the TLS handshake is one long blocking
  call). Order: incomplete → `esp_ota_abort`; complete → `esp_ota_end` → **re-read the COMMITTED flash and
  SHA-256 it** (verify the on-flash artifact, not just the received stream — like Tier-1 hashes the swapped
  fd) → `esp_ota_set_boot_partition(next)` → NVS `state=V, boots=0` → reboot. **One attempt, fail-closed:**
  any failure clears state, restores the WDT, and boots the OLD image (server's register-time
  mismatch-resolution marks it `failed`; operator re-pushes). Verify lib: **vendored TweetNaCl** (Ed25519
  + SHA-512, public domain) used only at offer time; the download path needs only HW SHA-256.
- **Rollback — NVS boot-count guard (primary), + `mark_app_valid` belt-and-suspenders.** *Deviation + why:*
  the stock Arduino-IDE precompiled bootloader does **not** enable `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`,
  so native auto-revert isn't guaranteed without a custom bootloader (an ESP-IDF/PlatformIO build —
  deferred, see memory `project_esp32_stays_arduino`). Primary mechanism: in `'V'` state the guard
  increments `boots` (persisted FIRST), and at `boots >= 3` reverts to the **explicit known-good slot
  recorded by label at flash time** (`esp_ota_get_running_partition()->label`, found via
  `esp_partition_find_first` — not inferred via `get_next_update_partition`, so a crash-timing quirk can't
  make it "revert" onto the bad image), and only if that slot holds a bootable app (first byte `0xE9`) —
  switching to a blank slot would brick. Belt-and-suspenders: a successful registration calls
  `esp_ota_mark_app_valid_cancel_rollback()` (a harmless no-op if the bootloader lacks rollback, but if
  this core build *did* enable it, it cancels the bootloader's own pending-verify so the two mechanisms
  don't fight) and clears the record. **Verify-boot watchdog (closes the hang-without-reboot gap):** the
  boot-count guard above only advances on *reboots*, so on its own it catches a *reboot loop* but NOT an
  image that boots once and then **hangs** in `setup()` (SHA verification doesn't save you — a perfectly
  intact image can still hang; observed live 2026-06-10, a `PSRAM=opi` build on this QSPI-PSRAM board mapped
  zero PSRAM so the audio `ps_malloc` returned NULL and the sketch `while(1)`-hung, never rebooting, which
  soft-bricked the device until a USB reflash). To close it, the `'V'` boot arms a one-shot `esp_timer`
  (`OTA_VERIFY_REGISTER_TIMEOUT_MS`, 90 s) that runs on its own high-priority task — so it fires even through
  a wedged loop task — and force-`esp_restart()`s if the new image hasn't successfully registered in time;
  that reboot re-enters `ota_boot_path()` (which now runs **before** the PSRAM/buffer allocations, so the
  counter advances even when a later step hangs), incrementing `boots` toward the revert. `ota_on_registered()`
  disarms it on a healthy registration (clearing the record before disarming so a timer firing at the
  boundary is harmless). 90 s × 3 boots ≈ 4.5 min worst-case recovery; it is generous so a slow-but-healthy
  network registration is not false-reverted. Remaining residual: a crash/hang in the *very first* lines of
  `setup()` before `ota_boot_path()` runs (Serial/TFT/NeoPixel init) is still not counted — but those are
  fixed, allocation-free init calls. Native rollback (custom bootloader) is the heavier alternative if the
  ESP32 moves to an IDF build.

### 9. Config / secrets / DB
- `[ota]` in dawn.toml: `enabled`, `release_dir`, `download_token_ttl_sec`, `require_tls`(=true).
- No private key on the daemon. Device: keyring baked at build.
- DB: `ota_device_state` migration in auth_db schema; audit via existing `auth_log`.

### 10. Build host & ABI compatibility (cross-platform correctness)

The Jetson (daemon host, JetPack/Ubuntu) and a Tier 1 RPi (Raspberry Pi OS) are **different
operating systems**: different `libwebsockets` soname (`.so.16` vs `.so.19`), different glibc, etc.
A Jetson-built aarch64 binary **will not run on the Pi** even though both are arm64. (Confirmed
2026-06-07: a Jetson-built `dawn_satellite` failed on `dawn-kitchen` with
`libwebsockets.so.16: cannot open shared object file`.) So OTA artifacts are **OS/ABI-specific**.

- **Tier 1 release builds run in a Debian arm64 container matching the Pi's distro** (currently
  `debian:trixie-slim` — the target Pis run Debian 13 / trixie, libwebsockets 4.3.5 `.so.19`) —
  never on the Jetson. On an arm64 host the container build is **native (no qemu)**. The artifact
  links against the Pi's runtime; the offline signing step (§2) consumes that container-built
  binary. **Build env is a pre-built base image** `ghcr.io/the-oasis-project/dawn-satellite-build`
  (`Dockerfile.pibase`, public) with the heavy ML deps baked in — ONNX Runtime, libvosk, espeak-ng
  (rhasspy fork), piper-phonemize — at the **same pinned versions as `scripts/lib/libs.sh`** so the
  container's `/usr/local/lib` matches the Pi's (a version skew is the same runtime break as the
  soname). Built once, pushed, reused; rebuild + re-push only when a pinned dep or the base distro
  changes. whisper.cpp is NOT baked in (repo submodule, compiled per-build). The dev tool
  `dawn_satellite/build-pi.sh` builds from the live working tree (no commit): `SAT_BUILD=full`
  (default) uses this base for a deployable voice satellite, `SAT_BUILD=headless` uses the apt-only
  `Dockerfile.pibuild` for connect/register/OTA-control tests. Phase 2 adds offline signing on top.
  **The base must track the Pi's distro** — bump when the Pi is upgraded (stock bookworm links
  `.so.17`, wrong for a trixie Pi; verified 2026-06-07).
- **`abi_tag` in the signed manifest** identifies the build target — a coarse, device-computable
  string: `<os_id>-<os_version_codename>-<arch>` from `/etc/os-release` + `uname -m`
  (e.g. `debian-trixie-aarch64`). The container stamps it at build; the device computes its own at
  apply time. (Granularity can tighten later to include a `libwebsockets` soname / glibc floor if
  coarse distro matching proves insufficient.)
- **Device-side check before flash:** reject (`ota_reject{reason:abi_mismatch}`) if
  `manifest.abi_tag != device.abi_tag`. The marker-commit rollback (§7) / native rollback (§8) is
  the backstop, not the primary guard — a mismatched image must be refused, not installed-then-
  reverted.
- **The CI `satellite-build` job is a compile gate, NOT a release-artifact producer.** It builds the
  headless config on Ubuntu only to catch build breakage; deployable Pi binaries come exclusively
  from the Pi-OS container build above.
- **ESP32 (Tier 2)** is firmware on bare metal — no OS/glibc dependency — so `abi_tag` for it is
  just `esp32s3` (the existing `platform`); the cross-distro problem is RPi-specific.

---

## Phasing
1. **Foundation (field-migration, max lead time):** `firmware_version` + OTA-capability advert in
   registration; `ota_device_state` table + admin version visibility; **ESP32 partitions.csv
   bootstrap** documented. Zero apply risk. *(SHIPPED 2026-06-06.)*
   — **RPi install-path migration (R1) moved to Phase 3** (2026-06-06 decision): it restructures a
   working satellite's service for no Phase-1 payoff and is only testable alongside the apply code.
2. **Server:** offline keygen/sign tooling, release store, `ota_manifest.c` (+ host unit test,
   incl. `abi_tag`), **Pi-OS arm64 container build** for Tier 1 release artifacts (§10), HTTPS
   download route (token + path hardening), push spine (§4) + state machine (§5), admin
   publish/list/push/status/abort over both transports.
3. **Tier 1 (RPi) apply:** install-path migration (R1, §7); **device-side `abi_tag` check** (§10);
   libcurl download, libsodium verify-before-commit (0700, no TOCTOU), swap+self-restart,
   boot-count rollback.
4. **Tier 2 (ESP32) apply:** reboot-to-clean OTA, esp_ota stream (WDT + 4 KB chunks), Ed25519 verify
   (TweetNaCl) + SHA-256, **NVS boot-count guard** rollback (native rollback needs a custom bootloader —
   deferred; see §8). *(SHIPPED 2026-06-10.)*
5. **Hardening:** canary-then-rollout for `push all`, anti-rollback enforcement audit, audit log,
   docs. (Resume intentionally dropped both tiers v1.)

---

## Verification
- **Unit (host):** manifest sign/verify roundtrip; verify-rejects-tampered-bytes; numeric semver
  compare + anti-rollback; binary-manifest parse only-after-verify.
- **Integration:** publish (offline-signed) → push to a test RPi → download→verify→swap→restart→
  reconnect reports new version → server commits; ESP32 same with reboot-to-clean + partition
  rollback (pull power mid-update → boots old slot, NVS/secret intact).
- **Negative (must reject):** bad signature; SHA-256 mismatch; duplicate-key/oversized manifest;
  expired / wrong-uuid / replayed token; downgrade (older `version`) without `allow_downgrade`;
  install below an `min_version` anti-skip floor; path traversal;
  plaintext (non-TLS) OTA attempt.
- **Resilience:** RPi crash-on-boot new binary → auto-rollback before `StartLimitBurst`; daemon
  restart mid-update → reconciled on reconnect; `push all` → canary first.
