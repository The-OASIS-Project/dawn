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
- **`context_head`** — the turn context's first line, `[system_time]` (`prompt_turn_head`; never empty, so the head is always that line).
- **`focus_items`** — the ranked retrieved items as data (`build_focus_block`): source, item id, date, one-line text with DAWN's markers defused, score, and each held item's handle `[M<n>]` for the whole conversation (`focus_handles.c`; the date is when the item was learned, saved or happens). Retrieval embeds a short follow-up (six words or fewer) together with the previous question, so "what's that for?" finds what the conversation was about; keyword and date matching stay on the turn's own words. Which of them the turn sends is decided at the seam (below).
- **`context_tail`** — per-turn notes after the items (a spoken turn's transcription hint).
- **`focus_panel`** — what the WebUI context panel is told about the retrieval, the builder's own type; the seam tells it each item's place once decided.

The builder runs for guests too (user 0: no memory, no user context).

### What the prefix engine appends

- **The frozen prefix** — `messages[0]`, `MESSAGE_KIND_PREFIX`, made only by `src/core/prefix_message.c` (CI guard `scripts/check_message_kind_confined.sh`). Set by the conversation's first turn and never changed. It fixes the prompt, a per-conversation **tag** (`dawn-ctx-` + 8 random hex digits) filled into `context_rules`, and the frozen tool set. A conversation saved before prefixes were frozen adopts one on its next turn, as a declared **boundary**: earlier turns replay without their reasoning (text and tool calls stay), and the conversation's floor (`conversations.reasoning_floor_msg_id`) rises.
- **What is in force** — `_in_force` on the prefix (`src/core/prefix_in_force.c`, persisted as `conversations.in_force_hash`): each section's hash and text, the directives' hash, the hashes of the tool definitions in force. A section that changed since (a persona edit) is appended after the turn's question as `MESSAGE_KIND_INSTRUCTION`, naming what changed with the new text; changed directives as `MESSAGE_KIND_DIRECTIVE`. The newest of each is in force. A change to the tools is a `tool_change` row (see [Tools on the wire](#tools-on-the-wire)).
- **The turn's context** — in front of the question: `--- USER MEMORY (tag) ---` when it changed since the conversation last had it (`MESSAGE_KIND_MEMORY`), then `--- TURN CONTEXT (tag) ---` (`MESSAGE_KIND_TURN_CONTEXT`) with the context, a per-turn note, and new device notices for the turn's user. Both are anchored to the question row (`messages.context_of`). A background job's report (`MESSAGE_KIND_ENVELOPE`) gets its context as a separate message before it, so the report stays one untrusted block.
- **Incremental items** — the seam (`session_focus.c`, `core/focus/focus_incremental.c`) reads which items the history's earlier turn contexts show: the lines declared under `[retrieved items: N]` right after a tagged frame's `[system_time]` line, the newest line per handle winning (a withdrawn line, or a turn summarized away, shows nothing). It sends only the items that are new or changed, under that declaring line, names the rest on `[still relevant: M3, M7]`, and restates the citation reminder for both. Untrusted text has these lines defused (`llm_context_neutralize`), and an imitation of a handle that does reach the history after its newest line makes the item be sent again. Nothing is tracked beside the history, so a reload, a compaction or a forget needs no bookkeeping.
- **Tool-loop notes** — `MESSAGE_KIND_LOOP_NOTE`, persisted with the turn.

The turn's record (what it appended, the prefix and tool set it ran under, its boundary) is saved once its question row exists (`session_prefix_question_saved`, from every question writer), in one transaction (`conv_db_save_turn`). A reload rebuilds the same request (`memory_history_request_context`, `src/memory/memory_history_loader.c`): the context rows fold back in front of their questions (`llm_history_kind.c`).

**Compaction.** A long history is compacted only at a turn seam, never mid tool round (a turn's reasoning is bound to its request as it stands). `src/core/session_compaction.c`:
- **Ahead.** When a turn ends near the soft threshold, the oldest range (up to the kept last exchanges) is copied under the lock and summarized on a worker (`llm_context_summarize` → the pure core `src/llm/llm_compaction.c`: L1 → L2 → mechanical L3; a cancel stops without L3). The worker never touches the history or the database. Only rows already saved are summarized: a range ends before any message a turn record still holds, and on a saved row or a tool exchange (whose rows are found by call id).
- **At the seam** (`session_prefix_apply_turn`): a ready summary is applied if its range is still the start of the history. The range goes; the summary becomes a `summary` part in front of the first kept question (`llm_history_attach_summary`, one rendering live and on reload, framed with the tag, no ids); every turn's reasoning is dropped (a declared boundary); what is in force is reset to what the history shows (`prefix_in_force_reset_to_history`), so instructions and directions that were only in the summarized part are appended again after the question. The turn's record saves the summary node, the summary and the watermark with its rows, in one `conv_db_save_turn` transaction. A reload renders the request byte for byte.
- **Sync.** A turn that would reach the hard threshold with none ready summarizes first (`session_compaction_prepare`), on the turn's own thread, the lock not held during the call; the turn's cancel (a barge-in, a Stop, a teardown) stops it. The local mic dispatches on its LLM worker, not the audio loop.
- **Ownership.** What a summary reads is its runner's own copy (the worker's, or the turn's); the session holds only the range's refs and the result. A teardown closes the session to new summaries, cancels and joins the worker, and frees the rest, at `session_destroy` and at shutdown (`session_free`). A replaced context (a new conversation, another owner) drops a pending summary and what a voice surface kept (`session_compaction_reset_locked`).
- **Mid-turn.** Past the hard threshold after tools ran, the tool loop closes the turn: a foreground turn answers with what it has (tools off), or says it ran out of room; a background job goes on in a continuation turn (`job_worker.c`), compacted at its seam.
- **Model switch.** `switch_llm` takes effect with the next message: that seam judges the history against the new model's window and summarizes with the model switched from.
- **Voice.** A voice surface's history is saved whole: the messages a compaction took out are kept as the rows they become (no reasoning, no images) and saved first at the voice save, all rows in one transaction (`conv_db_add_rows`; the watermark is the last of the removed rows), and memory extraction reads them.
- **Jobs.** A continuation's envelope question is a turn start the cut can land on; a continuation whose seam compacted nothing stops the job rather than fill the same context again.
- Summaries are written from the conversation's own messages: the summarizer's input drops injected context (memory, turn context) and an earlier summary's part (it is given once, first), so a summary is on the same footing as the messages it replaces. The input is fenced by a delimiter made for each call. Every render of a summary, a stored one's on reload too, is neutralized and tag-masked, and framed as a model's record that carries no instructions. `context_expand` with no IDs shows the latest summary's original messages; a summary node of a private conversation shows only within its lineage.

**Untrusted text.** Everything DAWN puts in front of the model but didn't write passes through `llm_context_neutralize()` (`src/llm/llm_context_text.c`). That covers focus items, remembered preferences and summaries, tool results (`llm_tools_execute`, the scheduler's direct briefing calls, the MQTT device-data relay), a job's title and result, device notices, and compaction summaries. Matching runs on a shadow of the text that sees through invisible characters and lookalike letters. It rewrites only the spans that imitate DAWN's framing or carry a tag-shaped string (whose digits it withholds), and keeps every other byte. It is linear in the text's length. `context_rules` tells the model that only tagged framing and system messages are DAWN's. The conversation's own secret is masked in tool results, the persisted reply and compaction summaries (`session_prefix_mask_secret`). A tool call carrying it, in any encoding, is refused (`call_carries_tag`).

**Forgetting.** A user's removal (forgetting a memory, deleting one in the memory panel, forgetting a conversation, deleting or replacing a document, deleting all memories, deleting an account) is recorded by TEMP delete triggers that fire only while the removal is marked (`conv_db_withdraw_intent_begin`, `src/auth/auth_db_withdraw.c`). Nightly decay, merges and a note's re-indexing record nothing. `session_withdraw_forgotten` then withdraws each removed item from every conversation it was injected into: stored rows (`conv_db_withdraw`) and every live session's history. `scripts/check_user_removal_marked.sh` fails the build when a user-facing delete isn't marked. Its `[M<n>]` lines are rewritten, and the conversation's reasoning floor rises (pending until a turn built after the removal is saved), since earlier reasoning may repeat it. A turn built before a removal and saved after it is withdrawn as it is saved, inside `conv_db_save_turn`, and its reasoning goes behind the floor even if another session's turn has already settled it (`conversations.reasoning_floor_seq` keeps the last withdrawal that changed the conversation). Removals are ordered by the withdrawal sequence (`conv_db_withdraw_seq`, the `withdrawn_items` AUTOINCREMENT high-water mark), not by the clock. `document_chunks` ids are never reused (AUTOINCREMENT), so a removed chunk's record can't name a new one.

### Tools on the wire

`llm_tools_request_tools()` (`src/llm/llm_tools_filter.c`) picks a request's tools: none on a suppressed turn, the read-only allowlist in a research session, else the conversation's tools, or the currently enabled set when it has none. A tool in force but not available here right now stays listed, is named in the directives, and is refused if called (`llm_tools_enabled_for_session`). A forced final call (iteration cap) keeps the tools with `tool_choice: none`, so the cached prefix is unchanged.

**Frozen by value.** A conversation's tools are definitions, not names (`src/llm/llm_tool_defs.c`): `{name, description, parameters}`, the registry's neutral projection, rendered per provider when a request is built (Claude: `src/llm/llm_claude_tools.c`). The first turn freezes every registered definition on the prefix message. A definition must pass `llm_tool_def_valid()` (a plain name, description and schema within caps, valid UTF-8), so an MCP server's definitions are stored and replayed exactly as first sent, never re-read from a server that has since changed or gone. A runtime value set that would change a schema (the HUD's discovered elements) stays out of it: the live set goes in the standing directions and the tool's `validate_call` refuses a call outside it.

