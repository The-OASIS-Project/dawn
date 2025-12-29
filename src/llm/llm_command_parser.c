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

/* Local */
#include "config/dawn_config.h"
#include "core/command_executor.h"
#include "core/command_registry.h"
#include "core/session_manager.h"
#include "dawn.h"
#include "llm/llm_interface.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "mosquitto_comms.h"
#include "text_to_command_nuevo.h"
#include "tools/smartthings_service.h"
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
   "1. Keep responses concise - max 30 words unless asked to explain.\n"
   "2. Use available tools when the user requests actions or information.\n"
   "3. Do not include URLs unless explicitly asked.\n"
   "4. If a request is ambiguous, ask for clarification.\n"
   "5. After tool execution, provide a brief confirmation.\n";

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
   "9. Do not include URLs in responses unless the user explicitly asks for links.\n";

/* Vision rules (only if vision is enabled) */
static const char *LEGACY_RULES_VISION =
   "VISION: When user asks what they're looking at, send ONLY "
   "<command>{\"device\":\"viewing\",\"action\":\"get\"}</command>. When the system then "
   "provides an image, describe what you see in detail.\n";

/* Weather rules (always available - uses Open-Meteo free API) */
static const char *LEGACY_RULES_WEATHER =
   "WEATHER: Use action 'today' (current), 'tomorrow' (2-day), or 'week' (7-day forecast).\n"
   "   Example: <command>{\"device\":\"weather\",\"action\":\"week\",\"value\":\"City, State\"}"
   "</command>. If user provides location, use it directly. Only ask for location if not "
   "specified. Choose action based on user's question (e.g., 'this weekend' -> week, "
   "'right now' -> today).\n";

/* Search rules (only if SearXNG endpoint is configured) */
static const char *LEGACY_RULES_SEARCH =
   "SEARCH: <command>{\"device\":\"search\",\"action\":\"ACTION\",\"value\":\"query\"}</command> "
   "Actions: web, news, science, tech, social, define, papers. No URLs aloud.\n";

/* Calculator rules (always available - local computation) */
static const char *LEGACY_RULES_CALCULATOR =
   "CALCULATOR: Actions: 'evaluate' (math), 'convert' (units), 'base' (hex/bin), 'random'.\n"
   "   evaluate: <command>{\"device\":\"calculator\",\"action\":\"evaluate\",\"value\":\"2+3*4\"}"
   "</command>\n"
   "   convert: <command>{\"device\":\"calculator\",\"action\":\"convert\",\"value\":\"5 miles "
   "to km\"}</command>\n"
   "   base: <command>{\"device\":\"calculator\",\"action\":\"base\",\"value\":\"255 to hex\"}"
   "</command>\n"
   "   random: <command>{\"device\":\"calculator\",\"action\":\"random\",\"value\":\"1 to 100\"}"
   "</command>\n";

/* URL fetcher rules (always available - basic HTTP fetch) */
static const char *LEGACY_RULES_URL =
   "URL: Fetch and read content from a URL. Use when you need to read a specific webpage.\n"
   "   <command>{\"device\":\"url\",\"action\":\"get\",\"value\":\"https://example.com\"}"
   "</command>\n";

/* LLM control rules (always available - query and switch AI backend) */
static const char *LEGACY_RULES_LLM_STATUS =
   "LLM: Query or switch the AI model. Actions: 'get' (status), 'set' (switch).\n"
   "   get: <command>{\"device\":\"llm\",\"action\":\"get\"}</command>\n"
   "   set: <command>{\"device\":\"llm\",\"action\":\"set\",\"value\":\"local\"}"
   "</command> or \"cloud\"\n";

