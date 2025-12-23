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
 *
 * Native LLM Tool/Function Calling Implementation
 *
 * This module provides native tool calling support for OpenAI, Claude, and
 * local LLMs (via llama.cpp with --jinja flag). Tools are defined once and
 * converted to provider-specific formats.
 *
 * Tool calling reduces system prompt size by ~70% and improves reliability
 * by using structured responses instead of parsing <command> tags from text.
 */

#include "llm/llm_tools.h"

#include <limits.h>
#include <mosquitto.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "core/command_executor.h"
#include "core/command_registry.h"
#include "core/session_manager.h"
#include "core/worker_pool.h"
#include "dawn.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_interface.h"
#include "logging.h"
#include "mosquitto_comms.h"
#include "tools/string_utils.h"

/* Forward declarations for utility functions from mosquitto_comms.c */
extern unsigned char *read_file(const char *filename, size_t *length);
extern char *base64_encode(const unsigned char *buffer, size_t length);

/* Timeout for viewing MQTT responses (10 seconds) */
#define VIEWING_MQTT_TIMEOUT_MS 10000

/* =============================================================================
 * Static Tool Definitions
 * ============================================================================= */

static tool_definition_t s_tools[LLM_TOOLS_MAX_TOOLS];
static int s_tool_count = 0;
static int s_enabled_count = 0; /* Cached enabled count, updated by llm_tools_refresh() */
static bool s_initialized = false;

/* =============================================================================
 * Pending Vision Data (for viewing tool)
 * ============================================================================= */

static char *s_pending_vision_image = NULL;
static size_t s_pending_vision_size = 0;
static pthread_mutex_t s_pending_vision_mutex = PTHREAD_MUTEX_INITIALIZER;

/* =============================================================================
 * Tool Execution Notification Callback
 * ============================================================================= */

static tool_execution_callback_fn s_execution_callback = NULL;

void llm_tools_set_execution_callback(tool_execution_callback_fn callback) {
   s_execution_callback = callback;
}

/**
 * @brief Notify registered callback about tool execution
 */
static void notify_tool_execution(const char *tool_name,
                                  const char *tool_args,
                                  const char *result,
                                  bool success) {
   if (s_execution_callback) {
      /* Get current command context (session) for callback */
      session_t *session = session_get_command_context();
      s_execution_callback((void *)session, tool_name, tool_args, result, success);
   }
}

/* Forward declaration for atomic pending vision update */
static void set_pending_vision_locked(char *image, size_t size);

/* =============================================================================
 * Security Helpers
 * ============================================================================= */

/**
 * @brief Validate a file path for security
 *
 * Checks that the path is within allowed directories and doesn't contain
 * path traversal attempts. Resolves symlinks to prevent symlink attacks.
 */
static bool validate_file_path(const char *path) {
   if (!path || path[0] == '\0') {
      return false;
   }

   /* Reject relative paths */
   if (path[0] != '/') {
      LOG_WARNING("Rejected relative path: %s", path);
      return false;
   }

   /* Reject path traversal attempts (check before resolving) */
   if (strstr(path, "..") != NULL) {
      LOG_WARNING("Rejected path with traversal: %s", path);
      return false;
   }

   /* Resolve symlinks to get canonical path - prevents symlink attacks */
   char resolved[PATH_MAX];
   if (realpath(path, resolved) == NULL) {
      LOG_WARNING("Could not resolve path (file may not exist): %s", path);
      return false;
   }

   /* Allowed directories for viewing response files */
   const char *allowed_prefixes[] = { "/home/jetson/recordings/", "/tmp/", "/home/jetson/oasis/" };
   size_t prefix_count = sizeof(allowed_prefixes) / sizeof(allowed_prefixes[0]);

   for (size_t i = 0; i < prefix_count; i++) {
      if (strncmp(resolved, allowed_prefixes[i], strlen(allowed_prefixes[i])) == 0) {
         return true;
      }
   }

   LOG_WARNING("Rejected path outside allowed directories: %s (resolved: %s)", path, resolved);
   return false;
}

/**
 * Check if a string looks like base64-encoded image data.
 * Base64 image data is typically long and contains only valid base64 chars.
 */
static bool is_base64_image_data(const char *str) {
   if (!str || strlen(str) < 100) {
      return false; /* Too short to be an image */
   }

   /* Check first few chars for base64 pattern (starts with / for JPEG) */
   /* JPEG base64 typically starts with "/9j/" */
   if (str[0] == '/' && str[1] == '9' && str[2] == 'j') {
      return true;
   }

   /* PNG base64 typically starts with "iVBOR" */
   if (strncmp(str, "iVBOR", 5) == 0) {
      return true;
   }

   return false;
}

/* =============================================================================
 * Vision Processing
 *
 * There are TWO vision data paths for session isolation:
 *
 * 1. NATIVE TOOL PATH (session-isolated):
 *    - execute_viewing_sync() stores vision in tool_result_t.vision_image
 *    - LLM streaming code (llm_openai.c, llm_claude.c) picks it up from results
 *    - Each session's tool results are isolated from other sessions
 *    - Caller must free tool_result_t.vision_image after use
 *
 * 2. VOICE COMMAND PATH (global, single-session):
 *    - viewingCallback in mosquitto_comms.c calls llm_tools_process_vision_data()
 *    - Vision stored in global s_pending_vision_image (mutex-protected)
 *    - dawn.c main loop picks it up via llm_tools_get_pending_vision()
 *    - This path is single-session so global state is acceptable
 *    - Consumer MUST call llm_tools_clear_pending_vision() after use
 *
 * The global pending vision functions are DEPRECATED for multi-session use.
 * New code should use tool_result_t.vision_image for session isolation.
 * ============================================================================= */