**Changes are appended** (`src/core/prefix_tools.c`). At each seam what is registered is compared with what is in force (the frozen set plus every change still in the history); a difference is one `tool_change` row (`MESSAGE_KIND_TOOL_CHANGE`, role system) with the full definitions it adds or changes. Enable flags never count: a tool toggled or a component going offline appends nothing, and a tool no longer registered keeps its definition (a call to it is refused). Changes are bounded per conversation (`PREFIX_TOOL_CHANGES_MAX`) and per MCP server per hour; past a bound the definitions in force stay (logged once) and what was held back is compared at a later seam. A change a compaction summarized away is appended again, unbounded. A row is stored one of two ways:

- **inline**: sent in place as `tool_addition` blocks (the Claude API itself, models under `models.toml [inline_tools]`, beta `inline-tools-2026-09-15`); the `tools` array and everything cached before the change stay as they were.
- **folded**: merged into the request's `tools` (a same-name definition replaced in place, a new one appended); the turn declares a **boundary**, so earlier turns replay without their reasoning. Every other provider, a model not listed, and a turn with no question fold.

Whether a stored inline row goes in place is the conversation's record, never the process's. When the API rejects the beta, the Claude provider retries the turn's request folded and reports it in the turn's provider-neutral result (`llm_take_inline_tools_rejected()`, `src/llm/llm_turn_result.c`); the session records it on the conversation (`session_prefix_inline_tools_rejected`) with a declared boundary, so its later turns fold too, after a restart as well. A target that stops taking inline changes (another model, another endpoint) is marked the same way at the next seam.

