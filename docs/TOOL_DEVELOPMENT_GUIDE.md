# DAWN Tool Development Guide

This guide explains how to create new tools for DAWN using the modular tool registry system. Tools are standalone modules that can be compiled in or out via CMake options, and they own all their own code including configuration, callbacks, and LLM metadata.

## Overview

The tool registry provides:
- **Compile-time exclusion**: Tools can be disabled via `-DDAWN_ENABLE_X_TOOL=OFF`
- **Self-contained modules**: Each tool owns its config, parser, callback, and LLM schema
- **Bridge mode**: New tools work alongside legacy callbacks during migration
- **Capability flags**: Security classifications for dangerous, network, filesystem, and secrets access

## Architecture

### Why Everything is `static`

In the tool implementation file, most declarations are `static`:

```c
static mytool_config_t s_config = { ... };           // Internal config
static void mytool_parse_config(...) { ... }         // Internal function
static char *mytool_callback(...) { ... }            // Internal function
static const tool_metadata_t mytool_metadata = {...}; // Internal metadata

int mytool_tool_register(void) { ... }               // ONLY this is external
```

**Why?** Encapsulation. The tool is a self-contained module:
- The registry stores **pointers** to the config, parser, callback, and metadata
- Only `mytool_tool_register()` needs to be callable from outside (from `tools_init.c`)
- Everything else is accessed through those stored pointers
- This prevents naming conflicts between tools and keeps the global namespace clean

The registration function passes the address of the static metadata struct to the registry, which stores pointers to all the tool's internal components.

## Quick Start

To create a new tool:

1. Create `include/tools/mytool_tool.h` (minimal - just registration function)
2. Create `src/tools/mytool_tool.c` (config, callback, metadata, registration)
3. Add to `cmake/DawnTools.cmake`
4. Add include and registration call to `src/tools/tools_init.c`
5. Rebuild

## File Structure

```
include/tools/
├── tool_registry.h      # Core registry API
├── tools_init.h         # Central registration entry point
├── shutdown_tool.h      # Example: shutdown tool header
└── mytool_tool.h        # Your tool header

src/tools/
├── tool_registry.c      # Core registry implementation
├── tools_init.c         # Central registration (add new tools here)
├── shutdown_tool.c      # Example: shutdown tool implementation
└── mytool_tool.c        # Your tool implementation
```

---

## Data Structure Reference

### `treg_param_t` - Parameter Definition

Defines a single parameter that the LLM can pass to your tool.

```c
typedef struct {
   const char *name;                               /* Parameter name */
   const char *description;                        /* Description for LLM */
   tool_param_type_t type;                         /* Parameter type */
   bool required;                                  /* Is this parameter required? */
   tool_param_mapping_t maps_to;                   /* How to map to callback args */
   const char *field_name;                         /* Custom field name (MAPS_TO_CUSTOM only) */
   const char *enum_values[TOOL_PARAM_ENUM_MAX];   /* Allowed values (ENUM type only) */
   int enum_count;                                 /* Number of enum values */
   const char *unit;                               /* Unit hint (e.g., "pixels", "seconds") */
} treg_param_t;
```

#### Field Details

| Field | Required | Description |
|-------|----------|-------------|
| `name` | Yes | Parameter name as seen by LLM (e.g., `"query"`, `"action"`, `"volume"`) |
| `description` | Yes | Human-readable description for the LLM to understand when to use this parameter |
| `type` | Yes | Data type - see Parameter Types below |
| `required` | Yes | If `true`, LLM must provide this parameter; if `false`, it's optional |
| `maps_to` | Yes | How this parameter maps to callback arguments - see Parameter Mapping below |
| `field_name` | Only if `maps_to = TOOL_MAPS_TO_CUSTOM` | The custom field name in the JSON |
| `enum_values` | Only if `type = TOOL_PARAM_TYPE_ENUM` | Array of allowed string values |
| `enum_count` | Only if `type = TOOL_PARAM_TYPE_ENUM` | Number of values in `enum_values` |
| `unit` | No | Optional hint about units (displayed to LLM, not enforced) |

#### Parameter Types (`tool_param_type_t`)