/**
 * @brief Process vision data from either base64 or file path
 *
 * This is the unified handler for vision data from any source (native tools,
 * callbacks, MQTT responses). It detects the data type and stores in pending vision.
 *
 * @param data Vision data - either base64-encoded image or a file path
 * @param error_buf Output buffer for error messages (can be NULL)
 * @param error_len Size of error buffer
 * @return true on success (image stored in pending vision), false on failure
 */
static bool process_vision_data(const char *data, char *error_buf, size_t error_len) {
   if (!data || data[0] == '\0') {
      if (error_buf) {
         snprintf(error_buf, error_len, "Error: No vision data provided");
      }
      return false;
   }

   /* Check if data is an error response */
   if (strncmp(data, "ERROR:", 6) == 0) {
      if (error_buf) {
         snprintf(error_buf, error_len, "%s", data);
      }
      return false;
   }

   char *base64_image = NULL;

   if (is_base64_image_data(data)) {
      /* Data is already base64-encoded image */
      LOG_INFO("Vision data is inline base64 (%zu bytes)", strlen(data));
      base64_image = strdup(data);
      if (!base64_image) {
         if (error_buf) {
            snprintf(error_buf, error_len, "Error: Memory allocation failed");
         }
         return false;
      }
   } else {
      /* Data is a file path - validate, read, and encode */
      LOG_INFO("Vision data is file path: %s", data);

      /* Validate path for security */
      if (!validate_file_path(data)) {
         if (error_buf) {
            snprintf(error_buf, error_len, "Error: Invalid or unsafe file path: %s", data);
         }
         return false;
      }

      /* Read file */
      size_t file_size = 0;
      unsigned char *file_content = read_file(data, &file_size);
      if (!file_content) {
         if (error_buf) {
            snprintf(error_buf, error_len, "Error: Could not read image file: %s", data);
         }
         return false;
      }

      /* Encode to base64 */
      base64_image = base64_encode(file_content, file_size);
      free(file_content);

      if (!base64_image) {
         if (error_buf) {
            snprintf(error_buf, error_len, "Error: Could not encode image to base64");
         }
         return false;
      }
      LOG_INFO("Image file encoded: %zu bytes base64", strlen(base64_image));
   }

   /* Store in pending vision (takes ownership of base64_image) */
   size_t image_size = strlen(base64_image) + 1;
   set_pending_vision_locked(base64_image, image_size);

   LOG_INFO("Vision image stored in pending vision: %zu bytes", image_size);
   return true;
}

/**
 * @brief Process vision data and return base64 image
 *
 * Handles both inline base64 data and file paths. Returns the base64 image
 * that the caller must free.
 *
 * @param data Vision data - either base64-encoded image or a file path
 * @param image_out Output: allocated base64 image string (caller must free)
 * @param image_size_out Output: size of image data
 * @param error_buf Output buffer for error messages (can be NULL)
 * @param error_len Size of error buffer
 * @return true on success, false on failure
 */
static bool extract_vision_image(const char *data,
                                 char **image_out,
                                 size_t *image_size_out,
                                 char *error_buf,
                                 size_t error_len) {
   if (!data || data[0] == '\0') {
      if (error_buf) {
         snprintf(error_buf, error_len, "Error: No vision data provided");
      }
      return false;
   }

   /* Check if data is an error response */
   if (strncmp(data, "ERROR:", 6) == 0) {
      if (error_buf) {
         snprintf(error_buf, error_len, "%s", data);
      }
      return false;
   }

   char *base64_image = NULL;

   if (is_base64_image_data(data)) {
      /* Data is already base64-encoded image */
      LOG_INFO("Vision data is inline base64 (%zu bytes)", strlen(data));
      base64_image = strdup(data);
      if (!base64_image) {
         if (error_buf) {
            snprintf(error_buf, error_len, "Error: Memory allocation failed");
         }
         return false;
      }
   } else {
      /* Data is a file path - validate, read, and encode */
      LOG_INFO("Vision data is file path: %s", data);

      /* Validate path for security */
      if (!validate_file_path(data)) {
         if (error_buf) {
            snprintf(error_buf, error_len, "Error: Invalid or unsafe file path: %s", data);
         }
         return false;
      }

      /* Read file */
      size_t file_size = 0;
      unsigned char *file_content = read_file(data, &file_size);
      if (!file_content) {
         if (error_buf) {
            snprintf(error_buf, error_len, "Error: Could not read image file: %s", data);
         }
         return false;
      }

      /* Encode to base64 */
      base64_image = base64_encode(file_content, file_size);
      free(file_content);

      if (!base64_image) {
         if (error_buf) {
            snprintf(error_buf, error_len, "Error: Could not encode image to base64");
         }
         return false;
      }
      LOG_INFO("Image file encoded: %zu bytes base64", strlen(base64_image));
   }

   *image_out = base64_image;
   *image_size_out = strlen(base64_image) + 1;
   return true;
}

