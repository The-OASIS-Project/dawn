# Ubuntu packages (`dawn_<version>+ubuntu<release>_amd64.deb`)

The daemon's CPU server build for Ubuntu 24.04 and 26.04 on x86-64, one package per release.
Installing it is covered in
[GETTING_STARTED_SERVER.md](../../docs/GETTING_STARTED_SERVER.md#ubuntu-package-deb); this is how
it is built.

```bash
git submodule update --init whisper.cpp
UBUNTU=24.04 packaging/deb/build-deb.sh [VERSION]   # or 26.04; needs docker
packaging/deb/check-deb.sh build-deb/dawn_<version>+ubuntu24.04_amd64.deb
```

`ci.yml` does both for each release on every PR; `release.yml` does them for every `v*` tag and publishes
the packages with the satellite's.

| File | Role |
|---|---|
| `build-deb.sh` | Builds the Dockerfile's `dawn-deps` stage on the chosen release (pinned digest) and runs `stage.sh` in it |
| `stage.sh` | Builds the `deb` CMake preset and lays out, links and packages the tree |
| `../deb_closure.sh` | Shared with the satellite package: apt-owned libraries become `Depends:`, source-built ones are bundled |
| `debian/` | Control template and maintainer scripts |
| `dawn.service`, `dawn-models.service` | The daemon, and the first-start model download it pulls in |
| `dawn-fetch-models` | Downloads the Whisper and embedding models (pinned revisions, SHA-256 checked) |
| `dawn-tls` | Before each start: this host's CA and the WebUI certificate, made or renewed as needed |
| `dawn.toml.in` | The package's changes from the defaults; postinst copies it (and `secrets.toml.example`, which sets nothing) to `/etc/dawn` when absent |
| `copyright` | Licenses of everything the package ships |
| `check-deb.sh` | Boots a systemd container of the package's release and runs `smoke-test.sh` in it, rebooting it halfway |
| `smoke-test.sh` | lintian; install (enabled, not started), start (models, certificate, daemon over HTTPS), restart, upgrade (restarts the daemon), reboot (comes up by itself), remove, purge |

Design choices worth knowing before changing anything:

- **Standard layout, not `/opt`.** Programs in `/usr/bin`, read-only data in `/usr/share/dawn`,
  configuration in `/etc/dawn`, state in `/var/lib/dawn` (FHS; `/opt` would need `/etc/opt` and
  `/var/opt`, and lintian rejects it outright).
- **Bundled libraries stay private.** ONNX Runtime, piper-phonemize, the rhasspy espeak-ng fork and
  whisper/ggml live in `/usr/lib/dawn`, found through a RUNPATH of `/usr/lib/dawn` (Policy's rule for
  private libraries), never an `ld.so.conf.d` entry, which would make the espeak-ng fork the
  system's copy. The fork's data is in `/usr/lib/dawn/share/espeak-ng-data`, the path compiled into it.
- **Configuration is not a conffile.** The daemon rewrites `dawn.toml` and `secrets.toml` on every
  WebUI settings save, so it owns them; postinst creates them only when absent.
- **ggml is built for x86-64-v3** (`GGML_NATIVE=OFF`), not the build machine's CPU; preinst
  refuses a CPU without AVX2 rather than letting the daemon die with SIGILL.
- **HTTPS by default.** Browsers allow the microphone only in a secure context, so remote WebUI
  voice needs it. `dawn-tls` makes a per-host CA once and keeps a CA-signed server certificate
  current (renewed before expiry, reissued when the host's name or addresses change); a
  certificate the CA did not sign is the operator's and is left alone.
- **No network in maintainer scripts.** Models too large to ship are fetched by
  `dawn-models.service` before the daemon's first start.
- **Purge keeps `/var/lib/dawn`** (databases, downloaded models) and the `dawn` user that owns it.
