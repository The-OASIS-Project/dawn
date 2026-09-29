# LLM Subsystem

Source: `src/llm/`, `include/llm/`. The sentence buffer (`common/src/utils/sentence_buffer.c`) lives in the common library because it is also used by Tier 1 satellites.

Part of the [D.A.W.N. architecture](../../../ARCHITECTURE.md) — see the main doc for layer rules, threading model, and lock ordering.

---

**Purpose**: Large Language Model integration with streaming support.

## Architecture Pattern: Strategy + Observer

- **Strategy**: multiple LLM providers (OpenAI, Claude, Gemini, OpenRouter, local) via a unified interface.
- **Observer**: streaming responses notify the sentence buffer for real-time TTS.

## Key Components

- **llm_interface.c/h**: LLM abstraction layer
   - `LLMContext` struct: provider-agnostic context
   - `llm_init()`: initialize selected provider
   - `llm_send_message()`: send message, get complete response (blocking)
   - `llm_send_message_streaming()`: send message, stream response chunks
   - Provider selection based on configuration (`OPENAI_MODEL`, `ANTHROPIC_MODEL`)

- **llm_openai.c/h**: OpenAI API implementation
   - Supports GPT-5 series, GPT-4o, GPT-4
   - Supports llama.cpp local server (OpenAI-compatible endpoint)
   - Supports Ollama with runtime model switching
   - Supports Google Gemini (via OpenAI-compatible endpoint)
   - Both blocking and streaming modes
   - Conversation history management
   - Extended thinking support (reasoning_effort for OpenAI/Gemini models)

- **llm_claude.c/h**: Claude API implementation
   - Supports Claude 4.6 Opus/Sonnet, Claude 4.5 Sonnet
   - Streaming support
   - Different API format than OpenAI (Messages API)
   - Extended thinking support with configurable token budget
   - Full thinking content visibility (unlike OpenAI/Gemini)

- **llm_streaming.c/h**: Streaming response handler
   - Manages Server-Sent Events (SSE) connections
   - Buffers and parses incoming chunks
   - Notifies sentence buffer for TTS integration

- **sse_parser.c/h**: Server-Sent Events parser
   - Parses SSE format: `data: {...}\n\n`
   - Extracts JSON content from events
   - Handles partial events across network chunks

- **`common/src/utils/sentence_buffer.c`, `common/include/utils/sentence_buffer.h`**: Sentence boundary detection (shared with satellite)
   - Buffers streaming text until complete sentence
   - Detects sentence boundaries (`.`, `!`, `?`), bullets, numbered lists, `:\n`, `\n\n`
   - Sends complete sentences to TTS for natural phrasing
   - Reduces perceived latency (speak while generating)

- **llm_command_parser.c/h**: JSON command extraction
   - Extracts `<command>` JSON tags from LLM responses
   - Validates JSON structure
   - Handles malformed JSON gracefully

- **llm_rate_limit.c/h**: Cloud API rate limiter
   - Process-wide sliding window throttle (default 40 RPM, configurable)
   - Gates all cloud LLM call paths; local providers bypass
   - Interrupt-aware blocking (wakes on shutdown signal)

## Prompt construction (append-only, cache-aware)

A conversation's request is **append-only**: nothing already sent to the model is rewritten. That keeps the provider's prompt cache valid across the whole conversation, and keeps a model's signed reasoning replayable (Anthropic refuses reasoning whose earlier prompt changed). It is built in three layers:

1. **The prompt builder** — `dawn_build_prompt()` (`src/webui/webui_auth_helpers.c`), registered as the session prompt builder. For each turn it returns a `composed_prompt_t` (`include/core/prompt_parts.h`) describing what a conversation *starting now* would get, plus this turn's context. It does not decide what reaches the model.
2. **The prefix engine** — `session_prefix_apply_turn()` (`src/core/session_prefix.c`, API in `include/core/session_prefix.h`). It compares the composed prompt with what the conversation already has in force and appends only the differences, as kind-marked messages.
3. **The renderers** — each provider formatter turns those kind-marked messages into its own wire shape.