/**
 * Execute viewing command with synchronous MQTT wait.
 *
 * Uses command_execute_sync() for the MQTT portion, then extracts the
 * vision image and stores it in the tool result.
 *
 * @param action Action name (e.g., "look")
 * @param query Query string (e.g., "what do you see?")
 * @param tool_result Tool result structure to populate (includes vision_image)
 * @return true on success (image captured and stored in result), false on failure
 */
static bool execute_viewing_sync(const char *action,
                                 const char *query,
                                 tool_result_t *tool_result) {
   /* Check if vision is enabled (viewing-specific pre-check) */
   if (!is_vision_enabled_for_current_llm()) {
      snprintf(tool_result->result, LLM_TOOLS_RESULT_LEN,
               "Vision is not enabled for the current AI model. "
               "Switch to cloud LLM or enable vision for local LLM in the config.");
      return false;
   }

   /* Get MQTT client and topic */
   struct mosquitto *mosq = worker_pool_get_mosq();
   if (!mosq) {
      snprintf(tool_result->result, LLM_TOOLS_RESULT_LEN, "Error: MQTT not available");
      return false;
   }

   char topic[64] = "dawn";
   if (!command_registry_validate("viewing", topic, sizeof(topic))) {
      snprintf(tool_result->result, LLM_TOOLS_RESULT_LEN, "Error: viewing device not configured");
      return false;
   }

   /* Use unified sync executor for the MQTT wait portion */
   cmd_exec_result_t exec_result;
   int rc = command_execute_sync("viewing", action, query, mosq, topic, &exec_result,
                                 VIEWING_MQTT_TIMEOUT_MS);

   if (rc != 0 || !exec_result.success) {
      if (exec_result.result) {
         snprintf(tool_result->result, LLM_TOOLS_RESULT_LEN, "%s", exec_result.result);
      } else {
         snprintf(tool_result->result, LLM_TOOLS_RESULT_LEN, "Error: Viewing command failed");
      }
      cmd_exec_result_free(&exec_result);
      return false;
   }

   /* Extract vision image and store in tool result */
   char *vision_image = NULL;
   size_t vision_size = 0;
   bool success = extract_vision_image(exec_result.result, &vision_image, &vision_size,
                                       tool_result->result, LLM_TOOLS_RESULT_LEN);
   cmd_exec_result_free(&exec_result);

   if (success) {
      tool_result->vision_image = vision_image;
      tool_result->vision_image_size = vision_size;
      snprintf(tool_result->result, LLM_TOOLS_RESULT_LEN,
               "Image captured successfully. Please analyze and respond to the user's request.");
      LOG_INFO("Vision image stored in tool result: %zu bytes", vision_size);
   }
   return success;
}

/* =============================================================================
 * Table-Driven Parameter Extraction
 * ============================================================================= */

/**
 * @brief Extract parameters from tool arguments using registry metadata
 *
 * Uses the maps_to field from command parameters to route values to
 * device/action/value fields, replacing the 160-line switch statement.
 *
 * @param cmd Command definition from registry
 * @param args Parsed JSON arguments
 * @param device Output device name (for meta-tools with device_map)
 * @param action Output action name
 * @param value Output value string
 * @param custom_fields Output for custom field mappings (e.g., setting for smartthings)
 * @return 0 on success, non-zero on validation error
 */
