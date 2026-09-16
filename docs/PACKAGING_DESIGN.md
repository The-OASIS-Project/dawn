# DAWN Ubuntu Packaging Design

**Status:** Design / scoping. Planned-but-unstarted — this doc is an **untracked working reference**, not committed (per the design-doc commit policy: docs graduate to the repo only when they describe shipped or in-flight code).

**Review incorporated:** master-code-reviewer pass 2026-09-15 (cross-checked against the repo + a live `ubuntu:24.04` apt index). Its Critical/High findings changed the shape of §2b, §3, §4, §5, and §6 — folded in below. Verdict was "needs changes before implementation starts"; with them applied, the fpm-on-noble approach and CPU-first phasing stand.

**Goal:** Ship DAWN (the daemon) for general use as Ubuntu `.deb` packages, built on GitHub runners. Two x86-64 variants:

1. **CPU** — `dawn` — CPU-only ONNX Runtime, no GPU deps.
2. **NVIDIA/CUDA** — `dawn-cuda` — GPU-accelerated Whisper via `libggml-cuda.so`; depends on the user's NVIDIA CUDA install.

**Target distro:** Ubuntu 24.04 LTS (noble). Both variants are **x86-64 only** — the Jetson (ARM64 + JetPack CUDA) is a separate build-from-source target and is out of scope here.

**Decisions locked in (2026-09-15):**
- CUDA libs: **depend on NVIDIA's apt repo**, do **not** redistribute NVIDIA libs.
- GPU functional testing: on the developer's own x86-64 NVIDIA laptop (runners have no GPU).
- Sequencing: **CPU package first**, end-to-end; CUDA variant reuses the same scaffolding.

---

## 1. Why this is a "bundled-app" package, not Debian-policy-pure

Most link/runtime deps ship in Ubuntu and become apt `Depends:`. Four dependency sets are not in apt (or cannot be), and one (the espeak-ng fork) is specifically *incompatible* with the distro version. So the package installs DAWN's binaries **and** those four bundled `.so` sets under `/opt/dawn`. This is a self-contained `/opt` application package — correct and shippable, not an archive-quality decomposition.

**Bundled libs are isolated to the daemon, not the host** (see §5, H1): the binary resolves them via `$ORIGIN` RUNPATH, **not** a global `ld.so.conf.d` entry. A global entry would make the rhasspy espeak-ng fork the system-wide `libespeak-ng.so.1` for every program (speech-dispatcher, orca) — unacceptable.

---

## 2. Dependency provenance

Source of truth: `scripts/lib/deps.sh` (apt), `scripts/lib/libs.sh` (from-source), `Dockerfile`/`Dockerfile.cuda`, `CMakeLists.txt`.

### 2a. APT — Ubuntu 24.04 runtime `Depends:`

**Do not hand-list these.** Noble renamed most of them (the `t64` 64-bit-time_t transition) — 9 of a naive bookworm-derived list are wrong, 4 with no `Provides:` fallback (`libwebsockets17`→`libwebsockets19t64`, `libmujs2`→`libmujs3`, `libgumbo1`→`libgumbo2`, `libjpeg62-turbo`→`libjpeg-turbo8`). **Generate the list with `dpkg-shlibdeps`** (needs `dpkg-dev` + a stub `debian/control`) run against staged `dawn`, `dawn-admin`, and every bundled `.so` **inside the noble container**, then feed to `fpm --depends`. The table below (verified against noble apt, 2026-09-15) is the expected result / fallback:

| Area | Packages |
|---|---|
| net/crypto/db | `libcurl4t64 libjson-c5 libssl3t64 libsqlite3-0 libsodium23 libwebsockets19t64 libopus0` |
| logging | `libspdlog1.12` (pulls `libfmt9`) |
| MQTT | `libmosquitto1`; **Recommends:** `mosquitto` (broker not hard-required — both Dockerfiles run with none) |
| audio | `libasound2t64 libpulse0 libsndfile1 libflac12t64 libsamplerate0` |
| codecs | `libmpg123-0t64 libvorbis0a libvorbisfile3` |
| documents (MuPDF is **static** — no `libmupdf` runtime pkg exists; these are its `Libs.private`) | `libfreetype6 libharfbuzz0b libzip4t64 libxml2 libgumbo2 libopenjp2-7 libjbig2dec0 libmujs3 libjpeg-turbo8 zlib1g` |
| calendar | `libical3t64` |
| BM25 | `libstemmer0d` |
| base | `libuuid1 libncurses6 libtinfo6 libgomp1 libstdc++6 libgcc-s1 libc6 (≥2.38) ca-certificates adduser` + `curl \| wget` (model fetch) |
| optional code-projects | `libgit2-1.7` (only if `DAWN_ENABLE_CODE_PROJECTS` on; **off** in the `server` preset) |

