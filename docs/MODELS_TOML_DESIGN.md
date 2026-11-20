# models.toml — model reference data: context windows, pricing, capabilities

**Status:** context windows and capabilities shipped (Aug–Nov 2026); `[pricing]` in-flight (Nov 2026). Replaces the hard-coded per-model context tables
in `src/llm/llm_context.c` with an editable data file.

## Problem

Per-model context windows for the *direct* cloud providers (OpenAI, Anthropic,
Gemini) live in hard-coded C tables (`s_openai_models[]`, `s_claude_models[]`,
`s_gemini_models[]`). Updating a number — a new model, or a changed window like
Claude's 200K→1M GA — means editing C and **recompiling**. The tables also drift:
gpt-5.4/5.5/5.6 were missing entirely, and every Claude flagship was stale at 200K.

Dynamic fetch is **not** an option for these three: OpenAI/Anthropic/Gemini
`/models` endpoints return only `id`/`object`/`created`/`owned_by` — **no context
window**. (OpenRouter's `/api/v1/models` *does* expose `context_length`, and local
llama.cpp exposes `/props`; DAWN already fetches those live. Those two stay dynamic
and are out of scope here.)

## Design

A standalone, **read-only** TOML data file — `models.toml` — is the source of
truth for direct-provider context windows. DAWN reads it once at startup and
**never rewrites it**, so it is deliberately exempt from the `dawn.toml` settings
round-trip (no `config_to_json`/`config_write_toml`/WebUI wiring, none of the
nine-file hazard). Edit + restart to update; no rebuild.

### File format

```toml
# Prefix -> max input context window (tokens). Longest-prefix-match wins;
# entries are sorted by prefix length at load, so file order does not matter.

[openai]
"gpt-5.6" = 1050000     # gpt-5.6, gpt-5.6-luna, gpt-5.6-sol ...
"gpt-5.5" = 1050000
"gpt-5.4" = 256000
"gpt-5"   = 400000
"gpt-4.1" = 1047576
"gpt-4o"  = 128000

[anthropic]
"claude-opus-4-6"   = 1000000
"claude-sonnet-4-6" = 1000000
"claude-haiku-4-5"  = 200000

[gemini]
"gemini-3" = 1048576
```

Each provider is a TOML table of `"<model-id-prefix>" = <tokens>`. TOML does not
guarantee key order within a table, which is fine: the loader sorts every
provider's entries by **prefix length descending**, so `"gpt-5.6"` always beats
`"gpt-5"` regardless of position — eliminating the ordering footgun the old
first-match-in-array C tables had.

### Location & fallback

- Found via the existing config search path: `./models.toml`, then
  `~/.config/dawn/models.toml`, then `/etc/dawn/models.toml` (first found wins).
- Shipped as a committed `models.toml` in the repo root (no secrets → committed
  directly, not an `.example`); the installer copies it beside `dawn.toml`.
- **Fallback:** if the file is absent or a model isn't listed, the lookup returns
  0 and the caller uses the existing per-provider default
  (`LLM_CONTEXT_DEFAULT_OPENAI` = 128000, the Claude/Gemini defaults). DAWN always
  works. That default is *conservative* (safe against over-running the window)
  only for models whose real window ≥ the default — i.e. every model listed.
  OpenAI models shut down by 2026-10-23 (the sub-128K `gpt-4`, `gpt-3.5-turbo`
  and `gpt-4-32k`, GPT-4 Turbo, o1, o3-mini, o4-mini, gpt-4.1-nano; OpenAI's
  deprecations page, read 2026-11-18) are left out of every table.
- There is **no** compiled per-model table any more — `models.toml` is the single
  place those numbers live, which is the whole point. The only compiled numbers
  are the per-provider catch-all defaults.

### Load path

