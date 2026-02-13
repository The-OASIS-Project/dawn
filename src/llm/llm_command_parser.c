/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 */

/* Std C */
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Local */
#include "config/dawn_config.h"
#include "core/command_executor.h"
#include "core/session_manager.h"
#include "dawn.h"
#include "llm/llm_interface.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "mosquitto_comms.h"
#include "text_to_command_nuevo.h"
#include "tools/smartthings_service.h"
#include "tools/tool_registry.h"
#include "ui/metrics.h"

/* =============================================================================
 * System Prompt Strings
 * =============================================================================
 * These prompts define the AI's behavior rules. There are two modes:
 *
 * 1. NATIVE TOOL CALLING (default): Uses NATIVE_TOOLS_RULES - a minimal prompt
 *    since the LLM receives tool schemas via API (OpenAI function calling /
 *    Claude tool_use).
 *
 * 2. LEGACY <command> TAG MODE: Uses LEGACY_RULES_* prompts that instruct the
 *    LLM to emit <command>{"device":"x","action":"y"}</command> JSON tags.
 *
 * See get_system_instructions() for the branching logic.
 * ============================================================================= */

// clang-format off

/* Rules for native tool calling mode (default) */
static const char *NATIVE_TOOLS_RULES =
   "RULES\n"
   "1. Keep responses concise and conversational.\n"
   "2. Use available tools when the user requests actions or information.\n"
   "3. If a request is ambiguous, ask for clarification.\n"
   "4. After tool execution, summarize the results briefly. Do not call the same tool again.\n"
   "5. Search results include snippets with key information. Answer from snippets directly.\n"
   "   Only fetch a URL if the user asks for details about a specific article.\n"
   "6. Do NOT lead responses with weather, time, or location info unless explicitly asked.\n"
   "   Vary your greetings and openers. The user's context below is for tool use only.\n";

/* Core behavior rules for <command> tag mode (legacy) */
static const char *LEGACY_RULES_CORE =
   "Do not use thinking mode. Respond directly without internal reasoning.\n"
   "Max 30 words plus <command> tags unless the user says \"explain in detail\".\n"
   "\n"
   "RULES\n"
   "1. For Boolean / Analog / Music actions: one sentence, then the JSON tag(s). No prose after "
   "the tag block.\n"
   "2. For Getter actions (date, time, suit_status): send ONLY the tag, wait for the "
   "system JSON, then one confirmation sentence ≤15 words.\n"
   "3. Use only the devices and actions listed below; never invent new ones.\n"
   "4. If a request is ambiguous (e.g., \"Mute it\"), ask one-line clarification.\n"
   "5. If the user wants information that has no matching getter yet, answer verbally with no "
   "tags.\n"
   "6. Device \"info\" supports ENABLE / DISABLE only—never use \"get\" with it.\n"
   "7. To mute playback after clarification, use "
   "<command>{\"device\":\"volume\",\"action\":\"set\",\"value\":0}</command>.\n"
   "8. Multiple commands can be sent in one response using multiple <command> tags.\n"
   "9. Do NOT lead responses with comments about location, weather, or time of day.\n"
   "   Vary your greetings. The user's context below is for tool use only.\n";

// clang-format on

/* Tool-specific rules (LEGACY_RULES_VISION, LEGACY_RULES_WEATHER, etc.) have been removed.
 * Tool instructions are now generated dynamically from the tool_registry via
 * generate_command_tag_instructions() and build_capabilities_list(). */

/* =============================================================================
 * End Legacy Prompt Strings
 * ============================================================================= */

// Static buffer for command prompt - make it static, make it large
#define PROMPT_BUFFER_SIZE 65536
static char command_prompt[PROMPT_BUFFER_SIZE];
static int prompt_initialized = 0;

// Static buffer for remote command prompt
static char remote_command_prompt[PROMPT_BUFFER_SIZE];
static int remote_prompt_initialized = 0;

// Static buffer for localization context
#define LOCALIZATION_BUFFER_SIZE 512
static char localization_context[LOCALIZATION_BUFFER_SIZE];
static int localization_initialized = 0;

