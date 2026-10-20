# Installing the Ubuntu package

DAWN's server build as a `.deb` for **Ubuntu 24.04 or 26.04 on x86-64**: no compiling. It is the
CPU build in server mode: you talk to it from the WebUI or a satellite, not a local microphone. It
needs an x86-64-v3 (AVX2) CPU. For a Jetson, a GPU build or local audio, build from source
([GETTING_STARTED.md](../GETTING_STARTED.md), [GETTING_STARTED_SERVER.md](GETTING_STARTED_SERVER.md)).

## Install

From the [latest release](https://github.com/The-OASIS-Project/dawn/releases/latest), download the
package for your Ubuntu release and `SHA256SUMS`, then:

```bash
sha256sum --check --ignore-missing SHA256SUMS
sudo apt install ./dawn_<version>+ubuntu24.04_amd64.deb   # or +ubuntu26.04
sudo systemctl start dawn
```

The install enables the service but doesn't start it. The first start takes a minute or two: it
downloads the speech-recognition and search models (about 180 MB) and creates this host's CA and
the WebUI's HTTPS certificate. After that, DAWN starts at every boot.

Remove a source install first: `sudo services/dawn-server/install.sh --uninstall` from your DAWN
checkout, answering **no** when it asks to remove the databases. The package uses the same
`/var/lib/dawn/db`, so your conversations, memory and users carry over; API keys don't (they were in
`/usr/local/etc/dawn/secrets.toml`). The package refuses to install beside a source install.

## First steps

1. **Create the admin account** as in
   [First admin account](GETTING_STARTED_SERVER.md#first-admin-account). With the package, the setup
   token is in the journal, `sudo journalctl -u dawn | grep 'Token:' | tail -1`, and `dawn-admin` is on
   your `PATH` but needs `sudo`: `sudo dawn-admin user create <name> --admin`. To use `dawn-admin`
   without `sudo`, join the `dawn` group (`sudo adduser $USER dawn`); members can also read DAWN's
   configuration and data, so on a shared machine keep using `sudo`.
2. **Trust this host's CA**, `/etc/dawn/ssl/ca.crt`, on each device you use DAWN from:
   [Trusting the DAWN CA](GETTING_STARTED_SERVER.md#trusting-the-dawn-ca).
3. **Open `https://<this-host>:3000`**, sign in, and add an LLM API key under Settings. Keys are
   saved in `/etc/dawn/secrets.toml`; to edit it by hand, see
   [secrets.toml](GETTING_STARTED_SERVER.md#secretstoml), then `sudo systemctl restart dawn`.
4. **Satellites:** until you set a [registration key](DAP2_SATELLITE.md#registration-key), any device
   on your network can register as one. Make one with `openssl rand -hex 32`, set it as
   `satellite_registration_key` under `[secrets]` in `/etc/dawn/secrets.toml`, and
   `sudo systemctl restart dawn`.

## Where things are

| Path | What |
|---|---|
| `/etc/dawn/dawn.toml` | Configuration. Every option: `/usr/share/doc/dawn/dawn.toml.example` |
| `/etc/dawn/secrets.toml` | API keys and credentials (readable only by the `dawn` user) |
| `/etc/dawn/ssl/` | This host's CA (`ca.crt`) and the WebUI certificate, renewed automatically |
| `/var/lib/dawn/` | Databases (`db/`), models, everything DAWN learns |
| `/usr/lib/dawn/`, `/usr/share/dawn/` | DAWN's own libraries and files; don't edit |

The WebUI's settings panel rewrites `dawn.toml` when you save. DAWN runs as the `dawn` user;
`sudo systemctl status dawn` and `sudo journalctl -u dawn` show what it's doing.

## Upgrade and remove

- **Upgrade:** `sudo apt install ./dawn_<newer version>+ubuntu<release>_amd64.deb`. A running DAWN is
  restarted on the new version; your configuration and data stay.
- **Remove:** `sudo apt remove dawn` stops and removes DAWN but keeps `/etc/dawn` and `/var/lib/dawn`,
  so a reinstall picks up where you left off.
- **Purge:** `sudo apt purge dawn` also deletes `/etc/dawn` (configuration, keys, the CA). It still
  keeps `/var/lib/dawn`; delete that by hand if you want your data gone.

## Problems

- **"this CPU lacks avx2"** at install: the package needs an x86-64-v3 CPU. Build from source instead.
- **"a source install of the DAWN daemon is present"**: remove it as in *Install*, then install again.
- **Speech recognition or search doesn't work:** the model download may have failed (no network at
  first start). `sudo dawn-fetch-models` retries it; then `sudo systemctl restart dawn`.
- **The browser warns about the certificate:** the CA isn't trusted on that device yet (step 2), or
  you used an address the certificate doesn't cover. It covers this host's name and its addresses at
  start-up; a restart reissues it after they change.
- Anything else: `sudo journalctl -u dawn -b`, and
  [Troubleshooting](GETTING_STARTED_SERVER.md#troubleshooting).