/* SmartThings home automation rules (only if SmartThings is authenticated) */
static const char *LEGACY_RULES_SMARTTHINGS =
   "SMARTTHINGS: Control smart home devices. Actions: list, status, on, off, brightness, "
   "color, temperature, lock, unlock.\n"
   "   list: <command>{\"device\":\"smartthings\",\"action\":\"list\"}</command>\n"
   "   status: <command>{\"device\":\"smartthings\",\"action\":\"status\",\"value\":\"device "
   "name\"}</command>\n"
   "   on/off: <command>{\"device\":\"smartthings\",\"action\":\"on\",\"value\":\"living room "
   "light\"}</command>\n"
   "   brightness: <command>{\"device\":\"smartthings\",\"action\":\"brightness\","
   "\"value\":\"lamp 75\"}</command> (0-100)\n"
   "   color: <command>{\"device\":\"smartthings\",\"action\":\"color\",\"value\":\"desk "
   "light red\"}</command> (red,orange,yellow,green,cyan,blue,purple,pink,white)\n"
   "   temperature: <command>{\"device\":\"smartthings\",\"action\":\"temperature\","
   "\"value\":\"thermostat 72\"}</command> (50-90F)\n"
   "   lock/unlock: <command>{\"device\":\"smartthings\",\"action\":\"lock\",\"value\":\"front "
   "door\"}</command>\n";

/* Core examples (always included in legacy mode) */
static const char *LEGACY_EXAMPLES_CORE =
   "\n=== EXAMPLES ===\n"
   "User: Turn on the armor display.\n"
   "FRIDAY: HUD online, boss. "
   "<command>{\"device\":\"armor_display\",\"action\":\"enable\"}</command>\n"
   "System-> {\"response\":\"armor display enabled\"}\n"
   "FRIDAY: Display confirmed, sir.\n"
   "\n"
   "User: What time is it?\n"
   "FRIDAY: <command>{\"device\":\"time\",\"action\":\"get\"}</command>\n"
   "System-> {\"response\":\"The time is 4:07 PM.\"}\n"
   "FRIDAY: Time confirmed, sir.\n"
   "\n"
   "User: Mute it.\n"
   "FRIDAY: Need specifics, sir—audio playback or mic?\n"
   "\n"
   "User: Mute playback.\n"
   "FRIDAY: Volume to zero, boss. "
   "<command>{\"device\":\"volume\",\"action\":\"set\",\"value\":0}</command>\n"
   "System-> {\"response\":\"volume set\"}\n"
   "FRIDAY: Muted, sir.\n";

/* Weather example (only if weather is enabled) */
static const char *LEGACY_EXAMPLES_WEATHER =
   "\nUser: What's the weather in Atlanta?\n"
   "FRIDAY: <command>{\"device\":\"weather\",\"action\":\"today\",\"value\":\"Atlanta, Georgia\"}"
   "</command>\n"
   "System-> {\"location\":\"Atlanta, Georgia, US\",\"current\":{\"temperature_f\":52.3,...},"
   "\"forecast\":[{\"date\":\"2025-01-15\",\"high_f\":58,...}]}\n"
   "FRIDAY: Atlanta right now: 52°F, partly cloudy. Today's high 58°F, low 42°F. Light jacket "
   "weather, boss!\n";

/* Command response format instructions for LOCAL interface (includes HUD-specific hints) */
static const char *LEGACY_LOCAL_COMMAND_INSTRUCTIONS =
   "When I ask for an action that matches one of these commands, respond with both:\n"
   "1. A conversational response (e.g., \"I'll turn that on for you, sir.\")\n"
   "2. The exact JSON command enclosed in <command> tags\n\n"
   "For example: \"Let me turn on the map for you, sir. <command>{\"device\": \"map\", "
   "\"action\": \"enable\"}</command>\"\n\n"
   "The very next message I send you will be an automated response from the system. You should "
   "use that information then to "
   "reply with the information I requested or information on whether the command was "
   "successful.\n"
   "Command hints:\n"
   "The \"viewing\" command will return an image to you so you can visually answer a query.\n"
   "When running \"play\", the value is a simple string to search the media files for.\n"
   "Current HUD names are \"default\", \"environmental\", and \"armor\".\n";

