# Unified Image Store System

## Context

Four features need image storage infrastructure: web image search, local image generation (sd.cpp), MMS received images, and document image extraction. DAWN already has `image_store.c` with SQLite BLOB storage for user-uploaded vision images, but BLOB storage bloats the database and requires malloc+memcpy per serve. Migrating to filesystem storage with SQLite metadata enables efficient `lws_serve_http_file()` zero-copy serving, proper retention policies per source type, and a single API surface for all consumers.

MIRAGE (helmet HUD) needs new notification widgets to display images, SMS, and phone call info — triggered by MQTT messages from DAWN, positioned via config.json, auto-dismissed on timer or voice command.

---

## Phase 1: Image Store Migration (DAWN core) ✓ COMPLETE

Migrate from SQLite BLOB to filesystem storage. Everything else depends on this.

### Schema Changes (v29 -> v30)

**New `images` table** (metadata only, no BLOB):
```sql
CREATE TABLE IF NOT EXISTS images (
   id TEXT PRIMARY KEY,
   user_id INTEGER NOT NULL,
   source INTEGER NOT NULL DEFAULT 0,
   retention_policy INTEGER NOT NULL DEFAULT 0,
   mime_type TEXT NOT NULL,
   size INTEGER NOT NULL,
   filename TEXT NOT NULL,
   created_at INTEGER NOT NULL,
   last_accessed INTEGER,
   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
);
```

Also add `image_id TEXT DEFAULT NULL` to `phone_sms_log` (for MMS attachment references).

**Migration code** in `auth_db_core.c`:
1. Create `<data_dir>/images/` directory with `0750` permissions
2. Wrap entire migration in a single SQLite transaction
3. Export each existing BLOB row to file: `<images_dir>/<id>.<ext>`, `fsync()` each
4. Create `images_new` with new schema (no BLOB, add source/retention_policy/filename)
5. `INSERT INTO images_new SELECT ...` from old table
6. `DROP TABLE images; ALTER TABLE images_new RENAME TO images`
7. Rebuild indexes
8. Commit transaction only after all files are fsynced (prevents data loss on crash)

### New Types in `image_store.h`

```c
typedef enum {
   IMAGE_SOURCE_UPLOAD = 0,     /* User upload (vision) */
   IMAGE_SOURCE_GENERATED = 1,  /* sd.cpp output */
   IMAGE_SOURCE_SEARCH = 2,     /* Web image search cache */
   IMAGE_SOURCE_MMS = 3,        /* Received MMS */
   IMAGE_SOURCE_DOCUMENT = 4,   /* Extracted from document */
} image_source_t;

typedef enum {
   IMAGE_RETAIN_DEFAULT = 0,    /* Global retention_days */
   IMAGE_RETAIN_PERMANENT = 1,  /* Never auto-delete */
   IMAGE_RETAIN_CACHE = 2,      /* LRU eviction at size cap */
} image_retention_t;
```

### API Changes

| Function | Change |
|----------|--------|
| `image_store_save()` | Backward-compat wrapper calling `_save_ex()` with SOURCE_UPLOAD, RETAIN_DEFAULT |
| `image_store_save_ex()` | **NEW** — adds source + retention params. Writes file to disk, metadata to DB |
| `image_store_load()` | Reads from file instead of BLOB (same signature, caller still frees) |
| `image_store_get_path()` | **NEW** — returns filesystem path for zero-copy HTTP serving |
| `image_store_cleanup()` | Enhanced: DEFAULT=retention days, CACHE=LRU at size cap, PERMANENT=skip. Uses transaction: SELECT expired IDs → unlink files → DELETE rows |
| Others | Unchanged |

**Internal write pattern**: write to `<images_dir>/.<id>.<ext>.tmp` (using `O_NOFOLLOW` to prevent symlink attacks), `fsync()`, then `rename()` for atomicity. File write happens OUTSIDE the auth_db mutex — sequence is: write file (no lock) -> lock -> DB insert -> unlock. On DB insert failure, unlink the file. This avoids blocking all auth_db consumers during disk I/O.

