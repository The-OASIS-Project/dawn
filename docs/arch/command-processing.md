# Command Processing Architecture

Part of the [D.A.W.N. architecture](../../ARCHITECTURE.md) — see the main doc for layer rules, threading model, and lock ordering.

---

DAWN supports two parallel command processing paths that both converge on a unified executor.

## Tool Registry System

The **tool_registry** (`src/tools/tool_registry.c`) is the primary mechanism for registering modular tools. Each tool is a self-contained module with its own metadata, parameters, and callback:

```c
static const tool_metadata_t my_tool_metadata = {
   .name = "my_tool",              // API name for LLM tool calls
   .device_string = "my device",   // Internal device identifier
   .description = "Tool description for LLM schema",
   .params = my_tool_params,       // Parameter definitions
   .param_count = 2,
   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_NETWORK,
   .default_remote = true,
   .callback = my_tool_callback,
};
```

**Key features**:

- **O(1) lookup**: FNV-1a hash tables for name, device_string, and aliases
- **Self-registration**: each tool calls `tool_registry_register()` during init
- **LLM schema generation**: `llm_tools_get_openai_format()` / `llm_tools_get_claude_format()` build provider-specific schemas
- **Capability flags**: `TOOL_CAP_NETWORK`, `TOOL_CAP_DANGEROUS`, etc. for safety classification

