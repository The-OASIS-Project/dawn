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

### Moving from a source install

The package refuses to install beside a source install, and uninstalling one deletes its CA. First
copy your keys and the CA, so satellites and browsers that trust it keep working:

```bash
sudo install -d -m 750 -g dawn /etc/dawn /etc/dawn/ssl
sudo install -m 600 /usr/local/etc/dawn/secrets.toml /etc/dawn/
sudo install -m 644 /var/lib/dawn/ssl/ca.crt /etc/dawn/ssl/
sudo install -m 600 /dev/null /etc/dawn/ssl/ca.key
sudo openssl pkey -in /var/lib/dawn/ssl/ca.key -out /etc/dawn/ssl/ca.key   # asks the CA passphrase
```

The package renews DAWN's certificate on its own, so it keeps the CA key without a passphrase,
readable only by root.

Then run `sudo services/dawn-server/install.sh --uninstall` from your DAWN checkout, answering
**no** to both questions (databases, configuration). Keep the `dawn` user it suggests deleting: the
package uses it, and it owns your databases. Then install the package. It uses the same
`/var/lib/dawn/db`, so your conversations, memory and users carry over. Your other settings don't:
make them again in the WebUI. If your old `secrets.toml` had no `satellite_registration_key`, add
one (step 4).

## First steps

1. **Create the admin account** with the one-time setup token DAWN prints at its first start
   ([First admin account](GETTING_STARTED_SERVER.md#first-admin-account) has the details):
   - Get the token: `sudo journalctl -u dawn | grep 'Token:' | tail -1`. It's valid for 5 minutes;
     if it has expired, `sudo systemctl restart dawn` prints a new one.
   - `sudo dawn-admin user create <name> --admin`, paste the token when asked, and choose a password.
   - Sign in to the WebUI (step 3) with that name and password.

   `dawn-admin` needs `sudo`. To use it without, join the `dawn` group (`sudo adduser $USER dawn`);
   members can also read DAWN's configuration and data, so on a shared machine keep using `sudo`.
2. **Trust this host's CA**, `/etc/dawn/ssl/ca.crt`, on each device you use DAWN from:
   [Trusting the DAWN CA](GETTING_STARTED_SERVER.md#trusting-the-dawn-ca). `/etc/dawn` isn't readable
   by your user, so first make a copy you can hand out: `sudo cp /etc/dawn/ssl/ca.crt ~/dawn-ca.crt`.
3. **Open `https://<this-host>:3000`**, sign in, and add an LLM API key under Settings. Keys are
   saved in `/etc/dawn/secrets.toml`; to edit it by hand, see
   [secrets.toml](GETTING_STARTED_SERVER.md#secretstoml), then `sudo systemctl restart dawn`.
4. **Satellites** need this host's [registration key](DAP2_SATELLITE.md#configure-satellites),
   made at install: Settings → Satellite Management → **Pair new satellite** in the WebUI shows it,
   or `sudo grep satellite_registration_key /etc/dawn/secrets.toml`. If it's missing
   or commented out (a `secrets.toml` from a source install), make one with `openssl rand -hex 32`,
   set it as `satellite_registration_key` under `[secrets]`, and `sudo systemctl restart dawn`;
   until then any device on your network can register as a satellite.
5. **Web search** needs a search service, which the package doesn't include. Either run a
   [SearXNG container](../GETTING_STARTED.md#searxng-setup-for-web-search) (free, local; DAWN looks
   for it at `http://127.0.0.1:8384`), or use [Tavily](../GETTING_STARTED.md#tavily-commercial-search--url-extract)
   (a hosted API with a free tier): add your Tavily key with the API keys under Settings, and set
   Settings → Web Search → engine to `tavily`. Without either, every search fails.

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

- **Upgrade:** there's no apt repository yet, so `apt upgrade` doesn't update DAWN. Watch the
  [releases](https://github.com/The-OASIS-Project/dawn/releases) and install a newer package the same
  way: `sudo apt install ./dawn_<newer version>+ubuntu<release>_amd64.deb`. A running DAWN is
  restarted on the new version; your configuration and data stay. On 24.04 the PDF library (MuPDF)
  is built into DAWN, so its security fixes arrive with a DAWN release, not Ubuntu's updates.
- **Remove:** `sudo apt remove dawn` stops and removes DAWN but keeps `/etc/dawn` and `/var/lib/dawn`,
  so a reinstall picks up where you left off.
- **Purge:** `sudo apt purge dawn` also deletes the configuration, keys and CA in `/etc/dawn` (files
  you added there stay). It still keeps `/var/lib/dawn`; delete that by hand if you want your data
  gone.

## Problems

- **"this CPU lacks avx2"** at install: the package needs an x86-64-v3 CPU. Build from source instead.
- **"a source install of the DAWN daemon is present"**: move off it as in
  [Moving from a source install](#moving-from-a-source-install), then install again.
- **DAWN keeps restarting after the first start:** the model download probably failed (no network
  at first start), and DAWN can't start without its speech-recognition model.
  `sudo dawn-fetch-models` retries it; then `sudo systemctl restart dawn`.
- **The browser warns about the certificate:** the CA isn't trusted on that device yet (step 2), or
  you used an address the certificate doesn't cover. It covers this host's name and its addresses at
  start-up; a restart reissues it after they change.
- Anything else: `sudo journalctl -u dawn -b`, and
  [Troubleshooting](GETTING_STARTED_SERVER.md#troubleshooting).