// Static buffer for dynamic system instructions
#define SYSTEM_INSTRUCTIONS_BUFFER_SIZE 8192
static char system_instructions_buffer[SYSTEM_INSTRUCTIONS_BUFFER_SIZE];
static int system_instructions_initialized = 0;

/**
 * @brief Checks if vision is enabled for the current LLM type
 *
 * Vision availability is controlled by the vision_enabled setting for the
 * session's LLM type (cloud or local). This ensures that when the user
 * switches LLMs at runtime, the vision check reflects the session's config.
 *
 * @return 1 if vision is available, 0 otherwise
 */
int is_vision_enabled_for_current_llm(void) {
   /* Check session context first (set during streaming calls) */
   session_t *session = session_get_command_context();
   if (session) {
      session_llm_config_t config;
      session_get_llm_config(session, &config);
      if (config.type == LLM_CLOUD) {
         return g_config.llm.cloud.vision_enabled;
      } else if (config.type == LLM_LOCAL) {
         return g_config.llm.local.vision_enabled;
      }
      return 0;
   }

   /* Fallback to global state for paths without session context */
   llm_type_t current = llm_get_type();
   if (current == LLM_CLOUD) {
      return g_config.llm.cloud.vision_enabled;
   } else if (current == LLM_LOCAL) {
      return g_config.llm.local.vision_enabled;
   }
   return 0;
}


/**
 * @brief Builds dynamic system instructions based on enabled features
 *
 * Assembles LEGACY_RULES_CORE plus feature-specific rules based on config.
 * This ensures the LLM only sees instructions for features that are available.
 *
 * Features checked:
 * - Vision: Requires vision_enabled for current LLM type (cloud or local)
 * - Weather: Always available (Open-Meteo free API)
 * - Search: Requires SearXNG endpoint configured
 * - Calculator: Always available (local computation)
 * - URL: Always available (basic HTTP fetch)
 *
 * @return Pointer to static buffer containing assembled instructions
 */
/**
 * @brief Invalidates cached system instructions, forcing rebuild on next call
 *
 * Call this when capabilities change at runtime (e.g., SmartThings
 * authenticates, devices are loaded, etc.) so the next call to
 * get_system_instructions() rebuilds the prompt with updated capabilities.
 */
void invalidate_system_instructions(void) {
   system_instructions_initialized = 0;
   prompt_initialized = 0;
   remote_prompt_initialized = 0;
   LOG_INFO("System instructions cache invalidated - will rebuild on next LLM call");
}

/* =============================================================================
 * Dynamic Command Tag Generation (from tool_registry)
 * ============================================================================= */

/**
 * @brief Check if a tool is available at runtime
 *
 * Checks both the enabled status and the optional is_available() callback.
 *
 * @param tool The tool metadata
 * @return true if tool is available, false otherwise
 */
static bool is_tool_available(const tool_metadata_t *tool) {
   if (!tool)
      return false;

   /* Check if tool is enabled (handles DANGEROUS capability check) */
   if (!tool_registry_is_enabled(tool->name))
      return false;

   /* Check runtime availability if function is provided */
   if (tool->is_available && !tool->is_available())
      return false;

   return true;
}

/**
 * @brief Generate command tag instructions for a single tool
 *
 * Output format:
 *   TOOL_NAME: Description
 *     <command>{"device":"name","action":"ACTION","value":"VALUE"}</command>
 *     Actions: action1, action2, action3
 *
 * @param tool The tool metadata
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @return Number of bytes written
 */