### What the builder returns

- **`sections[]`** — the system prompt in named sections (`prompt_sections_add`, `src/core/prompt_sections.c`): `identity_override`, `persona`, `rules`, `tool_defaults` (guests only), `user_context`, `user_identity`, `memory_rules`, `citation_rules`, `tool_discipline`, `recall_routing`, `background_deliveries`, `context_rules`. `stable_prefix` is their join. The base prompt is **surface-neutral**: one `get_command_prompt()` for every surface; nothing about the room, voice or channel is in it.
- **`directives`** — the surface's standing directions, the full set every turn (`build_directives`): tools unavailable here right now (`llm_tools_build_disabled_hint`), a satellite's or the local mic's room, a messaging channel, spoken output + speech-to-text input (local mic, satellites, WebUI with voice on), a background job's headless mode. A detached reinvoke turn sets `session->keeps_directions` and sends none, so the conversation's stay in force.
- **`tool_names` / `tool_schemas`** — the tool set to freeze (every registered tool except the research-only ones, independent of what is enabled right now: `llm_tools_freeze_names`) and a hash of each schema (`llm_tools_schema_hashes`).
- **`memory_body`** — preferences + recent conversation summaries (`memory_build_context`, unframed).
- **`volatile_block`** — this turn's context, unframed: `[system_time]` and the ranked focus items (`build_focus_block`), each held item numbered `[M<n> source]` for the whole conversation (`focus_handles.c`).

The builder runs for guests too (user 0: no memory, no user context).

### What the prefix engine appends

- **The frozen prefix** — `messages[0]`, `MESSAGE_KIND_PREFIX`, made only by `src/core/prefix_message.c` (CI guard `scripts/check_message_kind_confined.sh`). Set by the conversation's first turn and never changed. It fixes the prompt, a per-conversation **tag** (`dawn-ctx-` + 8 random hex digits) filled into `context_rules`, and the frozen tool set. A conversation saved before prefixes were frozen adopts one on its next turn, as a declared **boundary**: earlier turns replay without their reasoning (text and tool calls stay), and the conversation's floor (`conversations.reasoning_floor_msg_id`) rises.
- **What is in force** — `_in_force` on the prefix (`src/core/prefix_in_force.c`, persisted as `conversations.in_force_hash`): each section's hash and text, the directives' hash, the tool schemas' hashes. A section that changed since (a persona edit) is appended after the turn's question as `MESSAGE_KIND_INSTRUCTION`, naming what changed with the new text; changed directives as `MESSAGE_KIND_DIRECTIVE`. The newest of each is in force. A frozen tool's schema changing is logged; the tool set itself changing (an MCP server connecting) is a boundary.
- **The turn's context** — in front of the question: `--- USER MEMORY (tag) ---` when it changed since the conversation last had it (`MESSAGE_KIND_MEMORY`), then `--- TURN CONTEXT (tag) ---` (`MESSAGE_KIND_TURN_CONTEXT`) with the context, a per-turn note, and new device notices for the turn's user. Both are anchored to the question row (`messages.context_of`). A background job's report (`MESSAGE_KIND_ENVELOPE`) gets its context as a separate message before it, so the report stays one untrusted block.
- **Tool-loop notes** — `MESSAGE_KIND_LOOP_NOTE`, persisted with the turn.

The turn's record (what it appended, the prefix and tool set it ran under, its boundary) is saved once its question row exists (`session_prefix_question_saved`, from every question writer), in one transaction (`conv_db_save_turn`). A reload rebuilds the same request (`memory_history_request_context`, `src/memory/memory_history_loader.c`): the context rows fold back in front of their questions (`llm_history_kind.c`).

