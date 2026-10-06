# Lossless Context Management (LCM)

Inspired by the LCM paper (Ehrlich & Blackman, 2026). Four phases to make context compaction invisible, reliable, and lossless. All four phases shipped April 2026.

> **Superseded in part (September 2026).** Compaction now happens only at a turn seam, never inside a tool
> round, because a provider binds a turn's reasoning to the request as it stands. The old in-loop async
> compaction, the mid-loop and pre-switch compaction, the `[COMPACTED ...]` tag and the separate watermark
> write described below are gone; instead a range is summarized ahead on a per-session worker and applied
> at a turn seam: see `src/core/session_compaction.c` and the Compaction section of
> [arch/subsystems/llm.md](arch/subsystems/llm.md). The escalation levels (Phase 1), summary nodes and
> `context_expand` drill-down still stand.

---

## Phase 1: Three-Level Summarization Escalation (Shipped)

Replaced single-shot LLM summarization with a three-level escalation loop that guarantees convergence.

### Levels

| Level | Strategy | LLM Call? |
|-------|----------|-----------|
| L1 (Normal) | "100 words or less" detailed summary | Yes |
| L2 (Aggressive) | "5 bullets max" compressed summary | Yes |
| L3 (Deterministic) | Mechanical truncation (~80 chars/msg, 728-byte budget) | No |

### Key implementation details

- **Escalation loop** in `llm_context_compact()` replaces the old single-shot path. Calculates a target = `context_size * (compact_hard_threshold - 0.20)` with floor at 30%. Loops L1→L2→L3 until `kept_tokens + summary_tokens <= target`.
- **Size-gate**: if L1 summary is longer than the input it summarized, skips L2 and goes directly to L3 (model clearly not following instructions).
- **Dynamic prompt buffer** in `compact_with_llm()` — replaces the old 8192-byte stack buffer that silently truncated large conversations. Fixes a pre-existing bug for 128K+ context models.
- **Data-marking delimiters** (`---BEGIN/END-CONVERSATION-d8a3f1e7---`) around conversation JSON in summarization prompts reduce prompt injection risk.
- **`estimate_tokens_range()`** — range-based token estimation. `llm_context_estimate_tokens()` delegates to it. Used for both escalation decisions and kept-token calculations.
- **Compaction level** plumbed through `llm_compaction_result_t.level` → `webui_send_compaction_complete()` → WebUI JSON payload.
- **O(n) history clearing** — delete from end instead of front (was O(n^2)).
- **26 unit tests** in `tests/test_context_compaction.c` covering deterministic compaction, target calculation, token estimation, and level ordering.

### Files modified

`include/llm/llm_context.h`, `src/llm/llm_context.c`, `include/webui/webui_internal.h`, `include/webui/webui_server.h`, `src/webui/webui_server.c`, `www/js/ui/history.js`, `tests/test_context_compaction.c`, `tests/test_context_compaction_impl.c`, `tests/CMakeLists.txt`

---

## Phase 2: Async Dual-Threshold Compaction (Shipped)

Background compaction between turns eliminates the "Compacting my memory" pause for WebUI sessions.

### Thresholds

| Threshold | Default | Behavior |
|-----------|---------|----------|
| Below soft | < 60% | No compaction |
| Soft | 60% | Async: deep-copy history, compact in background thread, merge next turn |
| Hard | 85% | Blocking synchronous compaction (safety net) |

Session 0 (local mic) skips async — keeps the blocking TTS feedback path.

### State machine (per-session, embedded in `session_t`)

```
IDLE ──(soft threshold after turn)──> RUNNING
RUNNING ──(bg thread completes)──> READY
READY ──(merge before next LLM call)──> IDLE
```

### Key implementation details