static int extract_params_from_registry(const cmd_definition_t *cmd,
                                        struct json_object *args,
                                        char *device,
                                        size_t device_len,
                                        char *action,
                                        size_t action_len,
                                        char *value,
                                        size_t value_len) {
   if (!cmd) {
      return 1;
   }

   /* Set defaults */
   device[0] = '\0';
   strcpy(action, "get"); /* Default action */
   value[0] = '\0';

   /* For meta-tools, start with the tool's device_string as device */
   if (cmd->is_meta_tool) {
      safe_strncpy(device, cmd->device_string, device_len);
   }

   if (!args) {
      return 0; /* No arguments to extract */
   }

   /* Track if we need to append to value (for smartthings device+value pattern) */
   bool first_value = true;

   /* Iterate through command parameters and extract based on maps_to */
   for (int i = 0; i < cmd->param_count; i++) {
      const cmd_param_t *param = &cmd->parameters[i];
      struct json_object *arg_val = NULL;

      if (!json_object_object_get_ex(args, param->name, &arg_val)) {
         continue; /* Parameter not provided */
      }

      const char *str_val = NULL;
      char int_buf[32];

      /* Get string representation based on type */
      if (param->type == CMD_PARAM_INTEGER) {
         snprintf(int_buf, sizeof(int_buf), "%d", json_object_get_int(arg_val));
         str_val = int_buf;
      } else if (param->type == CMD_PARAM_NUMBER) {
         snprintf(int_buf, sizeof(int_buf), "%g", json_object_get_double(arg_val));
         str_val = int_buf;
      } else if (param->type == CMD_PARAM_BOOLEAN) {
         str_val = json_object_get_boolean(arg_val) ? "true" : "false";
      } else {
         str_val = json_object_get_string(arg_val);
      }

      if (!str_val) {
         continue;
      }

      /* Validate enum values if applicable */
      if (param->type == CMD_PARAM_ENUM && param->enum_count > 0) {
         bool valid = false;
         for (int j = 0; j < param->enum_count; j++) {
            if (strcmp(str_val, param->enum_values[j]) == 0) {
               valid = true;
               break;
            }
         }
         if (!valid) {
            LOG_WARNING("Invalid enum value '%s' for parameter '%s'", str_val, param->name);
            return 1; /* Validation failed */
         }
      }

      /* Route value based on maps_to */
      switch (param->maps_to) {
         case CMD_MAPS_TO_ACTION:
            safe_strncpy(action, str_val, action_len);
            break;

         case CMD_MAPS_TO_DEVICE:
            /* For meta-tools, resolve through device_map if available */
            if (cmd->is_meta_tool && cmd->device_map_count > 0) {
               const char *resolved = command_registry_resolve_device(cmd, str_val);
               if (resolved) {
                  safe_strncpy(device, resolved, device_len);
               } else {
                  /* No mapping found, use value directly */
                  safe_strncpy(device, str_val, device_len);
               }
            } else {
               safe_strncpy(device, str_val, device_len);
            }
            break;

         case CMD_MAPS_TO_VALUE:
            if (first_value) {
               safe_strncpy(value, str_val, value_len);
               first_value = false;
            } else {
               /* Append with space (for smartthings device+value pattern) */
               size_t cur_len = strlen(value);
               if (cur_len > 0 && cur_len < value_len - 2) {
                  value[cur_len] = ' ';
                  safe_strncpy(value + cur_len + 1, str_val, value_len - cur_len - 1);
               }
            }
            break;

         case CMD_MAPS_TO_CUSTOM:
            /* Custom fields like "custom:setting" - append to value with prefix */
            /* For now, just append to value like the smartthings pattern */
            if (first_value) {
               safe_strncpy(value, str_val, value_len);
               first_value = false;
            } else {
               size_t cur_len = strlen(value);
               if (cur_len > 0 && cur_len < value_len - 2) {
                  value[cur_len] = ' ';
                  safe_strncpy(value + cur_len + 1, str_val, value_len - cur_len - 1);
               }
            }
            break;
      }
   }

   /* If a value was set but action is still default "get", change to "set"
    * This handles tools like hud_mode where only a value parameter is provided.
    *
    * IMPORTANT: Only apply this conversion if the tool doesn't have an explicit
    * action parameter (maps_to == CMD_MAPS_TO_ACTION). Tools like weather have
    * an enum parameter that maps to action (today/tomorrow/week) - when no time
    * is specified, action stays "get" which weatherCallback interprets as "today".
    * We must NOT override this to "set" just because a location value was given.
    */
   if (value[0] != '\0' && strcmp(action, "get") == 0) {
      bool has_action_param = false;
      for (int i = 0; i < cmd->param_count; i++) {
         if (cmd->parameters[i].maps_to == CMD_MAPS_TO_ACTION) {
            has_action_param = true;
            break;
         }
      }
      /* Only convert to "set" for tools without explicit action parameters */
      if (!has_action_param) {
         strcpy(action, "set");
      }
   }

   return 0;
}

/* =============================================================================
 * Registry-Based Tool Generation
 * ============================================================================= */

/**
 * @brief Generate a tool definition from a command registry entry
 *
 * Called for each command in the registry. Only creates tools for commands
 * that have a description (indicating they have a tool block in config).
 */
static void generate_tool_from_cmd(const cmd_definition_t *cmd, void *user_data) {
   (void)user_data;

   /* Only create tools for commands with descriptions (have tool blocks) */
   if (cmd->description[0] == '\0') {
      return;
   }

   if (s_tool_count >= LLM_TOOLS_MAX_TOOLS) {
      LOG_ERROR("Maximum tool count (%d) reached, skipping '%s'", LLM_TOOLS_MAX_TOOLS, cmd->name);
      return;
   }

   tool_definition_t *t = &s_tools[s_tool_count++];
   memset(t, 0, sizeof(*t));

   safe_strncpy(t->name, cmd->name, LLM_TOOLS_NAME_LEN);
   safe_strncpy(t->description, cmd->description, LLM_TOOLS_DESC_LEN);
   t->device_name = cmd->device_string;
   t->enabled = cmd->enabled;

   /* Copy parameters (types are now unified - cmd_param_type_t used in both) */
   for (int i = 0; i < cmd->param_count && i < LLM_TOOLS_MAX_PARAMS; i++) {
      const cmd_param_t *src = &cmd->parameters[i];
      tool_param_t *dst = &t->parameters[t->param_count++];

      safe_strncpy(dst->name, src->name, LLM_TOOLS_NAME_LEN);
      safe_strncpy(dst->description, src->description, sizeof(dst->description));
      dst->type = src->type; /* Direct assignment - same enum type */
      dst->required = src->required;

      /* Copy enum values */
      for (int j = 0; j < src->enum_count && j < LLM_TOOLS_MAX_ENUM_VALUES; j++) {
         safe_strncpy(dst->enum_values[j], src->enum_values[j], sizeof(dst->enum_values[j]));
         dst->enum_count++;
      }
   }
}

/* =============================================================================
 * Tool Initialization
 * ============================================================================= */

void llm_tools_init(void) {
   if (s_initialized) {
      return;
   }

   s_tool_count = 0;

   /* Auto-generate tools from command registry */
   command_registry_foreach_enabled(generate_tool_from_cmd, NULL);

   s_initialized = true;
   LOG_INFO("Initialized %d LLM tools from registry", s_tool_count);

   /* Refresh availability based on current config */
   llm_tools_refresh();

   /* Log which tools are enabled */
   char enabled_list[512] = "";
   int offset = 0;
   for (int i = 0; i < s_tool_count && offset < 500; i++) {
      if (s_tools[i].enabled) {
         offset += snprintf(enabled_list + offset, 512 - offset, "%s%s", offset > 0 ? ", " : "",
                            s_tools[i].name);
      }
   }
   LOG_INFO("Enabled tools: %s", enabled_list);
}

