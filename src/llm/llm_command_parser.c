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
#include "dawn.h"
#include "logging.h"
#include "mosquitto_comms.h"
#include "text_to_command_nuevo.h"
#include "ui/metrics.h"

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
 * current LLM type (cloud or local). Cloud models typically support vision,
 * while local models may or may not (e.g., LLaVA, Qwen-VL support vision).
 *
 * @return 1 if vision is available, 0 otherwise
 */
int is_vision_enabled_for_current_llm(void) {
   if (strcmp(g_config.llm.type, "cloud") == 0) {
      return g_config.llm.cloud.vision_enabled;
   } else if (strcmp(g_config.llm.type, "local") == 0) {
      return g_config.llm.local.vision_enabled;
   }
   return 0;
}


/**
 * @brief Builds dynamic system instructions based on enabled features
 *
 * Assembles AI_RULES_CORE plus feature-specific rules based on config.
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
const char *get_system_instructions(void) {
   if (system_instructions_initialized) {
      return system_instructions_buffer;
   }

   int len = 0;
   int remaining = SYSTEM_INSTRUCTIONS_BUFFER_SIZE;

   // Always include core rules
   len += snprintf(system_instructions_buffer + len, remaining - len, "%s\n", AI_RULES_CORE);

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

   len += snprintf(system_instructions_buffer + len, remaining - len, ".\n\n");

   // Add feature-specific rules based on config
   if (is_vision_enabled_for_current_llm()) {
      len += snprintf(system_instructions_buffer + len, remaining - len, "%s\n", AI_RULES_VISION);
      LOG_INFO("System instructions: Vision rules included (cloud LLM with vision support)");
   }

   // Weather always available
   len += snprintf(system_instructions_buffer + len, remaining - len, "%s\n", AI_RULES_WEATHER);

   // Search only if endpoint configured
   if (is_search_enabled()) {
      len += snprintf(system_instructions_buffer + len, remaining - len, "%s\n", AI_RULES_SEARCH);
      LOG_INFO("System instructions: Search rules included (SearXNG endpoint: %s)",
               g_config.search.endpoint);
   } else {
      LOG_INFO("System instructions: Search rules EXCLUDED (no SearXNG endpoint configured)");
   }

   // Calculator always available
   len += snprintf(system_instructions_buffer + len, remaining - len, "%s\n", AI_RULES_CALCULATOR);

   // URL fetcher always available
   len += snprintf(system_instructions_buffer + len, remaining - len, "%s\n", AI_RULES_URL);

   // LLM status always available
   len += snprintf(system_instructions_buffer + len, remaining - len, "%s\n", AI_RULES_LLM_STATUS);

   // Add examples
   len += snprintf(system_instructions_buffer + len, remaining - len, "%s", AI_EXAMPLES_CORE);

   // Weather example always included
   len += snprintf(system_instructions_buffer + len, remaining - len, "%s\n", AI_EXAMPLES_WEATHER);

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

   // Start with persona, system instructions, localization context, then command intro
   // Persona is replaceable via config. System instructions are built dynamically based on
   // which features are enabled (search, vision, etc.)
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

   // Add response format instructions (from dawn.h)
   prompt_len += snprintf(command_prompt + prompt_len, PROMPT_BUFFER_SIZE - prompt_len, "%s",
                          AI_LOCAL_COMMAND_INSTRUCTIONS);

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

   // Start with persona, system instructions, localization context, then command intro
   // Persona is replaceable via config. System instructions are built dynamically based on
   // which features are enabled (search, vision, etc.)
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

   // Add response format instructions (from dawn.h)
   prompt_len += snprintf(remote_command_prompt + prompt_len, PROMPT_BUFFER_SIZE - prompt_len, "%s",
                          AI_REMOTE_COMMAND_INSTRUCTIONS);

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
 * @brief Validate that a device exists in the commands config
 *
 * SECURITY: This prevents LLM prompt injection attacks from executing commands
 * for non-existent or hallucinated devices. Only devices defined in
 * commands_config_nuevo.json are allowed.
 *
 * @param device The device name to validate
 * @param topic_out Output buffer for the device's topic (can be NULL)
 * @param topic_out_size Size of topic_out buffer
 * @return true if device exists and is valid, false otherwise
 */
static bool validate_device_in_config(const char *device, char *topic_out, size_t topic_out_size) {
   if (!device || device[0] == '\0') {
      return false;
   }

   bool device_found = false;

   FILE *configFile = fopen(g_config.paths.commands_config, "r");
   if (!configFile) {
      LOG_ERROR("LLM command validation: Cannot open commands config");
      return false;
   }

   char buffer[10 * 1024];
   int bytes_read = fread(buffer, 1, sizeof(buffer) - 1, configFile);
   fclose(configFile);

   if (bytes_read <= 0) {
      return false;
   }
   buffer[bytes_read] = '\0';

   struct json_object *config_json = json_tokener_parse(buffer);
   if (!config_json) {
      return false;
   }

   struct json_object *devices_obj, *device_config_obj;
   if (json_object_object_get_ex(config_json, "devices", &devices_obj) &&
       json_object_object_get_ex(devices_obj, device, &device_config_obj)) {
      device_found = true;

      /* Extract topic if requested */
      if (topic_out && topic_out_size > 0) {
         struct json_object *topic_obj;
         if (json_object_object_get_ex(device_config_obj, "topic", &topic_obj)) {
            const char *topic = json_object_get_string(topic_obj);
            strncpy(topic_out, topic, topic_out_size - 1);
            topic_out[topic_out_size - 1] = '\0';
         } else {
            strncpy(topic_out, "dawn", topic_out_size - 1);
            topic_out[topic_out_size - 1] = '\0';
         }
      }
   }

   json_object_put(config_json);
   return device_found;
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
                  char topic[64] = "dawn";

                  /* SECURITY: Validate device against allowlist before execution */
                  if (validate_device_in_config(device, topic, sizeof(topic))) {
                     /* Device is valid - execute command */
                     int rc = mosquitto_publish(mosq, NULL, topic, strlen(command), command, 0,
                                                false);
                     if (rc != MOSQ_ERR_SUCCESS) {
                        LOG_ERROR("Error publishing command: %s", mosquitto_strerror(rc));
                     } else {
                        LOG_INFO("LLM COMMAND EXECUTED: device=%s topic=%s", device, topic);
                        metrics_log_activity("LLM CMD: %s", command);
                        commands_found++;
                     }
                  } else {
                     /* SECURITY AUDIT: Unknown device rejected - possible prompt injection */
                     LOG_WARNING("LLM COMMAND REJECTED: Unknown device '%s' - not in allowlist",
                                 device);
                     LOG_WARNING("  Full command was: %s", command);
                     metrics_log_activity("LLM CMD BLOCKED: %s", device);
                  }
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