static int generate_command_tag_instructions(const tool_metadata_t *tool,
                                             char *buffer,
                                             size_t buffer_size) {
   if (!tool || !buffer || buffer_size == 0)
      return 0;

   int len = 0;
   int remaining = (int)buffer_size;

   /* Find action and value parameters */
   const treg_param_t *action_param = NULL;
   const treg_param_t *value_param = NULL;

   for (int i = 0; i < tool->param_count; i++) {
      if (tool->params[i].maps_to == TOOL_MAPS_TO_ACTION) {
         action_param = &tool->params[i];
      } else if (tool->params[i].maps_to == TOOL_MAPS_TO_VALUE) {
         value_param = &tool->params[i];
      }
   }

   /* Determine default action based on device_type when no action param exists */
   const char *default_action = NULL;
   if (!action_param) {
      switch (tool->device_type) {
         case TOOL_DEVICE_TYPE_BOOLEAN:
            default_action = "enable/disable";
            break;
         case TOOL_DEVICE_TYPE_ANALOG:
            default_action = "set";
            break;
         case TOOL_DEVICE_TYPE_GETTER:
            default_action = "get";
            break;
         case TOOL_DEVICE_TYPE_MUSIC:
            default_action = "play";
            break;
         case TOOL_DEVICE_TYPE_TRIGGER:
            default_action = "trigger";
            break;
         default:
            default_action = "get";
            break;
      }
   }

   /* Tool name in uppercase */
   char upper_name[TOOL_NAME_MAX];
   int i;
   for (i = 0; tool->name[i] && i < TOOL_NAME_MAX - 1; i++) {
      upper_name[i] = (tool->name[i] >= 'a' && tool->name[i] <= 'z') ? tool->name[i] - 32
                                                                     : tool->name[i];
   }
   upper_name[i] = '\0';

   /* Header: TOOL_NAME: Description */
   len += snprintf(buffer + len, remaining - len, "%s: %s\n", upper_name, tool->description);

   /* Example command tag - always include action field */
   len += snprintf(buffer + len, remaining - len, "  <command>{\"device\":\"%s\"", tool->name);

   if (action_param) {
      len += snprintf(buffer + len, remaining - len, ",\"action\":\"ACTION\"");
   } else if (default_action) {
      /* Use default action for tools without action param */
      len += snprintf(buffer + len, remaining - len, ",\"action\":\"%s\"", default_action);
   }

   if (value_param) {
      /* Use param name as hint */
      const char *value_hint = value_param->name ? value_param->name : "value";
      len += snprintf(buffer + len, remaining - len, ",\"value\":\"%s\"", value_hint);
   }
   len += snprintf(buffer + len, remaining - len, "}</command>\n");

   /* List available actions if enum type */
   if (action_param && action_param->type == TOOL_PARAM_TYPE_ENUM && action_param->enum_count > 0) {
      len += snprintf(buffer + len, remaining - len, "  Actions: ");
      for (int j = 0; j < action_param->enum_count; j++) {
         if (j > 0)
            len += snprintf(buffer + len, remaining - len, ", ");
         len += snprintf(buffer + len, remaining - len, "%s", action_param->enum_values[j]);
      }
      len += snprintf(buffer + len, remaining - len, "\n");
   }

   /* Add a blank line for readability */
   len += snprintf(buffer + len, remaining - len, "\n");

   return len;
}

/**
 * @brief Context for combined capabilities + command tag generation
 *
 * This allows building both outputs in a single iteration pass over tools.
 */
typedef struct {
   /* Capabilities list */
   char *cap_buffer;
   size_t cap_buffer_size;
   int cap_offset;
   int cap_count;

   /* Command tags */
   char *cmd_buffer;
   size_t cmd_buffer_size;
   int cmd_offset;
} combined_build_ctx_t;

/**
 * @brief Callback to build both capability entry and command tag in single pass
 */
static void build_combined_entry(const tool_metadata_t *tool, void *user_data) {
   combined_build_ctx_t *ctx = (combined_build_ctx_t *)user_data;

   if (!is_tool_available(tool))
      return;

   /* Skip mqtt_only tools that are hardware-specific */
   if (tool->mqtt_only && !tool->sync_wait)
      return;

   /* Build capability entry */
   int cap_remaining = (int)ctx->cap_buffer_size - ctx->cap_offset;
   if (cap_remaining > 0) {
      /* Add comma separator after first item */
      if (ctx->cap_count > 0) {
         ctx->cap_offset += snprintf(ctx->cap_buffer + ctx->cap_offset, cap_remaining, ", ");
         cap_remaining = (int)ctx->cap_buffer_size - ctx->cap_offset;
      }

      /* Generate capability description based on tool type */
      if (tool->is_getter) {
         ctx->cap_offset += snprintf(ctx->cap_buffer + ctx->cap_offset, cap_remaining,
                                     "get %s info", tool->name);
      } else if (tool->device_type == TOOL_DEVICE_TYPE_MUSIC) {
         ctx->cap_offset += snprintf(ctx->cap_buffer + ctx->cap_offset, cap_remaining, "control %s",
                                     tool->name);
      } else {
         ctx->cap_offset += snprintf(ctx->cap_buffer + ctx->cap_offset, cap_remaining, "use %s",
                                     tool->name);
      }
      ctx->cap_count++;
   }

   /* Build command tag instruction */
   int cmd_remaining = (int)ctx->cmd_buffer_size - ctx->cmd_offset;
   if (cmd_remaining > 0) {
      ctx->cmd_offset += generate_command_tag_instructions(tool, ctx->cmd_buffer + ctx->cmd_offset,
                                                           cmd_remaining);
   }
}