| Type | Description | Example Values |
|------|-------------|----------------|
| `TOOL_PARAM_TYPE_STRING` | Free-form text | `"hello world"`, `"search query"` |
| `TOOL_PARAM_TYPE_INT` | Integer number | `42`, `-5`, `0` |
| `TOOL_PARAM_TYPE_NUMBER` | Floating-point number | `3.14`, `0.5`, `-2.7` |
| `TOOL_PARAM_TYPE_BOOL` | Boolean true/false | `true`, `false` |
| `TOOL_PARAM_TYPE_ENUM` | String from a fixed set | One of the values in `enum_values` |

#### Parameter Mapping (`tool_param_mapping_t`)

Determines how the parameter value is passed to your callback function:

| Mapping | Passed As | Use Case |
|---------|-----------|----------|
| `TOOL_MAPS_TO_VALUE` | `value` argument | Primary data (search query, filename, number) |
| `TOOL_MAPS_TO_ACTION` | `action` argument | Subcommand/verb (play, stop, get, set) |
| `TOOL_MAPS_TO_DEVICE` | Device lookup | Meta-tools that dispatch to other tools |
| `TOOL_MAPS_TO_CUSTOM` | Named field | Special cases requiring custom field names |

**Callback signature reminder:**
```c
char *callback(const char *action, char *value, int *should_respond);
//              ^-- MAPS_TO_ACTION  ^-- MAPS_TO_VALUE
```

#### Example Parameter Definitions

```c
/* Simple string parameter (required) */
{
   .name = "query",
   .description = "The search query to look up",
   .type = TOOL_PARAM_TYPE_STRING,
   .required = true,
   .maps_to = TOOL_MAPS_TO_VALUE,
}

/* Enum parameter for action selection (optional with default).
 * Name a TOOL_MAPS_TO_ACTION param "action" — the generic action/value
 * contract (and the scheduler's tool_action field) maps onto the param
 * literally named "action", so a differently-named selector won't be
 * filled correctly when the tool is scheduled. */
{
   .name = "action",
   .description = "Search category: web, news, science, or images",
   .type = TOOL_PARAM_TYPE_ENUM,
   .required = false,
   .maps_to = TOOL_MAPS_TO_ACTION,
   .enum_values = { "web", "news", "science", "images" },
   .enum_count = 4,
}

/* Integer parameter with units */
{
   .name = "volume",
   .description = "Volume level to set",
   .type = TOOL_PARAM_TYPE_INT,
   .required = true,
   .maps_to = TOOL_MAPS_TO_VALUE,
   .unit = "percent",  /* Informational only */
}
```

---

### `tool_metadata_t` - Complete Tool Definition

The main structure that defines everything about your tool.

```c
typedef struct {
   /* Identity */
   const char *name;                                  /* API name */
   const char *device_string;                         /* Callback device name */
   const char *topic;                                 /* MQTT topic */
   const char *aliases[TOOL_ALIAS_MAX];               /* Alternative names */
   int alias_count;                                   /* Number of aliases */

   /* LLM Tool Schema */
   const char *description;                           /* Tool description for LLM */
   const treg_param_t *params;                        /* Parameter definitions */
   int param_count;                                   /* Number of parameters */

   /* Device Mapping (for meta-tools) */
   const tool_device_map_t *device_map;               /* Maps param values to devices */
   int device_map_count;                              /* Number of device map entries */

   /* Behavior Flags */
   tool_device_type_t device_type;                    /* boolean, analog, getter, etc. */
   tool_capability_t capabilities;                    /* Security/capability flags */
   bool skip_followup;                                /* Don't send result back to LLM */
   bool mqtt_only;                                    /* Only available via MQTT */
   bool sync_wait;                                    /* Wait for MQTT response */
   bool default_local;                                /* Available to local sessions */
   bool default_remote;                               /* Available to remote sessions */

   /* Config */
   void *config;                                      /* Pointer to tool's config struct */
   size_t config_size;                                /* sizeof() the config struct */
   tool_config_parser_fn config_parser;               /* Parser for TOML section */
   const char *config_section;                        /* TOML section name */

   /* Secret Requirements */
   const tool_secret_requirement_t *secret_requirements;

   /* Lifecycle */
   tool_init_fn init;                                 /* Called after config parse */
   tool_cleanup_fn cleanup;                           /* Called at shutdown */

   /* Callback */
   tool_callback_fn callback;                         /* Execute tool functionality */
} tool_metadata_t;
```

#### Identity Fields

