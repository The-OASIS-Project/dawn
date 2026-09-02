# Memory Subsystem

Source: `src/memory/`, `include/memory/`

Part of the [D.A.W.N. architecture](../../../ARCHITECTURE.md) — see the main doc for layer rules, threading model, and lock ordering.

---

**Purpose**: Persistent memory system for user facts, preferences, conversation summaries, entity graph, and semantic embeddings.

## Architecture: Sleep Consolidation Model + Entity Graph + Dynamic Context Injection

Memory extraction happens at session end, not during conversation — zero latency to conversations while building a persistent user profile. The entity graph captures people, places, pets, projects, and their relationships. Per-turn dynamic context injection (May 2026, Phase 1) ranks facts / entities / relations / summaries / document chunks / calendar events for the current user input and prepends them to the LLM prompt as a focus block, so the model doesn't have to call the memory tool for routine recall. When the model *does* need to reach for something explicitly, a unified cross-source **`recall`** tool (June 2026) runs the same ranking pipeline on demand via `focus_compose_ex()`, returning the best matches across all of those sources in one call instead of separate memory / document / calendar lookups.

```
┌───────────────────────────────────────────────────────────────────────┐
│                    DURING CONVERSATION                                 │
├───────────────────────────────────────────────────────────────────────┤
│  • Full conversation in LLM context window                            │
│  • Core facts + preferences + entity graph pre-loaded at session start│
│  • Memory tool available for explicit remember/search/forget          │
│  • Hybrid search: keyword + semantic similarity via embeddings        │
│  • Zero extraction overhead                                           │
└───────────────────────────────────────────────────────────────────────┘
                                │
                                │ Session ends (WebSocket disconnect/timeout)
                                ▼
┌───────────────────────────────────────────────────────────────────────┐
│                    SESSION END EXTRACTION                              │
├───────────────────────────────────────────────────────────────────────┤
│  • Load conversation messages from database                           │
│  • Build extraction prompt with transcript + existing profile         │
│  • Existing entities fed into prompt to prevent duplicates            │
│  • Call extraction LLM (can differ from conversation model)           │
│  • Parse JSON: facts, preferences, corrections, summary,             │
│    entities, and relations                                            │
│  • Store in SQLite (skip if conversation marked private)              │
│  • Generate embeddings for new facts and entities                     │
│  • Runs in background thread (non-blocking)                           │
└───────────────────────────────────────────────────────────────────────┘
```

## Key Components

- **memory_types.h**: Data structures
   - `memory_fact_t`, `memory_preference_t`, `memory_summary_t`
   - `memory_entity_t` (name, type, canonical_name, mention_count, first/last_seen)
   - `memory_relation_t` (subject→relation→object with optional literal values)

- **memory_db.c/h**: SQLite CRUD operations
   - Prepared statements for all memory tables (facts, preferences, summaries, entities, relations, entity-aliases, merge-proposals)
   - Header-only split (May 2026): umbrella `memory_db.h` re-exports `memory_db_entities.h` + `memory_db_embeddings.h` + `memory_db_provenance.h` + `memory_db_aliases.h` via transitive include — external callers compile unchanged
   - Entity upsert with `RETURNING id` (SQLite 3.37.2+) + `memory_db_entity_upsert_at()` variant accepting `first_seen` / `last_seen` overrides for reextract parity (so re-extracting a 2026-Jan conversation in May doesn't stamp every row with today's date)
   - Relation creation with entity FK or literal value; `memory_db_relation_supersede()` auto-closes exclusive relations with `valid_to` (atomic single statement returning `out_old_fact_id` for fact-supersede propagation)
   - Entity search by keyword (LIKE) and by ID; Bundle 2 (May 2026) adds correlated subqueries on `_get_by_name` + `_search` + admin canonical-list aggregating `mention_count` (SUM) / `first_seen` (MIN) / `last_seen` (MAX) across the equivalence class `WHERE id = e.id OR canonical_id = e.id`
   - Bulk relation loading (`memory_db_relation_list_all_by_user()`)
   - Windowed list functions (Bundle 3, May 2026): `memory_db_fact_list_window` + `_summary_list_window` take `(since_ts, until_ts, sort_asc, limit)` — `until_ts=0` sentinel resolves to INT64_MAX. Four new prepared statements (ASC/DESC × fact/summary) back the new `recent` parameters (`limit` / `sort` / `before`). The legacy `_list_since` statements are subsumed by `_list_window` with `until_ts=0` and queued for retirement.
   - Entity listing for extraction prompt dedup
   - Similarity detection for duplicate prevention
   - Access counting with time-gated confidence reinforcement
   - Atomic decay via custom SQLite `powf()` function (no row iteration)
   - Combined stats query (facts + preferences + summaries + entities in one SELECT)