/* Command response format instructions for REMOTE interface (no HUD-specific hints) */
static const char *LEGACY_REMOTE_COMMAND_INSTRUCTIONS =
   "When I ask for an action that matches one of these commands, respond with both:\n"
   "1. A conversational response (e.g., \"I'll get that for you, sir.\")\n"
   "2. The exact JSON command enclosed in <command> tags\n\n"
   "For example: \"The current time is 3:45 PM. <command>{\"device\": \"time\", "
   "\"action\": \"get\"}</command>\"\n\n"
   "The very next message I send you will be an automated response from the system. You should "
   "use that information then to "
   "reply with the information I requested or information on whether the command was "
   "successful.\n";

// clang-format on

/* =============================================================================
 * End Legacy Prompt Strings
 * ============================================================================= */

// Static buffer for command prompt - make it static, make it large
#define PROMPT_BUFFER_SIZE 65536
static char command_prompt[PROMPT_BUFFER_SIZE];
static int prompt_initialized = 0;

// Static buffer for remote command prompt (excludes local-only topics)
static char remote_command_prompt[PROMPT_BUFFER_SIZE];
static int remote_prompt_initialized = 0;

// Topics excluded from remote clients (local-only commands)
static const char *excluded_remote_topics[] = { "hud", "helmet", NULL };

// Static buffer for localization context
#define LOCALIZATION_BUFFER_SIZE 512
static char localization_context[LOCALIZATION_BUFFER_SIZE];
static int localization_initialized = 0;

// Static buffer for dynamic system instructions
#define SYSTEM_INSTRUCTIONS_BUFFER_SIZE 8192
static char system_instructions_buffer[SYSTEM_INSTRUCTIONS_BUFFER_SIZE];
static int system_instructions_initialized = 0;

/**
 * @brief Checks if search functionality is available
 *
 * Search requires a configured SearXNG endpoint.
 */
static int is_search_enabled(void) {
   return g_config.search.endpoint[0] != '\0';
}

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

