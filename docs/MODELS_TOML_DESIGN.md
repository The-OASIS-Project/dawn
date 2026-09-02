# models.toml — model context-window registry

**Status:** in-flight (Aug 2026). Replaces the hard-coded per-model context tables
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
  only for models whose real window ≥ the default — i.e. every modern model. The
  legacy sub-128K OpenAI entries (`gpt-4` 8K, `gpt-3.5-turbo` 16K, `gpt-4-32k` 32K)
  would be **over**-estimated by the 128K default (compaction fires late → the
  provider can 400), so those models specifically require `models.toml` present.
  A conscious trade: we do not keep a compiled floor for models effectively
  extinct in 2026, rather than re-introduce the hard-coded table this replaces.
- There is **no** compiled per-model table any more — `models.toml` is the single
  place those numbers live, which is the whole point. The only compiled numbers
  are the per-provider catch-all defaults.

### Load path

`llm_context_init()` calls a new `load_model_registry()`:
1. Locate `models.toml` via the config search path (reuse `config_file_readable`).
2. Parse with tomlc99 (`tools/toml.h`, already a dependency).
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

## Shipped default numbers (researched Aug 2026)

OpenAI (developers.openai.com + model reference):

| prefix | context | note |
|---|---|---|
| gpt-5.6 | 1050000 | luna/sol etc. |
| gpt-5.5 | 1050000 | API window (Codex surface caps at 400K) |
| gpt-5.4 | 256000 | |
| gpt-5.2 / gpt-5.1 / gpt-5* | 400000 | |
| gpt-4.1* | 1047576 | |
| o1/o3/o4* | 200000 | o1-mini/preview 128000 |
| gpt-4o* | 128000 | |
| gpt-4-turbo / gpt-4-32k / gpt-4 | 128000 / 32768 / 8192 | |
| gpt-3.5* | 16384 | |

Anthropic (**1M is GA at standard pricing since March 2026 — no beta header**):

| prefix | context | note |
|---|---|---|
| claude-opus-4-8 / 4-7 / 4-6 | 1000000 | GA 1M |
| claude-sonnet-4-6 | 1000000 | GA 1M |
| claude-sonnet-5 / fable-5 / mythos-5 | 1000000 | GA 1M |
| claude-haiku-4-5 | 200000 | |
| claude-opus-4-5 / sonnet-4-5 and older | 200000 | |
| claude-3* | 200000 | |

Gemini (unchanged): all current families 1048576.

> **Behavior note:** bumping the Claude flagships 200K→1M changes when DAWN
> compacts a Claude conversation (much later). If a deployment's account tier does
> not actually grant 1M, edit `models.toml` down — no rebuild.