/* =============================================================================
 * Tool Availability Refresh
 * ============================================================================= */

void llm_tools_refresh(void) {
   if (!s_initialized) {
      return;
   }

   const secrets_config_t *secrets = config_get_secrets();

   for (int i = 0; i < s_tool_count; i++) {
      tool_definition_t *t = &s_tools[i];

      /* Default all tools to enabled */
      t->enabled = true;

      /* SmartThings requires authentication */
      if (strcmp(t->name, "smartthings") == 0) {
         t->enabled = (secrets->smartthings_access_token[0] != '\0' ||
                       secrets->smartthings_client_id[0] != '\0');
      }

      /* Search requires configured endpoint */
      if (strcmp(t->name, "search") == 0) {
         t->enabled = (g_config.search.endpoint[0] != '\0');
      }
   }

   /* Update cached enabled count */
   s_enabled_count = 0;
   for (int i = 0; i < s_tool_count; i++) {
      if (s_tools[i].enabled) {
         s_enabled_count++;
      }
   }

   LOG_INFO("Refreshed tool availability: %d enabled", s_enabled_count);
}

void llm_tools_cleanup(void) {
   s_tool_count = 0;
   s_initialized = false;
}

/* =============================================================================
 * Schema Generation - Helper Functions
 * ============================================================================= */

static const char *param_type_to_json_type(cmd_param_type_t type) {
   switch (type) {
      case CMD_PARAM_STRING:
      case CMD_PARAM_ENUM:
         return "string";
      case CMD_PARAM_INTEGER:
         return "integer";
      case CMD_PARAM_NUMBER:
         return "number";
      case CMD_PARAM_BOOLEAN:
         return "boolean";
      default:
         return "string";
   }
}

/* Build the parameters/input_schema JSON object (shared format) */
static struct json_object *build_parameters_schema(const tool_definition_t *tool) {
   struct json_object *schema = json_object_new_object();
   json_object_object_add(schema, "type", json_object_new_string("object"));

   struct json_object *properties = json_object_new_object();
   struct json_object *required = json_object_new_array();

   for (int i = 0; i < tool->param_count; i++) {
      const tool_param_t *p = &tool->parameters[i];

      struct json_object *prop = json_object_new_object();
      json_object_object_add(prop, "type",
                             json_object_new_string(param_type_to_json_type(p->type)));
      json_object_object_add(prop, "description", json_object_new_string(p->description));

      /* Add enum values if present */
      if (p->type == CMD_PARAM_ENUM && p->enum_count > 0) {
         struct json_object *enum_arr = json_object_new_array();
         for (int j = 0; j < p->enum_count; j++) {
            json_object_array_add(enum_arr, json_object_new_string(p->enum_values[j]));
         }
         json_object_object_add(prop, "enum", enum_arr);
      }

      json_object_object_add(properties, p->name, prop);

      if (p->required) {
         json_object_array_add(required, json_object_new_string(p->name));
      }
   }

   json_object_object_add(schema, "properties", properties);
   json_object_object_add(schema, "required", required);

   return schema;
}

/* =============================================================================
 * Schema Generation - OpenAI Format
 * ============================================================================= */

struct json_object *llm_tools_get_openai_format(void) {
   if (!s_initialized || llm_tools_get_enabled_count() == 0) {
      return NULL;
   }

   struct json_object *tools_array = json_object_new_array();

   for (int i = 0; i < s_tool_count; i++) {
      const tool_definition_t *t = &s_tools[i];
      if (!t->enabled) {
         continue;
      }

      /*
       * OpenAI format:
       * {
       *   "type": "function",
       *   "function": {
       *     "name": "weather",
       *     "description": "...",
       *     "parameters": { ... }
       *   }
       * }
       */
      struct json_object *tool_obj = json_object_new_object();
      json_object_object_add(tool_obj, "type", json_object_new_string("function"));

      struct json_object *function = json_object_new_object();
      json_object_object_add(function, "name", json_object_new_string(t->name));
      json_object_object_add(function, "description", json_object_new_string(t->description));
      json_object_object_add(function, "parameters", build_parameters_schema(t));

      json_object_object_add(tool_obj, "function", function);
      json_object_array_add(tools_array, tool_obj);
   }

   return tools_array;
}

/* =============================================================================
 * Schema Generation - Claude Format
 * ============================================================================= */

struct json_object *llm_tools_get_claude_format(void) {
   if (!s_initialized || llm_tools_get_enabled_count() == 0) {
      return NULL;
   }

   struct json_object *tools_array = json_object_new_array();

   for (int i = 0; i < s_tool_count; i++) {
      const tool_definition_t *t = &s_tools[i];
      if (!t->enabled) {
         continue;
      }

      /*
       * Claude format:
       * {
       *   "name": "weather",
       *   "description": "...",
       *   "input_schema": { ... }
       * }
       */
      struct json_object *tool_obj = json_object_new_object();
      json_object_object_add(tool_obj, "name", json_object_new_string(t->name));
      json_object_object_add(tool_obj, "description", json_object_new_string(t->description));
      json_object_object_add(tool_obj, "input_schema", build_parameters_schema(t));

      json_object_array_add(tools_array, tool_obj);
   }

