# OAuth 2.0 / Crypto Subsystem

Source: `src/tools/oauth_client.c`, `src/core/crypto_store.c`

Part of the [D.A.W.N. architecture](../../../ARCHITECTURE.md) — see the main doc for layer rules, threading model, and lock ordering.

---

**Purpose**: Shared OAuth 2.0 authentication and encrypted credential storage.

## Key Components

- **oauth_client.c/h**: OAuth 2.0 client with PKCE S256
   - Provider-agnostic design (currently Google, extensible to Microsoft 365)
   - Authorization URL generation with PKCE challenge
   - Code exchange and token refresh
   - Token storage in `oauth_tokens` SQLite table (encrypted via crypto_store)
   - WebUI popup consent flow with `postMessage` origin validation

- **crypto_store.c/h**: Shared libsodium encryption module
   - `crypto_secretbox` (XSalsa20-Poly1305) for symmetric encryption
   - Key file: `dawn.key` (auto-generated on first use, 256-bit)
   - Used by OAuth (token encryption) and email (password encryption)
   - `crypto_store_encrypt()` / `crypto_store_decrypt()` API
   - `crypto_store_keyed_digest()`: a BLAKE2b digest under a per-purpose subkey
     (`crypto_kdf`) of the same key. Stable on one install, meaningless without
     its key file. Used for the API-key tag in stored LLM turn blocks
     (`llm_key_tag`), so a stored row can't be used to test guesses at a key.

## OAuth Flow

```
WebUI → popup window → Google consent → redirect to /oauth/callback
  → callback page posts auth code via postMessage to opener
  → opener sends code to daemon via WebSocket
  → daemon exchanges code for tokens (PKCE verification)
  → tokens encrypted and stored in oauth_tokens table
  → automatic refresh on expiry
```