- **memory_db_alias.c**: Soft-alias resolver cascade + Phase 2 auto-merge gate (May 2026)
   - Six-stage cascade: exact-canonical → token-Jaccard candidates → type filter (`thing` carve-out) → embedding cosine → exclusive-relation + contact overlap → composite-band routing
   - Composite scorer: 0.30 name_jaccard + 0.30 embedding_cosine + 0.25 exclusive_relation_overlap + 0.10 contact_field_overlap + 0.05 type_match, +0.10 substring bonus, +0.20 `user_self` bonus, type-veto
   - Two-band routing: composite ≥ `[memory.entity_merge] auto_threshold` (default 0.90) → silent `alias_link`; composite ∈ [`review_threshold`, `auto_threshold`) (default 0.50-0.90) → row in `memory_entity_merge_proposals` for operator review
   - Propose-all-in-band: every Stage-6-scored candidate ≥ `review_threshold` gets its own proposal row (surfaces secondary matches the prior winner-only path silently hid — e.g. "Jonathan Smith" legitimately matching both Jon + Dawn Smith)
   - Longer-canonical AUTO swap: when inbound has more name tokens than the cascade winner AND both are person/pet/place type, swap so the longer form becomes canonical and the shorter form becomes the alias (e.g. "Jonathan Smith" arriving second after "Jon" is already canonical)
   - Auto-promote `user_self`: inline at extraction when a fresh canonical matches `users.real_name`; also lazy sweep on Settings save via `memory_db_entity_auto_promote_user_self_by_real_name()`
   - Single-level alias invariant: `alias_link` refuses if source row already has dependents (other rows pointing to it as `canonical_id`) — bounds equivalence-class depth to 1
   - Reversible: `memory_db_entity_alias_unlink()` clears `canonical_id` back to NULL and stamps `unlinked_at` on the audit row

- **memory_db_provenance.c/h**: Phase B provenance batch readers (May 2026)
   - Four batch source readers: `memory_db_facts_get_sources`, `_relations_get_sources`, `_summaries_get_sources`, `_prefs_get_sources`
   - Each returns parallel `conv_id` / `msg_id_start` / `msg_id_end` arrays for any positive N, auto-chunked at 32 IDs per SQL pass
   - Privacy JOIN against `conversations.is_private` so private conversations don't leak through `with_source` retrievals
   - Provenance-extend semantics: same-conv widen / no-prov adopt / newer replace / older no-op

- **memory_focus_adapters.c/h**: Per-turn focus block (May 2026, Dynamic Context Injection Phase 1)
   - Ranks facts / entities / relations / summaries / document chunks / calendar events / user_content
   - Hybrid `summary_adapter` merges keyword + semantic results by summary id and re-ranks by max-score (Step 3 of summary backfill)
   - Memory-tool double-dip mitigation: prompt nudge in `core/prompt_compose.c` framing tells the LLM the focus block already contains its top hits
   - Source-type tagging via `focus_source.h` taxonomy (INTERNAL / EXTERNAL / USER_CONTENT) routes the injection filter only at the trust boundary