### Config Addition

```c
// images_config_t gets:
int cache_size_mb;  /* LRU cache cap for RETAIN_CACHE images, default 200 */
```

### Prepared Statement Changes

- `stmt_image_create` — updated SQL (no BLOB, add source/retention_policy/filename)
- `stmt_image_get_data` — **removed** (was BLOB fetch), replaced by `stmt_image_get_file` returning filename
- `stmt_image_delete_old` — split into default retention + cache LRU queries
- **New**: `stmt_image_cache_total_size` — `SELECT SUM(size) FROM images WHERE retention_policy=2`

### Files Modified

- `include/image_store.h` — enums, `_save_ex()`, `_get_path()`
- `src/image_store.c` — filesystem internals
- `include/auth/auth_db_internal.h` — schema v30, updated stmts
- `src/auth/auth_db_core.c` — migration, SCHEMA_SQL, stmt init
- `include/config/dawn_config.h` — `cache_size_mb`
- `src/config/config_parser.c` — parse `cache_size_mb`
- `dawn.toml.example` — document `cache_size_mb`

### Testing

- New `tests/test_image_store.c`: save/load/delete, filesystem verification, retention policies, cache eviction, migration from BLOB
- Manual: existing vision upload flow still works end-to-end

---

## Phase 2: HTTP Serving Update ✓ COMPLETE (merged into Phase 1)

Replace malloc-based download with zero-copy file serving.

### Changes to `webui_images.c`

**GET /api/images/:id** (`webui_images_handle_download`):
1. `image_store_get_metadata()` — access check + get mime_type
2. `image_store_get_path()` — get filesystem path
3. `lws_serve_http_file(wsi, path, mime, sec_hdrs, len)` — kernel-level streaming

Access policy: SOURCE_UPLOAD and SOURCE_MMS require ownership check. SOURCE_SEARCH, SOURCE_GENERATED, SOURCE_DOCUMENT are accessible to any authenticated user (user_id=0 bypass).

Security headers: `Content-Disposition: inline`, `X-Content-Type-Options: nosniff`, `Cache-Control: private, max-age=31536000` added as extra headers to `lws_serve_http_file()` call.

**POST /api/images** — unchanged, `image_store_save()` handles filesystem internals.

### Files Modified

- `src/webui/webui_images.c` — download handler rewrite

---

## Phase 3: Web Image Search Tool ✓ COMPLETE

