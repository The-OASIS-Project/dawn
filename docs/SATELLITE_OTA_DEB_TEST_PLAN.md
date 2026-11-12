# Satellite OTA + .deb — Manual Test Plan

Covers everything on branch `satellite_ota` **since the last manual test (server-side OTA #11)**:
Phase 3 device-apply, the runtime keyring, the Debian-correct path migration, and the `.deb`.
Run on a Tier-1 Pi (dawn-kitchen) with an **SSH session held open + a known-good binary copy** for
recovery. Tail `journalctl -u dawn-satellite -f` throughout. Server side (push/WebUI) is already
verified; this validates the *device* end-to-end.

Already verified by the dev's machine (no action unless re-checking): full `build-pi.sh` clean, no
warnings; `package-deb.sh` builds a 4.7M deb with complete Depends + bundled whisper/ggml/onnx/
piper/espeak; launcher is libc-only; marker codec + launcher decision-matrix pass native tests.

## A. Prereqs
1. Rebuild + **push** the base image (it now carries libcurl+libsodium):
   `docker push ghcr.io/the-oasis-project/dawn-satellite-build:trixie-arm64`
2. Build artifacts: `SAT_BUILD=full ./dawn_satellite/build-pi.sh` then `./dawn_satellite/package-deb.sh 2.1.0`.

## B. `.deb` install on a clean device  (the new bootstrap path)
1. `sudo dpkg -i dawn-satellite_2.1.0_arm64.deb` (or `apt install ./...deb` to pull Depends).
   - Expect: `dawn` user created; service enabled + started; first-install guidance printed.
2. **Layout** — confirm the Debian-correct paths:
   - `ls -l /usr/bin/dawn-satellite-launch` → root:root 0755 (frozen launcher = ExecStart).
   - `ls -l /var/lib/dawn-satellite/bin/dawn_satellite` → dawn:dawn (OTA-swappable live copy).
   - `ls /var/lib/dawn-satellite/ota/{tmp,rollback}` → 0700; `rollback/dawn_satellite` seeded.
   - `ls /usr/lib/dawn-satellite/*.so*` → bundled libs; `ldconfig -p | grep dawn-satellite` resolves them.
   - `cat /etc/ld.so.conf.d/dawn-satellite.conf` → `/usr/lib/dawn-satellite`.
   - config at `/etc/dawn-satellite/satellite.toml`.
3. **No missing libs** (the Depends/espeak completeness check):
   `ldd /var/lib/dawn-satellite/bin/dawn_satellite | grep -i 'not found'` → **empty**.
   `systemctl status dawn-satellite` → active (text-only mode is fine without models/keyring).
4. **Re-install / upgrade** (`dpkg -i` again) → live binary refreshed from `.dist`, service restarts cleanly.

## C. OTA keyring  (fail-closed trust anchor)
1. `./build-debug/ota-keytool keygen --out-dir /tmp/otakeys` (or reuse the Phase-2 test key).
2. **No keyring yet** → push an offer (Section E) → device replies `ota_reject "no OTA keyring provisioned"`.
3. `sudo install -m644 -o root -g root /tmp/otakeys/ota_signing.pub /etc/dawn/ota_pubkey` → restart.
   - Confirm `/etc/dawn/ota_pubkey` is root-owned and NOT writable by `dawn` (it's outside ReadWritePaths).

## D. Models  (voice mode)
- Models: `sudo dawn-satellite-fetch-models` (VAD + Piper TTS + Whisper tiny-q5_1, matching the shipped config).
- Else copy models manually into `/var/lib/dawn-satellite/models/` (chown dawn).
- Enable voice in `/etc/dawn-satellite/satellite.toml`, `systemctl restart`, confirm wake-word + a query works.

## E. OTA device-apply matrix  (push from the server: `dawn-admin ota push --uuid <u> --version <v>` or WebUI)
Stage a signed release (`ota-keytool sign --image <new binary> --version 2.1.x --platform rpi --tier 1
--abi-tag debian-trixie-aarch64 --sk /tmp/otakeys/ota_signing.key`) into the daemon's release_dir.
Reset the device row between runs: `sqlite3 /var/lib/dawn/auth.db "UPDATE ota_device_state SET state='idle',target_version=NULL WHERE uuid='<u>'"`.

**CI coverage column** — each row's *decision logic* now has host unit tests that run in CI
(`make tests-ci && ctest -L ci`).  ✅ = fully covered by a suite; ◑ = the decision is covered but
an integration aspect (systemd restart cadence, real ELF swap/exec, TLS download, wall-clock timer,
power loss) still needs a device run; 🖥 = device-only.  Suites: `test_ota_apply` (offer
accept/reject), `test_ota_launch` (commit/count/rollback decision), `test_ota_marker` (marker codec),
`test_ota` (server engine).

| # | Case | Induce | Expect | CI coverage |
|---|---|---|---|---|
| 1 | **Happy path** | push a newer signed 2.1.x | ack→downloading→verifying→applying→rebooting→exit(0)→launcher execs new→re-registers new version→server `success`; `bin/` new, `ota/rollback/` old | ◑ decisions: `test_ota_apply` (accept) + `test_ota_launch` (commit) + `test_ota::test_finalize_on_register`; ELF swap + systemd re-exec = device |
| 2 | Bad signature | sign with a non-keyring key | immediate `ota_reject "bad signature"`, no download | ✅ `test_ota_apply::test_bad_signature` |
| 3 | SHA mismatch | serve a patched image, manifest unchanged | `ota_status failed "image hash mismatch"`, no swap | 🖥 worker download+hash not extracted; device-only |
| 4 | ABI mismatch | sign `--abi-tag debian-bookworm-aarch64` | immediate `ota_reject "abi mismatch…"` | ✅ `test_ota_apply::test_abi_mismatch` + `test_build_abi_tag` |
| 5 | Downgrade | running 2.1.x, push 2.0.0 | blocked w/o `--allow-downgrade`; proceeds with it | ✅ `test_ota_apply::test_downgrade_blocked_then_allowed` (+ `test_min_version_floor`) |
| 6 | Already current | push the running version | `ota_reject "already on this version"` | ✅ `test_ota_apply::test_already_current` |
| 7 | **Crash-loop rollback** | push a 2.1.x that exits 1 before register | launcher boots 1→2→3 → restores rollback → restored binary registers; "ROLLBACK" in log; within StartLimitBurst | ◑ `test_ota_launch::test_threshold_ramp_then_rollback`; systemd `StartLimitBurst` cadence = device |
| 8 | **Dead-on-arrival rollback** | push a corrupt/non-exec ELF (can't `execv`) | launcher counts each failed exec → rolls back after 3 (proves rollback works when the binary never runs) | ◑ count→rollback decision in `test_ota_launch::test_threshold_ramp_then_rollback`; real non-exec-ELF `execv` failure = device |
| 9 | Empty rollback slot | rm `ota/rollback/dawn_satellite`, force crash-loop | launcher logs "no restore target", keeps marker, does NOT blank the binary | ✅ `test_ota_launch::test_empty_slot_keeps_marker` + `test_nonexec_slot_keeps_marker` |
| 10 | Transient-network guard | push good 2.1.x, kill daemon during post-swap register window | NO rollback (≥60s → `ran_ok` → launcher commits); recovers when daemon returns | ◑ `test_ota_launch::test_ran_ok_commits` + `test_expired_commits`; 60 s timer + daemon-kill = device |
| 11 | Phantom-rollback guard | after a committed update, kill the binary repeatedly (unrelated) | NO rollback (marker already cleared → normal Restart=always) | ✅ `test_ota_launch::test_no_marker_boots` |
| 12 | Single-flight | two rapid pushes | second → `ota_reject "busy"` | ◑ server `test_ota::test_single_flight`; device-side `g_inflight` guard = device |
| 13 | Power-cut | hard power-cycle mid-apply / corrupt the marker | old binary boots + registers + self-resolves; corrupt marker → fail-safe normal boot | ◑ `test_ota_marker` (atomic replace + fail-safe parse); real power loss = device |

Section C (keyring fail-closed) is covered by `test_ota_apply::test_no_keyring_fail_closed` +
`test_load_keyring`.  Server-side offer/finalize/reconcile are covered by `test_ota`.

## E2. ESP32 (Tier 2) OTA apply matrix  (Phase 4 — `dawn_satellite_arduino/ota_apply.cpp`)
**All hardware — no CI** (Arduino firmware; the Tier-1 host suites don't cover the sketch). Prep: USB-flash
the OTA-capable sketch once (dual-OTA partition scheme), vendor `tweetnacl.{c,h}`, provision `ota_pubkey.h`
with the operator key, bump `FIRMWARE_VERSION`, **Export Compiled Binary**, then stage with
`ota-release.sh --platform esp32 --tier 2 --abi-tag esp32s3 --image <sketch>.ino.bin`. Reset the device row
between runs (same SQL as §E). Watch the Serial Monitor (115200) for `OTA:` lines.

**Build-config trap (cost us a long debug session):** the Adafruit Feather ESP32-S3 TFT is an **ESP32-S3R2 =
QSPI PSRAM**. Build with `PSRAM=enabled` (the board default / `build-esp32.sh`), NOT `PSRAM=opi` — OPI mode on
this chip maps **zero** PSRAM (`psramFound()` true but `getPsramSize()==0`), so the audio `ps_malloc` returns
NULL and the sketch hangs at "Audio alloc fail!". Tell-tale on the post-OTA boot serial: `PSRAM found: size=0`.
A healthy boot prints `PSRAM found: size=2097152`.

| # | Case | Induce | Expect |
|---|---|---|---|
| 1 | **Happy path** ✅ *verified 2026-06-10 (2.1.0→2.2.0, Office Speaker)* | push a newer signed esp32 build | ack→`rebooting`→reboot→(WiFi+NTP)→download→SHA ok→boot switch→reboot→registers new version→server `success`; new slot active |
| 2 | Bad signature | sign with a non-baked key | immediate `ota_reject "bad signature"`, no reboot |
| 3 | ABI mismatch | sign `--abi-tag esp32` (not `esp32s3`) | `ota_reject "abi mismatch"` |
| 4 | Downgrade | running 2.1.x, push 2.0.0 | `ota_reject "downgrade blocked"` w/o flag; proceeds with `--allow-downgrade` |
| 5 | Already current | push the running version | `ota_reject "already on this version"` |
| 6 | Oversize | manifest `image_size` > slot cap | `ota_reject "malformed offer"` |
| 7 | Single-flight | two rapid pushes | second → `ota_reject "busy"` |
| 8 | **SHA mismatch** | serve a patched image, manifest unchanged | post-reboot download fails hash → boot NOT switched → boots OLD image → server marks `failed` on re-register |
| 9 | **Boot-count rollback** | push an image that boots, **reboots**, but can't register (e.g. wrong server in a test build). NB: the guard only counts *reboots* — an image that **hangs/crashes in `setup()` without rebooting** (e.g. the PSRAM trap above) will NOT be reverted (OTA_DESIGN §8 limitation), so don't induce this case with a hanging image | after 3 verify boots the NVS guard reverts to the old slot (Serial "rollback — … reverting"); old image registers |
| 10 | Empty/blank old slot | (first-ever OTA, sibling slot blank) force the rollback path | guard logs "no bootable old slot", does NOT switch (no brick), clears state |
| 11 | Power-cut mid-download | pull power during the download path | boots OLD image (boot partition never switched pre-hash); server marks `failed` on re-register; re-push works |
| 12 | NTP unavailable | block NTP during the download path | download aborts cleanly (no cert-date failure loop), boots OLD image |
| 13 | **Verify-boot watchdog (hang in setup)** | push an image that boots but HANGS in setup() without rebooting (e.g. a build that fails an early alloc and `while(1)`s) | the 90 s `esp_timer` watchdog force-reboots (Serial "verify watchdog — not registered in time"); each reboot increments `boots` (guard runs before the hang), reverting to the old slot after 3 — no permanent soft-brick |

## F. Done criteria
B passes (clean install, no missing libs, service up) + C (keyring fail-closed + provisioned) + E rows
1/2/3/7/8/12 at minimum.  The ✅/◑ decision cores now run in CI; the remaining device runs verify the
integration aspects (ELF swap, systemd cadence, download, timers, power loss).  ESP32 Phase 4 (§E2) is
hardware-only — rows 1/2/8/9/11 at minimum before relying on it.  Then commit + merge `satellite_ota`.
Phase 5 (canary, signed-downgrade-flag, optional ESP32 native-rollback bootloader) remains.
