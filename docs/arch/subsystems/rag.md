# Document Search / RAG Subsystem

Source: `src/tools/document_*.c`, `src/core/embedding_engine.c`, `src/webui/webui_doc_library.c`

Part of the [D.A.W.N. architecture](../../../ARCHITECTURE.md) — see the main doc for layer rules, threading model, and lock ordering.

---

**Purpose**: Retrieval-Augmented Generation — upload documents, chunk and embed them, then search or read them via LLM tools.

## Architecture: Shared Embedding Engine + Per-Document Chunking + Dual Tool Interface

```
┌───────────────────────────────────────────────────────────────────────┐
│                     DOCUMENT INGESTION                                 │
├───────────────────────────────────────────────────────────────────────┤
│  WebUI upload (drag-and-drop / file picker)                           │
│  → webui_doc_library.c receives file                                  │
│  → document_chunker.c: paragraph split → sentence split → overlap     │
│  → embedding_engine_embed() per chunk (ONNX/Ollama/OpenAI)           │
│  → document_db.c: store document + chunks + embeddings in SQLite      │
│  → SHA-256 dedup prevents re-indexing identical files                  │
└───────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌───────────────────────────────────────────────────────────────────────┐
│                     LLM TOOL ACCESS                                    │
├───────────────────────────────────────────────────────────────────────┤
│                                                                        │
│  document_search(query)        document_read(document, start, count)  │
│  ├─ Embed query                ├─ Find document by name/ID            │
│  ├─ Load all user chunks       ├─ Load chunks in order (paginated)    │
│  ├─ Cosine similarity rank     ├─ Return text with pagination hint    │
│  ├─ Keyword boost (top 50)     └─ LLM calls again for next page      │
│  └─ Return top 5 with scores                                          │
│                                                                        │
└───────────────────────────────────────────────────────────────────────┘
```

## Key Components

- **embedding_engine.c/h** (`src/core/`): Shared embedding infrastructure
   - Extracted from `memory_embeddings.c` — used by both memory and document systems
   - Multi-provider: ONNX local (`bge-small-en-v1.5-int8`), Ollama, OpenAI-compatible
   - Thread-safe embed() with mutex protection
   - ARM NEON vectorized dot product for cosine similarity
   - `embedding_engine_init()`, `embedding_engine_embed()`, `embedding_engine_cosine()`

- **document_db.c/h** (`src/tools/`): SQLite CRUD for documents and chunks
   - Uses shared `auth.db` handle with prepared statements
   - `document_db_create()`, `document_db_delete()`, `document_db_list()`
   - `document_db_find_by_name()`: fuzzy name lookup with LIKE + exact-match priority
   - `document_db_chunk_create()`, `document_db_chunk_read()`, `document_db_chunk_search_load()`
   - Access control: queries filter by `user_id = ? OR is_global = 1`

- **document_chunker.c/h** (`src/tools/`): Text chunking for embedding
   - Paragraph-aware splitting (double newline boundaries)
   - Sentence boundary splitting for oversized paragraphs
   - Configurable target/max token sizes and overlap
   - **Structure-aware chunking** for YAML sequences and CSV: one record per chunk, so a search hit maps to a single record instead of a windowed span

- **document_search.c/h** (`src/tools/`): Semantic search tool
   - Two-pass scoring: cosine similarity first, keyword boost on top 50 only
   - Results formatted with source citations and relevance scores
   - Uses `result_extended` for results exceeding the 8KB fixed buffer

- **document_grep_tool.c** (`src/tools/`): Literal substring search over indexed document/note chunks
   - Returns the matched line plus adjacent context (not meaning-based) for exact lookups where semantic search is too fuzzy
   - Gap-safe range read (`document_db_chunk_read_range`, `BETWEEN chunk_index`) so an embed-failure gap in the chunk index doesn't misalign results
   - Unindexed substring scan — see the FTS fast-path follow-up in `docs/TODO.md` for the large-corpus ceiling

- **document_read.c/h** (`src/tools/`): Paginated document reader
   - Parameters: `document` (name), `start_chunk` (offset), `count` (page size, max 20)
   - Returns ordered chunk text with pagination hint for next page
   - Enables full-document summarization via iterative reading

- **webui_doc_library.c** (`src/webui/`): WebUI Document Library panel
   - Upload with file type validation (PDF, DOCX, TXT, MD)
   - Per-user document count limits (configurable)
   - List, delete, index, and toggle-global WebSocket endpoints
   - Admin "All Users" view with username resolution (JOIN on users table)
   - Global visibility toggle: owner or admin can share/unshare documents
   - Audit logging for admin cross-user operations
   - `conn_check_admin_quiet()` for soft-fallback admin checks (no error response)