- **memory_citation.c** (Aug 2026, feature-gated on `[memory] citation_enabled`): the citation signal (from RMM, arXiv 2503.08026 — the readout only, not its RL reranker). When on, the focus block numbers each citeable candidate `[M# source]`; the model echoes the ones it used in a trailing `<cited>M1,M7</cited>` tag that `llm_response_finalize` strips (a live stream-strip seam, `text_filter_cited_tags`, keeps it out of the browser mid-stream). Each turn's injected-vs-cited item ids + their `final_score` land in the `memory_citation_audit` table (schema v78) → **injection precision** (are we injecting memory the model actually uses?) via `benchmarks/citation_audit_summary.py`. A `context_citations` WS frame gold-highlights the cited rows in the WebUI Context panel. Log-only today; Phase 2 (citation-only confidence reinforcement) is designed but not built. See `docs/MEMORY_CITATION_DESIGN.md`.

- **memory_fact_search.c/h**: Hybrid-search public surface
   - Extracted from `memory_embeddings.c` (header-only split May 2026)
   - Keyword + cosine + temporal-proximity scoring; category pre-filter; bitemporal `as_of` / `include_historical` handling

- **memory_history_loader.c/h**: Shared message-history loader
   - Used by both `memory_recovery.c` and `memory_summarize_missing.c`
   - Strips inline base64 image data via `strip_image_markers()` before extraction (replaces `[IMAGE:data:image/...]` blocks with `[image]`)
   - `RECOVERY_MIN_TEXT_CHARS = 50` threshold marks truly-empty conversations as up-to-date with no LLM call

- **memory_summarize_missing.c/h**: Summary backfill admin worker (May 2026)
   - `dawn-admin memory summarize-missing` (opcode `ADMIN_MSG_MEMORY_SUMMARIZE_MISSING = 0x90`)
   - Runs the canonical extraction prompt per conversation and stores summary only (for full reprocessing, use `memory reextract`)
   - `SUMMARIZE_HARD_CAP = 1000` bounds admin-driven LLM spend; defaults to `--dry-run`
   - Shared image-strip + history-load with the recovery worker via `memory_history_loader`

- **memory_source_dedup.c**: `source_dedup_set_t` — suppresses re-fetched provenance excerpts within a single retrieval call so multiple facts citing the same `(conv_id, msg_id_start, msg_id_end)` triple only emit one verbatim block

- **memory_embeddings.c/h**: Semantic embedding system
   - Calls shared `embedding_engine` (see [rag.md](rag.md)) for embed/cosine operations
   - Multi-provider support: Ollama, OpenAI, ONNX (configurable in `[memory.embeddings]`)
   - In-memory cache with mutex protection (facts: 1000 cap, entities: 500 cap)
   - Lazy cache loading on first search, invalidated after extraction
   - Cosine similarity search against cached embeddings
   - Hybrid search combining keyword and semantic results with configurable weights
   - Provider implementations: `memory_embed_ollama.c`, `memory_embed_openai.c`, `memory_embed_onnx.c`

- **memory_context.c/h**: Session start context builder
   - `memory_build_context()` builds ~800 token block
   - Loads preferences, top facts by confidence, recent summaries
   - Injected into LLM system prompt

- **memory_extraction.c/h**: Session end extraction
   - Triggered via `memory_trigger_extraction()`
   - Spawns background thread for non-blocking extraction
   - Parses LLM JSON response: facts, preferences, summaries, **entities, and relations**
   - Entity upsert with canonical name normalization
   - Embedding generation for newly created entities (skipped for existing)
   - Existing entity list fed into extraction prompt to prevent duplicate names
   - Respects conversation privacy flag