/* =============================================================================
 * End Dynamic Generation Functions
 * ============================================================================= */

/**
 * @brief Build system instructions for a specific mode into a provided buffer
 *
 * This is the core logic for building system instructions. It can be used
 * for both cached (global) and non-cached (session-specific) builds.
 *
 * @param mode The tool mode: "native", "command_tags", or "disabled"
 * @param buffer Output buffer to write instructions to
 * @param buffer_size Size of the output buffer
 * @return Number of bytes written (excluding null terminator)
 */
static int build_system_instructions_to_buffer(const char *mode, char *buffer, size_t buffer_size) {
   int len = 0;
   int remaining = (int)buffer_size;

   bool use_native = (strcmp(mode, "native") == 0);

   if (use_native) {
      len += snprintf(buffer + len, remaining - len, "%s\n", NATIVE_TOOLS_RULES);
      return len;
   }

   if (strcmp(mode, "disabled") == 0) {
      /* No tool instructions for disabled mode */
      buffer[0] = '\0';
      return 0;
   }

   /* command_tags mode - dynamic generation from tool_registry */

   /* Core behavior rules (static, minimal) */
   len += snprintf(buffer + len, remaining - len, "%s\n", LEGACY_RULES_CORE);

   /* Single-pass generation of both capabilities and command tags */
   char cap_buffer[2048];
   char cmd_buffer[16384];

   combined_build_ctx_t ctx = {
      .cap_buffer = cap_buffer,
      .cap_buffer_size = sizeof(cap_buffer),
      .cap_offset = 0,
      .cap_count = 0,
      .cmd_buffer = cmd_buffer,
      .cmd_buffer_size = sizeof(cmd_buffer),
      .cmd_offset = 0,
   };

   tool_registry_foreach_enabled(build_combined_entry, &ctx);

   /* Assemble capabilities list with header and footer */
   len += snprintf(buffer + len, remaining - len, "CAPABILITIES: You CAN %s.\n\n", cap_buffer);

   /* Append command tag instructions */
   int cmd_copy = (ctx.cmd_offset < remaining - len) ? ctx.cmd_offset : remaining - len - 1;
   if (cmd_copy > 0) {
      memcpy(buffer + len, cmd_buffer, cmd_copy);
      len += cmd_copy;
      buffer[len] = '\0';
   }

   /* Special case: SmartThings device list if authenticated */
   if (smartthings_is_authenticated()) {
      const st_device_list_t *devices = NULL;
      if (smartthings_list_devices(&devices) == ST_OK && devices && devices->count > 0) {
         len += snprintf(buffer + len, remaining - len, "Available SmartThings devices (%d):\n",
                         devices->count);
         for (int i = 0; i < devices->count && i < 30; i++) {
            const st_device_t *dev = &devices->devices[i];
            len += snprintf(buffer + len, remaining - len, "  - %s",
                            dev->label[0] ? dev->label : dev->name);
            if (dev->room[0]) {
               len += snprintf(buffer + len, remaining - len, " (%s)", dev->room);
            }
            len += snprintf(buffer + len, remaining - len, "\n");
         }
         if (devices->count > 30) {
            len += snprintf(buffer + len, remaining - len, "  - ... and %d more devices\n",
                            devices->count - 30);
         }
         len += snprintf(buffer + len, remaining - len, "\n");
      }
   }

   return len;
}