New LLM tool that searches SearXNG for images, pre-fetches them server-side, and caches via image store. See [WEB_IMAGE_SEARCH (atlas)](https://github.com/The-OASIS-Project/atlas/blob/main/dawn/archive/WEB_IMAGE_SEARCH.md) for full design.

### Tool Design

```
Name: image_search
Params: query (string, required), count (int, default 5)
Caps: TOOL_CAP_NETWORK
Not SCHEDULABLE, not DANGEROUS
```

### Callback Flow

1. `GET <searxng_url>/search?q=<query>&format=json&categories=images&safesearch=1`
2. Parse results: `img_src`, `thumbnail_src`, `title`, `source`, `resolution`
3. For each result (up to `count`):
   - Fetch `img_src` via libcurl (10s timeout, 4MB max, HTTPS/HTTP only via `CURLOPT_PROTOCOLS`)
   - **SSRF protection**: Reuse DNS-pinning defense from `document_index_tool.c` — resolve IP, reject RFC1918/link-local/loopback before connecting
   - Validate magic bytes, determine MIME type
   - `image_store_save_ex(0, data, size, mime, IMAGE_SOURCE_SEARCH, IMAGE_RETAIN_CACHE, id_out)`
   - Skip on 404, hotlink block, timeout, SSRF block — move to next candidate
4. Return structured result to LLM:
   ```json
   [{"url": "/api/images/img_xxx", "width": 1920, "height": 1080,
     "source": "wikipedia.org", "title": "..."}]
   ```
5. Optionally publish to HUD for MIRAGE display (top-level fields, matching `publish_hud()` pattern):
   ```json
   {
     "device": "image",
     "action": "display",
     "msg_type": "request",
     "image_url": "/api/images/img_xxx",
     "title": "Search result title",
     "source": "wikipedia.org",
     "ttl": 30,
     "timestamp": 1713100000000
   }
   ```
   Note: `action` is correct here — DAWN is commanding MIRAGE to display an image (imperative). `msg_type: "request"` is required on the shared `hud` topic per OCP v1.4.

### Tool Description (LLM dispatch guidance)

- WebUI session: count=5 (user scans grid)
- HUD/MIRAGE session: count=1 (single focal image)
- Voice-only satellite: don't call this tool; describe verbally

### Files Created

- `src/tools/image_search_tool.c`
- `include/tools/image_search_tool.h`

### Files Modified

- `include/tools/web_search.h` — add `SEARCH_TYPE_IMAGES`, declare `web_search_query_images_raw()`
- `src/tools/web_search.c` — implement `web_search_query_images_raw()`
- `src/tools/tools_init.c` — registration
- `cmake/DawnTools.cmake` — `DAWN_ENABLE_IMAGE_SEARCH_TOOL`

---

## Phase 4: MMS Support — ECHO Side — BLOCKED (firmware build does not include MMS)

**Status**: The SIM7600G-H firmware currently flashed (`LE20B04SIM7600G22`) does not include MMS AT commands (`AT+CMMSINIT`, `AT+CMMSRECV`, etc.). The `AT+CLAC` command list contains zero MMS-related entries. T-Mobile delivers incoming MMS as WAP Push notifications (`+CMTI` URC), but the modem discards the content immediately since it has no MMS handler — the message is empty when read back. Tested April 2026.

**Important caveat** (web research April 2026): The SIM7600 hardware/SDK family *does* support MMS via the standard SIMCom `AT+CMMS*` command set. Waveshare explicitly lists MMS in the SIM7600G-H's application capabilities, and the official `SIM7500_SIM7600 Series AT Command Manual` documents the full MMS interface. The absence on our unit appears to be specific to this firmware *build* — different regional/carrier builds (E-H, SA-H, G22 vs later Gxx) ship different feature sets. Multiple firmware revisions exist (LE11B09, LE11B12, LE20B03, LE20B04, LE20B05...) and firmware updates are supported over USB or FOTA.

**Unblocking paths, in order of effort**:
1. **Check for a newer SIM7600G-H firmware build that includes MMS** — compare `AT+CGMR` output against Techship/SIMCom firmware archives; a newer `LE20Bxx` build may add the MMS stack. Flash via USB using the SIMCom update tool. Firmware update carries bricking risk, but is the cheapest unblock if a compatible build exists.
2. **Verify MMSC / PDP config** — even with MMS AT commands present, MMS requires carrier MMSC URL, APN proxy, and a separate PDP context. T-Mobile MVNO plans vary on MMSC availability. Worth checking once the AT commands are there.
3. **Alternate modem hardware** — e.g., a Quectel EC25-A, which has well-documented MMS support on T-Mobile. Larger scope.
4. **T-Mobile email-to-MMS gateway** — `<number>@tmomail.net` delivers email as MMS. Uses existing DAWN email integration. Send-only (and receive if monitoring the reply-to); avoids modem MMS entirely.

Phase 5 (DAWN-side handler) is ready to implement whenever incoming MMS reaches the `echo/events` MQTT topic via any of these paths.

References:
- Waveshare SIM7600G-H wiki: https://www.waveshare.com/wiki/SIM7600G-H_4G_Module
- SIMCom AT Command Manual: https://simcom.ee/documents/SIM7600C/SIM7500_SIM7600%20Series_AT%20Command%20Manual_V1.01.pdf
- Techship firmware archives: https://techship.com/downloads/

Original design (for reference when unblocked):

Add MMS receive capability to the modem daemon.

### Modem Init Addition

Add `AT+CMMSINIT` to `modem_init()` in `modem.c` after SMS setup. Harmless if carrier doesn't support MMS.

### URC Handler Addition

New URC types in `urc_handler.c`: `+CMMTRECV` (MMS received notification). Deferred to main thread via command queue (same pattern as CMTI to avoid deadlock).

### MMS Download Flow

New `src/mms.c` module:
1. `AT+CMMSVIEW` — read MMS metadata (sender, subject, content-type, size)
2. `AT+CMMSDOWN=<index>` — download media to temp buffer
3. Base64 encode the binary data (~30 lines, no external lib)
4. Publish MQTT event on `echo/events` (OCP v1.4 compliant — Event Message with binary `data` payload):
   ```json
   {
     "device": "echo",
     "event": "mms_received",
     "msg_type": "event",
     "sender": "+15551234567",
     "subject": "Photo",
     "timestamp": 1713100000000,
     "data": {
       "type": "image/jpeg",
       "encoding": "base64",
       "content": "<base64 encoded image>",
       "size": 245000,
       "checksum": "a1b2c3..."
     }
   }
   ```
   Note: `event` field per OCP v1.4 Event Message spec (indicative — reporting something happened). `msg_type: "event"` recommended on dedicated `echo/events` topic (aids tooling/logging). `timestamp` in Unix milliseconds. `data` block follows OCP Data Transport spec (type + encoding + content + size + checksum).
5. Clean up temp data after publish

**Why base64-in-MQTT**: Carrier MMS max is typically 300KB-1MB. Base64 overhead (~33%) keeps payloads well under Mosquitto's default limit. Avoids shared filesystem permissions, race conditions, and cross-service coupling.

**Size safety**: DAWN-side handler must enforce a hard decoded-size cap (2MB) before base64 decoding, and validate `media_size` field matches actual decoded length. Decode in phone_service worker context, not inline in MQTT callback (which would block the event loop).

### Files Created (ECHO repo)

- `echo/src/mms.c` — MMS download, base64 encode
- `echo/include/mms.h`

### Files Modified (ECHO repo)

- `echo/src/modem.c` — `AT+CMMSINIT` in init sequence
- `echo/src/urc_handler.c` — `+CMMTRECV` handler
- `echo/src/oasis-echo.c` — wire up MMS command processing
- `echo/src/mqtt_comms.c` — `mqtt_build_mms_event_json()`
- `echo/include/echo.h` — MMS constants, any config additions
- `echo/CMakeLists.txt` — add mms.c

---

## Phase 5: MMS Support — DAWN Side

Depends on Phase 1 (image store) + Phase 4 (ECHO MMS).

### phone_service.c Addition

New handler in the echo/events dispatcher (after `sms_received`):
```
"mms_received" → base64 decode → validate magic bytes →
image_store_save_ex(user_id, data, size, mime, SOURCE_MMS, RETAIN_DEFAULT) →
phone_db_sms_log_insert(...) with image_id →
publish_hud() notification → TTS "New picture message from [contact]"
```

### HUD MQTT Message (to "hud" topic, matches existing `publish_hud()` pattern)

```json
{
  "device": "phone",
  "event": "mms_received",
  "msg_type": "event",
  "name": "John Doe",
  "number": "+15551234567",
  "preview": "Photo",
  "image_url": "/api/images/img_xxx",
  "ttl": 15,
  "timestamp": 1713100000000
}
```
Uses `event` field per OCP v1.4 — "mms_received" is indicative (reporting something that happened), not imperative. `msg_type: "event"` is required on the shared `hud` topic. Consistent with how `incoming_call`, `sms_received`, `call_active`, and `call_ended` are already published via `publish_hud()` in `phone_service.c`. MIRAGE parses top-level fields directly in `command_processing.c`. Timestamp in Unix ms per OCP.

### Files Modified

- `src/tools/phone_service.c` — mms_received event handler
- (phone_sms_log image_id column already added in Phase 1 migration)

---

## Phase 6: MIRAGE Notification Widgets + Contact Photos

Notification popups on MIRAGE's HUD for phone calls, SMS, and image search results. Plus contact photo support so incoming calls show the caller's face.

### Architecture: Config-Driven Elements

MIRAGE's core principle is user configurability — everything lives in config.json. Notification widgets are standard config.json elements (static backgrounds, text with dynamic sources, special photo/image elements) with HUD membership, positioning, font, and color all user-configurable. A `notification_group` field links related elements so the notification system shows/hides them as a unit.

The notification system manages **state only** — visibility, timeouts, dynamic text content, photo textures. The user controls all visual aspects via config.json.

### Image Delivery

- **Contact photos** (128x128, ~10-20KB): base64-encoded inline in MQTT HUD messages. No HTTP dependency.
- **Image search results** (up to 1MB): fetched via HTTP from DAWN with a service token in `secrets.json`. URL validated against allowlist `/api/images/img_[a-zA-Z0-9]{12}` before fetching.

### Texture Creation: Main Thread Only

Background threads download raw bytes into a mutex-protected buffer + dirty flag. Render thread creates SDL textures on the main thread. Same pattern as `render_map_element()` in element_renderer.c.

### Step 6a: Contact Photos (DAWN side) ✓ COMPLETE

**Schema v31**: `ALTER TABLE memory_entities ADD COLUMN photo_id TEXT DEFAULT NULL`

**DB functions**:
- `memory_db_entity_set_photo(user_id, entity_id, photo_id)` — ownership-checked UPDATE, returns NOT_FOUND if entity doesn't belong to user
- `memory_db_entity_get_photo(user_id, entity_id, out, size)` — returns photo_id string or empty
- `image_store_update_retention(id, user_id, retention)` — user-scoped retention policy update (used to upgrade to PERMANENT on bind, downgrade to DEFAULT on replace)

**Retention lifecycle**: When a photo is bound to an entity, the image is upgraded to `RETAIN_PERMANENT` (never auto-deleted). When replaced by a new photo, the old image is downgraded to `RETAIN_DEFAULT` (ages out normally). Implemented in `handle_entity_set_photo` — reads old photo_id before update, then adjusts both.

**WebSocket handlers**:
- `entity_set_photo` — accepts entity_id + photo_id (or null to clear), validates image ownership via `image_store_get_metadata()`, manages retention lifecycle
- `entity_ensure` — lightweight entity find-or-create by name (upsert), used for photo-only saves where no contact record is needed

**WebUI contacts modal**:
- Circular photo area at top of modal (96px, accent border, SVG placeholder silhouette)
- Click circle or "Upload Photo" button to select file (max 10MB pre-compression guard)
- Client-side compression: canvas resize to 256px max, JPEG 85%, dark fill (#1a1a2e) for transparent PNGs
- Preview shown in circle after upload; "Remove" button appears when photo is set
- Photo-only save flow: name + photo without contact type/value, creates entity via `entity_ensure`
- Existing photos shown when editing, with onerror fallback to placeholder
- Circular thumbnails (28px) in contact card list with delegated error handler
- Keyboard accessible: tabindex/role/aria-label on photo circle, Enter/Space triggers file picker
- Focus trap: Tab/Shift+Tab wraps within modal (matching calendar-accounts pattern)
- Focus-visible outlines on photo circle and buttons, mobile touch target sizing

**Phone service HUD photos**:
- `reverse_lookup()` extended to return entity_id alongside contact name
- `s_active_entity_id` added to call state tracking (set in `set_state_with_call`, cleared in `clear_call_tracking`)
- `build_contact_photo_json(user_id, entity_id)` — reads image file (256KB max), base64-encodes with OpenSSL BIO, returns `{"data": "<base64>", "mime": "image/jpeg"}` or NULL
- Photo included in `incoming_call`, `call_active`, and `sms_received` HUD messages (absent, not null, when no photo)
- `phone_service_config_t.user_id` replaces all hardcoded user_id=1 in event handlers

#### Files Modified (DAWN)

- `include/auth/auth_db_internal.h` — schema v31, photo + retention statements
- `src/auth/auth_db_core.c` — v31 migration, prepared statements, contacts queries with photo_id
- `include/image_store.h` — `image_store_update_retention()` declaration
- `src/image_store.c` — retention update implementation with user_id scoping
- `include/memory/memory_db.h` — `memory_db_entity_set_photo()`, `_get_photo()`
- `src/memory/memory_db.c` — photo functions with ownership checks
- `include/memory/contacts_db.h` — `photo_id[32]` in `contact_result_t`
- `src/memory/contacts_db.c` — photo_id in row_to_contact
- `include/webui/webui_contacts.h` — `handle_entity_set_photo()`, `handle_entity_ensure()`
- `src/webui/webui_contacts.c` — set_photo handler (ownership + retention), entity_ensure handler, entity_id in add response
- `src/webui/webui_server.c` — entity_set_photo + entity_ensure dispatch
- `include/tools/phone_service.h` — `user_id` field in config struct
- `src/tools/phone_service.c` — base64 photo in HUD, entity_id tracking, configurable user_id
- `www/js/ui/contacts.js` — photo upload/compress/preview/remove, photo-only save, focus trap, error fallbacks
- `www/css/components/contacts.css` — photo circle, thumbnail, remove button, hidden rules, mobile responsive
- `www/index.html` — photo area HTML in modal
- `www/js/dawn.js` — entity_set_photo_response + entity_ensure_response dispatch

### Step 6b: MIRAGE Notification System ✓ COMPLETE

#### Notification State Machine

```
HIDDEN → SHOWING (150ms fade-in phone, 250ms image)
SHOWING → VISIBLE (display, timeout counting)
VISIBLE → COMPACT (phone only — after 5s, collapse to single-line timer)
VISIBLE → HIDING (TTL expire, call_ended, or voice "dismiss")
COMPACT → HIDING (call_ended or voice "dismiss")
HIDING → HIDDEN (250ms fade-out)
```

#### Notification Slots & Priority

Two independent slots (phone + image). Priority: active call always wins over SMS; SMS queued during calls, shown after call_ended.

#### New Dynamic Text Sources

Added to `resolve_text_source()`:
- `*CALLER_NAME*`, `*CALLER_NUMBER*`, `*CALL_STATUS*`, `*SMS_PREVIEW*`
- `*NOTIFICATION_TITLE*`, `*NOTIFICATION_SOURCE*`

#### New Special Element Types

- `notification_photo` — renders decoded contact photo texture (or placeholder)
- `notification_image` — renders image search result (fetched via HTTP)

#### Config Example (user-configurable)

```json
{ "type": "static", "name": "phone_notif_bg", "file": "notification-phone-bg.png",
  "dest_x": 880, "dest_y": 80, "layer": 10,
  "huds": ["default", "environmental", "armor"],
  "notification_group": "phone" },

{ "type": "special", "name": "phone_notif_photo", "special_name": "notification_photo",
  "dest_x": 900, "dest_y": 100, "width": 128, "height": 128, "layer": 11,
  "huds": ["default", "environmental", "armor"],
  "notification_group": "phone" },

{ "type": "text", "string": "*CALL_STATUS*", "font": "Aldrich-Regular.ttf",
  "size": 22, "color": "0x02, 0xDF, 0xF1, 0xFF",
  "dest_x": 1048, "dest_y": 90, "layer": 11,
  "huds": ["default", "environmental", "armor"],
  "notification_group": "phone" },

{ "type": "text", "string": "*CALLER_NAME*", "font": "Aldrich-Regular.ttf",
  "size": 38, "color": "0xFF, 0xFF, 0xFF, 0xFF",
  "dest_x": 1048, "dest_y": 120, "layer": 11,
  "huds": ["default", "environmental", "armor"],
  "notification_group": "phone" },

{ "type": "text", "string": "*CALLER_NUMBER*", "font": "Aldrich-Regular.ttf",
  "size": 24, "color": "0x5A, 0x8A, 0x8D, 0xFF",
  "dest_x": 1048, "dest_y": 168, "layer": 11,
  "huds": ["default", "environmental", "armor"],
  "notification_group": "phone" }
```

#### Visual Design

- Colors: cyan `#02DFF1` labels, white values, `#5A8A8D` secondary, `#00FF88` active call
- Font: Aldrich-Regular.ttf
- Panel backgrounds: PNG assets with angular beveled corners (Iron Man aesthetic)
- Ringing: pulsing cyan at 1Hz
- Phone: 550x240, upper-right (880, 80)
- Image: 600x500, center (420, 250)

#### Phone Notification Layouts

```
Incoming:  [PHOTO 128x128] INCOMING CALL / John Doe / +1 (404) 555-0142 / ● RINGING
Active:    [PHOTO 128x128] CALL ACTIVE / John Doe / 00:03:45 (collapses after 5s)
Ended:     3s flash with duration, auto-dismiss
SMS:       [PHOTO 128x128] SMS RECEIVED / John Doe / Preview text (2 lines max, 15s TTL)
```

#### Files Created (MIRAGE)

- `src/ui/notification.c` — state machine, text sources, photo texture, timeout
- `include/ui/notification.h` — public API
- `src/util/http_fetch.c` — async HTTP image download
- `include/util/http_fetch.h`

#### Files Modified (MIRAGE)

- `src/comm/command_processing.c` — add phone + image device handlers
- `src/rendering/element_renderer.c` — call `notification_render()`, render special types
- `include/config/config_parser.h` — `notification_group` field, special type enums
- `src/config/config_parser.c` — parse notification_group, new special names
- `src/config/config_secrets.c` — parse `dawn_url` + `dawn_service_token`
- `config.json` — add notification element entries
- `secrets.json` — add `dawn_url` + `dawn_service_token`
- `CMakeLists.txt` — add new sources, link libcurl

#### New Assets

- `assets/notification-phone-bg.png` (550x240)
- `assets/notification-image-bg.png` (600x500)
- `assets/contact-placeholder.png` (128x128, geometric silhouette in cyan outline)

### Step 6c: DAWN Service Token Auth ✓ COMPLETE

Bearer token auth on `/api/images/:id` for MIRAGE image fetching.

**Authentication flow:**
- Session cookie auth tried first (normal browser path)
- Falls back to `Authorization: Bearer <token>` (RFC 6750, case-sensitive prefix)
- Constant-time comparison via `sodium_memcmp` over padded `CONFIG_API_KEY_MAX` buffer (no length leak)
- Early-out when no `service_token` configured (feature disabled by default)

**Access scoping:**
- `user_id=0` in `image_store_get_path` allows access to `GENERATED`, `SEARCH`, and `DOCUMENT` sources
- Private sources (`UPLOAD`, `MMS`) are blocked even for service tokens
- Normal user access (user_id > 0) unchanged — owners can still access their own uploads

**Security hardening:**
- Rate limiter on ALL Bearer attempts (valid or not): 120 req/min per IP, 32 IP slots
- Rate limit checked before authentication to throttle brute-force
- Minimum 32-character token enforced at config parse time (shorter tokens rejected with warning)
- TLS warning logged when Bearer token used on non-TLS connection
- Token never logged in any code path

**Configuration:** `secrets.toml` under `[secrets]`:
```toml
service_token = "<openssl rand -hex 32>"
```

#### Files Modified (DAWN)

- `include/config/dawn_config.h` — `service_token` field in `secrets_config_t`
- `src/config/config_parser.c` — parse service_token with min-length validation
- `src/webui/webui_http.c` — `is_service_token_authenticated()`, Bearer routing, rate limiter
- `src/image_store.c` — scoped access check (user_id=0 blocks private sources)
- `secrets.toml.example` — documented service_token field with generation command

### Implementation Order

1. ~~Contact photos (DAWN)~~ ✓
2. ~~MIRAGE notification system~~ ✓ — state machine, rendering, config parsing, mutex protection
3. ~~HTTP fetch module (MIRAGE)~~ ✓ — async curl with Bearer auth, URL allowlist, magic byte validation, 5MB cap, fetch-in-progress guard, shutdown synchronization, auth header zeroed on return. Scale-to-fit centered rendering. Auto-publish to HUD from `image_search_tool.c` for `SESSION_TYPE_LOCAL` voice sessions.
4. ~~DAWN service token auth~~ ✓ (includes auth_probe buffer fix: 16→512 bytes — the 16-byte buffer silently truncated Bearer headers, skipping auth entirely)
5. ~~MQTT routing (MIRAGE)~~ ✓ — phone + image handlers with device_str fix
6. ~~Asset creation~~ ✓ — notification panel PNGs, contact placeholder
7. ~~Integration test~~ ✓ — SMS, calls, photos, ringtone, LLM context, voicemail timeout, image display from voice search

---

## OCP v1.4 Compliance Summary

| Message | Topic | `msg_type` | Key Field | Notes |
|---------|-------|------------|-----------|-------|
| ECHO `mms_received` | `echo/events` | `event` | `event: "mms_received"` | Event Message with binary `data` payload (type + encoding + content + size + checksum) |
| DAWN→MIRAGE image display | `hud` | `request` | `action: "display"` | Request — imperative (commanding MIRAGE to show an image). `msg_type` required on shared `hud` topic |
| DAWN→MIRAGE MMS notification | `hud` | `event` | `event: "mms_received"` | Event — indicative (reporting MMS arrival). `msg_type` required on shared `hud` topic |
| All messages | — | — | `timestamp` | Unix milliseconds per OCP v1.4 |

- **`action` vs `event`**: `action` is imperative ("display this"), `event` is indicative ("this happened"). Per OCP v1.4, components MUST NOT reuse `action` for event names or `event` for commands.
- **`msg_type` on `hud` topic**: Required — shared topic carries both requests and events.
- **`msg_type` on `echo/events`**: Recommended — dedicated event topic, but field aids tooling/logging.
- **`data` block**: Used for binary transport (ECHO sending MMS image bytes). HUD messages use top-level fields per the established `publish_hud()` pattern.

---

## Future Phases (design-only, no code now)

### Image Generation (uses Phase 1 API)
- `image_store_save_ex(user_id, png_data, size, "image/png", SOURCE_GENERATED, RETAIN_PERMANENT, id)`
- HUD notification via same `device=image, action=display, msg_type=request` pattern

### Document Image Extraction (uses Phase 1 API)
- MuPDF extracts images during indexing
- `image_store_save_ex(user_id, data, size, mime, SOURCE_DOCUMENT, RETAIN_DEFAULT, id)`
- Reference image IDs in document chunks for vision API

---

## Verification

1. **Phase 1**: `test_image_store` passes — save/load/delete, files on disk, cleanup policies
2. **Phase 2**: `curl -X POST /api/images` upload, `curl /api/images/<id>` download, verify Content-Type and Cache-Control headers
3. **Phase 3**: Voice command "search for pictures of sunsets" → images cached locally, WebUI shows grid, MIRAGE shows focal image
4. **Phase 4**: Send MMS to SIM card → ECHO publishes `mms_received` on `echo/events` with base64 data
5. **Phase 5**: MMS arrives → image stored in DAWN, HUD notification sent, TTS spoken, viewable in WebUI
6. **Phase 6**: MQTT test messages → widgets appear on HUD, auto-dismiss after TTL
7. **Regression**: Existing vision upload flow unchanged — photo upload + AI analysis still works