**Noble drops libgit2 from source builds** — noble ships `libgit2` 1.7.2 (≥ 1.6 required, `CMakeLists.txt:402-411`); Jammy needed the `libs.sh` v1.8.1 source build because 22.04 ships 1.1. MuPDF 1.23 vs bookworm 1.21 remains a "confirm it compiles" item (link line confirmed compatible; C API drift is not verifiable statically).

### 2b. BUNDLED — carried under `/opt/dawn/lib`

Four sets for the **server** daemon (AEC/webrtc + libvosk are excluded by the preset — §3):

| Bundled | Why not apt | Acquisition (pinned) |
|---|---|---|
| **espeak-ng (rhasspy fork)** | Distro espeak-ng lacks `TextToPhonemesWithTerminator`. | git build, commit `8593723f10cfd9befd50de447f14bf0a9d2a14a4`, **`--prefix=/opt/dawn`** (see H2). Ship `libespeak-ng.so*` + `espeak-ng-data/` → `/opt/dawn/share/espeak-ng-data`. |
| **ONNX Runtime** | Not in apt (MIT). | **Prebuilt x64 tarball v1.22.0 for BOTH variants** (SHA-pinned, `Dockerfile:57-60`). See H3 — the from-source CUDA ORT build is dead weight and is dropped. |
| **piper-phonemize** | Not in apt. | git build, commit `ba3cc06c5248215928821f1393b2b854a936991a`. Ship `libpiper_phonemize.so*`. |
| **whisper.cpp + ggml** | Submodule, in-tree. | pin `d9b7613b34a343848af572cc14467fc5e82fc788`. Ship `libwhisper.so libggml.so libggml-base.so libggml-cpu.so`; CUDA variant adds **`libggml-cuda.so`**. Built with `-DGGML_NATIVE=OFF` (C1). |

**Simplifiers:** Piper's engine is **in-tree** (`src/tts/piper.cpp`, static, no `libpiper.so`; `CMakeLists.txt:339-342,1016`). AEC + Vosk are not in the server build.

### 2c. CUDA-only deps (CUDA variant) — corrected

The `dawn` binary itself links **no** CUDA lib on x86-64 (the cuSPARSE/cuSOLVER/cuRAND block at `CMakeLists.txt:1122-1131` is `PLATFORM==JETSON`-gated; x86 stays `AUTO`). The only CUDA consumer is `libggml-cuda.so`, whose NEEDED set is `libcudart.so.12`, `libcublas.so.12`(+cublasLt), `libcuda.so.1`.

- `Depends: cuda-cudart-12-6, libcublas-12-6` — **not** the `cuda-runtime-12-6` metapackage (drags in multi-GB).
- `libcuda.so.1` comes from the **driver**, cannot be a reliable `Depends:` → document **"NVIDIA driver ≥ 560 (CUDA 12.6)"** + a postinst `ldconfig -p | grep libcuda.so.1` warning.
- Document that NVIDIA's apt repo (`cuda-keyring`) must be configured first, and that Ubuntu multiverse `nvidia-cuda-toolkit` (12.0) is **not** a substitute.
- **No cuDNN dependency** — cuDNN is only an ORT-CUDA-EP dep, which we dropped (H3).

---

## 3. Packaged build configuration

Both variants build the **`server` preset** (`WEBUI=ON, AEC=OFF, SERVER_ONLY=ON, CODE_PROJECTS=off, VOSK=off, TUI=on`), with two mandatory additions:

- **`-DGGML_NATIVE=OFF`** (C1, **critical**). `whisper.cpp/ggml/CMakeLists.txt:97-115` defaults `GGML_NATIVE` ON ⇒ `-march=native`. A runner is an AVX-512 Xeon/EPYC, so the artifact **SIGILLs on any pre-Rocket-Lake Intel / pre-Zen4 AMD desktop**. `OFF` gives the AVX2/FMA/F16C baseline and (CUDA) the portable arch list `50/61/70/75/80-virtual 86/89-real` instead of `native` (which won't even configure on a GPU-less runner). **Document "x86-64-v3 (AVX2) required."** (True runtime CPU dispatch via `GGML_BACKEND_DL`+`GGML_CPU_ALL_VARIANTS` is possible but needs a one-line DAWN code change — `ggml_backend_load_all()` is not currently called — so it's out of scope here.)
- **A `deb` CMake preset** with `CMAKE_INSTALL_PREFIX=/opt/dawn` (H1) — do not reuse `server`, which `services/dawn-server/install.sh` relies on having a `/usr/local` RPATH.

Excluded by the preset: AEC off ⇒ no webrtc-audio-processing / Abseil; VOSK off ⇒ no libvosk, and whisper.cpp **is** linked.

CUDA delta: base image `nvidia/cuda:12.6.3-devel-ubuntu24.04`; `nvcc` auto-detected ⇒ `GGML_CUDA=ON`. ONNX stays the **CPU prebuilt tarball** (H3).

---

## 4. Build mechanism — `fpm` wrapping a noble container build

**`fpm`, not a hand-authored `debian/` tree** — the Dockerfiles are already tested dependency recipes; a `debian/` tree is ~3× the work and still bundles the same four libs.

**Build on `ubuntu:24.04`, not `debian:bookworm-slim`** — a bookworm binary won't satisfy a noble `.deb`'s `Depends:` (sonames + C++ ABI differ; libspdlog 1.10→1.12 is the clearest; the concrete floor is glibc ≥ 2.38 / libstdc++ from GCC 13). Reuse the Dockerfile *recipes* (same pinned commits, same ONNX tarball) on a noble base.

Flow (`packaging/deb/build-deb.sh`):
1. In `ubuntu:24.04`: apt the `-dev` deps (`deps.sh`), run the from-source dep builds (§2b, espeak `--prefix=/opt/dawn`), `cmake --preset deb -DGGML_NATIVE=OFF -DGIT_SHA=… && make`, `cmake --install … --prefix /opt/dawn`.
2. **RPATH fix (H1):** `CMakeLists.txt:22` bakes `CMAKE_INSTALL_RPATH` at *configure* time to the prefix's libdir; `--install --prefix` does not re-evaluate it, so the binary's RUNPATH would point at `/usr/local/lib` (confirmed via `readelf -d`). Either build with the `deb` prefix so RPATH is `/opt/dawn/lib`, or `patchelf --set-rpath '$ORIGIN'` the binaries and `$ORIGIN` the bundled `.so`s in staging. **No `ld.so.conf.d` entry.** Smoke test asserts `readelf -d` RUNPATH + `ldd` resolve only to `/opt/dawn/lib` or `/usr/lib`.
3. Stage the tree (§5), generate `Depends:` via `dpkg-shlibdeps` (§2a), `fpm -s dir -t deb --deb-no-default-config-files …`.

**Package version (H8 — must be monotonic):** `v*` tag builds → plain `X.Y.Z` from the tag (as `release.yml` does for the satellite). `workflow_dispatch` builds → `2.0.0+git<YYYYMMDD>.<GITHUB_RUN_NUMBER>.<sha>`. Never bare `+git<sha>` — hex sorts lexically, so dpkg would read a newer build as a downgrade. `VERSION_NUMBER` is fixed at `include/version.h:26`; `GIT_SHA` via `-DGIT_SHA`.

---

## 5. Install layout, config, RUNPATH, data paths