const char *get_system_instructions(void) {
   if (system_instructions_initialized) {
      return system_instructions_buffer;
   }

   int len = 0;
   int remaining = SYSTEM_INSTRUCTIONS_BUFFER_SIZE;

   // Check if native tool calling is enabled - use minimal prompt
   if (llm_tools_enabled(NULL)) {
      len += snprintf(system_instructions_buffer + len, remaining - len, "%s\n",
                      NATIVE_TOOLS_RULES);

      // Note: Tool schemas are sent separately in the API request
      LOG_INFO("Built system instructions for native tool calling (%d bytes)", len);
      system_instructions_initialized = 1;
      return system_instructions_buffer;
   }

   // Full prompt with <command> tag instructions for non-tool mode
   // Always include core rules
   len += snprintf(system_instructions_buffer + len, remaining - len, "%s\n", LEGACY_RULES_CORE);

   // Build capabilities list based on what's enabled
   len += snprintf(system_instructions_buffer + len, remaining - len, "\nCAPABILITIES: You CAN ");
   int first_cap = 1;

   // Weather is always available
   if (!first_cap)
      len += snprintf(system_instructions_buffer + len, remaining - len, ", ");
   len += snprintf(system_instructions_buffer + len, remaining - len, "get weather");
   first_cap = 0;

   // Search if configured
   if (is_search_enabled()) {
      len += snprintf(system_instructions_buffer + len, remaining - len, ", perform web searches");
   }

   // Calculator always available
   len += snprintf(system_instructions_buffer + len, remaining - len, ", do calculations");

   // URL fetcher always available
   len += snprintf(system_instructions_buffer + len, remaining - len, ", fetch URLs");

   // LLM status always available
   len += snprintf(system_instructions_buffer + len, remaining - len, ", report AI status");

   // Vision if enabled
   if (is_vision_enabled_for_current_llm()) {
      len += snprintf(system_instructions_buffer + len, remaining - len,
                      ", analyze images when asked what you see");
   }

   // SmartThings if authenticated
   if (smartthings_is_authenticated()) {
      len += snprintf(system_instructions_buffer + len, remaining - len,
                      ", control smart home devices via SmartThings");
   }

   len += snprintf(system_instructions_buffer + len, remaining - len, ".\n\n");

   // Add feature-specific rules based on config
   if (is_vision_enabled_for_current_llm()) {
      len += snprintf(system_instructions_buffer + len, remaining - len, "%s\n",
                      LEGACY_RULES_VISION);
      LOG_INFO("System instructions: Vision rules included (cloud LLM with vision support)");
   }

   // Weather always available
   len += snprintf(system_instructions_buffer + len, remaining - len, "%s\n", LEGACY_RULES_WEATHER);

   // Search only if endpoint configured
   if (is_search_enabled()) {
      len += snprintf(system_instructions_buffer + len, remaining - len, "%s\n",
                      LEGACY_RULES_SEARCH);
      LOG_INFO("System instructions: Search rules included (SearXNG endpoint: %s)",
               g_config.search.endpoint);
   } else {
      LOG_INFO("System instructions: Search rules EXCLUDED (no SearXNG endpoint configured)");
   }

   // Calculator always available
   len += snprintf(system_instructions_buffer + len, remaining - len, "%s\n",
                   LEGACY_RULES_CALCULATOR);

   // URL fetcher always available
   len += snprintf(system_instructions_buffer + len, remaining - len, "%s\n", LEGACY_RULES_URL);

   // LLM status always available
   len += snprintf(system_instructions_buffer + len, remaining - len, "%s\n",
                   LEGACY_RULES_LLM_STATUS);

   // SmartThings only if authenticated
   if (smartthings_is_authenticated()) {
      len += snprintf(system_instructions_buffer + len, remaining - len, "%s\n",
                      LEGACY_RULES_SMARTTHINGS);

      // Include device list so LLM knows what's available
      const st_device_list_t *devices = NULL;
      if (smartthings_list_devices(&devices) == ST_OK && devices && devices->count > 0) {
         len += snprintf(system_instructions_buffer + len, remaining - len,
                         "   Available devices (%d):\n", devices->count);
         for (int i = 0; i < devices->count && i < 30; i++) { /* Limit to 30 devices */
            const st_device_t *dev = &devices->devices[i];
            len += snprintf(system_instructions_buffer + len, remaining - len, "   - %s",
                            dev->label[0] ? dev->label : dev->name);
            if (dev->room[0]) {
               len += snprintf(system_instructions_buffer + len, remaining - len, " (%s)",
                               dev->room);
            }
            len += snprintf(system_instructions_buffer + len, remaining - len, "\n");
         }
         if (devices->count > 30) {
            len += snprintf(system_instructions_buffer + len, remaining - len,
                            "   - ... and %d more devices\n", devices->count - 30);
         }
      }
      LOG_INFO("System instructions: SmartThings rules included (%d devices)",
               devices ? devices->count : 0);
   } else if (smartthings_is_configured()) {
      LOG_INFO(
          "System instructions: SmartThings rules EXCLUDED (configured but not authenticated)");
   }

   // Add examples
   len += snprintf(system_instructions_buffer + len, remaining - len, "%s", LEGACY_EXAMPLES_CORE);

   // Weather example always included
   len += snprintf(system_instructions_buffer + len, remaining - len, "%s\n",
                   LEGACY_EXAMPLES_WEATHER);

   system_instructions_initialized = 1;
   LOG_INFO("Built dynamic system instructions (%d bytes)", len);

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
       g_config.localization.timezone[0] != '\0') {
      offset = snprintf(localization_context, LOCALIZATION_BUFFER_SIZE, "USER CONTEXT:");
      has_context = 1;
   }

   if (g_config.localization.location[0] != '\0') {
      offset += snprintf(localization_context + offset, LOCALIZATION_BUFFER_SIZE - offset,
                         " Location: %s (use as default for weather).",
                         g_config.localization.location);
   }

   if (g_config.localization.units[0] != '\0') {
      offset += snprintf(localization_context + offset, LOCALIZATION_BUFFER_SIZE - offset,
                         " Units: %s.", g_config.localization.units);
   }

   if (g_config.localization.timezone[0] != '\0') {
      offset += snprintf(localization_context + offset, LOCALIZATION_BUFFER_SIZE - offset,
                         " Timezone: %s.", g_config.localization.timezone);
   }

   if (has_context) {
      snprintf(localization_context + offset, LOCALIZATION_BUFFER_SIZE - offset, "\n\n");
   }

   localization_initialized = 1;
   return localization_context;
}