const char *get_system_instructions(void) {
   if (system_instructions_initialized) {
      return system_instructions_buffer;
   }

   /* Determine mode from global config */
   const char *mode = llm_tools_enabled(NULL) ? "native" : g_config.llm.tools.mode;
   if (!mode || mode[0] == '\0') {
      mode = "native";
   }

   int len = build_system_instructions_to_buffer(mode, system_instructions_buffer,
                                                 SYSTEM_INSTRUCTIONS_BUFFER_SIZE);

   system_instructions_initialized = 1;

   if (strcmp(mode, "native") == 0) {
      LOG_INFO("Built system instructions for native tool calling (%d bytes)", len);
   } else {
      LOG_INFO("Built dynamic system instructions (%d bytes)", len);
   }

   return system_instructions_buffer;
}

/**
 * @brief Builds the localization context string from config
 *
 * Creates a context string like:
 * "USER CONTEXT: Location: Atlanta, Georgia. Units: imperial. Timezone: America/New_York."
 *
 * Only includes fields that are configured (non-empty).
 */
static const char *get_localization_context(void) {
   if (localization_initialized) {
      return localization_context;
   }

   localization_context[0] = '\0';
   int offset = 0;
   int has_context = 0;

   // Check if any localization fields are set
   if (g_config.localization.location[0] != '\0' || g_config.localization.units[0] != '\0' ||
       g_config.localization.timezone[0] != '\0' || g_config.general.room[0] != '\0') {
      offset = snprintf(localization_context, LOCALIZATION_BUFFER_SIZE,
                        "TOOL DEFAULTS (for tool calls only, do not mention in conversation):");
      has_context = 1;
   }

   if (g_config.localization.location[0] != '\0') {
      offset += snprintf(localization_context + offset, LOCALIZATION_BUFFER_SIZE - offset,
                         " Location=%s.", g_config.localization.location);
   }

   if (g_config.general.room[0] != '\0') {
      offset += snprintf(localization_context + offset, LOCALIZATION_BUFFER_SIZE - offset,
                         " Room=%s.", g_config.general.room);
   }

   if (g_config.localization.units[0] != '\0') {
      offset += snprintf(localization_context + offset, LOCALIZATION_BUFFER_SIZE - offset,
                         " Units=%s.", g_config.localization.units);
   }

   if (g_config.localization.timezone[0] != '\0') {
      offset += snprintf(localization_context + offset, LOCALIZATION_BUFFER_SIZE - offset,
                         " TZ=%s.", g_config.localization.timezone);
   }

   // Always include current date so model knows what "today" means
   // This is critical for models with older training data cutoffs
   time_t now = time(NULL);
   struct tm tm_storage;
   struct tm *tm_info = localtime_r(&now, &tm_storage);
   if (tm_info) {
      char date_str[32];
      size_t date_len = strftime(date_str, sizeof(date_str), "%B %d, %Y", tm_info);
      if (date_len > 0) {
         if (!has_context) {
            offset = snprintf(
                localization_context, LOCALIZATION_BUFFER_SIZE,
                "TOOL DEFAULTS (for tool calls only, do not mention in conversation):");
            has_context = 1;
         }
         offset += snprintf(localization_context + offset, LOCALIZATION_BUFFER_SIZE - offset,
                            " Today=%s.", date_str);
      }
   }

   if (has_context) {
      snprintf(localization_context + offset, LOCALIZATION_BUFFER_SIZE - offset, "\n\n");
   }

   localization_initialized = 1;
   return localization_context;
}

/**
 * @brief Gets the persona description from config or builds default with dynamic AI name
 *
 * Returns g_config.persona.description if set, otherwise builds the default persona
 * using AI_PERSONA_NAME_TEMPLATE + AI_PERSONA_TRAITS with the configured AI name
 * from g_config.general.ai_name (or falling back to AI_NAME compile-time default).
 *
 * This allows runtime customization of the AI personality via config file while
 * keeping the system instructions (AI_SYSTEM_INSTRUCTIONS) always active.
 */