`llm_context_init()` calls a new `load_model_registry()`:
1. Locate `models.toml` via the config search path (reuse `config_file_readable`).
2. Parse with tomlc99 (`toml.h`, already a dependency).
3. For each of `[openai]`/`[anthropic]`/`[gemini]`, iterate keys
   (`toml_table_nkval` + `toml_key_in` + `toml_int_in`) into a heap array of
   `{ char prefix[N]; int context; }`, then sort by `strlen(prefix)` desc.
4. Store the three arrays in module state (freed in `llm_context_cleanup`).

`lookup_model_context()` iterates the loaded array for the provider (already
prefix-matched via `strncasecmp`); because entries are length-sorted, the first
match is the longest/most-specific. On no registry (file missing) or no match, it
returns 0 → provider default, exactly as today.

### Touch points (small — it is not settings config)

1. `src/llm/llm_context.c` — loader + swap the static tables for the loaded arrays.
2. `models.toml` — new committed data file with current numbers.
3. `scripts/` installer — copy `models.toml` beside `dawn.toml` (like the example).
4. `DEPENDENCIES.md`/`README` — one-line mention.
5. `docs/` — this doc.

No `dawn_config_t`, no `config_env.c`, no `webui_config.c`, no `schema.js`. This is
reference data, not user settings.

### Retired models (`[retired.<provider>]`)

A row names a model its vendor shuts down and the same vendor's replacement:
`"claude-sonnet-4-5" = { by = "claude-sonnet-5-5", on = "2026-11-30" }`.
`src/llm/llm_model_retire.c` loads the rows with the other tables and
`llm_resolve_config()` (the one place every request's model is decided) sends the
replacement instead, from the `on` date (UTC; none: now), logged once per model.
DAWN doesn't rewrite stored conversations for it (a later save from the picker
stores the model that now runs, as any pick does).  At
startup DAWN warns about each retired model in the settings (the model lists and
the extraction, compaction and silent-observe models).

- A row matches the id and its snapshots (the id, `-`, then digits and dashes
  starting with at least three digits), so `claude-opus-4` never catches
  `claude-opus-4-8`.
- A replacement that is itself retired is followed (a few steps).
- Only at the vendor's own URL: with `[llm.cloud] endpoint` (a proxy, Azure) the
  model id is a name the user chose, and is sent as it is.
- Applied in `llm_resolve_config()` (turns) and `llm_chat_completion_with_config()`
  (side calls that build their config: extraction, compaction, silent observe).
- `[retired.openrouter]` ships empty: OpenRouter ids differ from the vendors'
  (`anthropic/claude-sonnet-4.5`) and OpenRouter doesn't document what a retired
  slug does, so rows go in per slug as they are known.
- The replacement is always the same vendor's: the table is per provider, and an
  OpenRouter row whose replacement names another vendor is refused at load.
  DAWN never moves a conversation to a vendor nobody chose.

## Shipped default numbers (researched Aug 2026)

OpenAI (developers.openai.com + model reference):

| prefix | context | note |
|---|---|---|
| gpt-6-astra / gpt-6-sol / gpt-6.1-sol / gpt-6.1-terra / gpt-6-luna / gpt-6-cyber | 1050000 | added Nov 2026, each by name (no gpt-6 catch-all) |
| gpt-5.6-cyber | 400000 | under the gpt-5.6 row it read as 1050000 |
| gpt-5.6 | 1050000 | luna/sol etc. |
| gpt-5.5 | 1050000 | API window (Codex surface caps at 400K) |
| gpt-5.4 | 256000 | |
| gpt-5.2 / gpt-5.1 / gpt-5* | 400000 | |
| gpt-4.1* | 1047576 | |
| o3 / o3-pro | 200000 | deprecated, shut down Dec 2026 |
| gpt-4o* | 128000 | |

Anthropic (**1M is GA at standard pricing since March 2026 — no beta header**):

| prefix | context | note |
|---|---|---|
| claude-opus-4-8 / 4-7 / 4-6 | 1000000 | GA 1M |
| claude-sonnet-4-6 | 1000000 | GA 1M |
| claude-sonnet-5 / fable-5 / mythos-5 | 1000000 | GA 1M (fable-5 / mythos-5 also match 5.1 and 5.2) |
| claude-haiku-4-5 | 200000 | |
| claude-opus-4-5 / sonnet-4-5 and older | 200000 | |
| claude-3* | 200000 | |