| Field | Required | Description |
|-------|----------|-------------|
| `name` | Yes | Primary tool name used in API calls and LLM tool schema (e.g., `"weather"`, `"calculator"`) |
| `device_string` | Yes | Device name used for callback lookup - typically same as `name` |
| `topic` | Yes | MQTT topic for this tool (usually `"dawn"` for local tools) |
| `aliases` | No | Alternative names that also resolve to this tool (e.g., `{"calc", "math"}`) |
| `alias_count` | No | Number of aliases in the array (0 if no aliases) |

#### LLM Schema Fields

| Field | Required | Description |
|-------|----------|-------------|
| `description` | Yes | **Important!** This is what the LLM reads to decide when to use your tool. Be clear and specific about what the tool does and when to use it. |
| `params` | No | Pointer to array of `treg_param_t` parameter definitions. NULL if tool takes no parameters. |
| `param_count` | No | Number of parameters in the array. 0 if no parameters. |

**Writing Good Descriptions:**
```c
/* Bad - vague */
.description = "Does weather stuff"

/* Good - specific and helpful */
.description = "Get current weather conditions and forecast for a location. "
               "Provide a city name or 'here' for local weather. "
               "Returns temperature, conditions, humidity, and forecast."
```

#### Device Mapping (Meta-tools Only)

For tools that dispatch to other tools based on a parameter value:

| Field | Required | Description |
|-------|----------|-------------|
| `device_map` | No | Array of `tool_device_map_t` entries mapping parameter values to device names |
| `device_map_count` | No | Number of entries in device_map |

Example: An "audio_device" meta-tool that switches between capture and playback:
```c
static const tool_device_map_t audio_device_map[] = {
   { "capture", "audio capture device" },
   { "playback", "audio playback device" },
};
/* Then in metadata: */
.device_map = audio_device_map,
.device_map_count = 2,
```

#### Behavior Flags

