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
- **LLM schema generation**: `tool_registry_generate_llm_tools()` builds provider-specific schemas
- **Capability flags**: `TOOL_CAP_NETWORK`, `TOOL_CAP_DANGEROUS`, etc. for safety classification

**Registered tools** (as of March 2026):

- audio_tools, calculator_tool, calendar_tool, datetime_tool, document_read_tool
- document_search_tool, email_tool, homeassistant_tool, hud_tools, llm_status_tool
- memory_tool, music_tool, plan_executor_tool, reset_conversation_tool, scheduler_tool
- search_tool, shutdown_tool, switch_llm_tool, url_tool, viewing_tool, volume_tool, weather_tool

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
│  Regex patterns from JSON:   │       │    │  tools enabled = true?     │    │
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
│   1. Look up device in command_registry                                     │
│   2. If has_callback → invoke deviceCallbackArray[type].callback()          │
│   3. If mqtt_only → publish JSON to MQTT topic                              │
│   4. If sync_wait → use command_router for response (viewing)               │
└─────────────────────────────────────────────────────────────────────────────┘
                                             │
               ┌─────────────────────────────┼─────────────────────────────┐
               ▼                             ▼                             ▼
┌──────────────────────┐       ┌──────────────────────┐       ┌─────────────────┐
│ C CALLBACKS          │       │ MQTT-ONLY            │       │ SYNC WAIT       │
│ (mosquitto_comms.c)  │       │ (Hardware)           │       │ (viewing)       │
│                      │       │                      │       │                 │
│ deviceCallbackArray: │       │ Publish to topic:    │       │ Wait for MQTT   │
│ - weather → get_wea  │       │ - "hud" → helmet     │       │ response via    │
│ - music → play_music │       │ - "helmet" → helmet  │       │ command_router  │
│ - search → web_sear  │       │ - "homeassistant"    │       │                 │
│ - date → get_date    │       │                      │       │                 │
└──────────────────────┘       └──────────────────────┘       └─────────────────┘
```

## Command Definition Sources

Commands are defined via the **modular tool_registry** system:

1. **Tool Registry** (`src/tools/*.c`)
   - Each tool is a self-contained module with `tool_metadata_t` struct
   - Registered via `tool_registry_register()` during `tools_register_all()`
   - O(1) lookup via FNV-1a hash tables for name, device_string, and aliases

2. **Legacy Device Callbacks** (`mosquitto_comms.c`)
   - `deviceCallbackArray[]` maps device types to C functions
   - Core system devices: weather, music, search, homeassistant, etc.

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