   return tools_array;
}

/* =============================================================================
 * Tool Execution
 * ============================================================================= */

int llm_tools_execute(const tool_call_t *call, tool_result_t *result) {
   if (!call || !result) {
      return 1;
   }

   memset(result, 0, sizeof(*result));
   safe_strncpy(result->tool_call_id, call->id, LLM_TOOLS_ID_LEN);

   /* Look up command in registry for metadata */
   const cmd_definition_t *cmd = command_registry_lookup(call->name);
   if (!cmd) {
      snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Error: Unknown tool '%s'", call->name);
      result->success = false;
      return 1;
   }

   /* Parse arguments JSON */
   struct json_object *args = json_tokener_parse(call->arguments);
   if (!args && call->arguments[0] != '\0') {
      snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Error: Invalid JSON arguments");
      result->success = false;
      return 1;
   }

   /* Table-driven parameter extraction using registry metadata */
   char device_name[64];
   char action_name[64];
   char value_buf[4096];

   int extract_result = extract_params_from_registry(cmd, args, device_name, sizeof(device_name),
                                                     action_name, sizeof(action_name), value_buf,
                                                     sizeof(value_buf));

   if (args) {
      json_object_put(args);
   }

   if (extract_result != 0) {
      snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Error: Invalid parameters for '%s'",
               call->name);
      result->success = false;
      return 1;
   }

   /* Determine the actual device to use */
   const char *effective_device = device_name[0] != '\0' ? device_name : cmd->device_string;

   LOG_INFO("Executing tool '%s' -> device='%s', action='%s', value='%s'", call->name,
            effective_device, action_name, value_buf);

   /* Special handling for viewing (synchronous MQTT wait for image) */
   if (cmd->sync_wait && strcmp(call->name, "viewing") == 0) {
      result->success = execute_viewing_sync(action_name, value_buf, result);
      /* Notify callback of viewing tool execution */
      notify_tool_execution(call->name, call->arguments, result->result, result->success);
      return result->success ? 0 : 1;
   }

   /* Use unified command executor for all other commands */
   struct mosquitto *mosq = worker_pool_get_mosq();
   cmd_exec_result_t exec_result;

   int rc = command_execute(effective_device, action_name, value_buf, mosq, &exec_result);

   if (rc == 0 && exec_result.success) {
      if (exec_result.result) {
         safe_strncpy(result->result, exec_result.result, LLM_TOOLS_RESULT_LEN);
      } else {
         snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Tool '%s' completed", call->name);
      }
      result->success = true;
      result->skip_followup = cmd->skip_followup || exec_result.skip_followup;
   } else {
      if (exec_result.result) {
         safe_strncpy(result->result, exec_result.result, LLM_TOOLS_RESULT_LEN);
      } else {
         snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Error executing '%s'", call->name);
      }
      result->success = false;
   }

   /* Notify callback of tool execution (for WebUI debug display) */
   notify_tool_execution(call->name, call->arguments, result->result, result->success);

   cmd_exec_result_free(&exec_result);
   return rc;
}

int llm_tools_execute_all(const tool_call_list_t *calls, tool_result_list_t *results) {
   if (!calls || !results) {
      return 1;
   }

   results->count = 0;
   int failures = 0;

   for (int i = 0; i < calls->count && i < LLM_TOOLS_MAX_PARALLEL_CALLS; i++) {
      if (llm_tools_execute(&calls->calls[i], &results->results[results->count]) != 0) {
         failures++;
      }
      results->count++;
   }

   return failures > 0 ? 1 : 0;
}

bool llm_tools_should_skip_followup(const tool_result_list_t *results) {
   if (!results) {
      return false;
   }

   for (int i = 0; i < results->count; i++) {
      if (results->results[i].skip_followup) {
         return true;
      }
   }
   return false;
}

char *llm_tools_get_direct_response(const tool_result_list_t *results) {
   if (!results || results->count == 0) {
      return NULL;
   }

   /* For single result, just return it directly */
   if (results->count == 1) {
      return strdup(results->results[0].result);
   }

   /* For multiple results, concatenate them */
   size_t total_len = 0;
   for (int i = 0; i < results->count; i++) {
      total_len += strlen(results->results[i].result) + 2; /* +2 for newline */
   }

   char *response = malloc(total_len + 1);
   if (!response) {
      return NULL;
   }

   /* Use pointer offset instead of strcat to avoid O(n²) */
   char *ptr = response;
   for (int i = 0; i < results->count; i++) {
      size_t len = strlen(results->results[i].result);
      memcpy(ptr, results->results[i].result, len);
      ptr += len;
      if (i < results->count - 1) {
         *ptr++ = '\n';
      }
   }
   *ptr = '\0';

   return response;
}

/* =============================================================================
 * Tool Result Formatting for Conversation History
 * ============================================================================= */