- **memory_callback.c**: Tool handler for `MEMORY` device type
   - `search`: hybrid keyword + semantic search across all memory tables; optional `category` / `time_range` / `as_of` / `include_historical` / `with_source` params
   - `recent`: windowed retrieval — `query` (lower bound, default `"7d"`, max `"10y"`) + `before` (upper bound) bracket `[now − query, now − before]`; `sort` (`newest` default / `oldest`) + `limit` (1-50). Supports h/m/d/w/y units (Bundle 3, 2026-05-13).
   - `remember`: immediate fact storage with injection filter (`memory_filter_check()`)
   - `forget`: delete a specific fact by numeric ID
   - `merge_entities`: **soft-link** two entities (May 2026) — sets `source.canonical_id = target.id`, both rows preserved, reversible via `memory_db_entity_alias_unlink()`. Replaces the original Phase 6.7 hard-merge for the LLM-facing path; hard merge remains available via `dawn-admin memory entity consolidate`.
   - `save_contact`, `find_contact`, `list_contacts`, `delete_contact`: contact management
   - `append_graph_context()`: entity graph results appended to search output
   - Tool descriptor (canonical source) lives in `src/tools/memory_tool.c` (`memory_params[]`) — 11 params, generated into per-provider schemas by the tool registry

- **memory_filter.c/h**: Injection filter — runs at the trust boundary only (raw user input at ingestion + USER_CONTENT at retrieval); see "Security Guardrails" below
   - Unicode normalization: zero-width/invisible char stripping, homoglyph mapping, Latin-1 accent stripping, fullwidth ASCII mapping, tag character handling
   - ~118 blocked patterns across 17 categories (substring matching on normalized text)
   - ReAct co-occurrence check (blocks when >= 2 of thought:/action:/observation: appear)
   - Called from `memory_callback.c`, `memory_extraction.c`, and `webui_memory.c` before every `memory_db_fact_create()` / `memory_db_pref_upsert()` / entity/relation/summary storage
   - Stateless and thread-safe (pure function, no mutexes)

- **contacts_db.c/h**: Contacts database operations
   - Structured contact info (email, phone, address) linked to `memory_entities` via `entity_id`
   - CRUD: `contacts_add()`, `contacts_find()`, `contacts_update()`, `contacts_delete()`, `contacts_list()`
   - Case-insensitive search with LIKE escape

- **memory_db_entity_merge()**: Transactional **hard** entity merge (legacy Phase 6.7 path, now operator-only)
   - MERGE_EXEC macro for error-checked SQL within a transaction
   - Reassigns relations (both subject and object FKs) and contacts to target entity
   - Deletes self-referential relations created by reassignment
   - Deduplicates via ROW_NUMBER() window function
   - Absorbs mention count and time range from source entity
   - Deletes source row (no rollback — use soft-link `alias_link` if reversibility matters)
   - Exposed via `dawn-admin memory entity consolidate <source> <target>` for hardening a previously soft-linked alias

- **memory_maintenance.c/h**: Nightly decay orchestration
   - Called from auth maintenance thread (15-minute cycle)
   - Hour-gated with 20-hour double-execution guard
   - Per-user: decay facts → decay preferences → prune low-confidence → prune superseded → prune old summaries
   - Configurable rates, floors, and thresholds via `[memory.decay]`