- **doc-library.js / doc-library.css** (`www/`): WebUI frontend
   - Drag-and-drop upload with progress indicator
   - Document list with type badges, chunk counts, delete confirmation
   - Per-document "My own text" switch on the user's own documents (see below)
   - Admin controls: "All Users" toggle, global upload checkbox, per-document globe toggle
   - Owner badge (username) on documents in admin "All Users" view
   - SVG globe icon with filled/stroke states for global visibility
   - Focus trap and keyboard focus-visible styles for accessibility
   - Mobile-friendly: bottom-sheet layout, 44px+ touch targets

## Database Schema

Two tables in `auth.db`:

```sql
documents (id, user_id, filename, filepath, filetype, file_hash,
           num_chunks, is_global, created_at, original_blob_id, outside, own)

document_chunks (id, document_id, chunk_index, text, embedding, embedding_norm)
```

Indexes: `idx_doc_chunks_doc`, `idx_documents_user`, `idx_documents_hash`.

### Whose words a document holds

Two columns, kept apart (v114).  `documents.outside` (`outside_kind_t`) says where a document's
text came from: a note starts NONE; every other document (an upload, an attachment, saved text)
starts DOCUMENT; a research report is RESEARCH; a note written in a turn that read outside text
carries that text's kind.  `documents.own` says the user vouched for its text as their own words.
A chunk focus recalls into a turn counts as the user's words only when the reader owns the
document and either `own` is 1 or its kind is NONE (or UNKNOWN) (`OUTSIDE_OWN_SQL`,
`document_is_own`); any other chunk is marked outside text, so the memory batch that turn feeds
is tainted ("document") and can't overwrite what the user told DAWN.

The user sets the vouch with the panel's **My own text** switch (`set_document_own`,
`document_db_set_own`).  On sets `own` and keeps the kind, so a research report or an emailed
attachment still says what it is.  Off clears `own`, and a document with no recorded kind (NONE
or UNKNOWN) becomes DOCUMENT, since the user says it isn't their words; any other kind stays.
Only the user sets it; `document_manage` has no action for it, since a model steered by outside
text must not be able to vouch for that text.  A save, edit, append, recover or undo by the model
keeps the stored kind combined with what the turn read, and in a turn that read outside text also
clears `own` (the user vouched for the old text, not the new); a write in a clean turn, and an
edit or restore in the panel, leave `own` alone.  A version keeps its document's kind and gives
it back on restore, but not the vouch: a deleted document recovered comes back with `own` 0.

The document tools decide the same way.  `document_search`, `document_read` and `document_grep`
return their text unframed when every document in the result is the reader's own words
(`document_is_own`; a grep hit carries `own`, a ranked chunk `is_own`), and otherwise put the
whole result in a `DOCUMENT CONTENT` frame (`llm_tools_result_third_party`), which memory
extraction reads as a document and sees only as a stub.  The tools declare `TOOL_RESULTS_OWN`, so
an unframed result leaves the batch clean; `document_manage` returns only names and confirmations
and is own too.  `document_index` stays outside (it reports on a fetched page).

## Configuration

```toml
[documents]
chunk_target_tokens = 500
chunk_max_tokens = 1000
chunk_overlap_tokens = 50
max_search_results = 5
max_context_tokens = 2000
max_file_size_mb = 10
max_index_size_kb = 2048       # Per-document index size limit
max_indexed_documents = 50     # Per-user document count limit

# Original-file retention (v68): keep the raw uploaded PDF/DOCX alongside the
# extracted text so it can be downloaded (and re-processed later, e.g. image
# extraction). Stored in the generic blob store; FK from documents.original_blob_id.
originals_enabled = true
original_retention_days = 0          # 0 = keep forever
max_original_size_mb = 10
max_originals_per_user = 200
max_originals_total_mb_per_user = 2048
```

See `dawn.toml.example` for the full `[documents]` block, including the hybrid-search
weights (below).

## Related: Notes store, hybrid lexical search, versioning

The same `document_db` / `embedding_engine` foundation also backs the **notes / reference-text
store** (`DOC_KIND_NOTES`), which adds a **hybrid lexical (BM25/FTS5 + Porter2 stemming) +
semantic** retrieval channel, surgical edit/append, and version history with one-step undo
(`document_versions`). Schema evolved across v61 (hybrid + notes), v62 (versioning), v63
(multi-chunk edit), and v68 (original-file blob store). Full design:
[atlas/dawn/archive/NOTES_REFERENCE_STORE_DESIGN.md](https://github.com/The-OASIS-Project/atlas/blob/main/dawn/archive/NOTES_REFERENCE_STORE_DESIGN.md).
