# Satellite Pairing QR Protocol

This document specifies the `dawn://provision` URI scheme that DAWN's admin WebUI encodes into a QR code, and what a satellite client (the planned Android client, `docs/ANDROID_CLIENT_DESIGN.md`; not built yet) must do to consume it during onboarding.

The QR replaces the manual transcription of two values that an operator would otherwise have to type into the phone: the WebSocket server URL and the 64-character hex registration key. Username and password are NOT in the QR — the user still authenticates after scanning.

---

## URI shape

```
dawn://provision?server=<urlencoded-wss-url>&v=1#key=<urlencoded-hex>
```

Example:

```
dawn://provision?server=wss%3A%2F%2Fdawn.local%3A3000%2Fws&v=1#key=0123abcd...
```

| Field | Location | Required | Notes |
|---|---|---|---|
| `server` | query | yes | Full WebSocket URL with scheme + `/ws` path. `wss://` strongly preferred. The daemon refuses to emit a `ws://` payload when it is itself serving TLS. |
| `v` | query | yes | Schema version. Only `1` is defined. Unknown values MUST be rejected. |
| `key` | **fragment** | yes | 64-character hex registration key, URL-encoded. Placed in the fragment so the URI cannot leak the key via HTTP referers, link unfurlers, or browser history if a user pastes it somewhere outside the QR scanner. Parse with `Uri.getFragment()` then split on `=`. |

The total payload sits comfortably inside a QR code with `M` error correction.

---

## Android intent filter

Register on the activity that owns the pairing flow:

```xml
<activity android:name=".PairActivity" android:exported="true">
  <intent-filter android:autoVerify="false">
    <action android:name="android.intent.action.VIEW" />
    <category android:name="android.intent.category.DEFAULT" />
    <category android:name="android.intent.category.BROWSABLE" />
    <data android:scheme="dawn" android:host="provision" />
  </intent-filter>
</activity>
```

`autoVerify="false"` because `dawn://` is a private scheme, not bound to a real domain. Other apps can in principle claim it; the OS chooser is the only mitigation at this stage. See the "Show before store" rule below for the defense-in-depth layer.

When DAWN has a real domain in production, migrate to Play App Links with `autoVerify="true"` to pin the intent filter to your signing key.

---

## Validation (do this on every scan)

Reject the URI — and surface a clear "Invalid pairing code" error — if any of the following are not satisfied:

1. `uri.getHost() == "provision"`
2. `uri.getQueryParameter("v") == "1"`
3. `uri.getQueryParameter("server")` parses as a valid URL with scheme `ws://` or `wss://` and a non-empty host
4. `uri.getFragment()` is non-empty and decomposes into `key=<hex>` with at least 32 hex characters

Be strict. A malicious app claiming the `dawn://` scheme would deliver malformed payloads — the chooser is unreliable as a sole defense.

---

## Show-before-store (required UX)

After successful validation, present a confirmation screen BEFORE persisting anything:

- Show the parsed `server` URL in full.
- Show the registration key MASKED by default (`••••`) with a "Show" reveal toggle.
- Provide explicit "Continue" and "Cancel" buttons.

Only after the user taps "Continue" should the values be written to `EncryptedSharedPreferences`. Never silently provision — if a hostile QR somehow reached the user, this is the user's last chance to bail.

---

## What happens after pairing

The stored `server` URL + `key` feed into the existing satellite registration handshake (see `dawn_satellite/include/ws_client.h` and the existing satellite protocol doc):

1. Prompt the user for their DAWN username + password (existing login flow).
2. Generate a UUIDv4 for this device, persist alongside the key.
3. Open a WebSocket to the stored `server`.
4. Send the `satellite_register` message:
   ```json
   {
     "type": "satellite_register",
     "payload": {
       "uuid": "<persisted-uuidv4>",
       "name": "Phone — <model>",
       "tier": 2,
       "registration_key": "<the hex from the QR>",
       "capabilities": {
         "local_asr": false,
         "local_tts": false,
         "wake_word": false
       }
     }
   }
   ```
5. The daemon validates the key (constant-time compare, server-side) and returns `satellite_register_ack` with a `reconnect_secret` for future reconnects.

---

## Security considerations for the client

- **Treat the key as a password.** It rotates only via operator action on the server; assume a long-lived secret.
- **Local storage only.** Keep it in `EncryptedSharedPreferences` (Android Keystore-backed) — never plain `SharedPreferences`, never the filesystem.
- **No analytics / logging.** Never include the key in crash reports, telemetry, or remote log uploads. Mask it in your own debug logs.
- **Clipboard hygiene.** If the user ever copies the URI out of your UI for support purposes, warn them that cross-device clipboard sync (Universal Clipboard, Gboard, KDE Connect) may retain it on other devices outside your reach.
- **No `ws://` over the public internet.** The daemon emits `ws://` only when TLS isn't enabled (local-LAN dev). On a remote-reachable server, refuse to register over `ws://` even if the QR specifies it.

---

## Versioning

If the URI parameter shape needs to change in the future, bump `v` to `2` and update both the daemon's encoder (`www/js/admin/satellites.js`) and this document. Old clients reject unknown versions per the validation rules above, which forces an explicit upgrade path rather than silent breakage.