- **`async_compaction_t`** struct embedded in `session_t` with `_Atomic int state`, joinable `pthread_t`, inlined result fields (avoids circular include with `llm_compaction_result_t`), snapshot pointer + message count for merge validation, 60-second cooldown timer.
- **Deep copy** via `json_object_deep_copy()` under brief `history_mutex` hold (faster than serialize+reparse, half the peak memory).
- **Joinable thread** (not detached) with 256KB stack. `pthread_join` in both `session_destroy()` and `async_merge()` — no use-after-free regardless of LLM call duration.
- **Cancel flag**: `llm_set_cancel_flag(&session->disconnected)` in background thread aborts in-flight CURL transfer on disconnect.
- **Command context**: `session_set_command_context(session)` in background thread routes the LLM call to the session's provider, not the global daemon config.
- **Partial merge** preserves post-snapshot messages: saves refs to messages added between snapshot and merge, clears history, copies compacted result, re-appends saved messages. Heap-allocated (not VLA) for safety.
- **Trigger** in `llm_interface.c` after `llm_tool_iteration_loop()` returns (both call sites). **Merge** in `llm_tool_loop.c` before each auto-compact check.
- **Dedicated compaction provider** (`compact_use_session`, `compact_provider`, `compact_model` config) routes summarization to a fast cloud model (e.g., Haiku) while the conversation uses a local LLM. Eliminates local-LLM contention. WebUI settings: checkbox + provider/model fields that show/hide via `showWhen`.
- **Backward compat**: if only legacy `summarize_threshold` is set, derives `hard = summarize_threshold`, `soft = max(hard - 0.25, 0.30)`. Detection via `has_summarize` boolean (not float equality).
- **Config validation**: soft (0.05–0.90), hard (0.10–0.95), soft < hard invariant, `compact_provider` allowlist (claude/openai/gemini/local).
- **Ref leak fixes** in `llm_context_auto_compact_with_config()` and `llm_context_compact()` error paths — `session_get()` without matching `session_release()`.

### Files modified

`include/config/dawn_config.h`, `include/core/session_manager.h`, `include/llm/llm_context.h`, `include/webui/webui_internal.h`, `include/webui/webui_server.h`, `src/config/config_defaults.c`, `src/config/config_parser.c`, `src/config/config_validate.c`, `src/config/config_env.c`, `src/core/session_manager.c`, `src/llm/llm_context.c`, `src/llm/llm_tool_loop.c`, `src/llm/llm_interface.c`, `src/webui/webui_config.c`, `src/webui/webui_server.c`, `www/js/ui/history.js`, `www/js/ui/settings/schema.js`

### Live test results (April 27, 2026)

- **Cloud (Haiku) dedicated provider**: Async trigger → L1 summary in 2-3 seconds → merge on next message. Zero user-visible delay.
- **Local (Qwen 35B-A3B) as session + compaction provider**: L1 and L2 both timed out (180s each, local model contended with foreground conversation), L3 deterministic caught it. 6 minutes background wall time, but user was never blocked.
- **Local session + Haiku compaction provider**: L1 summary in 3 seconds. No contention. This is the recommended configuration for local LLM users.

---

## Phase 3: Lossless Pointers + Context Expand Tool (Shipped)

Compaction is non-destructive: the summary embeds a structured `[COMPACTED conv=N msgs=X-Y node=Z depth=D]` tag, and the `context_expand` tool retrieves original messages from the database on demand.

### Key implementation details