Gemini (unchanged): all current families 1048576. Gemini 4 (added Nov 2026)
shares its 1048576 between input and output.

## Max output per request (`max_output` on the `[thinking.*]` rows, Nov 2026)

Reasoning counts toward `max_tokens` (Anthropic) and `max_output_tokens`
(OpenAI), so a fixed 16K ceiling can end a high-effort reply after its
reasoning with no text. A streamed reply that reasons gets a ceiling by its
effort (`llm_reply_max_tokens`): low and minimal 16384, medium 32768, high
65536, xhigh and max 128000, never below `[llm] max_tokens` and never above the
row's `max_output`. Everything else keeps `[llm] max_tokens`: a call that isn't
streamed (both vendors warn against a long reply on one idle connection), a
reply that doesn't reason, a local model, and a row without `max_output`.
"Streamed" is the transport: briefings and memory extraction aren't streamed on
Claude or Chat Completions, but the OpenAI Responses API is always streamed (a
bare completion collects the stream), so there they are sized by effort too.

| prefix | max_output | source |
|---|---|---|
| claude-fable-5 / opus-5 / opus-5-5 / sonnet-5 / sonnet-5-5 / haiku-5-5 | 128000 | platform.claude.com models overview |
| claude-opus-4-6 / 4-7 / 4-8, claude-sonnet-4-6 | 128000 | |
| claude-opus-4-5 / sonnet-4-5 / haiku-4-5, claude-sonnet-4, claude-3-7-sonnet | 64000 | |
| claude-opus-4 / 4-1 | 32000 | retired on the Claude API |
| gpt-5 / 5.1 / 5.2 / 5.4 / 5.5 / 5.6 (and mini, nano) | 128000 | developers.openai.com model pages |
| o3 | 100000 | |
| claude-mythos-5 | 128000 | each the same model as its Fable (5, 5.1, 5.2) |
| gemini-3 / gemini-2.5 | 65536 | ai.google.dev model pages |
| gemini-4 (carbon, argon) / gemini-4-hydrogen | 65536 | synchronous limit; long decode (up to 1M) isn't used |
| gpt-6-astra / gpt-6-sol / gpt-6.1-sol / gpt-6.1-terra / gpt-6-luna | 128000 | |

Reasoning on the newest rows (Nov 2026): each listed GPT-6 model takes low through
max, and gpt-6-sol, gpt-6.1-terra and gpt-6-luna also take "none" (DAWN's
"disabled"); gpt-6-astra and gpt-6.1-sol can't turn reasoning off. GPT-6 has no
family row: gpt-6-cyber (its page lists no effort levels) and any later gpt-6
model get the OpenAI default, no reasoning parameters, as "gpt-5." does for a
newer gpt-5.x. Gemini 4 can't turn it off either; Carbon and Argon
take low/medium/high, Hydrogen adds xhigh (Google's OpenAI endpoint maps
`reasoning_effort` to `thinking_level`). Cache pricing: `write_5m` is a provider's default write lifetime, the only
write rate DAWN estimates with (Anthropic 5 minutes; OpenAI GPT-5.6 and later
30 minutes, since DAWN sets no lifetime), and `write_1h` its longer one
(Anthropic 1 hour; OpenAI 12 hours, 2.5x, Responses API only). The GPT-6 models write at 1.25x
and read at 0.10x (astra, sol, luna, cyber) or 0.05x (6.1 sol, 6.1 terra); Gemini 4 has implicit caching only, reads at 0.05x
with no write premium.

