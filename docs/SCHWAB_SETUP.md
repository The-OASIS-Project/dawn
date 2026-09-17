# Charles Schwab Setup (Stocks Tool)

DAWN's **stocks** tool provides live stock quotes and read-only access to your
Charles Schwab portfolio (holdings + balances across all your linked accounts).
This is **read-only** — it never places trades.

Enrollment is a one-time (per-week) command-line step because Schwab pins its
OAuth redirect to a URL registered in your developer app; it does not use DAWN's
WebUI callback.

---

## 1. Create / reuse a Schwab developer app

1. Sign in at <https://developer.schwab.com> and create an app (or reuse an
   existing one, e.g. from a prior project).
2. Add both API products the tool needs: **Accounts and Trading** and
   **Market Data**. The app must reach **"Ready For Use"** status.
3. Register a **Callback URL**. Schwab requires it to be **HTTPS on a loopback
   host with a port above 1024**, matched exactly — for example:

   ```
   https://127.0.0.1:8000/callback
   ```

4. Note the app's **App Key** (client id) and **Secret** (client secret).

Nothing listens on that callback URL — during linking you copy the URL Schwab
redirects you to out of the browser's address bar. (If you happen to have another
app — e.g. a `reactor-trading` server — actually listening on that port, it will
receive the redirect. It's plain HTTP so the browser's TLS attempt fails, but the
authorization code is still in the address bar; stop that server or register a
different port to avoid confusion.)

## 2. Configure the credentials

Add the app credentials to `secrets.toml` (never commit this file):

```toml
[secrets.schwab]
client_id = "your-schwab-app-key"
client_secret = "your-schwab-app-secret"
redirect_url = "https://127.0.0.1:8000/callback"   # must EXACTLY match the app registration
```

`redirect_url` defaults to `https://127.0.0.1:8000/callback` if omitted. You can
also set these in the WebUI **Settings → Secrets** panel, or via the
`DAWN_SCHWAB_CLIENT_ID` / `DAWN_SCHWAB_CLIENT_SECRET` / `DAWN_SCHWAB_REDIRECT_URL`
environment variables.

Restart the daemon (or save settings) so it picks up the credentials. Until the
credentials are set, the stocks tool stays hidden from the assistant.

## 3. Link your Schwab account

On the DAWN host, run:

```bash
dawn-admin schwab auth
```

This prints an authorization URL. Open it in a browser, sign in, and approve
access. Schwab redirects you to your callback URL (the page may show a
"can't connect" error — that's expected). **Copy the full URL from the address
bar** and paste it back at the prompt. You have about 5 minutes.

On success DAWN stores the tokens encrypted and auto-refreshes them from then on.

- Multi-user installs: `dawn-admin schwab auth --user <id>` (default is user 1).
- Check status any time: `dawn-admin schwab status` shows the link state and how
  many days remain before you must re-link.

## 4. Weekly re-link

Schwab's refresh token has a **fixed 7-day lifetime** and cannot be extended.
About once a week you must re-run `dawn-admin schwab auth`. When the link lapses,
a stocks request returns a clear "re-link" message instead of data, and
`dawn-admin schwab status` counts down the days.

## 5. Using it

Ask the assistant naturally, for example:

- "What's NVDA trading at?" / "Quote AAPL, AMD, and ARM."
- "Show me my portfolio." / "What are my Schwab balances?"

Notes:

- **Quotes may be delayed** if your Schwab app's market-data entitlement is for
  delayed data — such quotes are labeled `[delayed]`.
- **Account numbers are masked** to the last four digits everywhere they appear.
- The tool is read-only. Trading is a separate, future capability.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Tool never appears to the assistant | `client_id`/`client_secret` not set in `[secrets.schwab]`, or daemon not restarted. |
| "Your Schwab account isn't linked yet" | Run `dawn-admin schwab auth`. |
| "Your Schwab link expired" | The 7-day refresh token lapsed — re-run `dawn-admin schwab auth`. |
| Enrollment fails after pasting the URL | The paste may have taken >5 min, or the app key/secret/redirect don't match. Re-run and paste promptly; verify the callback URL matches the app registration exactly. |
| "URL origin/path does not match the configured redirect" | The pasted URL's scheme/host/port/path must equal `redirect_url`. |