| Field | Default | Description |
|-------|---------|-------------|
| `device_type` | - | Classification affecting action word parsing (see Device Types) |
| `capabilities` | `TOOL_CAP_NONE` | Security capabilities (see Capabilities) |
| `skip_followup` | `false` | `true` to skip LLM follow-up (see [skip_followup Behavior](#skip_followup-behavior) below) |
| `mqtt_only` | `false` | `true` if tool only works via MQTT (not voice/WebUI) |
| `sync_wait` | `false` | `true` to wait for MQTT response before continuing |
| `default_local` | `true` | `true` if available for local (microphone) sessions |
| `default_remote` | - | `true` if available for remote (WebUI/network) sessions |

#### Config Fields

| Field | Required | Description |
|-------|----------|-------------|
| `config` | No | Pointer to your static config struct (NULL if no config needed) |
| `config_size` | No | `sizeof(your_config_struct)` - used for validation |
| `config_parser` | No | Function to parse TOML config section (NULL if no config) |
| `config_section` | No | TOML section name, e.g., `"weather"` for `[weather]` section |

**Config parser signature:**
```c
void my_config_parser(toml_table_t *table, void *config);
// table may be NULL if section doesn't exist in config file
// config points to your static config struct
```

#### Secret Requirements

| Field | Required | Description |
|-------|----------|-------------|
| `secret_requirements` | Only if `TOOL_CAP_SECRETS` | NULL-terminated array of `tool_secret_requirement_t` |

```c
typedef struct {
   const char *secret_name;  /* Key in secrets.toml */
   bool required;            /* Fail init if missing? */
} tool_secret_requirement_t;
```

#### Lifecycle Functions

| Field | Required | Description |
|-------|----------|-------------|
| `init` | No | Called once after config parsing. Return 0 for success, non-zero to fail. |
| `cleanup` | No | Called once at shutdown. Free any resources. |

**Function signatures:**
```c
int my_init(void);      /* Return 0 on success */
void my_cleanup(void);  /* No return value */
```

#### Callback

| Field | Required | Description |
|-------|----------|-------------|
| `callback` | Yes | The main function that executes tool functionality |

**Callback signature:**
```c
char *my_callback(const char *action, char *value, int *should_respond);
```

- `action`: The action/verb (from `MAPS_TO_ACTION` parameter, or default action)
- `value`: The primary value (from `MAPS_TO_VALUE` parameter)
- `should_respond`: Set to 1 to return result to LLM, 0 to handle silently
- **Returns**: Heap-allocated string (caller frees), or NULL

---

### Device Types (`tool_device_type_t`)

Affects how voice commands are parsed and what action words are recognized:

| Type | Description | Typical Actions |
|------|-------------|-----------------|
| `TOOL_DEVICE_TYPE_BOOLEAN` | On/off toggle device | "enable X", "disable X", "turn on X", "turn off X" |
| `TOOL_DEVICE_TYPE_ANALOG` | Value-setting device | "set X to Y", "X to Y" |
| `TOOL_DEVICE_TYPE_GETTER` | Read-only query | "get X", "what is X", "check X" |
| `TOOL_DEVICE_TYPE_MUSIC` | Media playback control | "play", "stop", "pause", "next", "previous" |
| `TOOL_DEVICE_TYPE_TRIGGER` | Single action trigger | "trigger X", "activate X", "X" |
| `TOOL_DEVICE_TYPE_PASSPHRASE` | Requires passphrase verification | Same as trigger but value is passphrase |

### Capability Flags (`tool_capability_t`)

Can be combined with bitwise OR (`|`):

| Flag | Value | Description | Requirements |
|------|-------|-------------|--------------|
| `TOOL_CAP_NONE` | 0 | No special requirements | Default for safe tools |
| `TOOL_CAP_DANGEROUS` | 1 | Could cause harm if misused | Config must have `enabled` as first field, defaulting to `false` |
| `TOOL_CAP_NETWORK` | 2 | Requires network access | Informational |
| `TOOL_CAP_FILESYSTEM` | 4 | Accesses filesystem | Informational |
| `TOOL_CAP_SECRETS` | 8 | Uses secrets.toml credentials | Must declare `secret_requirements` |
| `TOOL_CAP_ARMOR_FEATURE` | 16 | OASIS armor-specific feature | Informational |

Example combining flags:
```c
.capabilities = TOOL_CAP_NETWORK | TOOL_CAP_SECRETS,
```

### skip_followup Behavior

The `skip_followup` flag controls whether the tool's callback result is sent back to the LLM for a follow-up response.

**When `skip_followup = false` (default):**
1. Tool callback executes and returns a result string
2. Result is sent back to the LLM
3. LLM generates a natural language response incorporating the result
4. User hears/sees the LLM's conversational response

**Example flow (weather tool):**
```
User: "What's the weather?"
→ Tool returns: {"temp": 72, "condition": "sunny"}
→ LLM responds: "It's currently 72 degrees and sunny outside."
```

**When `skip_followup = true`:**
1. Tool callback executes and returns a result string
2. Result is returned directly to the user (or discarded if NULL)
3. LLM does NOT generate a follow-up response
4. Saves an LLM API call and reduces latency

**Voice pipeline note:** When `skip_followup = true` and the callback returns NULL
(i.e., `should_respond = 0`), the tool loop signals this via a thread-local flag
(`llm_tool_loop_did_skip_followup()`). The voice pipeline in `dawn.c` checks this
flag to distinguish "tool completed silently" from "LLM API error" — without it,
a NULL response would trigger an error message ("I'm currently unavailable").
If your tool uses `skip_followup = true` with `should_respond = 0`, this is
handled automatically.

**Example flow (music play action):**
```
User: "Play some jazz"
→ Tool starts playback, returns NULL
→ Music plays immediately (no LLM response needed)
```

**When to use `skip_followup = true`:**
- **Media controls**: Play, pause, stop, next, previous - the action IS the response
- **Toggle switches**: Turning lights on/off - confirmation via device state is sufficient
- **Direct actions**: Where callback handles user notification (TTS, visual feedback)
- **Performance-critical**: When minimizing latency matters more than conversational flow

**When to use `skip_followup = false` (default):**
- **Getter tools**: Where the data needs to be presented conversationally
- **Complex results**: Where raw data benefits from LLM interpretation
- **Context-dependent**: Where the LLM should acknowledge and potentially act further
- **Error handling**: Where the LLM can explain what went wrong

**Note:** The callback can also control this per-invocation via the `should_respond` parameter:
```c
static char *my_callback(const char *action, char *value, int *should_respond) {
   if (some_condition) {
      *should_respond = 0;  /* Skip follow-up for this specific call */
      return NULL;
   }
   *should_respond = 1;  /* Normal flow */
   return strdup("Result for LLM to interpret");
}
```

---

## Creating a New Tool - Full Example

### Step 1: Header File (`include/tools/mytool_tool.h`)

The header is minimal - only the registration function needs external linkage:

```c
#ifndef MYTOOL_TOOL_H
#define MYTOOL_TOOL_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the mytool with the tool registry
 * @return 0 on success, non-zero on error
 */
int mytool_tool_register(void);

#ifdef __cplusplus
}
#endif

#endif /* MYTOOL_TOOL_H */
```

### Step 2: Implementation File (`src/tools/mytool_tool.c`)

```c
/*
 * [GPL header - see CLAUDE.md for template]
 *
 * MyTool - Description of what your tool does
 */

#include "tools/mytool_tool.h"
#include "tools/tool_registry.h"
#include "logging.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* TOML parsing */
#include "tools/toml.h"

/* =============================================================================
 * Tool Configuration
 *
 * Define your tool's configurable settings here. These can be set in dawn.toml
 * under a section matching config_section (e.g., [mytool]).
 * ============================================================================= */

typedef struct {
   /* For TOOL_CAP_DANGEROUS tools, 'enabled' MUST be first and default false */
   bool enabled;
   char api_endpoint[256];
   int timeout_seconds;
   bool verbose_logging;
} mytool_config_t;

/* Static config instance - set safe defaults */
static mytool_config_t s_config = {
   .enabled = true,           /* false for dangerous tools */
   .api_endpoint = "https://api.example.com",
   .timeout_seconds = 30,
   .verbose_logging = false,
};

/* =============================================================================
 * Config Parser
 *
 * Called during startup to read tool-specific settings from dawn.toml.
 * The table parameter may be NULL if the config section doesn't exist.
 * ============================================================================= */

static void mytool_parse_config(toml_table_t *table, void *config) {
   mytool_config_t *cfg = (mytool_config_t *)config;

   if (!table) {
      /* No [mytool] section in config - use defaults */
      return;
   }

   /* Parse boolean */
   toml_datum_t enabled = toml_bool_in(table, "enabled");
   if (enabled.ok) {
      cfg->enabled = enabled.u.b;
   }

   /* Parse string - TOML library allocates, we must free */
   toml_datum_t endpoint = toml_string_in(table, "api_endpoint");
   if (endpoint.ok) {
      strncpy(cfg->api_endpoint, endpoint.u.s, sizeof(cfg->api_endpoint) - 1);
      cfg->api_endpoint[sizeof(cfg->api_endpoint) - 1] = '\0';
      free(endpoint.u.s);
   }

   /* Parse integer */
   toml_datum_t timeout = toml_int_in(table, "timeout_seconds");
   if (timeout.ok) {
      cfg->timeout_seconds = (int)timeout.u.i;
   }

   /* Parse another boolean */
   toml_datum_t verbose = toml_bool_in(table, "verbose_logging");
   if (verbose.ok) {
      cfg->verbose_logging = verbose.u.b;
   }
}

/* =============================================================================
 * Tool Callback
 *
 * Main entry point when the tool is invoked. Called with:
 * - action: The verb/subcommand (from MAPS_TO_ACTION parameter)
 * - value: The primary data (from MAPS_TO_VALUE parameter)
 * - should_respond: Set to 1 to return result to LLM, 0 for silent handling
 *
 * Returns: Heap-allocated string (caller frees), or NULL
 * ============================================================================= */

static char *mytool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1;  /* Default: return result to LLM */

   if (s_config.verbose_logging) {
      LOG_INFO("mytool: action='%s', value='%s'", action ? action : "(null)",
               value ? value : "(null)");
   }

   /* Validate input */
   if (!value || strlen(value) == 0) {
      return strdup("Please provide a value for mytool.");
   }

   /* Handle different actions */
   if (!action || strcmp(action, "get") == 0) {
      /* Default action: get */
      char *result = malloc(256);
      if (result) {
         snprintf(result, 256, "MyTool result for '%s' (timeout: %ds)", value,
                  s_config.timeout_seconds);
      }
      return result;

   } else if (strcmp(action, "set") == 0) {
      /* Set action */
      LOG_INFO("mytool: setting value to '%s'", value);
      return strdup("Value set successfully.");

   } else if (strcmp(action, "status") == 0) {
      /* Status action */
      char *result = malloc(512);
      if (result) {
         snprintf(result, 512, "MyTool status: enabled=%s, endpoint=%s",
                  s_config.enabled ? "true" : "false", s_config.api_endpoint);
      }
      return result;
   }

   return strdup("Unknown action. Use: get, set, or status.");
}

/* =============================================================================
 * Lifecycle Functions (Optional)
 *
 * init: Called once after config parsing, before tool is used
 * cleanup: Called once at shutdown
 * ============================================================================= */

static int mytool_init(void) {
   LOG_INFO("mytool: initialized (endpoint=%s, timeout=%ds)",
            s_config.api_endpoint, s_config.timeout_seconds);

   /* Perform any one-time setup here */
   /* Return non-zero to indicate failure (tool won't be available) */
   return 0;
}

static void mytool_cleanup(void) {
   LOG_INFO("mytool: cleanup");
   /* Free any resources allocated in init */
}

/* =============================================================================
 * Parameter Definitions
 *
 * Define what parameters the LLM can pass to this tool.
 * These become the tool's schema in the LLM API.
 * ============================================================================= */

static const treg_param_t mytool_params[] = {
   {
      .name = "query",
      .description = "The query or value to process",
      .type = TOOL_PARAM_TYPE_STRING,
      .required = true,
      .maps_to = TOOL_MAPS_TO_VALUE,
   },
   {
      .name = "action",
      .description = "Action to perform: get (default), set, or status",
      .type = TOOL_PARAM_TYPE_ENUM,
      .required = false,
      .maps_to = TOOL_MAPS_TO_ACTION,
      .enum_values = { "get", "set", "status" },
      .enum_count = 3,
   },
};

/* =============================================================================
 * Tool Metadata
 *
 * Complete definition of the tool for the registry.
 * This struct is passed to tool_registry_register().
 * ============================================================================= */

static const tool_metadata_t mytool_metadata = {
   /* Identity - how the tool is named and addressed */
   .name = "mytool",              /* Primary name for API/LLM */
   .device_string = "mytool",     /* Device name for callback lookup */
   .topic = "dawn",               /* MQTT topic */
   .aliases = { "my_tool" },      /* Alternative names */
   .alias_count = 1,

   /* LLM Tool Schema - what the LLM sees */
   .description = "Process queries using MyTool. Use 'get' to retrieve data, "
                  "'set' to update values, or 'status' to check tool status.",
   .params = mytool_params,
   .param_count = 2,

   /* Device Mapping - NULL for simple tools */
   .device_map = NULL,
   .device_map_count = 0,

   /* Behavior Flags */
   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_NETWORK,  /* This tool makes network calls */
   .skip_followup = false,            /* Return results to LLM */
   .mqtt_only = false,                /* Available via voice/WebUI */
   .sync_wait = false,                /* Don't wait for MQTT response */
   .default_local = true,             /* Available for local sessions */
   .default_remote = true,            /* Available for remote sessions */

   /* Config - links to our config struct and parser */
   .config = &s_config,
   .config_size = sizeof(s_config),
   .config_parser = mytool_parse_config,
   .config_section = "mytool",        /* [mytool] in dawn.toml */

   /* Secrets - NULL if no API keys needed */
   .secret_requirements = NULL,

   /* Lifecycle */
   .init = mytool_init,
   .cleanup = mytool_cleanup,

   /* Callback - the main function */
   .callback = mytool_callback,
};

/* =============================================================================
 * Registration
 *
 * This is the ONLY function with external linkage.
 * Called from tools_init.c during startup.
 * ============================================================================= */

int mytool_tool_register(void) {
   return tool_registry_register(&mytool_metadata);
}
```

### Step 3: CMake Integration (`cmake/DawnTools.cmake`)

Add your tool option at the top:
```cmake
option(DAWN_ENABLE_MYTOOL_TOOL "Enable my custom tool" ON)
```

Add the conditional compilation:
```cmake
# MyTool
if(DAWN_ENABLE_MYTOOL_TOOL)
    add_definitions(-DDAWN_ENABLE_MYTOOL_TOOL)
    list(APPEND TOOL_SOURCES src/tools/mytool_tool.c)
    message(STATUS "DAWN: MyTool ENABLED")
else()
    message(STATUS "DAWN: MyTool DISABLED")
endif()
```

### Step 4: Registration in `tools_init.c`

Add the include near other tool includes in `src/tools/tools_init.c`:
```c
#ifdef DAWN_ENABLE_MYTOOL_TOOL
#include "tools/mytool_tool.h"
#endif
```

Add registration call in `tools_register_all()`:
```c
#ifdef DAWN_ENABLE_MYTOOL_TOOL
   if (mytool_tool_register() != 0) {
      LOG_WARNING("Failed to register mytool");
   }
#endif
```

**Note:** `dawn.c` only includes `tools/tools_init.h` and calls `tools_register_all()`. Individual tool headers and registration calls are centralized in `tools_init.c` to keep the main application decoupled from specific tools.

### Step 5: Config File (`dawn.toml`)

```toml
[mytool]
enabled = true
api_endpoint = "https://api.myservice.com/v1"
timeout_seconds = 60
verbose_logging = false
```

---

## Security Considerations

### Dangerous Tools

Tools with `TOOL_CAP_DANGEROUS` (like shutdown) must:

1. Have `enabled` as the **first** field in their config struct
2. Default `enabled` to `false`
3. Require explicit enable in `dawn.toml`

The registry validates this at registration time.

### Secret Requirements

Tools using `TOOL_CAP_SECRETS` must declare what secrets they need:

```c
static const tool_secret_requirement_t mytool_secrets[] = {
   { "mytool_api_key", true },   /* required - fail if missing */
   { "mytool_backup_key", false }, /* optional */
   { NULL, false }                 /* terminator */
};

/* In metadata: */
.capabilities = TOOL_CAP_SECRETS,
.secret_requirements = mytool_secrets,
```

Access secrets in your callback:
```c
const char *key = tool_registry_get_secret("mytool", "mytool_api_key");
if (!key) {
   return strdup("API key not configured");
}
```

### Constant-Time Passphrase Comparison

For passphrase-protected tools, use constant-time comparison to prevent timing attacks:

```c
static int constant_time_compare(const char *a, const char *b) {
   if (!a || !b) return 1;
   size_t len_a = strlen(a);
   size_t len_b = strlen(b);
   volatile int result = (int)(len_a ^ len_b);
   size_t min_len = (len_a < len_b) ? len_a : len_b;
   for (size_t i = 0; i < min_len; i++) {
      result |= ((unsigned char)a[i]) ^ ((unsigned char)b[i]);
   }
   return result;
}
```

---

## Migration Checklist

When migrating an existing callback from `mosquitto_comms.c`:

1. [ ] Identify the callback function and its entry in `deviceCallbackArray`
2. [ ] Identify any config in `dawn_config.h` and parser in `config_parser.c`
3. [ ] Create header file with registration function declaration
4. [ ] Create implementation file with:
   - [ ] Config struct (copy from dawn_config.h)
   - [ ] Config parser (copy/adapt from config_parser.c)
   - [ ] Callback function (copy from mosquitto_comms.c)
   - [ ] Parameter definitions
   - [ ] Tool metadata
   - [ ] Registration function
5. [ ] Update callback to use local `s_config` instead of `g_config`
6. [ ] Add CMake option to `DawnTools.cmake`
7. [ ] Add include and registration call to `tools_init.c`
8. [ ] Build and test with tool enabled
9. [ ] Build and test with tool disabled
10. [ ] (Later) Remove legacy code from source files

---

## Testing

### Build with Tool Enabled
```bash
cmake --preset debug
make -C build-debug -j8
```

### Build with Tool Disabled
```bash
cmake --preset debug -DDAWN_ENABLE_MYTOOL_TOOL=OFF
make -C build-debug -j8
# Should compile without mytool code
```

### Runtime Testing
1. Enable tool in `dawn.toml` if needed
2. Test via voice command
3. Test via MQTT
4. Test via WebUI

---

## Registry API Reference

### Registration
```c
int tool_registry_register(const tool_metadata_t *metadata);
```

### Lookup
```c
const tool_metadata_t *tool_registry_lookup(const char *name);
const tool_metadata_t *tool_registry_find(const char *name_or_alias);
tool_callback_fn tool_registry_get_callback(const char *name);
bool tool_registry_is_enabled(const char *name);
```

### Config Access
```c
const char *tool_registry_get_secret(const char *tool_name, const char *secret_name);
const char *tool_registry_get_config_string(const char *path);
```

### Iteration
```c
void tool_registry_foreach(tool_foreach_callback_t callback, void *user_data);
int tool_registry_count(void);
int tool_registry_enabled_count(void);
```

---

## Two-Step Tool Pattern (Instruction Loader)

Some tools need detailed operational instructions that are too large to include in every conversation's system prompt. The two-step pattern solves this: the LLM calls a lightweight "load instructions" tool first, gets the rules it needs, then calls the actual tool.

For the full design rationale, see the [TWO_STEP_TOOL_PATTERN.md](https://github.com/The-OASIS-Project/atlas/blob/main/dawn/archive/TWO_STEP_TOOL_PATTERN.md) archive.

### When to Use

A tool should use the two-step pattern when:
- **Instructions are large** (>500 tokens) — would waste context in conversations that don't use the tool
- **Instructions vary by sub-task** — different modules for different use cases
- **Output quality depends on following specific rules** — the difference between "works" and "works well" is significant
- **Instructions evolve independently** — edit markdown files without recompiling

Do NOT use this pattern for simple tools where the function calling schema is sufficient (email, timers, weather, etc.).

### Architecture

```
tool_instructions/              ← Project root (alongside www/, models/)
├── render_visual/              ← One directory per tool
│   ├── _core.md                ← Always loaded (shared rules for all modules)
│   ├── diagram.md              ← Module: loaded on demand
│   └── chart.md                ← Module: loaded on demand
└── test_greeter/               ← Test/demo tool
    ├── _core.md
    ├── formal.md
    └── casual.md
```

The `instruction_loader` module (`src/tools/instruction_loader.c`) provides a single generic function that any two-step tool can use:

```c
#include "tools/instruction_loader.h"

char *content = NULL;
int rc = instruction_loader_load("render_visual", "diagram,chart", &content);
// content is heap-allocated, caller must free()
```

- Reads `_core.md` first (if present), then each comma-separated module
- Sanitizes module names (rejects `/`, `\`, `..` — prevents path traversal)
- Dynamically allocates output, capped at 128 KB
- Returns concatenated content with `---` separators between sections

### How to Create a Two-Step Tool

A two-step tool registers **two** entries in the tool registry:

1. **`{tool}_load_instructions`** — getter that calls `instruction_loader_load()` and returns content
2. **`{tool}`** — the actual tool that executes after the LLM has read the instructions

Both are registered from a single `*_tool_register()` function.

#### Step 1: Create Instruction Files

Create a directory under `tool_instructions/` with your tool name. Add `_core.md` for shared rules and one `.md` file per module.

#### Step 2: Implement the Tool

```c
#include "tools/instruction_loader.h"
#include "tools/tool_registry.h"

#define TOOL_DIR "my_tool"

/* Instruction loader callback */
static char *load_instructions_callback(const char *action, char *value,
                                         int *should_respond) {
   (void)action;
   *should_respond = 1;

   if (!value || value[0] == '\0')
      return strdup("Error: provide module names.");

   char *content = NULL;
   int rc = instruction_loader_load(TOOL_DIR, value, &content);
   if (rc != 0 || !content)
      return strdup("Error: failed to load instructions.");

   return content; /* Caller (registry) frees this */
}

/* Actual tool callback */
static char *my_tool_callback(const char *action, char *value,
                               int *should_respond) {
   *should_respond = 1;
   /* Your tool logic here */
   return strdup("Tool result");
}
```

#### Step 3: Register Both Tools

```c
/* Metadata for the instruction loader */
static const tool_metadata_t load_metadata = {
   .name = "my_tool_load_instructions",
   .device_string = "my tool instructions",
   .description = "Load guidelines before using my_tool. Call this FIRST.",
   .params = load_params,     /* modules param (STRING type) */
   .param_count = 1,
   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_FILESYSTEM,
   .callback = load_instructions_callback,
   /* ... */
};

/* Metadata for the actual tool */
static const tool_metadata_t tool_metadata = {
   .name = "my_tool",
   .description = "Do the thing. Always call my_tool_load_instructions first.",
   /* ... */
   .callback = my_tool_callback,
};

int my_tool_register(void) {
   int rc = tool_registry_register(&load_metadata);
   if (rc != 0) return rc;
   return tool_registry_register(&tool_metadata);
}
```

#### Step 4: CMake + tools_init.c

Same as any other tool — add option, conditional source, include, and registration call.

### Key Design Details

- **Modules param is a comma-separated STRING**, not an array (avoids adding array type to the registry)
- **`_core.md` is always loaded** when present — the LLM doesn't need to request it
- **Instructions are plain markdown on disk** — edit and test without recompiling
- **Path traversal is sanitized** — module names with `/`, `\`, or `..` are rejected
- **Buffer is dynamically allocated** and capped at `INSTRUCTION_LOADER_MAX_SIZE` (128 KB)

### Reference

See the [TWO_STEP_TOOL_PATTERN.md](https://github.com/The-OASIS-Project/atlas/blob/main/dawn/archive/TWO_STEP_TOOL_PATTERN.md) archive for the full design rationale, assessment matrix for when to use this pattern, and a complete end-to-end example flow.
