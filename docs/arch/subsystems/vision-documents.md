# Vision / Image & Document Upload Subsystems

Source: `src/image_store.c`, `src/webui/webui_images.c`, `src/webui/webui_documents.c`, `www/js/ui/vision.js`, `www/js/ui/documents.js`

Part of the [D.A.W.N. architecture](../../../ARCHITECTURE.md) — see the main doc for layer rules, threading model, and lock ordering.

---

## Vision / Image Subsystem

**Purpose**: Image upload, storage, vision AI integration, and image search for the WebUI.

### Architecture: Client-Side Compression + Server-Side Filesystem Storage

```
┌───────────────────────────────────────────────────────────────────────┐
│                        Browser (WebUI)                                 │
│                                                                        │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │                    Input Methods                                 │  │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐        │  │
│  │  │  File    │  │  Paste   │  │  Drag &  │  │  Camera  │        │  │
│  │  │  Upload  │  │  (Ctrl+V)│  │  Drop    │  │  Capture │        │  │
│  │  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘        │  │
│  │       └─────────────┴─────────────┴─────────────┘               │  │
│  │                           │                                      │  │
│  │                    ┌──────▼──────┐                               │  │
│  │                    │ Compression │ (max 1024px, JPEG 85%)        │  │
│  │                    └──────┬──────┘                               │  │
│  └───────────────────────────┼─────────────────────────────────────┘  │
│                              │                                        │
│  ┌───────────────────────────▼─────────────────────────────────────┐  │
│  │ POST /api/images (multipart/form-data)                          │  │
│  └───────────────────────────┬─────────────────────────────────────┘  │
└──────────────────────────────┼────────────────────────────────────────┘
                               │
                               ▼
┌───────────────────────────────────────────────────────────────────────┐
│                        DAWN Server                                     │
│                                                                        │
│  ┌───────────────────────────────────────────────────────────────┐    │
│  │ webui_images.c - HTTP Endpoint Handler                        │    │
│  │                                                               │    │
│  │  POST /api/images  → Validate → Store → Return image ID       │    │
│  │  GET /api/images/:id → get_path() → lws_serve_http_file()     │    │
│  └───────────────────────────┬───────────────────────────────────┘    │
│                              │                                        │
│  ┌───────────────────────────▼───────────────────────────────────┐    │
│  │ image_store.c - Filesystem Storage + SQLite Metadata          │    │
│  │                                                               │    │
│  │  Files: <data_dir>/images/<id>.<ext>                          │    │
│  │  Table: images (id, user_id, source, retention_policy, ...)   │    │
│  │  Sources: upload, generated, search, MMS, document            │    │
│  │  Retention: default (age-based), permanent, cache (LRU)       │    │
│  │  - Thread-safe: file I/O outside mutex, metadata ops inside   │    │
│  │  - Atomic writes (tmp + fsync + rename + O_NOFOLLOW)          │    │
│  │  - Zero-copy HTTP serving via kernel sendfile                 │    │
│  └───────────────────────────────────────────────────────────────┘    │
│                                                                        │
│  ┌───────────────────────────────────────────────────────────────┐    │
│  │ LLM Vision Integration                                        │    │
│  │                                                               │    │
│  │  llm_openai.c: data:image/jpeg;base64,... format              │    │
│  │  llm_claude.c: source.type="base64", source.media_type=...    │    │
│  └───────────────────────────────────────────────────────────────┘    │
└───────────────────────────────────────────────────────────────────────┘
```

### Key Components

- **vision.js**: client-side image handling (1,400+ lines)
   - Input methods: file picker, clipboard paste, drag-and-drop, camera capture
   - Camera API with front/rear switching (`navigator.mediaDevices.getUserMedia`)
   - Client-side compression via Canvas API (configurable max dimension, default 1024px, JPEG 85%)
   - Multi-image support (configurable max per message, default 5)
   - LocalStorage caching of uploaded images by ID
   - Security: SVG explicitly excluded to prevent XSS attacks
   - Accessibility: ARIA announcements, keyboard navigation

- **image_store.c/h**: server-side image storage
   - Filesystem-backed: images stored as files in `<data_dir>/images/`, SQLite holds metadata only
   - Image ID format: `img_` + 12 alphanumeric characters (getrandom)
   - Source tracking: `IMAGE_SOURCE_UPLOAD`, `_GENERATED`, `_SEARCH`, `_MMS`, `_DOCUMENT`
   - Retention policies: `IMAGE_RETAIN_DEFAULT` (age-based), `_PERMANENT` (never delete), `_CACHE` (LRU at size cap)
   - Atomic file writes: O_NOFOLLOW tmp + fsync + rename, file I/O outside auth_db mutex
   - Source-aware access control: UPLOAD/MMS require ownership, SEARCH/GENERATED/DOCUMENT accessible to any auth'd user
   - Configurable limits: max size, max per user, retention days, cache size (MB)

- **webui_images.c/h**: HTTP endpoint handlers
   - `POST /api/images`: upload image, returns `{id, mime_type, size}`
   - `GET /api/images/:id`: zero-copy file serving via `lws_serve_http_file()` (kernel sendfile)
   - Authentication required, source-aware access check
   - Security headers: Cache-Control, X-Content-Type-Options, Content-Disposition