### Tool images

A tool's image (a camera capture) enters through `llm_tools.c` into `src/llm/llm_tool_images.c`: it must be a JPEG, PNG, GIF or WebP by its bytes. For a turn with a user it is stored owner-only (`IMAGE_SOURCE_CAPTURE`) and **unbound**; the tool row that names it (`messages.images`) binds it when saved, and the conversation records it (`conversation_images`). A guest's capture stays in memory. The result's content is `[text part, image parts]` on every history format, each part marked with the stored id, and a reload rebuilds the same bytes from the file. Each provider renders that one shape (`src/llm/llm_tool_images_render.c`):

- **Claude** (and OpenRouter's `anthropic/` models, on its Messages endpoint): image blocks inside the `tool_result`.
- **OpenAI Responses**: a `function_call_output` of `input_text` and `input_image` items.
- **Chat Completions** (OpenAI, Gemini, OpenRouter's other models, local): the tool messages carry text, and one user message after a turn's tool messages carries their images.
- **A model without vision**: fixed text in place of each image.

**`models.toml [max_request_images]`** gives, per vendor, the images (`count`) and their base64 bytes (`bytes`) one request may carry (`anthropic_200k` for Claude models with a 200K window, `other` for other OpenRouter vendors, `local` for llama.cpp / Ollama). A capture that would take the next request past either is refused in its turn (a tool loop has no seam to compact at) and its stored image deleted. A history nearing either is compacted at the next seam, the turns holding its oldest images summarized away. When none can go (they are in what a compaction keeps, or past a row not saved yet), none is planned again for that conversation until a row of it is saved, and that is logged once.

An unbound capture no row binds is reclaimed after `IMAGE_UNBOUND_GRACE_SEC` (24 h), unless a live session of its owner still names it (a voice session's unsaved history, a running job's).

### Provider handling

- **Claude** (`llm_claude_format.c`, also OpenRouter's `anthropic/` models): the prefix is the top-level `system` with `cache_control`, plus a breakpoint on the last tool. Instructions and directives go as mid-conversation system messages for models listed under `models.toml [mid_system]`; otherwise as a user-turn note headed `[Operator note <tag>]`. The turn's context goes into the question's user message.
- **OpenAI Responses** (`llm_openai_responses_input.c`): the prefix is `instructions` (byte-stable), notes are system items, and the context goes as an item before the question.
- **Chat Completions** (`llm_openai_history.c`): the prefix is the first system message. Notes are system messages on native OpenAI (`api.openai.com`); on other carriers (OpenRouter's other models, Gemini, local) they are tagged in-band notes. The context goes into the question's message.

Cache-token accounting is unified across providers (Claude `cache_creation`/`cache_read`, the Responses usage struct). Gemini caching is unreliable upstream; see the Gemini native-API notes in `docs/TODO.md`.

## Turn blocks: replay and persistence

Every assistant turn in a session's history carries provider-neutral **turn blocks** (`include/llm/llm_turn_blocks.h`, under the `_blocks` key): its text, its tool calls, and any reasoning a vendor signed or encrypted for itself (a Claude `thinking` block, an OpenAI Responses reasoning item, OpenRouter `reasoning_details`, a Gemini thought signature). Each formatter renders a turn from its blocks in its own shape. Vendor data goes back only to the carrier (the endpoint and a tag of the API key, `llm_request_carrier`) and format that issued it, for every vendor including Claude; everywhere else it is left out, never turned into text.

Blocks are persisted with their row so a reloaded conversation replays each turn as the model produced it:

- **Writing.** A history message becomes rows through `llm_history_rows_append()` (`src/llm/llm_history_rows.c`): the canonical `{role, content, tool_calls, tool_call_id}` rows every reader expects, plus the assistant turn's blocks in the stored shape (`llm_turn_blocks_to_stored`, a versioned `{"v":1,"blocks":[...]}` envelope serialized exactly). The tool loop, the voice save and every final-answer writer save through `conv_db_add_row()` (`src/auth/auth_db_messages.c`). A final answer's blocks come from the copy `llm_call_finalize()` keeps as the reply joins the history (`session_take_reply_blocks()`), never from reading the history afterwards.
- **Reading.** Only a load that becomes an LLM context asks for blocks: `memory_history_load_for_llm()` and the WebUI restore (`memory_history_load_rows(..., with_blocks = true)`). The loader reads `messages.llm_blocks` through `conv_db_get_messages_for_llm()`, parses it after the read (outside the database lock), and attaches blocks only when they parse (`llm_turn_blocks_from_stored`, fail-closed) and record the row's tool calls. Display, export, search, admin and memory reads never see the column; `scripts/check_llm_blocks_confined.sh` enforces this at build time.
- **Bounds.** 4 MiB per row (so a row always fits a load) and assistant rows only (a schema CHECK; `llm_blocks_len`, placed before `llm_blocks`, holds the byte length so no check reads the blob), 8 MiB of blocks per load (older rows past it load without them). Advancing the compaction watermark clears blocks at or below it right after, in batches, and the periodic auth cleanup sweeps any it missed; a trigger drops a row's blocks when its text or tool calls change.

## Prompt-cache monitor

Every provider call's usage passes through `llm_cache_monitor_record()` (`src/llm/llm_cache_monitor.c`),
which logs one `LLM cache:` line, queues a row for `llm_usage_log` (90 days), and gives a
conversation's own calls a cache state (the `cache_state` column, and the `cache_state` field of the
WebUI's idle `metrics_update` frame):

- **Anthropic (direct):** the next call should read what the previous one read or wrote. Under 90% of
  that, or nothing at all on a prompt of 4096+ tokens, is a **warm miss**.
- **Local llama.cpp** (only it reports what it reused; Ollama isn't judged): the next call should read
  the previous prompt from the slot's KV cache, unless
  another conversation or a side call used the server in between (`shared`). Its misses log at INFO
  (latency, not cost).
- **OpenAI, and OpenRouter's OpenAI and Anthropic models:** tracked for state with no expected read
  (OpenAI drops earlier turns' reasoning by design; OpenRouter routes Anthropic best-effort without
  BYOK); three warm calls in a row reading nothing are a warm miss.
- **Not judged:** Gemini, other vendors, side calls (extraction, compaction, briefings), and the local
  microphone's turns (session 0, shared with session-less calls).
- **Known events are never misses:** a TTL gap (measured from the request), a model, tool-set (or
  `tool_choice`), system or thinking change, the conversation's first image, and a rewritten history
  (`llm_cache_monitor_history_rewritten()`: compaction, a forget's withdrawal, a turn's rollback, a
  history cleared or replaced, as a research round does).

A warm miss logs a WARNING (one per provider and model per 10 minutes) with the expected and actual read
and, on Anthropic, its own diagnosis. Reasoning the binding controls dropped for a prefix change is a
DAWN bug: besides the rate-limited WARNING, the admins' WebUI gets a `cache_alert` toast once per
conversation (queued by the LLM worker, sent from the main loop's flush through a weak hook the WebUI
overrides). `dawn-admin cache stats [--since 24h|7d] [--provider p]` reads the usage log back: calls,
prompt tokens, coverage, input tokens' worth saved, warm misses and dropped reasoning per provider,
model and kind. The thresholds are constants, not settings.

## OpenRouter Provider

OpenRouter (`https://openrouter.ai/api/v1`) fronts many vendors' models behind one key, with
an OpenAI-compatible endpoint and an Anthropic Messages one. DAWN treats it as a **first-class cloud provider**, not a mode: set
`[llm.cloud] provider = "openrouter"` (key `openrouter_api_key` in `secrets.toml`, or
`OPENROUTER_API_KEY` env) and the main chat routes through OpenRouter with one key. Its model
list (`openrouter_models`) uses `vendor/model` IDs (e.g. `anthropic/claude-sonnet-4`).
Most models go through its OpenAI-compatible Chat Completions endpoint, on the `llm_openai_*`
request/SSE path. An **`anthropic/` model goes through OpenRouter's Anthropic Messages endpoint**
(`/api/v1/messages`) on the Claude path instead, so it gets the same request as direct Claude: the
cache breakpoints, the thinking-binding controls (`input_transformations` comes back only when
something was dropped) and replayed thinking blocks.

- **Which calls:** `llm_uses_anthropic_messages()` (`llm_claude_route.c`), used wherever a call's
  wire format is chosen (`llm_interface.c` and the tool loop's provider switch): Claude, or
  OpenRouter on `openrouter.ai` itself with an `anthropic/` slug (a `:variant` too). A custom
  OpenRouter endpoint stays on Chat Completions.
- **Route and provider are separate.** The Claude path reads its route from the endpoint
  (`llm_claude_route()`): OpenRouter takes a Bearer key, its attribution headers, the binding
  controls only (it drops the diagnostics object and rejects a tool defined in a message, so tool
  changes fold), `provider: {order: ["anthropic"], allow_fallbacks: false}` (a conversation's cache
  and its thinking's binding are per platform) and `session_id: "dawn-conv-<id>"`. The stream is
  parsed as Messages events (`llm_stream_create_messages`) but usage books under
  `CLOUD_PROVIDER_OPENROUTER`, so prices, the context window and metrics follow OpenRouter's vendor
  routing. The cache monitor's expected read stays first-party only: a miss through OpenRouter's
  own Anthropic accounts may be routing, not a defect.
- **Capabilities by Anthropic's id:** the formatter resolves thinking and mid-conversation system
  messages from `llm_model_anthropic_id()` (`anthropic/claude-opus-5.5` → `claude-opus-5-5`).
- **Reasoning doesn't cross routes:** a turn's stored blocks are bound to their carrier (host + key
  tag), so a conversation moving between OpenRouter and direct Claude thinks fresh; text and tool
  calls carry over.

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