**Untrusted text.** Everything DAWN puts in front of the model but didn't write passes through `llm_context_neutralize()` (`src/llm/llm_context_text.c`). That covers focus items, remembered preferences and summaries, tool results (`llm_tools_execute`, the scheduler's direct briefing calls, the MQTT device-data relay), a job's title and result, device notices, and compaction summaries. Matching runs on a shadow of the text that sees through invisible characters and lookalike letters. It rewrites only the spans that imitate DAWN's framing or carry a tag-shaped string (whose digits it withholds), and keeps every other byte. It is linear in the text's length. `context_rules` tells the model that only tagged framing and system messages are DAWN's. The conversation's own secret is masked in tool results, the persisted reply and compaction summaries (`session_prefix_mask_secret`). A tool call carrying it, in any encoding, is refused (`call_carries_tag`).

**Forgetting.** A user's removal (forgetting a memory, deleting one in the memory panel, forgetting a conversation, deleting or replacing a document, deleting all memories, deleting an account) is recorded by TEMP delete triggers that fire only while the removal is marked (`conv_db_withdraw_intent_begin`, `src/auth/auth_db_withdraw.c`). Nightly decay, merges and a note's re-indexing record nothing. `session_withdraw_forgotten` then withdraws each removed item from every conversation it was injected into: stored rows (`conv_db_withdraw`) and every live session's history. `scripts/check_user_removal_marked.sh` fails the build when a user-facing delete isn't marked. Its `[M<n>]` lines are rewritten, and the conversation's reasoning floor rises (pending until a turn built after the removal is saved), since earlier reasoning may repeat it. A turn built before a removal and saved after it is withdrawn as it is saved, inside `conv_db_save_turn`, and its reasoning goes behind the floor even if another session's turn has already settled it (`conversations.reasoning_floor_seq` keeps the last withdrawal that changed the conversation). Removals are ordered by the withdrawal sequence (`conv_db_withdraw_seq`, the `withdrawn_items` AUTOINCREMENT high-water mark), not by the clock. `document_chunks` ids are never reused (AUTOINCREMENT), so a removed chunk's record can't name a new one.

### Tools on the wire

`llm_tools_request_tools()` (`src/llm/llm_tools_filter.c`) picks a request's tools: none on a suppressed turn, the read-only allowlist in a research session, else the conversation's frozen set, or the currently enabled set with no frozen set. A frozen tool not available here right now stays listed, is named in the directives, and is refused if called (`llm_tools_enabled_for_session`). A forced final call (iteration cap) keeps the tools with `tool_choice: none`, so the cached prefix is unchanged.

### Provider handling

- **Claude** (`llm_claude_format.c`): the prefix is the top-level `system` with `cache_control`, plus a breakpoint on the last tool. Instructions and directives go as mid-conversation system messages for models listed under `models.toml [mid_system]`; otherwise as a user-turn note headed `[Operator note <tag>]`. The turn's context goes into the question's user message.
- **OpenAI Responses** (`llm_openai_responses_input.c`): the prefix is `instructions` (byte-stable), notes are system items, and the context goes as an item before the question.
- **Chat Completions** (`llm_openai_history.c`): the prefix is the first system message. Notes are system messages on native OpenAI (`api.openai.com`); on other carriers (OpenRouter, Gemini, local) they are tagged in-band notes. The context goes into the question's message.

Cache-token accounting is unified across providers (Claude `cache_creation`/`cache_read`, the Responses usage struct). Gemini caching is unreliable upstream; see the Gemini native-API notes in `docs/TODO.md`.

## Turn blocks: replay and persistence

Every assistant turn in a session's history carries provider-neutral **turn blocks** (`include/llm/llm_turn_blocks.h`, under the `_blocks` key): its text, its tool calls, and any reasoning a vendor signed or encrypted for itself (a Claude `thinking` block, an OpenAI Responses reasoning item, OpenRouter `reasoning_details`, a Gemini thought signature). Each formatter renders a turn from its blocks in its own shape. Vendor data goes back only to the carrier (the endpoint and a tag of the API key, `llm_request_carrier`) and format that issued it, for every vendor including Claude; everywhere else it is left out, never turned into text.

Blocks are persisted with their row so a reloaded conversation replays each turn as the model produced it:

- **Writing.** A history message becomes rows through `llm_history_rows_append()` (`src/llm/llm_history_rows.c`): the canonical `{role, content, tool_calls, tool_call_id}` rows every reader expects, plus the assistant turn's blocks in the stored shape (`llm_turn_blocks_to_stored`, a versioned `{"v":1,"blocks":[...]}` envelope serialized exactly). The tool loop, the voice save and every final-answer writer save through `conv_db_add_row()` (`src/auth/auth_db_messages.c`). A final answer's blocks come from the copy `llm_call_finalize()` keeps as the reply joins the history (`session_take_reply_blocks()`), never from reading the history afterwards.
- **Reading.** Only a load that becomes an LLM context asks for blocks: `memory_history_load_for_llm()` and the WebUI restore (`memory_history_load_rows(..., with_blocks = true)`). The loader reads `messages.llm_blocks` through `conv_db_get_messages_for_llm()`, parses it after the read (outside the database lock), and attaches blocks only when they parse (`llm_turn_blocks_from_stored`, fail-closed) and record the row's tool calls. Display, export, search, admin and memory reads never see the column; `scripts/check_llm_blocks_confined.sh` enforces this at build time.
- **Bounds.** 4 MiB per row (so a row always fits a load) and assistant rows only (a schema CHECK; `llm_blocks_len`, placed before `llm_blocks`, holds the byte length so no check reads the blob), 8 MiB of blocks per load (older rows past it load without them). Advancing the compaction watermark clears blocks at or below it right after, in batches, and the periodic auth cleanup sweeps any it missed; a trigger drops a row's blocks when its text or tool calls change.

## OpenRouter Provider

OpenRouter (`https://openrouter.ai/api/v1`) is an OpenAI-wire-compatible endpoint fronting
many vendors' models. DAWN treats it as a **first-class cloud provider**, not a mode: set
`[llm.cloud] provider = "openrouter"` (key `openrouter_api_key` in `secrets.toml`, or
`OPENROUTER_API_KEY` env) and the main chat routes through OpenRouter with one key. Its model
list (`openrouter_models`) uses `vendor/model` IDs (e.g. `anthropic/claude-sonnet-4`).
Because OpenRouter is OpenAI-compatible for every model it serves (including Anthropic ones),
all calls reuse the existing `llm_openai_*` request/SSE path (`CLOUD_PROVIDER_OPENROUTER`
falls into the OpenAI-compatible branch everywhere, never the native Claude path).

**One authority — the provider enum.** `provider` (a `CLOUD_PROVIDER_*` enum resolved by the
init/refresh ladder) is the single source of truth for the active provider; there is no
separate mode flag. A stored bare model name is canonically remapped to its OpenRouter
`vendor/model` slug at the single request choke point (`llm_resolve_config`), so a legacy
conversation's bare id is never sent verbatim.

**Per-purpose independence.** Each background purpose (memory-extraction in
`memory_extraction.c`, compaction in `llm_context.c`, silent-observe, scheduler) follows its
OWN provider setting (`extraction_provider` / `compact_provider` /
`[llm.silent_observe] provider`, or the scheduler's `provider`). Set one to `"openrouter"`
and put a `vendor/model` slug in that purpose's normal model field (empty → the main
OpenRouter default). **No global override** forces an aux call onto OpenRouter — a
Claude-configured extraction runs on direct Claude even while the main chat is on OpenRouter.

Other specifics: OpenRouter never routes to `/v1/responses`
(`should_dispatch_to_responses_api` skips `openrouter.ai`); requests carry optional
`HTTP-Referer` + `X-Title` attribution headers; a `provider="openrouter"` config with no key
falls back to auto-detect, and a keyed aux purpose with no key **errors honestly** rather
than silently switching providers; and `get_context_size()` best-effort-strips the `vendor/`
prefix to reuse the known context-window tables, else a conservative 128K default.

> **History:** OpenRouter was originally a `[llm.cloud] use_openrouter` gateway *toggle* that
> force-rerouted all cloud traffic. It was promoted to a first-class provider (Aug 2026); the
> retired `use_openrouter` bool is auto-migrated to `provider = "openrouter"` at load (once,
> then dropped). The three per-purpose `*_openrouter_model` shadow fields were retired with
> it — each purpose's normal model field now carries the slug.

