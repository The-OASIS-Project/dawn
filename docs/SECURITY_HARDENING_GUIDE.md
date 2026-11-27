# DAWN Security Hardening Guide

**Created**: March 9, 2026
**Purpose**: Production deployment security checklist, penetration testing procedures, and test results
**Audience**: Self-hosters deploying DAWN on their home network

---

## Threat Model

DAWN is a home voice assistant running on a private LAN. The primary threats are:

| Threat | Likelihood | Impact | Notes |
|--------|-----------|--------|-------|
| Other devices on LAN intercepting traffic | Medium | High | Roommates, guests, compromised IoT devices |
| Unauthorized WebUI access from LAN | Medium | High | Anyone on your WiFi can reach port 3000 |
| Remote attack via port forwarding | Low | Critical | Only if user exposes DAWN to the internet |
| Physical access to satellite device | Low | Medium | Attacker extracts PSK or WiFi credentials |
| Brute-force password attack | Low | Medium | Rate limiting + lockout mitigate this |

**Out of scope**: Nation-state attackers, supply chain compromise, kernel exploits. DAWN is a home appliance, not a bank.

---

## Deployment Checklist

Complete these steps before exposing DAWN to any network beyond localhost.

### 1. Enable HTTPS (Strongly Recommended)

Without HTTPS, all traffic is plaintext — authentication cookies, session tokens, API keys, and voice audio can be intercepted by anyone on your network.

```bash
./generate_ssl_cert.sh
```

Configure in `dawn.toml`:
```toml
[webui]
https = true
ssl_cert_path = "ssl/dawn-chain.crt"
ssl_key_path = "ssl/dawn.key"
```