static const char *get_persona_description(void) {
   static char dynamic_persona[1024]; /* Template ~30 + traits ~500 = ~550 max */
   static int persona_built = 0;

   // If global config has a custom persona, use it directly
   if (g_config.persona.description[0] != '\0') {
      return g_config.persona.description;
   }

   // Build dynamic persona with configured AI name (only once)
   if (!persona_built) {
      const char *ai_name = g_config.general.ai_name[0] != '\0' ? g_config.general.ai_name
                                                                : AI_NAME;

      // Capitalize first letter for proper noun (more respectful!)
      char capitalized_name[64];
      snprintf(capitalized_name, sizeof(capitalized_name), "%s", ai_name);
      if (capitalized_name[0] >= 'a' && capitalized_name[0] <= 'z') {
         capitalized_name[0] -= 32;
      }

      // Build the persona: "Your name is <Name>. <traits>"
      snprintf(dynamic_persona, sizeof(dynamic_persona),
               AI_PERSONA_NAME_TEMPLATE " " AI_PERSONA_TRAITS, capitalized_name);
      persona_built = 1;
      LOG_INFO("Built dynamic persona with AI name: %s", capitalized_name);
   }

   return dynamic_persona;
}

/**
 * @brief Builds the system prompt for the local interface
 *
 * All commands are now defined via the modular tool_registry system.
 * Native tool calling is the primary mode; legacy <command> tag mode
 * falls back to a minimal prompt without JSON-defined commands.
 */
static void initialize_command_prompt(void) {
   if (prompt_initialized) {
      return;
   }

   // Check tool calling mode:
   // - "native": Use native function/tool calling (minimal prompt)
   // - "command_tags": Use <command> tag instructions (legacy)
   // - "disabled": No tool/command handling at all
   const char *tools_mode = g_config.llm.tools.mode;

   // All modes now use the same minimal prompt structure
   // Tool definitions are provided via native tool calling API
   int prompt_len = snprintf(command_prompt, PROMPT_BUFFER_SIZE,
                             "%s\n\n"  // Persona (from config or AI_PERSONA)
                             "%s\n\n"  // System instructions
                             "%s",     // Localization context
                             get_persona_description(), get_system_instructions(),
                             get_localization_context());

   prompt_initialized = 1;

   if (strcmp(tools_mode, "native") == 0) {
      LOG_INFO("AI prompt initialized (native tools mode). Length: %d", prompt_len);
   } else if (strcmp(tools_mode, "disabled") == 0) {
      LOG_INFO("AI prompt initialized (tools disabled). Length: %d", prompt_len);
   } else {
      LOG_INFO("AI prompt initialized (command_tags mode). Length: %d", prompt_len);
   }
}

/**
 * @brief Gets the local command prompt string (all commands including HUD/helmet)
 *
 * @return The local command prompt string
 */
const char *get_local_command_prompt(void) {
   if (!prompt_initialized) {
      initialize_command_prompt();
   }
   return command_prompt;
}

/**
 * @brief Builds the remote command prompt (excludes local-only topics like hud, helmet)
 *
 * All commands are now defined via the modular tool_registry system.
 * Native tool calling filters remote-available tools automatically.
 */
static void initialize_remote_command_prompt(void) {
   if (remote_prompt_initialized) {
      return;
   }

   const char *tools_mode = g_config.llm.tools.mode;

   // All modes now use the same minimal prompt structure
   // Tool definitions are provided via native tool calling API
   int prompt_len = snprintf(remote_command_prompt, PROMPT_BUFFER_SIZE,
                             "%s\n\n"  // Persona (from config or AI_PERSONA)
                             "%s\n\n"  // System instructions
                             "%s",     // Localization context
                             get_persona_description(), get_system_instructions(),
                             get_localization_context());

   remote_prompt_initialized = 1;

   if (strcmp(tools_mode, "native") == 0) {
      LOG_INFO("Remote AI prompt initialized (native tools mode). Length: %d", prompt_len);
   } else if (strcmp(tools_mode, "disabled") == 0) {
      LOG_INFO("Remote AI prompt initialized (tools disabled). Length: %d", prompt_len);
   } else {
      LOG_INFO("Remote AI prompt initialized (command_tags mode). Length: %d", prompt_len);
   }
}

/**
 * @brief Gets the remote command prompt string (for network satellite clients)
 *
 * This prompt excludes local-only commands (HUD, helmet) and only includes
 * general commands like date, time, etc.
 *
 * @return The remote command prompt string
 */
const char *get_remote_command_prompt(void) {
   if (!remote_prompt_initialized) {
      initialize_remote_command_prompt();
   }
   return remote_command_prompt;
}