- **memory_recovery.c/h**: Extraction recovery worker
   - Dedicated background thread (`nice 10`) that runs an immediate startup pass and re-scans on `[memory.recovery] recurring_interval_seconds`
   - Picks up conversations whose `last_extracted_msg_count < message_count` after `idle_threshold_seconds` (default 1 h) — typically left behind by a daemon crash mid-extraction or by an extraction that returned an error
   - Skips private conversations and rows with fewer than 2 messages (matches `memory_trigger_extraction()`'s own gate)
   - Per-conversation tracking: `extraction_attempts` (cap defaults to 2; `0` = unlimited) and `extraction_last_attempt_at`. New activity on the conversation auto-resets the counter via the `extraction_last_attempt_at < updated_at` clause in the scan SQL
   - Reuses `memory_trigger_extraction()`. Successful extraction clears the recovery counters atomically through the existing `memory_db_set_last_extracted` UPDATE
   - Sequentially processes one stuck conversation at a time, blocking on `memory_extraction_in_progress()` (5-minute per-conv timeout) so the per-user extraction slot never queues

- **memory_embed_recompute.c/h**: Embedding recomputation worker
   - Detects model swaps via `system_metadata.embedding_model_id` vs `g_config.memory.model_id`
   - On mismatch, launches a background thread (`nice 10`) that re-embeds all `memory_facts` then `memory_entities` per user, followed by a deferred `document_chunks` pass
   - Per-user gate: `users.embeddings_model_id` is set only after both the facts and entities passes succeed — a crash between passes leaves it NULL and triggers a full retry on next start
   - `system_metadata.embedding_model_id` is updated only after the chunks pass completes, so an interrupted chunk pass also retries cleanly
   - Configured via `[memory.embeddings]` `model_id`, `recompute_on_model_change`, `recompute_batch_size`, `recompute_batch_sleep_ms`

## Database Schema

Memory tables in the auth database (`/var/lib/dawn/auth.db`). Schema version **v46** at time of writing — see [atlas/dawn/memory/SYSTEM_DESIGN.md §6](https://github.com/The-OASIS-Project/atlas/blob/main/dawn/memory/SYSTEM_DESIGN.md#6-storage-schema) for the full column-by-column definitions including provenance triples, bitemporal relation bounds, fact categories, summary embeddings, and the entity-alias surface.

```sql
-- Facts: discrete pieces of information (v34 category, v40 source_* triple)
memory_facts (id, user_id, fact_text, confidence, source, created_at,
              last_accessed, access_count, superseded_by, normalized_hash,
              embedding, embedding_norm, category,
              source_conversation_id, source_msg_id_start, source_msg_id_end)

-- Preferences: communication style preferences (v42 source_* triple)
memory_preferences (id, user_id, category, value, confidence, source,
                    created_at, updated_at, reinforcement_count,
                    source_conversation_id, source_msg_id_start, source_msg_id_end)

-- Summaries: conversation digests (v42 source_* triple, v45 embedding)
memory_summaries (id, user_id, session_id, summary, topics, sentiment,
                  created_at, message_count, duration_seconds, consolidated,
                  source_conversation_id, source_msg_id_start, source_msg_id_end,
                  embedding)

-- Entities: people, places, pets, projects, etc. (v43 alias + identity columns)
memory_entities (id, user_id, name, entity_type, canonical_name,
                 embedding, embedding_norm, photo_id,
                 first_seen, last_seen, mention_count,
                 canonical_id, is_user_self)
   UNIQUE(user_id, canonical_name)
   canonical_id → memory_entities(id) ON DELETE SET NULL  -- NULL = self is canonical
   UNIQUE INDEX (user_id) WHERE is_user_self = 1          -- one self per user

-- Entity aliases: append-only audit log (v43)
memory_entity_aliases (id, user_id, source_entity_id, target_entity_id,
                       source_canonical_name, target_canonical_name,
                       link_kind, reason, composite_score, evidence_json,
                       linked_at, consolidated_at, unlinked_at, unlink_reason)

-- Entity merge proposals: review-band staging for Phase 2 auto-merge gate (v43)
memory_entity_merge_proposals (id, user_id, source_entity_id, target_entity_id,
                               composite_score, evidence_json,
                               proposed_at, resolved_at, resolution)

-- Relations: entity-to-entity or entity-to-literal (v33 valid_from/to, v42 source_*)
memory_relations (id, user_id, subject_entity_id, relation, object_entity_id,
                  object_value, fact_id, confidence, created_at,
                  valid_from, valid_to,
                  source_conversation_id, source_msg_id_start, source_msg_id_end)
   FK subject_entity_id → memory_entities(id)
   FK object_entity_id → memory_entities(id) (nullable, literal if NULL)

-- Contacts: structured contact info linked to entities
contacts (id, user_id, entity_id, field_type, value, label, created_at)
   FK entity_id → memory_entities(id)
   field_type: "email", "phone", "address"
   label: "work", "personal", "mobile", "home", "other", NULL

-- Users (v44 user-identity columns relevant to memory)
users (..., real_name, preferred_address, identity_aliases, embeddings_model_id, ...)
```

Schema-version highlights since v33:
- **v33** — `memory_relations.valid_from/valid_to` (bitemporal); auto-close via `memory_db_relation_supersede`
- **v34** — `memory_facts.category` + `users.categories_backfilled_at`
- **v37** — `document_chunks.created_at` for temporal scoring
- **v38** — `summary_nodes` (LCM Phase-4 hierarchical compaction DAG)
- **v39** — `conversations.extraction_attempts` + `extraction_last_attempt_at` for recovery worker
- **v40** — provenance triples on `memory_facts` (+ `last_extracted_msg_id` on conversations)
- **v41** — `system_metadata` table + `users.embeddings_model_id`
- **v42** — `conversations.anchor_date` (cat-2 temporal extraction) + `source_*` triples extended to preferences / summaries / relations (Phase B)
- **v43** — entity alias surface (`memory_entities.canonical_id` + `is_user_self` + `memory_entity_aliases` + `memory_entity_merge_proposals` + 5 partial indexes)
- **v44** — `users.real_name` / `preferred_address` / `identity_aliases` (entity-merge link-user-self synthetic-seed)
- **v45** — `memory_summaries.embedding` (semantic summary search)
- **v46** — `UPDATE users SET embeddings_model_id = NULL` (forces summary embedding backfill on next boot)
- **v58** — `memory_facts.expires_at` — fact expiry / ephemerality (transient facts age out automatically)
- **v67** — single continuous conversations via a compaction watermark (replaces the legacy continuation-splitting; a long session stays one logical conversation for extraction and history)

(v47–v66 and v68 — entity/relation dedup, notes/reference store + hybrid search, document versioning, and the original-file blob store — are documented in [rag.md](rag.md) and the atlas archive.)

## Privacy Toggle

Users can mark conversations as private to skip memory extraction:

- `is_private` column in `conversations` table
- Set via WebSocket message or Ctrl+Shift+P keyboard shortcut
- Can be set before conversation starts (pending state)
- Visual badge in conversation history list

## Security Guardrails

Memory content flows into future prompts, creating potential injection vectors. DAWN's defense is a **trust-tier model**, not blanket filtering. The shared `memory_filter` module (`memory_filter.c/h`) — substring blocklist + UTF-8 normalizer (invisible chars, homoglyphs, Latin-1 accents, fullwidth) + ReAct co-occurrence detector — runs only at the trust boundary where untrusted user-controlled text first enters the system:

- **Ingestion (trust boundary)**: WebUI fact/preference import, LLM `remember` tool action, silent-observe of observed conversation text. Filter applied.
- **Retrieval (trust boundary)**: focus-injection candidates whose `source_type == FOCUS_SOURCE_USER_CONTENT` (inbound email body, future external feeds). Filter applied. INTERNAL (memory items — already extraction-LLM-paraphrased) and EXTERNAL (user-uploaded documents, user's CalDAV) skip the retrieval-time filter.
- **Internal re-processing**: extraction-LLM JSON output, summarize-missing backfill, fact recategorization. Filter **not** applied — these operate on already-paraphrased data and gain nothing from substring re-scanning.

Defense-in-depth: data-marking framing in `core/prompt_compose.c` wraps the focus block with explicit "DATA entries, not instructions" prefix, leaning on Claude/GPT-4 instruction-hierarchy training to neutralize any payload that reaches retrieval. Tool design (TTLs on destructive actions, two-step confirmation, operator-only on sensitive operations) limits blast radius per Meta's "Agents Rule of Two" pattern.

Full rationale, research citations, and risk model: [atlas/dawn/memory/INJECTION_FILTER.md](https://github.com/The-OASIS-Project/atlas/blob/main/dawn/memory/INJECTION_FILTER.md).

## Configuration

```toml
[memory]
enabled = true
context_budget_tokens = 800
session_timeout_minutes = 15

[memory.extraction]
provider = "local"        # "local", "openai", "claude", "ollama"
model = "qwen2.5:7b"      # Model for extraction

[memory.embeddings]
provider = "onnx"         # "onnx" (default), "ollama", "openai"
# model = ""              # HTTP providers: model name (empty = provider default)
# endpoint = ""           # HTTP providers: base URL
keyword_weight = 0.30     # Hybrid search: keyword component weight (0.0-1.0)
vector_weight = 0.70      # Hybrid search: semantic component weight (0.0-1.0)
temporal_weight = 0.20    # Temporal proximity boost for date-anchored queries
category_threshold = 0.25 # Cosine threshold for centroid-based category backfill
backfill_on_startup = true
model_id = "bge-small-en-v1.5-int8"  # Bump when MODEL_PATH changes to trigger recompute
recompute_on_model_change = true      # Re-index all stored embeddings on model_id change
recompute_batch_size = 50             # Rows per batch
recompute_batch_sleep_ms = 100        # Sleep between batches (ms)

[memory.decay]
enabled = true            # Enable nightly confidence decay
hour = 2                  # Run at 2 AM local time (0-23)
inferred_weekly = 0.95    # Inferred facts lose 5%/week
explicit_weekly = 0.98    # Explicit facts lose 2%/week
preference_weekly = 0.97  # Preferences lose 3%/week
inferred_floor = 0.0      # Inferred facts can decay to zero
explicit_floor = 0.50     # Explicit facts never below 50%
preference_floor = 0.40   # Preferences never below 40%
prune_threshold = 0.25    # Delete facts below this confidence
summary_retention_days = 30
access_reinforcement_boost = 0.05  # +5% on access (1-hour cooldown)

[memory.recovery]
enabled = true                      # Re-extract conversations stuck behind crashes/failures
idle_threshold_seconds = 3600       # Treat as stuck after 1 h of inactivity
max_attempts = 2                    # Cap retries per conversation (0 = unlimited)
recurring_interval_seconds = 86400  # Daily rescan (0 = startup-only)

[memory.entity_merge]
enabled = true             # Phase 2 auto-merge gate at extraction
auto_threshold = 0.90      # Composite score for silent alias_link
review_threshold = 0.50    # Composite score floor for review-band proposal

[memory.focus_injection]
top_k = 12                 # Per-turn focus block size (May 2026 tuned default)
weight_recency = 0.15      # Recency contribution; tension with summary-relevant
                           # evidence documented in config_defaults.c
```

`[memory.entity_merge]` (May 2026): controls the Phase 2 auto-merge gate that runs at extraction time after the relations loop. Composite ≥ `auto_threshold` → silent `alias_link`; composite ∈ [`review_threshold`, `auto_threshold`) → row in `memory_entity_merge_proposals` for operator review. `enabled=false` disables both bands; the entity-merge cascade still runs for `dawn-admin memory entity link-user-self` and operator-driven WebUI merges.

`[memory.focus_injection]` (May 2026): controls the per-turn focus block. `top_k=12` (up from 8) leaves slots for the new semantic summary adapter — at 8 the summary adapter starved out of the block on facts-heavy turns. `weight_recency=0.15` is Phase 1j bench-tuned; live observation suggests 0.3 ranks recent summary matches above legacy paraphrase-ladder facts, but re-bench with summary-relevant probes is pending before promoting 0.3 (open tension documented in `config_defaults.c`).

## WebUI Memory Viewer

The memory viewer provides a browser-based interface for inspecting and managing all memory types:

- **Tabs**: Facts, Preferences, Summaries, Graph (entities), Contacts
- **Stats bar**: real-time counts for each memory type including contacts; entity counts now reflect equivalence-class totals across canonical + aliases (Bundle 2, May 2026)
- **Search**: filter memories by keyword across all tabs
- **Graph tab**: entity cards with type badges, expandable relations (→ outgoing, ← incoming), contact count badge on person entities, two-click entity merge (select source → click target → confirm — defaults to soft `alias_link`; operator hardens via dawn-admin). Entity `first_seen` / `last_seen` now span the original conversation timespan instead of "today" (Bug 2 follow-up to Bundle 1 — `memory_db_entity_upsert_at()` accepts `first_seen` / `last_seen` overrides so reextract preserves natural temporal spread).
- **Suggested-Merges panel** (Phase 2, May 2026): pending entity-merge proposals from `memory_entity_merge_proposals`. Approve writes the soft alias via `memory_db_entity_alias_link()`; reject just stamps `resolved_at`.
- **Memory-icon dot indicator** (Phase 2): mirrors the music-playing dot pattern, pulses 0.5s out of phase, lights when proposals are pending, `prefers-reduced-motion` carve-out suppresses both this dot's pulse and the music dot's pulse. First click while pending auto-routes to the Graph tab with a 600 ms accent-glow flash so the user's eye lands on why they're not on Facts; subsequent clicks respect whichever tab they had active.
- **Contacts tab**: contact cards with field_type/label badges, hover-reveal edit/delete, search, pagination. Add/edit modal with entity typeahead. Cross-linked from Graph tab person entities.
- **Delete**: per-item delete with confirmation, bulk "Forget Everything"
- **Import / Export**: transfer memories between DAWN instances or other AI assistants
- **Keyboard accessible**: tabindex, ARIA roles, Enter/Space activation, `aria-label` updates alongside `title` so screen-reader users learn about pending proposals
- **Endpoints**: `GET /api/memory/{facts,preferences,summaries,entities,stats}`, `DELETE /api/memory/{facts,preferences,summaries,entities}/:id`

## Memory Import / Export

Users can export their memories for backup or transfer, and import memories from other AI assistants (Claude, ChatGPT) or from a previous DAWN export.

**Export formats**:

- **DAWN JSON** (`dawn_memory` format, version 1): lossless export including facts, preferences, entities with relations, confidence scores, sources, and timestamps. Suitable for backup/restore between DAWN instances.
- **Human-readable text**: markdown-formatted list of facts and preferences. Portable — can be pasted into any AI assistant.

**Import sources**:

- **DAWN JSON**: direct restore from a previous export. Preserves metadata (confidence, source, timestamps).
- **Plain text**: one fact per line (bullets and markdown headers auto-stripped). Each line becomes a fact with `confidence=0.7`, `source="import"`. Supports paste or file upload.

**Deduplication**: import uses a two-stage duplicate detection pipeline:

1. **Hash check**: `memory_normalize_and_hash()` for O(1) exact duplicate detection via FNV-1a hash.
2. **Jaccard similarity**: fuzzy matching (threshold 0.7) catches paraphrased duplicates.

**Preview mode**: import runs in preview-then-commit workflow. The first request (`commit=false`) returns a preview of what will be imported (new items, duplicates skipped). The user reviews and confirms before the second request (`commit=true`) writes to the database.

**WebSocket messages**: `export_memories` / `export_memories_response`, `import_memories` / `import_memories_response`.

## Data Flow (Memory Lifecycle)

```
1. Session Start (WebSocket connect)
   ↓
2. Load user profile: memory_build_context(user_id)
   ↓
3. Inject facts/preferences/entity names into LLM system prompt
   ↓
4. During conversation:
   - User: "Remember I'm vegetarian" → memory_remember() → immediate storage
   - User: "What do you know about me?" → hybrid_search() → keyword + semantic
   - Search also returns entity graph context (ENTITIES section)
   ↓
5. Session End (WebSocket disconnect/timeout)
   ↓
6. Check privacy flag: if private, skip extraction
   ↓
7. memory_trigger_extraction() → background thread
   ↓
8. Load conversation, build extraction prompt (includes existing entity names)
   ↓
9. Call extraction LLM, parse JSON response
   ↓
10. Store new facts, update preferences, save summary
   ↓
11. Upsert entities (canonical name dedup), create relations
   ↓
12. Generate embeddings for new facts and entities (provider-specific)
   ↓
13. Invalidate embedding caches (once, not per-item)

--- Nightly Maintenance (runs at configured hour) ---

14. memory_run_nightly_decay() called from auth maintenance thread
   ↓
15. For each user: apply confidence decay (atomic SQL with powf())
   ↓
16. Prune facts below threshold (audit logged), prune old summaries
   ↓
17. Accessed facts reinforced (+0.05, time-gated to 1-hour cooldown)
```