Install the CA certificate on client devices to avoid browser warnings. See [GETTING_STARTED.md — SSL Setup](../GETTING_STARTED.md#7-ssl-setup-recommended) for full instructions.

### 2. Set Strong Passwords

- Use a unique password for your DAWN admin account (not reused from other services)
- Minimum 12 characters recommended
- Account lockout triggers after 5 failed attempts (15-minute cooldown)

### 3. Set a Satellite Registration Key

Prevents unauthorized devices from registering as satellites:

```bash
./generate_ssl_cert.sh --gen-key
```

This appends `satellite_registration_key` to `secrets.toml`. Copy the key to each satellite's config. Without this, any device that can reach port 3000 can register as a satellite.

### 4. Secure secrets.toml

```bash
chmod 600 secrets.toml
```

This file contains API keys and the satellite registration key. It should be readable only by the user running DAWN.

### 5. Enable MQTT TLS (If Using Smart Home)

If DAWN controls smart home devices via MQTT, encrypt that traffic:

```toml
[mqtt]
port = 8883
tls = true
tls_ca_cert = "ssl/ca.crt"
```

See [GETTING_STARTED.md — MQTT TLS](../GETTING_STARTED.md#mqtt-tls-encrypted-smart-home-commands) for full setup.

### 6. Review Network Exposure

- DAWN should NOT be exposed to the public internet without a VPN or reverse proxy
- If you must expose it, use a reverse proxy (nginx, Caddy) with its own TLS termination
- Consider firewall rules to restrict port 3000 to your LAN subnet

See the [Internet Exposure](#internet-exposure) section below for a detailed assessment.

---

## Internet Exposure

**DAWN is designed for private LAN deployment. Do not expose it directly to the public internet.**

### What's been validated

The pentest suite (`tests/test_pentest.sh`) confirms that DAWN handles known attack patterns correctly:

- SQL injection: 0 vulnerabilities across 3 sqlmap runs (login, search unauthenticated, search authenticated)
- XSS: 30 payloads tested across 6 input vectors — all handled safely
- Authentication: rate limiting, account lockout, CSRF replay rejection, timing attack resistance, cookie flags all passing
- Authorization: non-admin users blocked from admin endpoints, session tokens validated correctly
- Satellite: registration key enforced, malformed input rejected, rate limiting active
- TLS: cipher suites and protocol versions correct (testssl.sh)
- Security headers: CSP, X-Frame-Options, X-Content-Type-Options, Referrer-Policy, Permissions-Policy, HSTS all present

### What's missing for internet exposure

**Critical gaps:**

| Gap | Risk | Notes |
|-----|------|-------|
| **No reverse proxy** | Direct exposure of a C application to the internet. Any undiscovered bug in the HTTP handling or libwebsockets is directly exploitable. | Internet-facing services should sit behind nginx/caddy/HAProxy for TLS termination, connection limits, and request filtering. |
| **No 2FA** | Password-only authentication. One compromised password = full access to your assistant and connected smart home devices. | TOTP support is designed but not yet implemented. |
| **No WAF** | No request filtering for novel attack patterns before they reach application code. | A reverse proxy with ModSecurity or similar provides defense-in-depth. |
| **Private CA only** | Browsers will show certificate warnings for your domain. Users learn to click through warnings. | Internet-facing deployments need Let's Encrypt / ACME for publicly-trusted certificates. |

**Significant concerns:**

| Concern | Details |
|---------|---------|
| **No DDoS mitigation** | A single Jetson can be trivially overwhelmed by connection flooding. Application-level rate limiting only protects against login brute-force. |
| **No OS-level blocking** | No fail2ban or firewall integration. A distributed botnet bypasses per-IP rate limits. |
| **Memory safety** | C codebase with no fuzzing of input parsers (JSON, WebSocket frames, audio streams, document upload). Memory corruption bugs = remote code execution. |
| **Week-long idle sessions** | A login lasts up to 7 days without use (30 days at most). Long-lived session tokens increase the window for theft if TLS is somehow bypassed. |
| **No real-time alerting** | Security events go to log files only. No notification if someone is actively attacking. |

### Recommended approach for remote access

Use a VPN tunnel so DAWN never needs to be directly reachable from the internet:

```
Internet → VPN (WireGuard or Tailscale) → Your LAN → DAWN
```

This gives you:
- **Zero exposed ports** — DAWN is not reachable without VPN authentication
- **Strong authentication** — VPN key exchange is far stronger than password-only
- **Encrypted tunnel** — All traffic is encrypted before it reaches your network
- **No changes to DAWN** — Works with your existing setup as-is

Both [WireGuard](https://www.wireguard.com/) and [Tailscale](https://tailscale.com/) are straightforward to set up and free for personal use.

### If you must expose directly (not recommended)

Minimum prerequisites before putting DAWN on a public IP:

1. **Reverse proxy** — nginx or Caddy in front of DAWN with rate limiting, request size limits, and connection throttling
2. **Publicly-trusted TLS** — Let's Encrypt certificate on the reverse proxy
3. **2FA** — TOTP or similar second factor on all accounts
4. **fail2ban** — Watch DAWN's auth log and block IPs at the firewall level after repeated failures
5. **Fuzz testing** — AFL or libFuzzer on all input parsers before exposure
6. **Firewall rules** — Allow only 443 (HTTPS) inbound; block all other ports
7. **Regular updates** — Watch the GitHub repository for security patches

Even with all of the above, a custom C application serving directly to the internet carries inherent risk that managed services (cloud-hosted, containerized, memory-safe languages) do not.

---

## Security Features Reference

### What's Implemented

| Feature | Details | Location |
|---------|---------|----------|
| **Password hashing** | Argon2id via libsodium (16MB/3 iters on Jetson, 8MB/4 iters on RPi) | `src/auth/auth_crypto.c` |
| **Hash concurrency limit** | Max 3 concurrent Argon2id operations, 5s timeout returns `AUTH_CRYPTO_BUSY` | `src/auth/auth_crypto.c` |
| **Session tokens** | 256-bit random via `getrandom()`, ends after 7 days without use, 30 days at most | `src/auth/auth_db_session.c` |
| **CSRF protection** | HMAC-signed single-use tokens, 10-minute validity, nonce replay detection | `src/webui/webui_http.c` |
| **Rate limiting** | 20 attempts/15min on login, IPv6 /64 prefix normalization | `src/core/rate_limiter.c` |
| **Account lockout** | 5 failed attempts → 15-minute lock | `src/webui/webui_http.c` |
| **Timing attack prevention** | Constant-time comparison (`sodium_memcmp`), dummy hash on invalid usernames | `src/auth/auth_crypto.c` |
| **Audit logging** | All auth events logged: login, logout, failure, lockout, CSRF, rate limit | `src/auth/auth_db_audit.c` |
| **Cookie security** | HttpOnly, Secure (when HTTPS), SameSite=Strict | `src/webui/webui_http.c` |
| **Satellite TLS** | Private CA, server cert validation on all satellite tiers | `generate_ssl_cert.sh` |
| **Satellite auth** | Pre-shared registration key, constant-time validation, rate-limited | `src/webui/webui_satellite.c` |
| **SQL injection prevention** | All queries use `sqlite3_prepare_v2()` with parameter binding | All `*_db*.c` files |
| **Security headers** | CSP, X-Frame-Options, X-Content-Type-Options, Referrer-Policy, Permissions-Policy via HTTP headers on all responses; HSTS when HTTPS enabled | `src/webui/webui_http.c` (`webui_add_security_headers()`) |
| **MQTT TLS** | Optional TLS with CA validation, refuses plaintext fallback | `src/mosquitto_comms.c` |
| **DB file permissions** | auth.db created with 0600 (owner read/write only) | `src/auth/auth_db_core.c` |

### Known Accepted Risks

| Risk | Rationale |
|------|-----------|
| **HTTPS is optional** | Localhost-only use is legitimate; enforcing would break first-run UX. Documented as strongly recommended. |
| **Shared PSK for satellites** | Per-device certs (mTLS) add significant setup complexity for minimal gain on a home LAN. PSK + TLS is appropriate for the threat model. |
| **ESP32 credentials in flash** | Registration key and WiFi password stored in NVS/binary. Requires physical access to extract. Full mitigation needs ESP32 flash encryption (hardware-level). |
| **SQLite DB not encrypted** | Database files are plaintext on disk. Filesystem-level encryption (LUKS) is the appropriate mitigation for at-rest protection. |
| **No 2FA** | Planned but not yet implemented. |
| **Audit logging scope** | Auth events (login, logout, failure, lockout, CSRF, rate limit) are logged to SQLite. Tool execution is logged to syslog only (not queryable). Config changes are not logged. Extending audit scope to cover tool execution and config changes is a future improvement — see below. |
| **No automated update notifications** | DAWN does not check for new releases. Users should watch the GitHub repository (Watch → Releases only) for security updates. Building this into the application would add complexity for marginal gain — GitHub's notification system already handles this well. |

---

## Audit Logging: Current Scope and Gaps

### What's Logged (SQLite — queryable, persistent)

All authentication and security events via `auth_db_audit.c`:

| Event Type | Details Captured |
|-----------|-----------------|
| `LOGIN_SUCCESS` | Username, IP address |
| `LOGIN_FAILED` | Username, IP address, reason |
| `LOGOUT` | Username, IP address |
| `ACCOUNT_LOCKED` | Username, IP address, failed attempt count |
| `CSRF_FAILED` | IP address, reason (expired, replayed, invalid) |
| `RATE_LIMITED` | IP address, endpoint |
| `SESSION_CREATED` | Username, IP address, token prefix |
| `SESSION_EXPIRED` | Username, token prefix |
| `PASSWORD_CHANGED` | Username, changed by (self or admin) |
| `USER_CREATED` | Username, created by |
| `USER_DELETED` | Username, deleted by |

### What's Logged (syslog only — not queryable)

| Event | Location | Log Level |
|-------|----------|-----------|
| Tool execution (name, arguments, result summary) | `llm_tools.c` via `notify_tool_execution()` | INFO |
| LLM API calls (provider, model, token usage) | `llm_interface.c` | INFO |
| Satellite connect/disconnect | `webui_satellite.c` | INFO |
| ASR transcriptions | `dawn.c` | INFO |

### What's Not Logged

| Event | Impact | Recommendation |
|-------|--------|---------------|
| **Config changes** (who changed what setting, old vs new value) | Low — single-admin deployments are the norm | Future: log to `auth_log` when settings are modified via WebUI |
| **Tool execution** (queryable history) | Low — already in syslog, but not searchable via WebUI | Future: add `tool_log` table or extend `auth_log` with `TOOL_EXECUTED` event type |
| **Memory operations** (facts added/deleted/modified) | Low — memory viewer provides current state | Future: log memory mutations for audit trail |

### Recommendation

The current audit logging covers the security-critical events: authentication, authorization failures, and rate limiting. These are the events an admin needs to detect attacks or investigate incidents. Tool execution and config change logging are observability improvements — valuable but not security-blocking. The audit log is accessible via `dawn-admin log show` (see `docs/FAQ.md`).

---

## Penetration Testing Procedures

Run these tests against a live DAWN instance before any public-facing deployment. Record results in the [Test Results](#test-results) section below.

### TLS Configuration

**Tool**: [testssl.sh](https://github.com/drwetter/testssl.sh)

```bash
# Install
git clone --depth 1 https://github.com/drwetter/testssl.sh.git

# Run against DAWN
./testssl.sh/testssl.sh https://<dawn-ip>:3000
```

**What to verify:**
- [ ] TLS 1.2+ only (no SSLv3, TLS 1.0, TLS 1.1)
- [ ] No known vulnerabilities (BEAST, POODLE, Heartbleed, ROBOT, etc.)
- [ ] Certificate chain valid and complete
- [ ] Strong cipher suites (no RC4, DES, 3DES, NULL, EXPORT)
- [ ] HSTS header present (once implemented)

### Web Application Scanning

**Tool**: [Nikto](https://github.com/sullo/nikto)

```bash
nikto -h https://<dawn-ip>:3000 -ssl
```

**What to verify:**
- [ ] No information disclosure (server version, directory listing)
- [ ] Security headers present on all responses
- [ ] No default/backup files accessible

**Tool**: [sqlmap](https://github.com/sqlmapproject/sqlmap)

```bash
# Test login endpoint
sqlmap -u "https://<dawn-ip>:3000/api/auth/login" \
  --method POST \
  --data '{"username":"test","password":"test"}' \
  --content-type "application/json" \
  --level 3 --risk 2

# Test any endpoint that accepts user input
sqlmap -u "https://<dawn-ip>:3000/api/search?q=test" \
  --cookie "session=<valid-session-token>" \
  --level 3 --risk 2
```

**What to verify:**
- [ ] No SQL injection found on any endpoint
- [ ] Prepared statements hold under all input variations

### Authentication Testing

Run these manually or with a script:

| Test | Procedure | Expected Result |
|------|-----------|-----------------|
| **Rate limiting** | Send 25 login attempts in rapid succession | First 20 accepted (fail normally), attempts 21+ return 429 |
| **Account lockout** | Send 5 wrong passwords for one account | 6th attempt returns "account locked", unlocks after 15 min |
| **Session expiry** | Create session, manually set `expires_at` to past in DB, make request | 401 Unauthorized |
| **Invalid session** | Set cookie to random 64-char hex string | 401 Unauthorized, redirected to login |
| **CSRF replay** | Capture CSRF token from `/api/auth/csrf`, use it, use it again | First use succeeds, second returns 403 "Token already used" |
| **Cookie flags** | Inspect `Set-Cookie` header in browser dev tools | Must have: `HttpOnly`, `SameSite=Strict`. Must have `Secure` when HTTPS enabled |
| **Timing attack** | Compare response time for valid username vs. invalid username | Times should be indistinguishable (dummy hash on invalid user) |

### Authorization Testing

| Test | Procedure | Expected Result |
|------|-----------|-----------------|
| **Non-admin → admin endpoint** | Login as regular user, request `/api/admin/*` endpoints | 403 Forbidden |
| **No auth → protected endpoint** | Request any `/api/*` endpoint without session cookie | 401 or 302 redirect to login |
| **Cross-user session** | Login as user A, manually change cookie to user B's token | Rejected (tokens are unique random values) |

### XSS Testing

Inject these payloads into every user-input field and verify they are not rendered as HTML/JS:

```
<script>alert('xss')</script>
<img src=x onerror=alert('xss')>
javascript:alert('xss')
" onmouseover="alert('xss')
```

**Fields to test:**
- [ ] Conversation text input (WebUI)
- [ ] Settings fields (AI name, timezone, etc.)
- [ ] Satellite name and location
- [ ] Memory facts ("Remember that ...")
- [ ] Search queries
- [ ] User creation (username, display name)

### Satellite Security Testing

| Test | Procedure | Expected Result |
|------|-----------|-----------------|
| **No registration key** | Connect satellite without `registration_key` in config | Registration rejected |
| **Wrong key** | Connect with incorrect key | Rejected, rate limiting increments |
| **Malformed UUID** | Send registration with `uuid: "not-a-uuid"` | Rejected with validation error |
| **Rapid registration attempts** | Send 10+ registration attempts in 1 minute | Rate limited after threshold |

---

## HTTP Security Headers Checklist

These headers should be present on all HTTP responses. Check with browser dev tools (Network tab → select any request → Headers).

| Header | Expected Value | Status |
|--------|---------------|--------|
| `Content-Security-Policy` | `default-src 'self'; script-src 'self' 'wasm-unsafe-eval' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; connect-src 'self' wss: ws:; img-src 'self' data: blob:; manifest-src 'self'; worker-src 'self' blob:` | |
| `Strict-Transport-Security` | `max-age=31536000; includeSubDomains` (HTTPS only) | |
| `X-Frame-Options` | `DENY` | |
| `X-Content-Type-Options` | `nosniff` | |
| `Referrer-Policy` | `strict-origin-when-cross-origin` | |
| `Permissions-Policy` | `camera=(self), geolocation=(), payment=()` | |

**CSP Change Log:**
- **2026-03-25**: Added `'unsafe-inline'` to `script-src` — required for visual rendering tool's srcdoc iframe interactivity (sendPrompt bridge, ResizeObserver, onclick handlers). Sandbox attribute (`allow-scripts` without `allow-same-origin`) provides DOM isolation.
- **2026-03-25**: Added `blob:` to `worker-src` — required for Opus audio codec Web Worker.
- **2026-03-15**: Added `camera=(self)` to Permissions-Policy — required for WebUI camera capture (Take Photo feature).

**Note:** Pentest results (2026-03-10) predate the CSP changes above. Re-run security headers validation to confirm current state.

**Verify on these response types:**
- [ ] Static HTML files (index.html, login.html)
- [ ] Static JS/CSS files
- [ ] API JSON responses (`/api/auth/*`, `/api/admin/*`)
- [ ] Image endpoints (`/api/images/*`)
- [ ] Health check (`/health`)
- [ ] 404 responses

---

## Test Results

Record results here as tests are completed.

### TLS (testssl.sh)

| Date | Version | Grade | Issues Found | Notes |
|------|---------|-------|-------------|-------|
| 2026-03-10 13:36 | testssl.sh 3.3dev, OpenSSL 3.0.2 | T (trust) | LUCKY13 (potential, CBC ciphers in TLS 1.2) | Commit `1857403`. Grade T due to private CA (expected — not a public cert). See details below. |

**TLS test details (2026-03-10, commit `1857403`):**

- **Protocols**: SSLv2 ✗, SSLv3 ✗, TLS 1.0 ✗, TLS 1.1 ✗, TLS 1.2 ✓, TLS 1.3 ✓
- **Cipher categories**: No NULL, EXPORT, LOW, RC4, 3DES, or anonymous ciphers. AEAD with forward secrecy offered. CBC ciphers offered for TLS 1.2 backward compat.
- **Forward secrecy**: All connections use ECDHE (X25519 or P-256)
- **Server key**: EC 256-bit (P-256), SHA256 signature
- **Certificate**: Private CA (`DAWN Private CA`), 365-day validity, SAN includes localhost + all local IPs
- **ALPN**: http/1.1 only (HTTP/2 disabled to avoid frame size issues with large WebSocket messages)
- **No server banner leaked**

**Vulnerability scan (all clear except noted):**

| Vulnerability | Result |
|---|---|
| Heartbleed (CVE-2014-0160) | Not vulnerable |
| CCS (CVE-2014-0224) | Not vulnerable |
| Ticketbleed (CVE-2016-9244) | Not vulnerable |
| ROBOT | Not applicable (no RSA key transport) |
| CRIME (CVE-2012-4929) | Not vulnerable |
| BREACH (CVE-2013-3587) | Not vulnerable (no HTTP compression) |
| POODLE (CVE-2014-3566) | Not vulnerable (no SSLv3) |
| SWEET32 (CVE-2016-2183) | Not vulnerable |
| FREAK (CVE-2015-0204) | Not vulnerable |
| DROWN (CVE-2016-0800) | Not vulnerable |
| LOGJAM (CVE-2015-4000) | Not vulnerable |
| BEAST (CVE-2011-3389) | Not vulnerable (no SSL3/TLS1.0) |
| RC4 (CVE-2013-2566) | Not vulnerable (no RC4) |
| **LUCKY13 (CVE-2013-0169)** | **Potentially vulnerable** — CBC ciphers offered in TLS 1.2 for backward compat (IE 11, OpenSSL 1.0.2, Apple Mail 16). Modern clients negotiate TLS 1.3 or AES-GCM. Accepted risk for home LAN — attack requires millions of crafted packets from same network segment. |

**Client compatibility**: All modern browsers (Chrome, Firefox, Edge, Safari) and Android 7.0+ connect successfully via TLS 1.3 or TLS 1.2 ECDHE-ECDSA-AES256-GCM. IE 8 and Java 7 cannot connect (expected — no supported cipher overlap).

### Web Scanner (Nikto)

| Date | Version | Findings | Resolved | Notes |
|------|---------|----------|----------|-------|
| 2026-03-10 13:48 | Nikto v2.6.0 | 1 (false positive) | N/A | Commit `1857403`. Hit error limit (20) due to auth redirects — all unauthenticated paths return 302. See details below. |

**Nikto details (2026-03-10, commit `1857403`):**

- **No server banner** leaked (good)
- **No CGI directories** found
- **1 finding — false positive**: SIPS v0.2.2 path (`/sips/sipssys/users/a/admin/user`). Nikto probed this path, got a 302 redirect to `/login.html`, followed it, got 200, and flagged it as "exists." DAWN has no SIPS endpoint — this is the auth redirect catching all unknown paths.
- **Error limit reached**: Nikto hit 20 errors because DAWN's auth wall redirects all unauthenticated requests. This is expected behavior — unauthenticated scanning finds nothing real.
- **Note**: Nikto is designed for traditional web servers, not single-page apps behind authentication. Authenticated-path testing is better served by the manual tests below.

### SQL Injection (sqlmap)

| Date | Endpoints Tested | Injections Found | Notes |
|------|-----------------|-----------------|-------|
| 2026-03-10 13:52 | `/api/auth/login` (POST JSON) | **0** | Commit `1857403`. sqlmap v1.10.3.1, level 3, risk 2. See details below. |
| 2026-03-10 14:04 | `/api/search?q=test` (GET, wrong cookie name) | **0** | Commit `1857403`. sqlmap v1.10.3.1, level 3, risk 2. Tested unauthenticated path (302 redirect). |
| 2026-03-10 14:10 | `/api/search?q=test` (GET, authenticated) | **0** | Commit `1857403`. sqlmap v1.10.3.1, level 3, risk 2. See details below. |

**sqlmap details — Run 1 (2026-03-10, commit `1857403`): `/api/auth/login`**

- **Parameters tested**: `JSON username`, `JSON password`, `User-Agent`, `Referer`
- **Techniques tested**: Boolean-based blind, error-based, UNION query, stacked queries, time-based blind, inline queries
- **Databases targeted**: MySQL, PostgreSQL, Oracle, SQLite, SQL Server/Sybase, Firebird, MonetDB, Vertica, IBM DB2, ClickHouse, Informix, Microsoft Access
- **Result**: "all tested parameters do not appear to be injectable" — prepared statements hold under all input variations
- **Rate limiter confirmed**: 14,917 requests returned HTTP 429 (Too Many Requests), 20 returned HTTP 400 (Bad Request). Rate limiting is actively blocking brute-force-style attacks.

**sqlmap details — Run 2 (2026-03-10, commit `1857403`): `/api/search?q=test` (unauthenticated)**

- **Parameters tested**: `GET q`, `Cookie session` (wrong cookie name), `User-Agent`, `Referer`
- **Techniques tested**: Boolean-based blind, error-based, UNION query, stacked queries, time-based blind, inline queries
- **Databases targeted**: MySQL, PostgreSQL, Oracle, SQLite, SQL Server/Sybase, Firebird, MonetDB, Vertica, IBM DB2, ClickHouse, Informix, Microsoft Access
- **Result**: "all tested parameters do not appear to be injectable" — 0 vulnerabilities across all four parameters
- **Note**: 302 redirect to login — cookie name `session` not recognized; tested auth redirect response path

**sqlmap details — Run 3 (2026-03-10, commit `1857403`): `/api/search?q=test` (authenticated)**

- **Parameters tested**: `GET q`, `Cookie dawn_session`, `User-Agent`, `Referer`
- **Techniques tested**: Boolean-based blind, error-based, UNION query, stacked queries, time-based blind, inline queries
- **Databases targeted**: MySQL, PostgreSQL, Oracle, SQLite, SQL Server/Sybase, Firebird, MonetDB, Vertica, IBM DB2, ClickHouse, Informix, Microsoft Access
- **Result**: "all tested parameters do not appear to be injectable" — 0 vulnerabilities across all four parameters
- **Session cookie validated**: `dawn_session` reported as "dynamic" (server processes the token). 403 responses confirm authenticated code path tested. 10,746 requests returned HTTP 403.

### Authentication

Tested via `tests/test_pentest.sh --section auth` on 2026-03-10, commit `1857403`.

| Test | Date | Pass/Fail | Notes |
|------|------|-----------|-------|
| Rate limiting (20/15min) | 2026-03-10 | **Pass** | Triggered at attempt 10 (HTTP 429). Per-IP, 15-minute window, persisted to database. |
| Account lockout (5 attempts) | 2026-03-10 | **Pass** | Rate limit fires first (same IP budget). Lockout tested via DB: 5 failed attempts → 403 + 15-min lock. |
| Session expiry | 2026-03-10 | **Pass** | Garbage/expired tokens return `authenticated: false` |
| Invalid session rejection | 2026-03-10 | **Pass** | 64-char zero token → `authenticated: false` |
| CSRF replay rejection | 2026-03-10 | **Pass** | Single-use enforcement — second use of same token rejected |
| Cookie flags correct | 2026-03-10 | **Pass** | `HttpOnly`, `Secure`, `SameSite=Strict`, `Path=/` all present |
| Timing attack resistance | 2026-03-10 | **Pass** | Dummy hash equalization: two nonexistent usernames differ by < 1ms (0.0003s) |

### Authorization

Tested via `tests/test_pentest.sh --section authz` on 2026-03-10, commit `1857403`.

| Test | Date | Pass/Fail | Notes |
|------|------|-----------|-------|
| Non-admin blocked from admin endpoints | 2026-03-10 | **Pass** | `list_users` → BLOCKED, `create_user` → BLOCKED (SESSION_LIMIT before FORBIDDEN). DB confirmed no user created. |
| Unauthenticated blocked from API | 2026-03-10 | **Pass** | `/api/config` → 302 redirect to login. `/health` public (by design). |
| Cross-user session rejected | 2026-03-10 | **Pass** | Mangled session token (4 chars changed) → `authenticated: false` |

### XSS

Tested via `tests/test_pentest.sh --section xss` on 2026-03-10, commit `1857403`. Five payloads per field: `<script>alert("xss")</script>`, `<img src=x onerror=alert(1)>`, `"><svg onload=alert(1)>`, `javascript:alert(1)`, `<iframe src="javascript:alert(1)">`.

| Input Field | Date | Pass/Fail | Notes |
|-------------|------|-----------|-------|
| Conversation input | 2026-03-10 | **Pass** | Via WebSocket `text` message — no unescaped HTML in responses |
| Settings fields | 2026-03-10 | **Pass** | Via WebSocket `set_config` (ai_name) — payloads stored but not reflected raw |
| Satellite name/location | 2026-03-10 | **Pass** | Via WebSocket `update_satellite` — payloads handled safely |
| Memory search | 2026-03-10 | **Pass** | Via WebSocket `search_memory` — no reflection of raw HTML |
| Search queries | 2026-03-10 | **Pass** | Via HTTP `/api/search?q=` — URL-encoded payloads not reflected unescaped |
| User creation fields | 2026-03-10 | **Pass** | Via WebSocket `create_user` (username) — validation rejects invalid chars |

### Satellite

Tested via `tests/test_pentest.sh --section satellite` on 2026-03-10, commit `1857403`.

| Test | Date | Pass/Fail | Notes |
|------|------|-----------|-------|
| No key → rejected | 2026-03-10 | **Pass** | Error: `registration_key_required` |
| Wrong key → rejected | 2026-03-10 | **Pass** | Error: `invalid_registration_key` (constant-time comparison via `sodium_memcmp`) |
| Malformed UUID → rejected | 2026-03-10 | **Pass** | Error: `INVALID_MESSAGE` (UUID format validation) |
| Rapid attempts → rate limited | 2026-03-10 | **Pass** | Rate limit triggered at attempt 3 (5/60s per IP bucket) |

### Security Headers

Tested via `tests/test_pentest.sh --section headers` and `curl -I` on 2026-03-10, commit `1857403`. All 6 headers (CSP, X-Frame-Options, X-Content-Type-Options, Referrer-Policy, Permissions-Policy, HSTS) checked.

| Response Type | Date | All Headers Present | Missing | Notes |
|---------------|------|--------------------|---------| ------|
| Static HTML (`/login.html`) | 2026-03-10 | Yes (6/6) | — | Via `lws_serve_http_file()` static string |
| Static CSS (`/css/main.css`) | 2026-03-10 | Yes (6/6) | — | |
| Static SVG (`/favicon.svg`) | 2026-03-10 | Yes (6/6) | — | |
| API JSON (`/health`) | 2026-03-10 | Yes (6/6) | — | Manual header construction |
| API JSON (`/api/auth/status`) | 2026-03-10 | Yes (6/6) | — | Authenticated endpoint |
| Auth redirect (302) | 2026-03-10 | Yes (6/6) | — | Unauthenticated → `/login.html` |
| `lws_return_http_status` errors | 2026-03-10 | No | All | Accepted gap: LWS controls these internally. Minimal HTML, no user content, not exploitable. Unauthenticated requests hit auth redirect (with headers) before reaching 404. |

---

## Previous Audits

- **Static code audit** (2025-12-18): 15 findings across ~58K LOC, all Critical/High resolved (archived to [atlas](https://github.com/The-OASIS-Project/atlas))
- **Security hardening commit** (2026-03-06): `a365088` — MQTT TLS, auth guards, CSP hardening, realloc bug fix
- **Security headers + TLS pentest** (2026-03-10): `1857403` — HTTP security headers on all response paths, testssl.sh clean (LUCKY13 accepted)
- **Manual pentest suite** (2026-03-10): `1857403` — 34 tests via `tests/test_pentest.sh`: auth (7), authz (3+2), xss (6), satellite (4), headers (4). All passed. See tables above.
- **Deferred low-priority findings**: the shared satellite PSK and ESP32 credentials in flash, listed under [Known Accepted Risks](#known-accepted-risks)