int llm_tools_add_results_openai(struct json_object *history, const tool_result_list_t *results) {
   if (!history || !results) {
      return 1;
   }

   /*
    * OpenAI format: Add a "tool" role message for each result
    * {
    *   "role": "tool",
    *   "tool_call_id": "call_xxx",
    *   "content": "result text"
    * }
    */
   for (int i = 0; i < results->count; i++) {
      const tool_result_t *r = &results->results[i];

      struct json_object *msg = json_object_new_object();
      json_object_object_add(msg, "role", json_object_new_string("tool"));
      json_object_object_add(msg, "tool_call_id", json_object_new_string(r->tool_call_id));
      json_object_object_add(msg, "content", json_object_new_string(r->result));

      json_object_array_add(history, msg);
   }

   return 0;
}

int llm_tools_add_results_claude(struct json_object *history, const tool_result_list_t *results) {
   if (!history || !results) {
      return 1;
   }

   /*
    * Claude format: Add a single "user" message with tool_result content blocks
    * {
    *   "role": "user",
    *   "content": [
    *     {
    *       "type": "tool_result",
    *       "tool_use_id": "toolu_xxx",
    *       "content": "result text"
    *     }
    *   ]
    * }
    */
   struct json_object *content_array = json_object_new_array();

   for (int i = 0; i < results->count; i++) {
      const tool_result_t *r = &results->results[i];

      struct json_object *block = json_object_new_object();
      json_object_object_add(block, "type", json_object_new_string("tool_result"));
      json_object_object_add(block, "tool_use_id", json_object_new_string(r->tool_call_id));
      json_object_object_add(block, "content", json_object_new_string(r->result));

      json_object_array_add(content_array, block);
   }

   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "role", json_object_new_string("user"));
   json_object_object_add(msg, "content", content_array);

   json_object_array_add(history, msg);

   return 0;
}

/* =============================================================================
 * Response Parsing
 * ============================================================================= */

int llm_tools_parse_openai_response(struct json_object *response, tool_call_list_t *out) {
   if (!response || !out) {
      return -1;
   }

   out->count = 0;

   /*
    * OpenAI response structure:
    * {
    *   "choices": [{
    *     "message": {
    *       "tool_calls": [{
    *         "id": "call_xxx",
    *         "function": {
    *           "name": "weather",
    *           "arguments": "{...}"
    *         }
    *       }]
    *     },
    *     "finish_reason": "tool_calls"
    *   }]
    * }
    */
   struct json_object *choices;
   if (!json_object_object_get_ex(response, "choices", &choices)) {
      return 1; /* No tool calls */
   }

   if (json_object_array_length(choices) == 0) {
      return 1;
   }

   struct json_object *first_choice = json_object_array_get_idx(choices, 0);
   struct json_object *message;
   if (!json_object_object_get_ex(first_choice, "message", &message)) {
      return 1;
   }

   struct json_object *tool_calls;
   if (!json_object_object_get_ex(message, "tool_calls", &tool_calls)) {
      return 1; /* No tool calls */
   }

   int len = json_object_array_length(tool_calls);
   for (int i = 0; i < len && out->count < LLM_TOOLS_MAX_PARALLEL_CALLS; i++) {
      struct json_object *tc = json_object_array_get_idx(tool_calls, i);
      struct json_object *id_obj, *function_obj;

      if (!json_object_object_get_ex(tc, "id", &id_obj) ||
          !json_object_object_get_ex(tc, "function", &function_obj)) {
         continue;
      }

      struct json_object *name_obj, *args_obj;
      if (!json_object_object_get_ex(function_obj, "name", &name_obj) ||
          !json_object_object_get_ex(function_obj, "arguments", &args_obj)) {
         continue;
      }

      tool_call_t *call = &out->calls[out->count++];
      safe_strncpy(call->id, json_object_get_string(id_obj), LLM_TOOLS_ID_LEN);
      safe_strncpy(call->name, json_object_get_string(name_obj), LLM_TOOLS_NAME_LEN);
      safe_strncpy(call->arguments, json_object_get_string(args_obj), LLM_TOOLS_ARGS_LEN);
   }

   return out->count > 0 ? 0 : 1;
}

int llm_tools_parse_claude_response(struct json_object *response, tool_call_list_t *out) {
   if (!response || !out) {
      return -1;
   }

   out->count = 0;

   /*
    * Claude response structure:
    * {
    *   "content": [
    *     {
    *       "type": "tool_use",
    *       "id": "toolu_xxx",
    *       "name": "weather",
    *       "input": { ... }
    *     }
    *   ],
    *   "stop_reason": "tool_use"
    * }
    */
   struct json_object *content;
   if (!json_object_object_get_ex(response, "content", &content)) {
      return 1;
   }

   int len = json_object_array_length(content);
   for (int i = 0; i < len && out->count < LLM_TOOLS_MAX_PARALLEL_CALLS; i++) {
      struct json_object *block = json_object_array_get_idx(content, i);
      struct json_object *type_obj;

      if (!json_object_object_get_ex(block, "type", &type_obj)) {
         continue;
      }

      if (strcmp(json_object_get_string(type_obj), "tool_use") != 0) {
         continue;
      }

      struct json_object *id_obj, *name_obj, *input_obj;
      if (!json_object_object_get_ex(block, "id", &id_obj) ||
          !json_object_object_get_ex(block, "name", &name_obj) ||
          !json_object_object_get_ex(block, "input", &input_obj)) {
         continue;
      }

      tool_call_t *call = &out->calls[out->count++];
      safe_strncpy(call->id, json_object_get_string(id_obj), LLM_TOOLS_ID_LEN);
      safe_strncpy(call->name, json_object_get_string(name_obj), LLM_TOOLS_NAME_LEN);

      /* Claude sends input as object, we need it as string */
      const char *input_str = json_object_to_json_string(input_obj);
      safe_strncpy(call->arguments, input_str, LLM_TOOLS_ARGS_LEN);
   }

   return out->count > 0 ? 0 : 1;
}