- **Message ID resolution at compaction time** — queries the DB via `conv_db_get_message_ids()` to map in-memory array indices to database message IDs. No changes to the message insertion pipeline. System prompt offset corrected (system prompt is in memory but not in DB).
- **Summary wrapper format**: `[COMPACTED conv=N msgs=X-Y node=Z depth=D] Previous conversation summary: ...` — structured prefix is unambiguous for any LLM to parse. Falls back to `[Previous conversation summary: ...]` when no DB conversation is available (local mic path).
- **`conv_id` parameter** added to `llm_context_compact()` signature. All callers updated. `trigger_conv_id` captured at async trigger time to avoid reading stale connection state from the background thread.
- **`webui_get_active_conversation_id()`** helper — resolves conv_id from session's WebSocket connection without coupling `llm_context.c` to WebUI internals.
- **`conv_db_get_message_ids()`** — returns ordered array of message IDs for a conversation. Dynamic array with realloc growth.
- **`conv_db_get_messages_by_range()`** — retrieves messages filtered by ID range with explicit ownership pre-check returning `AUTH_DB_FORBIDDEN` (not silent empty result like the JOIN-only approach).
- **`context_expand` tool** — new modular tool registered via `cmake/DawnTools.cmake`. All params optional: use `start_id`/`end_id` for raw messages, or `node_id` alone for hierarchical summaries (Phase 4). Token budget hardcoded at 4000. Range cap at 500 messages.
- **Continuation handling**: `conversation_id` from the `[COMPACTED]` tag points to the parent conversation. If omitted, the tool checks `continued_from` on the current conversation to find the parent. *(Legacy as of Phase 5: new conversations are no longer forked, so the marker's `conv=N` is the same conversation and the `continued_from` fallback only fires for pre-v67 chains.)*
- **`note_len` buffer** increased to `strlen(summary) + 256` for the longer COMPACTED prefix.

### Files modified

`include/auth/auth_db.h`, `src/auth/auth_db_conv.c`, `include/llm/llm_context.h`, `src/llm/llm_context.c`, `include/core/session_manager.h`, `include/webui/webui_server.h`, `src/webui/webui_server.c`, `include/tools/context_expand_tool.h`, `src/tools/context_expand_tool.c`, `src/tools/tools_init.c`, `cmake/DawnTools.cmake`

### Live test results (April 27, 2026)

- Model successfully called `context_expand` with IDs from `[COMPACTED]` tag, retrieved 5 original messages (1272 bytes) from parent conversation 627.
- Continuation chain worked: active conversation was 631 (3 continuations deep), expand correctly queried parent conversation 627.
- L1/L2 Claude HTTP 400 on image-containing history → L3 deterministic caught it. Follow-up: strip image content blocks before summarization.

---

## Phase 4: Summary DAG — Hierarchical Summaries (Shipped)

Each compaction creates a `summary_node` record linking to its predecessor, enabling multi-resolution drill-down through the compaction history.

### Schema (v38)

```sql
CREATE TABLE summary_nodes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    conversation_id INTEGER NOT NULL,
    prior_node_id INTEGER,
    depth INTEGER NOT NULL DEFAULT 0,
    msg_id_start INTEGER NOT NULL,
    msg_id_end INTEGER NOT NULL,
    level INTEGER NOT NULL DEFAULT 0,
    summary_text TEXT NOT NULL,
    token_count INTEGER,
    created_at INTEGER,
    FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE,
    FOREIGN KEY (prior_node_id) REFERENCES summary_nodes(id) ON DELETE SET NULL
);
```

### Key implementation details

- **Node creation** in `llm_context_compact()` after message IDs are resolved and summary text is generated. Queries `summary_node_get_latest(conv_id)` for the prior node; if not found in current conversation, traverses `continued_from` chain to find prior nodes from parent conversations. *(Phase 5: with single continuous conversations the prior node is always in the same conversation, so `summary_node_get_latest(conv_id)` hits directly and the `continued_from` traversal is legacy-only.)*
- **CRUD functions**: `summary_node_create()`, `summary_node_get()`, `summary_node_get_latest()`, `summary_node_free()` — all ad-hoc queries (not prepared statements) since compaction is infrequent.
- **`[COMPACTED]` tag** includes `node=Z depth=D` when node creation succeeds. Falls back to node-less format on DB failure (graceful degradation).
- **`context_expand` node_id path**: when `node_id` is provided (no other params needed), retrieves the node and its prior node's summary. Returns both summaries with metadata (depth, level, message range, conversation ID). Buffer right-sized to `summary_len + prior_len + 512` (not fixed 16KB).
- **Ownership check**: validates user owns the node's conversation via `conv_db_get()` before returning data. Prevents cross-user node enumeration.
- **FK cascade**: `ON DELETE CASCADE` on `conversation_id` ensures conversation deletion cleans up nodes. `ON DELETE SET NULL` on `prior_node_id` so deleting a node doesn't cascade-break children.

### How it works

First compaction creates S1 (depth=0). Second compaction creates S2 (depth=1, prior=S1). The model sees `[COMPACTED ... node=2 depth=1]` in its context. Two expansion modes:

- `context_expand(start_id=X, end_id=Y)` → raw original messages (Phase 3)
- `context_expand(node_id=2)` → S2's summary + S1's (prior) summary — multi-resolution view

The model can drill down iteratively: expand node 2 → see both summaries → expand node 1 → see the earliest summary → use `start_id`/`end_id` for raw messages if needed.

### Files modified

`include/auth/auth_db.h`, `include/auth/auth_db_internal.h`, `src/auth/auth_db_core.c`, `src/auth/auth_db_conv.c`, `src/llm/llm_context.c`, `src/tools/context_expand_tool.c`

### Live test results (April 27, 2026)

- Node 1 (depth=0) created on first compaction: `conv=632, msgs=14854-14857`
- Node 2 (depth=1, prior=1) created on second compaction: `conv=632, msgs=14854-14868`
- `context_expand(node_id=1)` returned node 1's summary with metadata
- `context_expand(node_id=2)` returned both node 2's and node 1's summaries — hierarchical drill-down confirmed
- Chain traversal across continuation conversations verified (node 2 linked to node 1 despite multiple continuation boundaries)

---

## Phase 5: Compaction Watermark — Single Continuous Conversations (Shipped)

Replaces fork-on-compaction. Previously, compaction archived the conversation (`is_archived=1`, read-only) and created a new continuation row (`continued_from` → parent) seeded with the summary — the fork was what bounded *reload* context. That produced a user-visible read-only hierarchy plus a "both-locked" bug (a duplicate `continue_conversation` archived the continuation child too, with no idempotency anywhere). Now compaction records an in-conversation **watermark** on the same row; the conversation stays single and always writable, and reload is bounded by the watermark instead of by the fork.

### Schema (v67)

- `conversations.context_watermark_msg_id INTEGER NOT NULL DEFAULT 0` — last compacted message id. `0` = never compacted (load all; the zero-risk gate so un-compacted conversations are byte-identical to pre-v67).
- One-time migration unlocks legacy split-archived conversations (`UPDATE conversations SET is_archived=0 WHERE is_archived=1` — the continuation split was the only writer of that flag).

### Key implementation details

- **Persist** — `conv_db_set_compaction_watermark()`, called from `llm_context_compact()` right after `summary_node_create`. A single atomic UPDATE writes `compaction_summary` + `context_watermark_msg_id` on the same row, guarded `WHERE id=? AND user_id=? AND ? >= context_watermark_msg_id` so a stale async compaction is a harmless no-op (never rewinds). No archive, no continuation row. Skipped (never writes 0) when the message id is unresolved (voice path with no command-context user).
- **Bounded restore** (gated on `watermark > 0`) — `conv_db_get_messages_after(conv_id, user_id, after_id, ...)` (full tool/reasoning columns, same ownership JOIN as `conv_db_get_messages`) loads only post-watermark messages; the summary is prepended. The WebUI **display** load stays full (the user sees the entire transcript); only the **LLM-context** restore is bounded. The same gate is applied inside the messaging forever-conversation loader (`memory_history_load_from_db`), so every reload path is bounded by one shared check.
- **Marker re-injection across reloads** — the summary is injected as an **assistant** message carrying a reconstructed `[COMPACTED conv=N msgs=X-Y node=Z depth=D]` marker (built by `conv_db_format_compaction_context()` from the latest `summary_node`), so the LLM keeps a `context_expand` handle to the originals *after a reload*, not just in the live session. Assistant role (not system) is required: `session_update_system_messages` rebuilds the leading context into exactly two system messages every turn and drops any other system message — so a system-role summary was silently lost. This was a latent bug since the prompt-cache two-system-message refactor (reloaded continuation summaries were also being dropped); fixed here by matching the live marker's assistant role.
- **Continuation machinery is now legacy.** `continued_from` is retained as a breadcrumb but is never written for new conversations; the Phase 3/4 `continued_from` chain-walk fires only for pre-v67 conversations. The old split path (`conv_db_create_continuation`, `handle_continue_conversation`, the `continue_conversation` WS message + the client's split trigger) is left **dormant** and logs a WARNING if invoked, pending removal after a production soak.

### Files modified

`include/auth/auth_db_internal.h`, `include/auth/auth_db.h`, `src/auth/auth_db_schema.c`, `src/auth/auth_db_migrations.c`, `src/auth/auth_db_migrations_v67.c` (new), `src/auth/auth_db_statements.c`, `src/auth/auth_db_conv.c`, `src/llm/llm_context.c`, `src/webui/webui_server.c`, `src/webui/webui_history.c`, `src/memory/memory_history_loader.c`, `www/js/ui/history.js`, `tests/test_auth_db.c`

### Live test results (June 16, 2026)

- conv 845: watermark set and advanced (17874 → 17891) across 8+ compactions, `is_archived=0`, no continuation child — one writable conversation throughout.
- Bounded reload restored 46 post-watermark messages + the summary; the full 94-message transcript still displayed in the UI.
- After reload, the `[COMPACTED conv=845 msgs=17834-17891 node=N depth=N]` marker was visible to the LLM (assistant role survived the per-turn rebuild); the model called `context_expand` on its own — `expanding msgs 17834-17891 from conv 845` — and retrieved the verbatim originals (recalled the user's actual first message).
- All previously-archived conversations unlocked (0 archived post-migration).
- Build clean, 88/88 CI, +2 unit tests (monotonic guard, bounded fetch); five-agent review applied.

---

## Implementation Order

```
Phase 1 (escalation) ──┐
                        ├──> Phase 3 (lossless pointers) ──> Phase 4 (DAG) ──> Phase 5 (watermark)
Phase 2 (async)  ───────┘
         ✓ shipped       ✓ shipped                    ✓ shipped        ✓ shipped
```

Phase 5 (June 2026) retired fork-on-compaction in favor of the in-conversation watermark — conversations stay single and writable; the continuation/`continued_from` model from Phases 3–4 is now legacy (dormant, fires only for pre-v67 conversations).

---

## Follow-up Optimizations

Observed during live testing with local Qwen 35B-A3B on 131K context.

| Item | Description |
|------|-------------|
| **Local summarization timeout** | `summarization_timeout_ms` (180s) is too generous for local models. Add `local_summarization_timeout_ms` config (default ~30s) or auto-scale based on provider type. Cuts worst case from ~6min to ~1min. |
| **Skip LLM levels for large prompts on local** | When serialized conversation exceeds ~32KB and provider is local, start at L3 directly. Pre-flight size check before entering the escalation loop. |
| **Don't async-compact against busy local LLM** | Async compaction contends with the foreground conversation for the same local LLM endpoint. Check if the local LLM is actively serving a foreground request (via `llm_streaming_active` or a semaphore) and defer async trigger until idle. Not an issue for cloud providers. |
| **Extract compaction helpers to separate file** | `llm_context.c` is at ~1860 lines. Extract `compact_with_llm`, `compact_deterministic`, `calculate_compaction_target`, `estimate_tokens_range`, and the async functions into `llm_context_compact.c` with a shared `llm_context_internal.h`. Also eliminates the test impl duplication (tests link against the real helpers). |
| **Cache running token estimate** | `estimate_tokens_range` walks every message on every trigger check. Cache the running total and update incrementally on `session_add_message`. |
| **History generation counter** | Replace raw `snapshot_history` pointer comparison with a monotonic counter bumped on conversation reset. Eliminates theoretical ABA risk from malloc reuse. |
| **Strip image content before summarization** | Claude Haiku returns HTTP 400 when the conversation being summarized contains image content blocks. Strip image references from `to_summarize` before passing to `compact_with_llm`. |