- **image_search_tool.c/h**: web image search via SearXNG
   - Queries `web_search_query_images_raw()` (shared SearXNG backend)
   - Concurrent image fetching via `curl_multi` (4 parallel connections, 10s wall-clock cap)
   - SSRF protection: DNS pinning via CURLOPT_RESOLVE, manual redirect-with-revalidation (1 hop max)
   - Magic byte validation (JPEG, PNG, GIF87a/89a, WebP) — Content-Type ignored
   - Cached as `IMAGE_SOURCE_SEARCH` + `IMAGE_RETAIN_CACHE` (LRU eviction at configurable cap)

### Data Flow (Image Upload)

```
1. User selects/pastes/drops/captures image
   ↓
2. Browser validates type (JPEG, PNG, GIF, WebP - NO SVG)
   ↓
3. Canvas API compresses to max 1024px, JPEG 85%
   ↓
4. POST /api/images with multipart form data
   ↓
5. Server validates auth, writes file atomically to <data_dir>/images/, stores metadata in SQLite
   ↓
6. Server returns image ID (e.g., "img_a1b2c3d4e5f6")
   ↓
7. Browser caches in localStorage, shows preview
   ↓
8. On send: turn frame carries BOTH the full base64 `images[]` (sent to the LLM this
   turn) AND `image_ids[]` (the /api/images ids, ordered to match)
   ↓
9. Daemon persists the user turn itself (server-authoritative): it builds
   `text` + `[IMAGE:<id>]` markers from image_ids and writes the row, promotes those
   images to permanent retention (post-persist, so only persisted turns pin images),
   and echoes `server_saved: true` so the client does NOT client-save the row
   ↓
10. On reload, `[IMAGE:<id>]` markers rehydrate into image_url content (owner-checked)
```

> **Persistence ownership (hard cut-over):** the daemon is the sole writer of user rows —
> text and image turns alike. Previously the browser client-saved image turns (it held the
> ids); now it sends the ids on the turn frame and the daemon owns persistence. `image_ids`
> is therefore **mandatory** on an image turn — without it the turn persists text-only and
> the images are lost on reload (no client-save fallback). Core stays image-agnostic: the
> marker grammar and image-store calls live in the WebUI layer
> (`webui_message_dispatch.c`, `webui_image_rehydrate.c`); `text_input_dispatch.c` just
> persists a caller-supplied string via `persist_content_override`.

### Vision Model Support

The system auto-detects vision capability based on model name:

| Model Pattern                           | Provider  | Vision Support       |
| --------------------------------------- | --------- | -------------------- |
| `gpt-4o`, `gpt-4-vision`, `gpt-4-turbo` | OpenAI    | Yes                  |
| `claude-3-*`                            | Anthropic | Yes                  |
| `gemini-*`                              | Google    | Yes                  |
| `llava-*`, `qwen-vl-*`, `cogvlm-*`      | Local     | Yes                  |
| Other models                            | Various   | No (button disabled) |

### Security Measures

- **SVG exclusion**: SVG files explicitly blocked to prevent XSS via embedded scripts
- **Data URI validation**: only `data:image/{jpeg,png,gif,webp};base64,` prefixes accepted
- **Base64 character validation**: only `[A-Za-z0-9+/=]` allowed in base64 portion
- **Authentication required**: all image endpoints require valid session
- **Per-user limits**: configurable maximum images per user

---

## Document Upload Subsystem

Source: `src/webui/webui_documents.c`, `www/js/ui/documents.js`

**Purpose**: Document upload, extraction, and attachment for LLM context — supports plain text, PDF (MuPDF), DOCX (libzip+libxml2), and HTML-to-markdown (html_parser.c).

> **Transient vs. persistent uploads**: this subsystem covers documents attached to a single conversation turn (extracted text injected into the message). Documents uploaded to the persistent **Document Library** (RAG) are chunked, embedded, and searchable — and since v68 the raw uploaded PDF/DOCX is also retained in a generic **blob store** (`documents.original_blob_id`) so it can be downloaded or re-processed later. See [rag.md](rag.md).

### Architecture: Client-Side Read + Server-Side Extraction + Transcript Chip Display