/* =============================================================================
 * Capability Checking
 * ============================================================================= */

bool llm_tools_enabled(const llm_resolved_config_t *config) {
   /* Check config option - default is false for backward compatibility */
   if (!g_config.llm.tools.native_enabled) {
      return false;
   }

   /* Check that tools system is initialized with enabled tools */
   if (!s_initialized || llm_tools_get_enabled_count() == 0) {
      return false;
   }

   /* If we have a specific config, check provider support */
   if (config) {
      /* All supported providers work with tool calling:
       * - OpenAI: Native function calling
       * - Claude: Native tool_use
       * - Local: llama.cpp with --jinja flag (Qwen, etc.) */
      return true;
   }

   /* No config provided - this happens during prompt building.
    * If native_enabled is true and tools are initialized, we should use
    * the minimal prompt. The actual LLM type check happens at call time. */
   llm_type_t type = llm_get_type();
   if (type == LLM_LOCAL || type == LLM_CLOUD) {
      return true;
   }

   /* LLM type not yet set (LLM_NONE during early init) - but config says
    * native tools are enabled and tools are initialized, so return true
    * to build the minimal prompt. Runtime calls will have proper type. */
   return true;
}

int llm_tools_get_enabled_count(void) {
   if (!s_initialized) {
      return 0;
   }
   return s_enabled_count;
}

void llm_tool_response_free(llm_tool_response_t *response) {
   if (response) {
      if (response->text) {
         free(response->text);
         response->text = NULL;
      }
   }
}

/* =============================================================================
 * Common Tool Execution Helper
 * ============================================================================= */

void llm_tools_prepare_followup(const tool_result_list_t *results, tool_followup_context_t *ctx) {
   if (!ctx) {
      return;
   }

   /* Initialize context */
   memset(ctx, 0, sizeof(*ctx));

   /* Check if we should skip follow-up (e.g., LLM was switched) */
   ctx->skip_followup = llm_tools_should_skip_followup(results);

   if (ctx->skip_followup) {
      /* Get direct response for TTS output */
      ctx->direct_response = llm_tools_get_direct_response(results);
   }

   /* Check for vision data in tool results (session-isolated) */
   if (results) {
      for (int i = 0; i < results->count; i++) {
         const tool_result_t *r = &results->results[i];
         if (r->vision_image && r->vision_image_size > 0) {
            ctx->has_pending_vision = true;
            ctx->pending_vision = r->vision_image;
            ctx->pending_vision_size = r->vision_image_size;
            break; /* Only one vision image per call */
         }
      }
   }
}

/* =============================================================================
 * Pending Vision Data Functions
 * ============================================================================= */

bool llm_tools_has_pending_vision(void) {
   pthread_mutex_lock(&s_pending_vision_mutex);
   bool result = s_pending_vision_image != NULL && s_pending_vision_size > 0;
   pthread_mutex_unlock(&s_pending_vision_mutex);
   return result;
}

const char *llm_tools_get_pending_vision(size_t *size_out) {
   pthread_mutex_lock(&s_pending_vision_mutex);
   if (size_out) {
      *size_out = s_pending_vision_size;
   }
   const char *result = s_pending_vision_image;
   pthread_mutex_unlock(&s_pending_vision_mutex);
   return result;
}

void llm_tools_clear_pending_vision(void) {
   pthread_mutex_lock(&s_pending_vision_mutex);
   if (s_pending_vision_image) {
      free(s_pending_vision_image);
      s_pending_vision_image = NULL;
   }
   s_pending_vision_size = 0;
   pthread_mutex_unlock(&s_pending_vision_mutex);
}

bool llm_tools_set_pending_vision(const char *base64_image, size_t size) {
   if (!base64_image || size == 0) {
      return false;
   }

   char *copy = malloc(size);
   if (!copy) {
      LOG_ERROR("llm_tools_set_pending_vision: malloc failed");
      return false;
   }
   memcpy(copy, base64_image, size);

   pthread_mutex_lock(&s_pending_vision_mutex);
   if (s_pending_vision_image) {
      free(s_pending_vision_image);
   }
   s_pending_vision_image = copy;
   s_pending_vision_size = size;
   pthread_mutex_unlock(&s_pending_vision_mutex);

   LOG_INFO("llm_tools_set_pending_vision: stored %zu bytes", size);
   return true;
}

bool llm_tools_process_vision_data(const char *data, char *error_buf, size_t error_len) {
   return process_vision_data(data, error_buf, error_len);
}

/**
 * @brief Atomically set pending vision data (internal, takes ownership)
 *
 * Clears any existing pending vision and sets new data.
 * Takes ownership of the image pointer (caller should not free).
 *
 * @param image Base64-encoded image data (takes ownership)
 * @param size Size of image data
 */
static void set_pending_vision_locked(char *image, size_t size) {
   pthread_mutex_lock(&s_pending_vision_mutex);
   if (s_pending_vision_image) {
      free(s_pending_vision_image);
   }
   s_pending_vision_image = image;
   s_pending_vision_size = size;
   pthread_mutex_unlock(&s_pending_vision_mutex);
}