Every request path is sized this way: Claude Messages (direct and OpenRouter),
OpenAI Responses, and Chat Completions (OpenAI before gpt-5.4, o-series, Gemini,
OpenRouter's other vendors). Each vendor documents that reasoning counts toward
the limit: Anthropic's `max_tokens`, OpenAI's `max_output_tokens` and
`max_completion_tokens`, and Gemini's `max_output_tokens` ("including thought
tokens"). gpt-5.3 has no model page, so no `max_output`: it keeps
`[llm] max_tokens`.

**Held to the window.** Every cloud request's ceiling is then
held to the room the model's context window leaves
(`llm_context_fit_reply`): the window less the request's input and a safety
margin, never below 1024. It is set once the request's tools are attached. The
input is the request as sent: its history, the question only when the history
doesn't already end with it (no builder sends it twice), and its tools
(`llm_compaction_reply_input`). It is calibrated by the session's measured
requests only when the request is that session's own conversation turn, and a
measured request carried its tools, so the calibrated count has them. A side
call (memory extraction, compaction, a briefing, a summarizer), a call with no
session, or a session not yet measured uses its plain estimate plus its tools,
never the local session's or another conversation's count
(`llm_cache_monitor_conversation_session`). The margin is 10% of the input
(the estimate reads ~4 characters a token, and code, non-English text and
images run denser) plus 2% of the window. Not every vendor needs it: on Claude 4.5+
and the OpenAI Responses API a reply that reaches the window just stops,
where older Claude models reject the request outright. Local servers aren't
clamped: they stop at their window rather than reject, and a local window read
before the server answers is only DAWN's 8K default.

## Prices (`[pricing.<provider>]`, Nov 2026)

One table per direct provider holds what DAWN knows about a model's price, so
nothing about it lives in C. Each row carries base prices, cache multiples, or
both:

```toml
[pricing.anthropic]
"claude-opus-5-5"   = { input = 4.00, output = 20.00, read = 0.05, write_5m = 1.25, write_1h = 2.0 }
"claude-sonnet-5-5" = { read = 0.05, write_5m = 1.25, write_1h = 2.0 }
"claude-sonnet-5"   = { input = 2.00, output = 10.00 }
```

- `input` / `output`: list prices in $ per million tokens. A row lists both or
  neither (one alone is logged and ignored). Read by
  `llm_pricing_rate()`; its one user today is the memory re-extraction cost
  estimate (`dawn-admin memory reextract`, `memory_db_admin.c`), which prices
  the cloud providers memory extraction accepts (`claude`, `openai`, and
  `openrouter` through its vendor's table) and gives no estimate for a name
  extraction runs locally (local, Ollama, `gemini`, `anthropic`).
- `read` / `write_5m` / `write_1h`: prompt-cache prices as multiples of the
  input price (`llm_cache_pricing()`), for the "saved by caching" estimates and
  cost telemetry. A multiple a row leaves out is the provider's default.
- Each group is matched by longest prefix **on its own**: a row with only
  cache multiples (`claude-sonnet-5-5`) takes its base prices from the longest
  shorter row that has them (`claude-sonnet-5`), and a row with only base
  prices (`gpt-4o-mini`) takes the multiples of the row above it (`gpt-4o`).
  The first rows kept every price the two old tables gave (a C `COST_TABLE`
  in `memory_db_admin.c` and `[cache_pricing]`); the OpenAI rows were then
  corrected to OpenAI's pricing page (standard tier, 2026-11-18), with a row
  for each model whose price differs from the row it would otherwise take, and
  the GPT-6.2 models added.
- A model no row gives cache multiples gets its provider's compiled default
  (Anthropic 0.10/1.25/2.00, OpenAI and Gemini 0.10/1.00/1.00). A model no row
  gives base prices has no price, and no cost estimate: none is made up. Gemini
  lists no base prices yet.
- OpenRouter's `vendor/model` ids are priced from their vendor's table, as for
  the other tables.

An older models.toml without `[pricing]` uses the built-in prices.

> **Behavior note:** bumping the Claude flagships 200K→1M changes when DAWN
> compacts a Claude conversation (much later). If a deployment's account tier does
> not actually grant 1M, edit `models.toml` down — no rebuild.