```
┌───────────────────────────────────────────────────────────────────────┐
│                        Browser (WebUI)                                 │
│                                                                        │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │                    Input Methods                                 │  │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐                       │  │
│  │  │  File    │  │  Drag &  │  │  Paste   │                       │  │
│  │  │  Button  │  │  Drop    │  │  (Ctrl+V)│                       │  │
│  │  └────┬─────┘  └────┬─────┘  └────┬─────┘                       │  │
│  │       └─────────────┴─────────────┘                              │  │
│  │                     │                                             │  │
│  │              ┌──────▼──────┐                                      │  │
│  │              │ Client-side │ (FileReader API, UTF-8 text)         │  │
│  │              │ text read   │                                      │  │
│  │              └──────┬──────┘                                      │  │
│  │                     │                                             │  │
│  │              ┌──────▼──────┐                                      │  │
│  │              │ Input chips │ (filename, size, remove button)      │  │
│  │              └──────┬──────┘                                      │  │
│  └─────────────────────┼─────────────────────────────────────────┘  │
│                        │ on send                                     │
│  ┌─────────────────────▼─────────────────────────────────────────┐  │
│  │ WebSocket: [ATTACHED DOCUMENT: file (N bytes)]...text...       │  │
│  │            [END DOCUMENT]                                      │  │
│  └─────────────────────┬─────────────────────────────────────────┘  │
│                        │                                             │
│  ┌─────────────────────▼─────────────────────────────────────────┐  │
│  │ Transcript: parseDocumentMarkers() strips markers → chips     │  │
│  │  ┌──────────────┐                                             │  │
│  │  │ [TXT] file ✕ │  ← clickable, opens viewer modal           │  │
│  │  └──────────────┘                                             │  │
│  └───────────────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────────────┘
                         │
                         ▼
┌───────────────────────────────────────────────────────────────────────┐
│                        DAWN Server                                     │
│                                                                        │
│  ┌───────────────────────────────────────────────────────────────┐    │
│  │ webui_documents.c - HTTP Endpoint Handler                     │    │
│  │                                                               │    │
│  │  POST /api/documents  → Validate ext → Extract → Return      │    │
│  │  POST /api/documents/summarize  → TF-IDF summarize           │    │
│  │  Supported: .txt, .md, .csv, .json, .xml, .yaml, .toml,     │    │
│  │             .c, .h, .py, .js, .html, .css, .sh, .log, etc.  │    │
│  │             .pdf (MuPDF), .docx (libzip+libxml2)             │    │
│  │  Configurable limits (default: 512 KB, up to 5 files)        │    │
│  └───────────────────────────────────────────────────────────────┘    │
│                                                                        │
│  Document text injected into LLM message as:                          │
│  [ATTACHED DOCUMENT: filename (N bytes)]\n...content...\n             │
│  [END DOCUMENT]                                                       │
│  Markers preserved in DB and LLM context; stripped client-side only   │
└───────────────────────────────────────────────────────────────────────┘
```

### Key Components

- **documents.js**: client-side document handling
   - Input methods: file picker button, drag-and-drop, cooperative with vision.js (images vs documents)
   - Client-side text extraction via FileReader API (no server round-trip for text files)
   - Input chip UI: filename, extension badge, size, remove button
   - `parseDocumentMarkers(text)`: regex extraction of document markers from transcript text
   - `openDocumentViewer(filename, content)`: modal viewer with focus trap, Escape/backdrop close
   - Allowed extensions: `.txt`, `.md`, `.csv`, `.json`, `.xml`, `.yaml`, `.yml`, `.toml`, `.ini`, `.cfg`, `.conf`, `.log`, `.c`, `.h`, `.cpp`, `.py`, `.js`, `.ts`, `.html`, `.css`, `.sh`, `.sql`, `.env`, `.pdf`, `.docx`
   - PDF and DOCX sent to server for extraction (no client-side read); text files read client-side
   - Toast feedback for unsupported file types on drop

- **webui_documents.c/h**: server-side upload handling
   - `POST /api/documents`: multipart form upload, extension validation, text extraction
      - Plain text/source files: UTF-8 read directly
      - PDF: MuPDF extraction (`fz_try`/`fz_catch` for error safety, page count cap)
      - DOCX: libzip + libxml2 parse of `word/document.xml` (XXE prevention, ZIP bomb limits)
      - HTML: routed through `html_parser.c` to markdown
   - `POST /api/documents/summarize`: TF-IDF auto-summarize for documents > 8,000 chars
   - Returns `{filename, content, size, estimated_tokens}` JSON response; large docs include `auto_summary`
   - Authentication required (uses session validation)

- **transcript.js** (document integration):
   - Document markers stripped BEFORE routing logic in `addTranscriptEntry()` to prevent misrouting when document content contains `<command>` tags
   - `createDocumentChips()`: renders clickable chips in conversation entries
   - History replay: `prependTranscriptEntry()` also parses markers for saved conversations

- **documents.css**: styling for input chips, transcript chips, and viewer modal
   - Transcript chips: squarer (6px radius) document-like appearance
   - Viewer modal: 700px panel, monospace `<pre>`, mobile full-screen at 480px

### Security Measures

- **textContent only**: document content rendered via `textContent`, never `innerHTML`
- **DOMPurify**: markdown-rendered message text sanitized before DOM insertion
- **CSP header**: `script-src 'self'` blocks any inline scripts in document content
- **Extension allowlist**: only known text extensions accepted; binary files rejected
- **Size limit**: configurable per file (default 512 KB, max 10 MB) enforced at both client and server
- **PDF safety**: `fz_try`/`fz_catch` wraps all MuPDF calls; page count capped to prevent DoS on large documents
- **DOCX safety**: XXE prevention (no external entity resolution in libxml2); ZIP bomb protection via entry size limits
- **Magic bytes validation**: PDF (`%PDF-`) and DOCX (ZIP `PK\x03\x04`) headers verified before extraction