**Registered tool modules** (as of November 2026, named by their register function;
`tools_register_all()` in `src/tools/tools_init.c` is the list of record, some behind build options. A
module's tool name can differ, e.g. `homeassistant` registers `home_assistant`, `schwab` registers
`stocks`, `url` registers `url_fetch`, and a module can register more than one tool; MCP servers' tools are added at runtime by the MCP bridge):

- attention, audio_device, calculator, calendar, code_project, coding_agent, context_expand, date
- deep_research, document_grep, document_index, document_manage, document_read, document_search
- email, faceplate, homeassistant, hud_control, hud_mode, image_search, job, llm_status, memory
- messaging, music, phone, plan_executor, recall, recording, render_visual, research_conclude
- research_mark_unanswerable, research_plan, research_record, reset_conversation, result_read
- scheduler, schwab, search, sfx, shutdown, stat, suit, switch_llm, time, tts, url, viewing
- visual_offset, voice_amplifier, volume, weather

## Command Flow

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           USER INPUT (Voice/Text)                           │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                      PROCESSING MODE (dawn.toml: commands.processing_mode)  │
│                                                                             │
│   direct_only ──────► Pattern match only, no LLM                            │
│   llm_only ─────────► Send everything to LLM                                │
│   direct_first ─────► Try patterns, fallback to LLM                         │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                    ┌─────────────────┴─────────────────┐
                    ▼                                   ▼
┌──────────────────────────────┐       ┌──────────────────────────────────────┐
│   PATH 1: DIRECT MATCHING    │       │         PATH 2: LLM INVOCATION       │
│   (text_to_command_nuevo.c)  │       │                                      │
│                              │       │    ┌────────────────────────────┐    │
│  Device-type patterns:       │       │    │  tools enabled = true?     │    │
│  "turn on %device_name%"     │       │    └────────────┬───────────────┘    │
│  "play %value%"              │       │                 │                    │
│                              │       │                 ▼                    │
│  Extracts device/action/val  │       │         ┌───────────────┐            │
└──────────────┬───────────────┘       │         │ NATIVE TOOLS  │            │
               │                       │         │               │            │
               │                       │         │ LLM returns   │            │
               │                       │         │ structured    │            │
               │                       │         │ tool_calls    │            │
               │                       │         └───────┬───────┘            │
               │                       │                 │                    │
               │                       └─────────────────┼────────────────────┘
               │                                         │
               │                                         ▼
               │                                 ┌─────────────┐
               │                                 │llm_tools_   │
               │                                 │execute()    │
               │                                 │             │
               │                                 │Parses tool  │
               │                                 │call struct  │
               │                                 └─────┬───────┘
               │                                       │
               └───────────────────────────────────────┤
                                                       │
                                                       ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                    UNIFIED COMMAND EXECUTOR (command_executor.c)            │
│                                                                             │
│   command_execute(device, action, value, mosq, &result)                     │
│                                                                             │
│   1. Look up device in tool_registry                                        │
│   2. If callback → invoke tool->callback()                                  │
│   3. If mqtt_only → publish JSON to MQTT topic                              │
│   4. If sync_wait → use command_router for response (viewing)               │
└─────────────────────────────────────────────────────────────────────────────┘
                                             │
               ┌─────────────────────────────┼─────────────────────────────┐
               ▼                             ▼                             ▼
┌──────────────────────┐       ┌──────────────────────┐       ┌─────────────────┐
│ TOOL CALLBACKS       │       │ MQTT-ONLY            │       │ SYNC WAIT       │
│ (src/tools/*.c)      │       │ (Hardware)           │       │ (viewing)       │
│                      │       │                      │       │                 │
│ tool->callback:      │       │ Publish to topic:    │       │ Wait for MQTT   │
│ - weather_tool       │       │ - "hud" → helmet     │       │ response via    │
│ - music_tool         │       │ - "helmet" → helmet  │       │ command_router  │
│ - search_tool        │       │                      │       │                 │
│ - datetime_tool      │       │                      │       │                 │
└──────────────────────┘       └──────────────────────┘       └─────────────────┘
```

## Command Definition Sources

Commands are defined via the **modular tool_registry** system:

1. **Tool Registry** (`src/tools/*.c`)
   - Each tool is a self-contained module with `tool_metadata_t` struct
   - Registered via `tool_registry_register()` during `tools_register_all()`
   - O(1) lookup via FNV-1a hash tables for name, device_string, and aliases

2. **MQTT commands** (`mosquitto_comms.c`)
   - Look up the device's callback in the tool registry (`tool_registry_get_callback()`); there is
     no separate callback table

## Native Tool Calling

Native function calling is the only LLM tool transport. Tools are sent to the provider as API
parameters, and the LLM replies with a structured `tool_calls` array that `llm_tools_execute()`
parses and dispatches through `command_execute()`.

| Aspect         | Native Tools                              |
| -------------- | ----------------------------------------- |
| **Definition** | command_registry → llm_tools              |
| **Prompt**     | Minimal (tools sent as API params)        |
| **Response**   | Structured `tool_calls` array             |
| **Filtering**  | `enabled_local`/`enabled_remote` per tool |
| **Execution**  | `command_execute()`                       |

This is distinct from **Path 1 (direct matching)**, which regex-matches the raw ASR/text input
*before* any LLM call and never involves the model.

## Tool Enable/Disable

Tools can be enabled/disabled per session type (local vs remote):

- Settings UI provides per-tool toggles.
- Disabled tools are omitted from the native tool schemas sent to the LLM.

## Who May Make a Call: Kinds of Action

Every tool action has a **kind**, declared in the tool's `action_kinds` table
(`tool_action_kind_entry_t`, `include/tools/tool_registry.h`). An action the table doesn't list
takes the tool's `default_kind`, which is `TOOL_KIND_ACT` unless set (deny by default; web
search defaults to `FETCH`, music to `DEVICE`).

| Kind | Meaning | Examples |
|---|---|---|
| `READ` | No effect anyone would notice | memory search, calendar today/range, HA status |
| `FETCH` | An outward read: the request reaches a host the caller picks, so it can carry data out | web search, `url_fetch` |
| `STATE` | State scoped to the session or run | the active code project, a research ledger |
| `DEVICE` | An effect someone in the home hears or sees | music, volume |
| `PREPARE` | Stages an item that does nothing until its confirm runs | email draft, call preview, HA unlock preview |
| `ACT` | Changes, sends or starts something | confirm_send, remember, set a timer |

A `PREPARE` entry names its confirm (an `ACT` in the same table). Registration refuses a table
that breaks these rules, and `scripts/check_tool_action_kinds.sh` checks every table at build
time. A tool whose kind depends on more than the action (on its configuration, or the device it
resolves) supplies `classify_call`.

**The caller** is decided from the turn (`tool_call_policy_caller`, `src/core/tool_call_policy.c`):
the **user**; an **unverified sender** (an SMS session: a text can claim any number); a
background **job** on its own session; **unattended** (a background turn such as a job's
follow-up, or no running user turn at all); or a user turn **carrying an email the user attached**.
On the last step of a turn that ends on a save, every caller is narrowed to the save-step row.

| Caller \ kind | act | read | fetch | state | device | prepare |
|---|---|---|---|---|---|---|
| user | allow | allow | allow | allow | allow | allow |
| unverified sender | code | allow | code | allow | code | allow |
| job | refuse | allow | allow | allow | refuse | refuse |
| unattended | refuse | allow | refuse | allow | refuse | refuse |
| attached email | refuse | allow | allow | allow | refuse | allow |
| save step | refuse | refuse | refuse | allow | refuse | refuse |

**code** = the call is held and DAWN texts the number what was asked plus a 6-digit code
(`llm_tools_reply_code.c`, `core/tool_call_challenge.c`, `core/reply_code.c`). The held call runs
once, unchanged, when that code comes back, and STOP cancels it. The text describes the call in
the tool's own words (`describe_call`), or else by its declared, non-empty parameters. The
approval is bound to the call's arguments and to that description, re-made when it runs, so a
changed target is refused. A forger can fake the sender of a text but can't receive the reply.

**Where it's decided:** once per call, in `llm_tools_execute_from_treg` (`tool_call_policy_check`:
the named action in the tool's own spelling, the effective action, its kind, the table). Plan
steps are each decided as they run, inside the plan's scope. An MQTT message that names a session
is decided at its entry (`mosquitto_comms.c`), always as unattended. Not gated, by design: the local mic's direct commands, MQTT messages that
name no session (broker trust), and scheduled briefing steps (only schedulable actions are
allowed at create time).

**Confirms are bound to their preview.** A `PREPARE` stages one item per session and kind in
`core/pending_slots` with a fresh id, which its preview shows and its confirm must name. Each
item records where it was made (`turn_origin_t`, `include/core/turn_origin.h`). A confirm
runs only in the same session, in the user's very next turn (or one of the next few where its
table allows it, or, approved by the user's SMS reply code, a later turn of that session), never in
a turn carrying an attached email or for an item prepared in a turn the user stopped, and only for
the item its id names. So an item staged again (a forged text, content the model read) is never what an earlier
preview's "yes" carries out. Email drafts keep their own tables with random ids under the same
rule.
