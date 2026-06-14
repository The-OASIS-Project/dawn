# Codebase Memory MCP (cbm-mcp) as a Systemd Service

Runs [codebase-memory-mcp](https://github.com/DeusData/codebase-memory-mcp) (**cbm**)
— a codebase knowledge-graph server — as a managed service that DAWN's coding
harness uses to index and query imported repositories.

## How it works (read this first)

cbm speaks MCP **only over stdio**, and its agents are expected to launch it as a
child process. DAWN's MCP bridge does the opposite: it's an **HTTP+SSE client**
that connects to an already-running server URL, and DAWN **never spawns
subprocesses** (a hard project invariant). Those two don't connect directly, so
we put a thin, off-the-shelf adapter between them:

```
   ┌─────────┐   SSE over HTTP        ┌──────────────┐   stdin/stdout pipe   ┌─────────┐
   │  DAWN   │ ───── (a URL) ───────► │  mcp-proxy   │ ───── (stdio MCP) ───► │   cbm   │
   │ bridge  │ ◄───────────────────── │  (adapter)   │ ◄──────────────────── │ (stdio) │
   └─────────┘                        └──────────────┘                       └─────────┘
     speaks SSE only                    translates SSE ⇄ stdio                speaks stdio only
     never spawns anything              owns the cbm child                    launched BY mcp-proxy
```

`mcp-proxy` owns the cbm child process, so DAWN's no-subprocess rule is honored:
the **operator's** service launches cbm, not DAWN. This service unit is just
"`mcp-proxy` wrapping `cbm`," supervised by systemd.

## What gets installed

| Path | What |
|------|------|
| `/usr/local/bin/codebase-memory-mcp` | the cbm binary (built from source) |
| `/usr/local/lib/cbm-mcp/venv/` | dedicated venv holding `mcp-proxy` |
| `/usr/local/etc/cbm-mcp/cbm-mcp.conf` | service configuration (EnvironmentFile) |
| `/etc/systemd/system/cbm-mcp.service` | the unit |
| `/etc/logrotate.d/cbm-mcp` | log rotation for `/var/log/dawn/cbm-mcp.log` |
| `/var/lib/dawn/cbm-cache/` | cbm's graph database (SQLite) |

Runs as the **`dawn`** user so it can read DAWN's clone tree
(`/var/lib/dawn/source`) and write its cache under `/var/lib/dawn`.

## Prerequisites

- A cbm source checkout (default location `~/code/codebase-memory-mcp`):
  ```bash
  git clone https://github.com/DeusData/codebase-memory-mcp.git ~/code/codebase-memory-mcp
  ```
- Build deps: `sudo apt install build-essential zlib1g-dev`
- Python venv support: `sudo apt install python3-venv`

> **Why libgit2 is disabled in the build.** cbm has an *optional* libgit2
> fast-path for git-history parsing, auto-enabled when pkg-config finds libgit2.
> Its `cbm.c` includes `<git2.h>` expecting the `git_allocator` type, but in
> modern libgit2 that type lives in `<git2/sys/alloc.h>`, so the build fails on
> any box that has libgit2 installed (DAWN ships one in `/usr/local`). cbm falls
> back to shelling out to `git log` when libgit2 is absent — functionally
> complete, just not the fast path. The installer therefore builds with
> `LIBGIT2_FLAGS= LIBGIT2_LIBS=`. (Worth an upstream one-line fix: add
> `#include <git2/sys/alloc.h>` to `cbm.c`.)

## Quick start

```bash
cd services/cbm-mcp
sudo ./install.sh
```

This builds cbm (if not already built), installs `mcp-proxy` into a venv, creates
the user/dirs, installs and starts the service, and prints the `dawn.toml` block
to add. Override the bind with `--port`/`--host` and the source dir with
`--cbm-src`.

## DAWN configuration

Add to `dawn.toml` (or use the WebUI → Settings → MCP Bridge panel). The alias
**must be exactly `cbm`** — DAWN's code-graph provider keys on that name.

```toml
[mcp]
enabled = true
dev_mode = true            # required to allow tls_verify = false on plain-http localhost

[[mcp.server]]
alias = "cbm"
url = "http://127.0.0.1:9750/sse"
transport = "http+sse"
enabled = true
capabilities = "dangerous"   # cbm needs index_repository / delete_project
tls_verify = false

[code_projects]
enabled = true
```

Restart DAWN (or save settings — the bridge re-reads config) and verify:

```bash
dawn-admin mcp status        # expect: cbm: connected, N tool(s)
```

Then import a repo from the WebUI **Coding** popover and watch it go
`cloning → indexing → ready`.

## Manual installation

If you prefer not to run `install.sh`:

```bash
# 1. Build cbm (libgit2 off)
cd ~/code/codebase-memory-mcp
scripts/build.sh LIBGIT2_FLAGS= LIBGIT2_LIBS=
sudo install -m 755 build/c/codebase-memory-mcp /usr/local/bin/codebase-memory-mcp

# 2. mcp-proxy in a venv
sudo python3 -m venv /usr/local/lib/cbm-mcp/venv
sudo /usr/local/lib/cbm-mcp/venv/bin/pip install mcp-proxy

# 3. dirs (dawn user must already exist, or: sudo useradd --system --no-create-home dawn)
sudo mkdir -p /var/lib/dawn/cbm-cache /var/log/dawn /usr/local/etc/cbm-mcp
sudo chown dawn:dawn /var/lib/dawn /var/lib/dawn/cbm-cache /var/log/dawn

# 4. config + unit + logrotate
sudo cp cbm-mcp.conf      /usr/local/etc/cbm-mcp/
sudo cp cbm-mcp.service   /etc/systemd/system/
sudo cp cbm-mcp-logrotate /etc/logrotate.d/cbm-mcp

# 5. enable
sudo systemctl daemon-reload
sudo systemctl enable --now cbm-mcp
```

## Configuration reference (`cbm-mcp.conf`)

| Key | Default | Notes |
|-----|---------|-------|
| `MCP_PROXY` | `/usr/local/lib/cbm-mcp/venv/bin/mcp-proxy` | adapter executable |
| `CBM_BIN` | `/usr/local/bin/codebase-memory-mcp` | cbm binary |
| `HOST` | `127.0.0.1` | **localhost only** — cbm has no auth; never expose off-box |
| `PORT` | `9750` | SSE port; DAWN connects to `http://HOST:PORT/sse` |
| `CBM_CACHE_DIR` | `/var/lib/dawn/cbm-cache` | cbm's graph DB (must be in `ReadWritePaths`) |
| `CBM_LOG_LEVEL` | `info` | `debug` / `info` / `warn` / `error` / `none` |
| `CBM_WORKERS` | *(blank)* | cap parallel-indexing workers; blank = auto-detect |

## Service management

```bash
systemctl status cbm-mcp
journalctl -u cbm-mcp -f          # or: tail -f /var/log/dawn/cbm-mcp.log
systemctl restart cbm-mcp          # after editing cbm-mcp.conf
systemctl stop cbm-mcp
```

## Verification

```bash
# SSE endpoint is up and emits the handshake DAWN waits for:
curl -sN --max-time 3 http://127.0.0.1:9750/sse | head -2
#   event: endpoint
#   data: /messages/?session_id=...

# cbm binary itself speaks MCP over stdio:
echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"t","version":"1"}}}' \
  | /usr/local/bin/codebase-memory-mcp
```

## Troubleshooting

| Problem | Fix |
|---------|-----|
| `dawn-admin mcp status` shows `cbm` disconnected | Is the service up (`systemctl status cbm-mcp`)? Does `dawn.toml` `[[mcp.server]]` `url` match `HOST:PORT`? Is `[mcp] enabled = true`? |
| Service won't start | `journalctl -u cbm-mcp -e`. Common causes: port already in use, `MCP_PROXY`/`CBM_BIN` path wrong. |
| Imports reach `error: …no code server connected` in DAWN | The bridge isn't connected — see the row above. cbm-mcp must be running *and* configured in `dawn.toml`. |
| Indexing fails but the server is connected | Check `/var/log/dawn/cbm-mcp.log`. Ensure the `dawn` user can read `/var/lib/dawn/source` and write `CBM_CACHE_DIR`. |
| Port conflict | Change `PORT` in `cbm-mcp.conf` **and** the `url` in `dawn.toml`, then restart both. |

## Files

```
services/cbm-mcp/
├── cbm-mcp.service       # systemd unit (mcp-proxy + cbm, runs as dawn)
├── cbm-mcp.conf          # EnvironmentFile (paths, bind, cache, log level)
├── cbm-mcp-logrotate     # /var/log/dawn/cbm-mcp.log rotation (copytruncate)
├── install.sh            # build + install + enable
└── README.md             # this file
```