> **Tech debt note:** each provider still has its own `(URL, key)` branch across the resolvers
> (`llm_resolve_config`, `llm_chat_completion_with_config`, `build_compaction_config`,
> `memory_extraction_resolve_config`, silent-observe, scheduler). Consolidating these into one
> resolver is a worthwhile future cleanup.

## LLM Worker Thread

LLM processing is non-blocking — the main audio loop never waits on an API call.

```
┌───────────────────────────────────────────────────────────┐
│                      Main Thread                          │
│  - State machine (never blocks on LLM)                    │
│  - Audio capture + VAD (continuous, 50ms intervals)       │
│  - ASR processing (Whisper/Vosk)                          │
│  - TTS synthesis (mutex protected)                        │
│  - LLM completion detection (polling llm_processing flag) │
└────────────┬──────────────────────────────────────────────┘
             │ spawns on-demand, max 1 concurrent
             ▼
┌───────────────────────────────────────────────────────────┐
│                   LLM Worker Thread                       │
│  - Blocking CURL call to LLM API                          │
│  - CURL progress callback (checks interrupt flag)         │
│  - Returns response via shared buffer                     │
│  - Thread-safe via llm_mutex                              │
└───────────────────────────────────────────────────────────┘
```

Request and response buffers use **ownership transfer** to prevent data races. The mutex is held only during transfer, not during processing.

### Interrupt Mechanism

Users can interrupt an in-flight LLM call by saying the wake word. The CURL progress callback checks `llm_interrupt_requested` periodically; wake word detection sets the flag via `llm_request_interrupt()`, returning non-zero from the callback aborts the transfer, and the main thread discards the partial response and rolls back conversation history.

## Data Flow (Streaming Mode)

```
User Query → LLM Provider (OpenAI/Claude/Local)
                    ↓ (SSE stream)
            SSE Parser → Streaming Handler
                    ↓ (text chunks)
            Sentence Buffer → TTS (as sentences complete)
                    ↓ (complete response)
            Command Parser → MQTT Commands
```

## Performance Comparison

| Provider                | Quality | TTFT      | Latency | Cost          |
| ----------------------- | ------- | --------- | ------- | ------------- |
| OpenAI GPT-5            | 100%    | ~300ms    | ~3.1s   | ~$0.01/query  |
| Claude 4.6 Sonnet       | 92.4%   | ~400ms    | ~3.5s   | ~$0.015/query |
| Gemini 2.5 Flash        | ~90%    | ~250ms    | ~2.5s   | ~$0.002/query |
| llama.cpp (Qwen3.6 35B-A3B MoE) | 94.0% | 264-332ms\* | ~1.6s | FREE |
| llama.cpp (Qwen3-4B Q4) | 94.8%   | 45-59ms\* | ~1.5s  | FREE          |
| llama.cpp (Gemma 4 31B) | 97.4%   | 659-730ms\* | slow | FREE          |
| Ollama (Qwen3-4B Q4)    | *not measured* | ~150ms | ~1.6s | FREE      |

**TTFT = Time To First Token** (lower = faster perceived response).

\* TTFT on the benchmark harness's ~66-token prompts — useful for ranking models,
useless as a latency estimate. Real DAWN traffic has a **median 2,360-token**
prompt, where Preset J's measured TTFT is **~3.8 s** (p90 12.7 s). Prompt
processing runs ~482 tok/s for Preset J and only ~219 tok/s for the dense
Qwen3.6 27B — a 2.2x spread the tok/s column completely hides. Quality and TTFT are from one sweep of all presets on 2026-09-03,
AGX Orin 64GB MAXN, llama.cpp b10626, 116-point FRIDAY suite. The Ollama row was
never benchmarked and its numbers are inherited estimates.

Qwen3.6 35B-A3B is the production default (llama-server Preset J);
it is the only local option that is voice-viable *with* vision. See
[services/llama-server/README.md](../../../services/llama-server/README.md).