**Prefix `/opt/dawn`** (self-contained, matches Docker). `dawn`+`dawn-admin` in `/opt/dawn/bin`; bundled libs `/opt/dawn/lib` resolved via `$ORIGIN` RUNPATH (H1); `espeak-ng-data` in `/opt/dawn/share` (H2 — the path is compiled into libespeak-ng at `--prefix`; if it's not where the lib expects, `espeak_Initialize` fails and TTS is *silently* disabled, `text_to_speech.cpp:734`; belt-and-braces `Environment=ESPEAK_DATA_PATH=/opt/dawn/share` in the unit). **PATH:** ship `/usr/bin/dawn` + `/usr/bin/dawn-admin` symlinks (the Chrome `/opt` precedent) — nothing else puts `/opt/dawn/bin` on PATH.

**Config in `/etc/dawn/`, NOT conffiles (H7, critical).** "Install-from-example-if-absent" and "conffile" are mutually exclusive, and the daemon **rewrites its own config**: `config_env.c` truncate-writes `dawn.toml` on every WebUI settings save + `.bak` beside it, so it needs write on both files *and* the dir. A root:root 0644 conffile ⇒ saves fail EACCES, `secrets.toml` world-readable, and every save marks the conffile "locally modified" ⇒ perpetual upgrade prompts. Instead: ship templates under `/opt/dawn/share/dawn/`; postinst copies if absent, `chown dawn:dawn`, `chmod 640 dawn.toml` / `600 secrets.toml`, dir `750`. Always pre-create `secrets.toml` (else a WebUI secrets save writes `/var/lib/dawn/secrets.toml`, search-priority 1, shadowing `/etc/dawn/secrets.toml`).

**Config-filename quirk:** the system search filename is `config.toml`, not `dawn.toml` (`config_parser.c:2309-2368`). The unit sidesteps it: `--config /etc/dawn/dawn.toml` explicitly.

**Absolute paths in the packaged config (M1) — runtime assets are CWD-relative by default and several have no config knob.** Ship a config *derived from `dawn.toml.example`* (not `services/dawn-server/dawn.toml`, which has stale keys — L4) with absolute: `www_path`, `[sfx] sound_path` (→ shipped `sound_assets/`), `data_dir="/var/lib/dawn/db"` (default is `~/.local/share/dawn` — `config_defaults.c:551` — so without this DBs land in `/var/lib/dawn/.local/share/dawn/`), `tts/asr models_path`. `tool_instructions` has **no knob** (`instruction_loader.c:118`) ⇒ symlink `/var/lib/dawn/tool_instructions → /opt/dawn/tool_instructions`. `models.toml` shipped to `/etc/dawn/`.

**Systemd unit (packaged copy — not the repo's `/usr/local`-hardcoded one):**
- `ExecStart=/opt/dawn/bin/dawn --config /etc/dawn/dawn.toml`, `WorkingDirectory=/var/lib/dawn`
- `User=dawn Group=dawn`; **no `SupplementaryGroups`** (M3 — SERVER_ONLY has no local audio/GPU; `render` doesn't exist on minimal installs and makes systemd refuse to start; CUDA `/dev/nvidia*` is 0666 via NVIDIA udev)
- `After=network-online.target`, `Wants=network-online.target`; `Wants=mosquitto.service` only as a soft start
- journald logging (drop `StandardOutput=append:` — M7: logrotate `create`+`HUP` can't rotate an inherited systemd fd)
- **Not auto-started on install**; enabled only.

---

## 6. Maintainer scripts

**`preinst` (M4 — coexistence with a prior source install):** detect `/usr/local/bin/dawn` or `/etc/systemd/system/dawn-server.service` and refuse/warn with the migration command (`services/dawn-server/install.sh --uninstall`) — otherwise the old unit (pointing at `/usr/local/bin/dawn`), a stale `/etc/ld.so.conf.d/dawn.conf`, a real `/var/lib/dawn/www` dir, and a `secrets.toml` symlink all fight the package.

**`postinst configure`:**
- Create `dawn` (`adduser --system --group --home /var/lib/dawn --no-create-home`). No supplementary groups; if any are ever added, guard with `getent group` (M3).
- `install -d -o dawn -g dawn` the specific dirs `/var/lib/dawn/{db,models,ota,source}` (M2/L2 — not a recursive `chown -R` every configure: slow + clobbers OTA-key/bind-mount ownership).
- Copy `/etc/dawn/{dawn.toml,secrets.toml,models.toml}` from templates if absent, with the perms in §5.
- Symlink the shipped Piper voice + Silero VAD into `/var/lib/dawn/models/` (staged from the **host checkout** — `.dockerignore` excludes `models/`, so they can't come from inside the build container; `scan_models_directory` + `is_path_within_www` accept symlinks safely). Symlink `tool_instructions`.
- **Restart-on-upgrade (H6):** fresh install (`$2` empty) → enable, don't start. Upgrade (`$2` set) → `deb-systemd-invoke restart` iff it was active (recorded in `prerm upgrade`). Without this, every upgrade leaves the daemon stopped.
- Print a one-line "run `dawn-fetch-models` (or start `dawn-models.service`)" notice — **no network fetch in postinst** (M2: hangs under the dpkg lock, ignores apt proxy, breaks the CI smoke).

**Model fetch (M2):** ship `dawn-fetch-models` (pinned HF *revision* URLs + sha256; writes `/var/lib/dawn/models/whisper.cpp/ggml-base.en.bin` and `models/embeddings/{bge-small-en-v1.5-int8.onnx, vocab.txt}` — **two** bge files, not one) run as `dawn` via a oneshot `dawn-models.service` (`ConditionPathExists=!…ggml-base.en.bin`, `Before=dawn.service`). Do **not** wrap `setup_models.sh` — it targets `$PROJECT_ROOT/models`, symlinks into the submodule, and has no checksums.

**`prerm`:** `deb-systemd-invoke stop` on remove; on `upgrade` record `is-active` for H6.
**`postrm remove`:** disable + `daemon-reload`; **preserve `/var/lib/dawn`** (auth.db, stat.db, OTA keys — never-delete-data culture).
**`postrm purge`:** remove `/etc/dawn`, and — given the data culture — **leave `/var/lib/dawn` with a printed notice** rather than `rm -rf` (guard against it being a mountpoint if removed). `deluser --system`, never raw `userdel` (L1).

---

## 7. Runtime data — ship vs. fetch

| Data | In `.deb`? | Notes |
|---|---|---|
| Piper voice `en_GB-alba-medium.onnx` | **Ship** (git-committed) | staged from host; symlinked into `/var/lib/dawn/models/` |
| Silero VAD `silero_vad_16k_op15.onnx` | **Ship** (git-committed) | hardcoded path `webui_always_on.c:495` — symlink into place |
| `espeak-ng-data/` | **Ship** → `/opt/dawn/share` | H2 |
| `www/`, `tool_instructions/`, `sound_assets/`, `commands_config_nuevo.json` | **Ship** | `sound_assets/` was missing from the first draft |
| Model license files (`models/PIPER_ALBA_LICENSE`, `models/SILERO_VAD_LICENSE`) | **Ship** → `/usr/share/doc/dawn/` | M8 |
| Whisper ggml base (~150 MB) | **Fetch** (`dawn-models.service`) | mutable HF URL — pin revision + sha256 |
| bge embedding (`bge-small-en-v1.5-int8.onnx` **+ `vocab.txt`**) | **Fetch** | two files |
| Vosk model | **Omit** | satellite/legacy only |

---

## 8. GitHub Actions — `.github/workflows/package-deb.yml`

- Runner `ubuntu-24.04`. Triggers: `v*` tag + `workflow_dispatch`. `actions/checkout` with `submodules: true` (whisper.cpp).
- **CPU job:** build **in a `ubuntu:24.04` container** (deterministic sonames), `-DGGML_NATIVE=OFF`, `dpkg-shlibdeps`-generated deps, `fpm`. Cache the from-source dep layers (keyed on pinned commits/versions) — ONNX tarball + espeak + piper-phonemize are the wall-clock, not the DAWN compile.
- **Smoke test (M10 — `--help` alone is too weak; it misses H2/M1/M3/M4).** In a clean noble container: install the `.deb`, then (1) `ldd /opt/dawn/bin/dawn | grep -c "not found"` == 0 + assert `$ORIGIN` RUNPATH; (2) boot the daemon with the packaged config ~15 s, require the "Loaded TTS voice model" / WebUI-listening lines + `dawn-admin ping`; (3) `lintian --fail-on error`; (4) **install → reinstall → upgrade (bumped version) → remove → purge**, asserting `/etc/dawn/*.toml` + `/var/lib/dawn/db/` survive remove and the service runs after upgrade. Upgrade testing is **phase 1**, not phase 3 — H6/H7 live exactly there.
- Upload `.deb` + `SHA256SUMS` as release assets.

**CUDA job (phase 2):** base `nvidia/cuda:12.6.3-devel-ubuntu24.04`, `GGML_CUDA=ON`, **CPU ONNX tarball** (H3). `Depends:` per §2c. **Always in-container** (M9 — the runner's preinstalled CMake 3.31 is fine for a CPU-ONNX build; if ORT-from-source were ever reintroduced it needs CMake ≥3.28 **&<3.31**). Runners compile + `--help`/`ldd`; **GPU inference is tested on the developer's NVIDIA laptop** before release. Note noble GCC 13 is within ONNX's ≤13 if ever needed.

---

## 9. Licensing

- DAWN **GPLv3** → corresponding-source obligation, satisfied by the public repo; record `Homepage` + `Vcs-Git` in metadata.
- **Static MuPDF (AGPL-3):** Debian Policy 7.8 → `Built-Using: mupdf (= 1.23.10+ds1-1build3)` (`fpm --deb-field`); `copyright` carries Artifex's AGPL notice. **AGPL §13 note for operators:** if someone modifies the network-served combination, AGPL applies to the whole — MuPDF cannot be dropped at runtime.
- **GPLv3 §6(d):** relying on rhasspy's GitHub for the espeak-ng (GPL-3) + piper-phonemize forks is thin — attach `espeak-ng-<commit>.tar.gz` + `piper-phonemize-<commit>.tar.gz` as release assets beside the `.deb` to fully discharge it.
- `copyright` also enumerates: ORT `ThirdPartyNotices.txt` (MIT), whisper/ggml (MIT), `www/js/{marked,purify}.min.js`, `www/js/vendor/{chart.umd.js,qrcode-generator}`, OFL fonts, model licenses.
- **CUDA/cuDNN never redistributed** (NVIDIA EULA) → `Depends:` only. Correct as designed.

---

## 10. Package variants must not collide (H5)

`dawn` and `dawn-cuda` install identical paths ⇒ dpkg refuses the second ("trying to overwrite"). `dawn-cuda`: `Conflicts: dawn`, `Replaces: dawn`, `Provides: dawn`; `dawn`: `Conflicts: dawn-cuda`.
**Alternative worth weighing** once the `GGML_BACKEND_DL` code change (C1) lands: one `dawn` package + a `dawn-ggml-cuda` backend package shipping only `libggml-cuda.so` + CUDA deps; ggml falls back to CPU if the dlopen fails — collapses the two-variant matrix into one base package.

---

## 11. Phasing

1. **CPU package, end-to-end** — `packaging/deb/` (`build-deb.sh`, `deb` preset, maintainer scripts, unit, `dawn-fetch-models`/`dawn-models.service`), `dpkg-shlibdeps` deps, packaged `dawn.toml`, the workflow **including the install→upgrade→purge smoke** (H6/H7/M10 live here). Prove the shape.
2. **CUDA variant** — same scaffolding: CUDA base image, `libggml-cuda.so`, NVIDIA `Depends:` (§2c), `Conflicts/Replaces` (§10); developer GPU-tests the artifact.
3. **(Later)** apt-repo hosting (PPA / self-hosted, signed) so `apt install dawn` works without a manual download; optional `dawn-ggml-cuda` backend split (§10); `-models` companion package vs. fetch service.

## 12. Out of scope here but surfaced (DAWN code changes, not packaging)

- **Runtime CPU dispatch** — `ggml_backend_load_all()` + `GGML_BACKEND_DL`/`GGML_CPU_ALL_VARIANTS` would let one build run on any x86-64 baseline (removes the AVX2 floor). Not currently called by `whisper.cpp`/`asr_whisper.c`.
- **GPU TTS** — Piper's CUDA EP (`piper.cpp:260-264`) is passed a literal `false` (`text_to_speech.cpp:730`); a `[tts] use_cuda` knob would need it *and* shipping `libonnxruntime_providers_cuda.so`/`_providers_shared.so` + cuDNN. Only then does an ORT-CUDA build earn its cost.
- **Pre-existing portability bug:** both `Dockerfile` and `Dockerfile.cuda` ship `-march=native` ggml today (C1) — the published Docker images have the same SIGILL-on-older-CPU exposure.

## 13. Open items to verify during build

- `dpkg-shlibdeps` output vs. the §2a table; MuPDF 1.23 compile check.
- `readelf`/`ldd` RUNPATH assertions actually pass with the chosen RPATH strategy.
- Whether to enable `DAWN_ENABLE_CODE_PROJECTS` (adds `libgit2-1.7` — apt-available on noble, cheap if wanted).
- Confirm the packaged `dawn.toml` keys match the current parser (use `dawn.toml.example`, not the stale `services/dawn-server/dawn.toml`).