char *build_remote_prompt_for_mode(const char *tool_mode) {
   if (!tool_mode) {
      tool_mode = "native";
   }

   /* Allocate output buffer */
   char *prompt = malloc(PROMPT_BUFFER_SIZE);
   if (!prompt) {
      LOG_ERROR("Failed to allocate prompt buffer");
      return NULL;
   }

   int len = 0;
   int remaining = PROMPT_BUFFER_SIZE;

   /* Add persona */
   len += snprintf(prompt + len, remaining - len, "%s\n\n", get_persona_description());

   /* Generate system instructions using the shared builder (respects mode parameter) */
   len += build_system_instructions_to_buffer(tool_mode, prompt + len, remaining - len);
   len += snprintf(prompt + len, remaining - len, "\n");

   /* Add localization context */
   len += snprintf(prompt + len, remaining - len, "%s", get_localization_context());

   LOG_INFO("Built prompt for %s mode. Length: %d", tool_mode, len);
   return prompt;
}

/**
 * @brief Parses an LLM response for commands and executes them
 *
 * This function looks for JSON commands enclosed in <command> tags in the LLM response,
 * extracts them, validates the device against the allowlist, and sends them through
 * the MQTT messaging system.
 *
 * SECURITY: Commands are validated against commands_config_nuevo.json before execution.
 * Unknown devices are rejected and logged for audit.
 *
 * @param llm_response The text response from the LLM
 * @param mosq The MQTT client instance
 * @return The number of commands found and processed
 */
int parse_llm_response_for_commands(const char *llm_response, struct mosquitto *mosq) {
   if (!llm_response || !mosq) {
      return 0;
   }

   int commands_found = 0;
   const char *start_tag = "<command>";
   const char *end_tag = "</command>";
   size_t start_tag_len = strlen(start_tag);
   size_t end_tag_len = strlen(end_tag);

   const char *search_start = llm_response;
   const char *cmd_start, *cmd_end;

   while ((cmd_start = strstr(search_start, start_tag)) != NULL) {
      cmd_start += start_tag_len;
      cmd_end = strstr(cmd_start, end_tag);

      if (cmd_end) {
         // Extract the command
         size_t cmd_len = cmd_end - cmd_start;
         char *command = (char *)malloc(cmd_len + 1);
         if (command) {
            strncpy(command, cmd_start, cmd_len);
            command[cmd_len] = '\0';

            LOG_INFO("LLM command extracted: %s", command);

            // Parse JSON
            struct json_object *cmd_json = json_tokener_parse(command);
            if (cmd_json) {
               struct json_object *device_obj;

               if (json_object_object_get_ex(cmd_json, "device", &device_obj)) {
                  const char *device = json_object_get_string(device_obj);

                  /* Use unified command executor - handles callbacks AND MQTT */
                  cmd_exec_result_t exec_result;
                  int rc = command_execute_json(cmd_json, mosq, &exec_result);

                  if (rc == 0 && exec_result.success) {
                     LOG_INFO("LLM COMMAND EXECUTED: device=%s via unified executor", device);
                     metrics_log_activity("LLM CMD: %s", command);
                     commands_found++;

                     /* If command returned data, log it (could be used for response) */
                     if (exec_result.result && exec_result.should_respond) {
                        LOG_INFO("  Command result: %.100s%s", exec_result.result,
                                 strlen(exec_result.result) > 100 ? "..." : "");
                     }
                  } else {
                     /* Command failed - could be unknown device or execution error */
                     LOG_WARNING("LLM COMMAND FAILED: device='%s' - %s", device,
                                 exec_result.result ? exec_result.result : "unknown error");
                     metrics_log_activity("LLM CMD FAILED: %s", device);
                  }

                  cmd_exec_result_free(&exec_result);
               } else {
                  LOG_WARNING("LLM COMMAND REJECTED: No device field in command JSON");
               }

               json_object_put(cmd_json);
            } else {
               LOG_ERROR("Failed to parse command JSON: %s", command);
            }

            free(command);
         }

         // Continue search after this command
         search_start = cmd_end + end_tag_len;
      } else {
         // No closing tag found, stop searching
         break;
      }
   }

   return commands_found;
}