/**
 * @brief Gets the persona description from config or falls back to compile-time default
 *
 * Returns g_config.persona.description if set, otherwise AI_PERSONA from dawn.h.
 * This allows runtime customization of the AI personality via config file while
 * keeping the system instructions (AI_SYSTEM_INSTRUCTIONS) always active.
 */
static const char *get_persona_description(void) {
   if (g_config.persona.description[0] != '\0') {
      return g_config.persona.description;
   }
   return AI_PERSONA;
}

/**
 * @brief Builds a simple command prompt string from the commands_config_nuevo.json file
 *
 * This function reads the config file, extracts command patterns,
 * and builds a simple string describing available commands for the LLM.
 */
static void initialize_command_prompt(void) {
   if (prompt_initialized) {
      return;
   }

   FILE *configFile = NULL;
   char buffer[10 * 1024];
   int bytes_read = 0;
   struct json_object *parsedJson = NULL;
   struct json_object *typesObject = NULL;
   struct json_object *devicesObject = NULL;

   // Start with persona, system instructions, localization context
   // Persona is replaceable via config. System instructions are built dynamically based on
   // which features are enabled (search, vision, etc.)

   // When native tool calling is enabled, skip the <command> tag instructions entirely
   // since tools handle structured commands natively
   if (llm_tools_enabled(NULL)) {
      int prompt_len = snprintf(command_prompt, PROMPT_BUFFER_SIZE,
                                "%s\n\n"  // Persona (from config or AI_PERSONA)
                                "%s\n\n"  // System instructions (minimal for tools mode)
                                "%s",     // Localization context
                                get_persona_description(), get_system_instructions(),
                                get_localization_context());
      prompt_initialized = 1;
      LOG_INFO("AI prompt initialized (native tools mode). Length: %d", prompt_len);
      return;
   }

   // Legacy mode: include <command> tag instructions
   int prompt_len = snprintf(
       command_prompt, PROMPT_BUFFER_SIZE,
       "%s\n\n"  // Persona (from config or AI_PERSONA)
       "%s\n\n"  // System instructions (dynamically built based on enabled features)
       "%s"      // Localization context (empty string if not configured)
       "You can also execute commands for me. These are the commands available:\n\n",
       get_persona_description(), get_system_instructions(), get_localization_context());

   LOG_INFO("Static prompt processed. Length: %d", prompt_len);

   // Read the commands config file (path from g_config)
   configFile = fopen(g_config.paths.commands_config, "r");
   if (configFile == NULL) {
      LOG_ERROR("Unable to open commands config file: %s", g_config.paths.commands_config);
      return;
   }

   if ((bytes_read = fread(buffer, 1, sizeof(buffer), configFile)) > 0) {
      buffer[bytes_read] = '\0';
   } else {
      LOG_ERROR("Failed to read config file");
      fclose(configFile);
      return;
   }


   if (bytes_read == sizeof(buffer)) {
      LOG_ERROR("Config file buffer is too small.");
      fclose(configFile);
      return;
   }

   fclose(configFile);

   LOG_INFO("Config file read for AI prompt. Length: %d", bytes_read);

   // Parse JSON
   parsedJson = json_tokener_parse(buffer);
   if (parsedJson == NULL) {
      LOG_ERROR("Failed to parse config JSON");
      return;
   }

   // Get the "types" and "devices" objects
   if (!json_object_object_get_ex(parsedJson, "types", &typesObject) ||
       !json_object_object_get_ex(parsedJson, "devices", &devicesObject)) {
      LOG_ERROR("Required objects not found in json");
      json_object_put(parsedJson);
      return;
   }

   // Add a section for each command type
   struct json_object_iterator type_it = json_object_iter_begin(typesObject);
   struct json_object_iterator type_it_end = json_object_iter_end(typesObject);

   while (!json_object_iter_equal(&type_it, &type_it_end)) {
      const char *type_name = json_object_iter_peek_name(&type_it);
      struct json_object *type_obj;
      json_object_object_get_ex(typesObject, type_name, &type_obj);

      prompt_len += snprintf(command_prompt + prompt_len, PROMPT_BUFFER_SIZE - prompt_len,
                             "== %s Commands ==\n", type_name);

      // Get the actions for this type
      struct json_object *actions_obj;
      if (json_object_object_get_ex(type_obj, "actions", &actions_obj)) {
         struct json_object_iterator action_it = json_object_iter_begin(actions_obj);
         struct json_object_iterator action_it_end = json_object_iter_end(actions_obj);

         while (!json_object_iter_equal(&action_it, &action_it_end)) {
            const char *action_name = json_object_iter_peek_name(&action_it);
            struct json_object *action_obj;
            json_object_object_get_ex(actions_obj, action_name, &action_obj);

            struct json_object *command_obj;
            if (json_object_object_get_ex(action_obj, "action_command", &command_obj)) {
               const char *command = json_object_get_string(command_obj);

               // Add the command format
               prompt_len += snprintf(command_prompt + prompt_len, PROMPT_BUFFER_SIZE - prompt_len,
                                      "- %s: %s\n", action_name, command);
            }

            json_object_iter_next(&action_it);
         }
      }

      // Add a list of devices for this type
      prompt_len += snprintf(command_prompt + prompt_len, PROMPT_BUFFER_SIZE - prompt_len,
                             "  Valid devices for this command only: ");

      // Find all devices of this type
      int device_count = 0;
      struct json_object_iterator dev_it = json_object_iter_begin(devicesObject);
      struct json_object_iterator dev_it_end = json_object_iter_end(devicesObject);

      while (!json_object_iter_equal(&dev_it, &dev_it_end)) {
         const char *device_name = json_object_iter_peek_name(&dev_it);
         struct json_object *device_obj;
         json_object_object_get_ex(devicesObject, device_name, &device_obj);

         struct json_object *device_type_obj;
         if (json_object_object_get_ex(device_obj, "type", &device_type_obj)) {
            const char *device_type = json_object_get_string(device_type_obj);

            if (strcmp(device_type, type_name) == 0) {
               if (device_count > 0) {
                  prompt_len += snprintf(command_prompt + prompt_len,
                                         PROMPT_BUFFER_SIZE - prompt_len, ", ");
               }
               prompt_len += snprintf(command_prompt + prompt_len, PROMPT_BUFFER_SIZE - prompt_len,
                                      "%s", device_name);
               device_count++;
            }
         }

         json_object_iter_next(&dev_it);
      }

      prompt_len += snprintf(command_prompt + prompt_len, PROMPT_BUFFER_SIZE - prompt_len, "\n\n");

      json_object_iter_next(&type_it);
   }

   // Add response format instructions (legacy <command> tag mode)
   prompt_len += snprintf(command_prompt + prompt_len, PROMPT_BUFFER_SIZE - prompt_len, "%s",
                          LEGACY_LOCAL_COMMAND_INSTRUCTIONS);

   json_object_put(parsedJson);
   prompt_initialized = 1;

   LOG_INFO("AI prompt initialized. Length: %d", prompt_len);
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
 * @brief Check if a topic is excluded for remote clients
 */
static int is_topic_excluded(const char *topic) {
   if (!topic)
      return 0;
   for (int i = 0; excluded_remote_topics[i] != NULL; i++) {
      if (strcmp(topic, excluded_remote_topics[i]) == 0) {
         return 1;
      }
   }
   return 0;
}

/**
 * @brief Builds the remote command prompt (excludes local-only topics like hud, helmet)
 *
 * This creates a prompt for network satellite clients that only includes
 * general commands (date, time, etc.) but excludes HUD and helmet controls.
 */
static void initialize_remote_command_prompt(void) {
   if (remote_prompt_initialized) {
      return;
   }

   FILE *configFile = NULL;
   char buffer[10 * 1024];
   int bytes_read = 0;
   struct json_object *parsedJson = NULL;
   struct json_object *typesObject = NULL;
   struct json_object *devicesObject = NULL;

   // Start with persona, system instructions, localization context
   // Persona is replaceable via config. System instructions are built dynamically based on
   // which features are enabled (search, vision, etc.)

   // When native tool calling is enabled, skip the <command> tag instructions entirely
   // since tools handle structured commands natively
   if (llm_tools_enabled(NULL)) {
      int prompt_len = snprintf(remote_command_prompt, PROMPT_BUFFER_SIZE,
                                "%s\n\n"  // Persona (from config or AI_PERSONA)
                                "%s\n\n"  // System instructions (minimal for tools mode)
                                "%s",     // Localization context
                                get_persona_description(), get_system_instructions(),
                                get_localization_context());
      remote_prompt_initialized = 1;
      LOG_INFO("Remote AI prompt initialized (native tools mode). Length: %d", prompt_len);
      return;
   }

   // Legacy mode: include <command> tag instructions
   int prompt_len = snprintf(
       remote_command_prompt, PROMPT_BUFFER_SIZE,
       "%s\n\n"  // Persona (from config or AI_PERSONA)
       "%s\n\n"  // System instructions (dynamically built based on enabled features)
       "%s"      // Localization context (empty string if not configured)
       "You can also execute commands for me. These are the commands available:\n\n",
       get_persona_description(), get_system_instructions(), get_localization_context());

   // Read the commands config file (path from g_config)
   configFile = fopen(g_config.paths.commands_config, "r");
   if (configFile == NULL) {
      LOG_ERROR("Unable to open commands config file: %s", g_config.paths.commands_config);
      remote_prompt_initialized = 1;
      return;
   }

   if ((bytes_read = fread(buffer, 1, sizeof(buffer), configFile)) > 0) {
      buffer[bytes_read] = '\0';
   } else {
      LOG_ERROR("Failed to read config file for remote prompt");
      fclose(configFile);
      remote_prompt_initialized = 1;
      return;
   }

   if (bytes_read == sizeof(buffer)) {
      LOG_ERROR("Config file buffer is too small.");
      fclose(configFile);
      remote_prompt_initialized = 1;
      return;
   }

   fclose(configFile);

   // Parse JSON
   parsedJson = json_tokener_parse(buffer);
   if (parsedJson == NULL) {
      LOG_ERROR("Failed to parse config JSON for remote prompt");
      remote_prompt_initialized = 1;
      return;
   }

   // Get the "types" and "devices" objects
   if (!json_object_object_get_ex(parsedJson, "types", &typesObject) ||
       !json_object_object_get_ex(parsedJson, "devices", &devicesObject)) {
      LOG_ERROR("Required objects not found in json for remote prompt");
      json_object_put(parsedJson);
      remote_prompt_initialized = 1;
      return;
   }

   // Add a section for each command type (only if it has non-excluded devices)
   struct json_object_iterator type_it = json_object_iter_begin(typesObject);
   struct json_object_iterator type_it_end = json_object_iter_end(typesObject);

   while (!json_object_iter_equal(&type_it, &type_it_end)) {
      const char *type_name = json_object_iter_peek_name(&type_it);
      struct json_object *type_obj;
      json_object_object_get_ex(typesObject, type_name, &type_obj);

      // First, check if there are any non-excluded devices of this type
      int has_devices = 0;
      struct json_object_iterator dev_check = json_object_iter_begin(devicesObject);
      struct json_object_iterator dev_check_end = json_object_iter_end(devicesObject);

      while (!json_object_iter_equal(&dev_check, &dev_check_end)) {
         struct json_object *device_obj;
         const char *device_name = json_object_iter_peek_name(&dev_check);
         json_object_object_get_ex(devicesObject, device_name, &device_obj);

         struct json_object *device_type_obj, *topic_obj;
         if (json_object_object_get_ex(device_obj, "type", &device_type_obj)) {
            const char *device_type = json_object_get_string(device_type_obj);
            if (strcmp(device_type, type_name) == 0) {
               // Check if topic is excluded
               const char *topic = NULL;
               if (json_object_object_get_ex(device_obj, "topic", &topic_obj)) {
                  topic = json_object_get_string(topic_obj);
               }
               if (!is_topic_excluded(topic)) {
                  has_devices = 1;
                  break;
               }
            }
         }
         json_object_iter_next(&dev_check);
      }

      // Skip this type if no devices are available for remote clients
      if (!has_devices) {
         json_object_iter_next(&type_it);
         continue;
      }

      prompt_len += snprintf(remote_command_prompt + prompt_len, PROMPT_BUFFER_SIZE - prompt_len,
                             "== %s Commands ==\n", type_name);

      // Get the actions for this type
      struct json_object *actions_obj;
      if (json_object_object_get_ex(type_obj, "actions", &actions_obj)) {
         struct json_object_iterator action_it = json_object_iter_begin(actions_obj);
         struct json_object_iterator action_it_end = json_object_iter_end(actions_obj);

         while (!json_object_iter_equal(&action_it, &action_it_end)) {
            const char *action_name = json_object_iter_peek_name(&action_it);
            struct json_object *action_obj;
            json_object_object_get_ex(actions_obj, action_name, &action_obj);

            struct json_object *command_obj;
            if (json_object_object_get_ex(action_obj, "action_command", &command_obj)) {
               const char *command = json_object_get_string(command_obj);

               prompt_len += snprintf(remote_command_prompt + prompt_len,
                                      PROMPT_BUFFER_SIZE - prompt_len, "- %s: %s\n", action_name,
                                      command);
            }

            json_object_iter_next(&action_it);
         }
      }

      // Add a list of devices for this type (only non-excluded ones)
      prompt_len += snprintf(remote_command_prompt + prompt_len, PROMPT_BUFFER_SIZE - prompt_len,
                             "  Valid devices for this command only: ");

      // Find all non-excluded devices of this type
      int device_count = 0;
      struct json_object_iterator dev_it = json_object_iter_begin(devicesObject);
      struct json_object_iterator dev_it_end = json_object_iter_end(devicesObject);

      while (!json_object_iter_equal(&dev_it, &dev_it_end)) {
         const char *device_name = json_object_iter_peek_name(&dev_it);
         struct json_object *device_obj;
         json_object_object_get_ex(devicesObject, device_name, &device_obj);

         struct json_object *device_type_obj, *topic_obj;
         if (json_object_object_get_ex(device_obj, "type", &device_type_obj)) {
            const char *device_type = json_object_get_string(device_type_obj);

            if (strcmp(device_type, type_name) == 0) {
               // Check if topic is excluded
               const char *topic = NULL;
               if (json_object_object_get_ex(device_obj, "topic", &topic_obj)) {
                  topic = json_object_get_string(topic_obj);
               }

               if (!is_topic_excluded(topic)) {
                  if (device_count > 0) {
                     prompt_len += snprintf(remote_command_prompt + prompt_len,
                                            PROMPT_BUFFER_SIZE - prompt_len, ", ");
                  }
                  prompt_len += snprintf(remote_command_prompt + prompt_len,
                                         PROMPT_BUFFER_SIZE - prompt_len, "%s", device_name);
                  device_count++;
               }
            }
         }

         json_object_iter_next(&dev_it);
      }

      prompt_len += snprintf(remote_command_prompt + prompt_len, PROMPT_BUFFER_SIZE - prompt_len,
                             "\n\n");

      json_object_iter_next(&type_it);
   }

   // Add response format instructions (legacy <command> tag mode)
   prompt_len += snprintf(remote_command_prompt + prompt_len, PROMPT_BUFFER_SIZE - prompt_len, "%s",
                          LEGACY_REMOTE_COMMAND_INSTRUCTIONS);

   json_object_put(parsedJson);
   remote_prompt_initialized = 1;

   LOG_INFO("Remote AI prompt initialized. Length: %d", prompt_len);
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
