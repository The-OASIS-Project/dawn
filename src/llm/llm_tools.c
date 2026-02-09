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
#include <time.h>

#include "config/dawn_config.h"
#include "core/command_executor.h"
#include "core/component_status.h"
#include "core/session_manager.h"
#include "core/worker_pool.h"
#include "dawn.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_context.h"
#include "llm/llm_interface.h"
#include "logging.h"
#include "mosquitto_comms.h"
#include "tools/hud_discovery.h"
#include "tools/string_utils.h"
#include "tools/tool_registry.h"
#include "webui/webui_server.h"

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

/* Thread safety for tool state modifications */
static pthread_mutex_t s_tools_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Cached token estimates (-1 = needs recalculation) */
static int s_token_estimate_local = -1;
static int s_token_estimate_remote = -1;

/* Thread-local suppression counter for temporarily disabling tools.
 * Used by subsystems like the search summarizer that need to make
 * LLM calls without tools being added to the request. */
static __thread int tl_suppress_count = 0;

/* Thread-local pointer to current resolved config.
 * Set by llm_tools_set_current_config() before LLM calls so that
 * llm_tools_enabled() can check session-specific tool_mode. */
static __thread const llm_resolved_config_t *tl_current_config = NULL;

/* =============================================================================
 * Tool Execution Notification Callback
 * ============================================================================= */

/**
 * @brief Callback for tool execution notifications
 *
 * @note THREAD SAFETY: This callback pointer is accessed atomically to support
 * safe reads during parallel tool execution. It should be set during application
 * initialization before tools are invoked, but atomic access prevents undefined
 * behavior if the timing assumption is violated.
 */
static tool_execution_callback_fn s_execution_callback = NULL;

void llm_tools_set_execution_callback(tool_execution_callback_fn callback) {
   __atomic_store_n(&s_execution_callback, callback, __ATOMIC_RELEASE);
}

/**
 * @brief Notify registered callback about tool execution
 */
static void notify_tool_execution(const char *tool_name,
                                  const char *tool_args,
                                  const char *result,
                                  bool success) {
   tool_execution_callback_fn cb = __atomic_load_n(&s_execution_callback, __ATOMIC_ACQUIRE);
   if (cb) {
      /* Get current command context (session) for callback */
      session_t *session = session_get_command_context();
      cb((void *)session, tool_name, tool_args, result, success);
   }
}

/* =============================================================================
 * Parallel Tool Execution Support
 * ============================================================================= */

/**
 * @brief Thread argument structure for parallel tool execution
 */
typedef struct {
   const tool_call_t *call;
   tool_result_t *result;
   session_t *session; /* Session context to propagate to spawned thread */
   int return_code;
} tool_exec_task_t;

/**
 * @brief List of tools that modify global state and must run sequentially
 *
 * All other tools are considered parallel-safe (HTTP calls, getters, etc.)
 */
static const char *SEQUENTIAL_TOOLS[] = {
   "switch_llm",         /* Modifies global LLM configuration */
   "reset_conversation", /* Modifies session conversation state */
   "music",              /* Controls audio playback state */
   "volume",             /* Modifies system volume */
   "local_llm_switch",   /* Modifies LLM routing */
   "cloud_llm_switch",   /* Modifies LLM routing */
   "cloud_provider",     /* Modifies LLM provider selection */
   "viewing",            /* Uses shared MQTT state for image capture */
   "shutdown",           /* Critical system operation */
   NULL                  /* Sentinel */
};

/**
 * @brief Check if a tool name is in the sequential tools list
 *
 * Used during tool initialization to set the parallel_safe flag.
 * Tools that modify global state (LLM config, audio, conversation) must
 * run sequentially. All others (HTTP calls, getters) are parallel-safe.
 *
 * @param tool_name Name of the tool to check
 * @return true if parallel-safe, false if must run sequentially
 */
static bool is_tool_parallel_safe(const char *tool_name) {
   for (int i = 0; SEQUENTIAL_TOOLS[i] != NULL; i++) {
      if (strcmp(tool_name, SEQUENTIAL_TOOLS[i]) == 0) {
         return false;
      }
   }
   return true;
}

/**
 * @brief Look up a tool's parallel_safe flag by name
 *
 * Uses pre-computed parallel_safe flag from tool definition, avoiding
 * repeated string comparisons during execution.
 *
 * @param tool_name Name of the tool to look up
 * @return true if parallel-safe, false if sequential (or unknown tool)
 */
static bool get_tool_parallel_safe(const char *tool_name) {
   for (int i = 0; i < s_tool_count; i++) {
      if (strcmp(s_tools[i].name, tool_name) == 0) {
         return s_tools[i].parallel_safe;
      }
   }
   /* Unknown tool - assume sequential for safety */
   return false;
}

/**
 * @brief Thread wrapper for parallel tool execution
 *
 * Propagates session context to the spawned thread so that tool callbacks
 * can access the correct session via session_get_command_context().
 *
 * @param arg Pointer to tool_exec_task_t
 * @return NULL (result stored in task structure)
 */
static void *tool_exec_thread(void *arg) {
   tool_exec_task_t *task = (tool_exec_task_t *)arg;

   LOG_INFO("Thread started for tool '%s'", task->call->name);

   /* Propagate session context to this thread */
   session_set_command_context(task->session);

   task->return_code = llm_tools_execute(task->call, task->result);

   /* Clear context before thread exit */
   session_set_command_context(NULL);

   return NULL;
}

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

   const tool_metadata_t *viewing_tool = tool_registry_find("viewing");
   if (!viewing_tool || !viewing_tool->topic) {
      snprintf(tool_result->result, LLM_TOOLS_RESULT_LEN, "Error: viewing tool not registered");
      return false;
   }
   char topic[64];
   safe_strncpy(topic, viewing_tool->topic, sizeof(topic));

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
 * Tool Registry-Based Tool Generation
 * ============================================================================= */

/**
 * @brief Check if a tool with given name already exists
 */
static bool tool_exists(const char *name) {
   for (int i = 0; i < s_tool_count; i++) {
      if (strcmp(s_tools[i].name, name) == 0) {
         return true;
      }
   }
   return false;
}

/**
 * @brief Generate a tool definition from a tool_registry entry
 *
 * Called for each tool registered with tool_registry. Creates LLM tool
 * definitions from the C struct metadata (replacing JSON-based definitions).
 */
static void generate_tool_from_treg(const tool_metadata_t *meta, void *user_data) {
   (void)user_data;

   /* Skip if no description (not an LLM-visible tool) */
   if (!meta->description || meta->description[0] == '\0') {
      return;
   }

   /* Skip if already registered (prevents duplicates during transition) */
   if (tool_exists(meta->name)) {
      return;
   }

   if (s_tool_count >= LLM_TOOLS_MAX_TOOLS) {
      LOG_ERROR("Maximum tool count (%d) reached, skipping '%s'", LLM_TOOLS_MAX_TOOLS, meta->name);
      return;
   }

   tool_definition_t *t = &s_tools[s_tool_count++];
   memset(t, 0, sizeof(*t));

   safe_strncpy(t->name, meta->name, LLM_TOOLS_NAME_LEN);
   safe_strncpy(t->description, meta->description, LLM_TOOLS_DESC_LEN);
   t->device_name = meta->device_string;
   t->enabled = true;       /* Will be updated by llm_tools_refresh() */
   t->enabled_local = true; /* Default to enabled for local */
   t->enabled_remote = meta->default_remote;
   t->armor_feature = (meta->capabilities & TOOL_CAP_ARMOR_FEATURE) != 0;
   t->parallel_safe = is_tool_parallel_safe(meta->name);

   /* Copy parameters */
   for (int i = 0; i < meta->param_count && i < LLM_TOOLS_MAX_PARAMS; i++) {
      const treg_param_t *src = &meta->params[i];
      tool_param_t *dst = &t->parameters[t->param_count++];

      safe_strncpy(dst->name, src->name, LLM_TOOLS_NAME_LEN);
      safe_strncpy(dst->description, src->description ? src->description : "",
                   sizeof(dst->description));
      dst->type = src->type;
      dst->required = src->required;

      /* Copy enum values */
      for (int j = 0; j < src->enum_count && j < LLM_TOOLS_MAX_ENUM_VALUES; j++) {
         if (src->enum_values[j]) {
            safe_strncpy(dst->enum_values[j], src->enum_values[j], sizeof(dst->enum_values[j]));
            dst->enum_count++;
         }
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

   /* Generate tools from tool_registry (compile-time C struct metadata) */
   tool_registry_foreach_enabled(generate_tool_from_treg, NULL);

   s_initialized = true;
   LOG_INFO("Initialized %d LLM tools from tool_registry", s_tool_count);

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

   /* Check if HUD/helmet hardware is available via status keepalive */
   bool hud_available = component_status_is_hud_online();

   for (int i = 0; i < s_tool_count; i++) {
      tool_definition_t *t = &s_tools[i];

      /* Default: capability enabled (local/remote defaults set during init from JSON) */
      t->enabled = true;

      /* Armor/HUD tools require successful discovery (helmet must be connected) */
      if (t->armor_feature) {
         t->enabled = hud_available;
      }

      /* SmartThings requires authentication */
      if (strcmp(t->name, "smartthings") == 0) {
         t->enabled = (secrets->smartthings_access_token[0] != '\0' ||
                       secrets->smartthings_client_id[0] != '\0');
      }

      /* Search requires configured endpoint */
      if (strcmp(t->name, "search") == 0) {
         t->enabled = (g_config.search.endpoint[0] != '\0');
      }

      /* Memory requires memory system to be enabled */
      if (strcmp(t->name, "memory") == 0) {
         t->enabled = g_config.memory.enabled;
      }
   }

   /* Update cached enabled count (total capability-enabled) */
   s_enabled_count = 0;
   for (int i = 0; i < s_tool_count; i++) {
      if (s_tools[i].enabled) {
         s_enabled_count++;
      }
   }

   LOG_INFO("Refreshed tool availability: %d enabled (HUD %s)", s_enabled_count,
            hud_available ? "available" : "unavailable");
}

void llm_tools_cleanup(void) {
   s_tool_count = 0;
   s_initialized = false;
}

/* =============================================================================
 * Schema Generation - Helper Functions
 * ============================================================================= */

static const char *param_type_to_json_type(tool_param_type_t type) {
   switch (type) {
      case TOOL_PARAM_TYPE_STRING:
      case TOOL_PARAM_TYPE_ENUM:
         return "string";
      case TOOL_PARAM_TYPE_INT:
         return "integer";
      case TOOL_PARAM_TYPE_NUMBER:
         return "number";
      case TOOL_PARAM_TYPE_BOOL:
         return "boolean";
      default:
         return "string";
   }
}

/**
 * @brief Build parameters schema from tool_registry with live enum values
 *
 * For tools from tool_registry, this queries the registry at schema generation
 * time to get current enum values (which may have been updated by discovery).
 */
static struct json_object *build_parameters_schema_from_treg(const char *tool_name,
                                                             int param_count) {
   struct json_object *schema = json_object_new_object();
   json_object_object_add(schema, "type", json_object_new_string("object"));

   struct json_object *properties = json_object_new_object();
   struct json_object *required = json_object_new_array();

   for (int i = 0; i < param_count; i++) {
      /* Query tool_registry for effective parameter (includes discovery overrides) */
      const treg_param_t *p = tool_registry_get_effective_param(tool_name, i);
      if (!p) {
         continue;
      }

      struct json_object *prop = json_object_new_object();
      json_object_object_add(prop, "type",
                             json_object_new_string(
                                 param_type_to_json_type((tool_param_type_t)p->type)));
      json_object_object_add(prop, "description",
                             json_object_new_string(p->description ? p->description : ""));

      /* Add enum values if present */
      if (p->type == TOOL_PARAM_TYPE_ENUM && p->enum_count > 0) {
         struct json_object *enum_arr = json_object_new_array();
         for (int j = 0; j < p->enum_count; j++) {
            if (p->enum_values[j]) {
               json_object_array_add(enum_arr, json_object_new_string(p->enum_values[j]));
            }
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

/**
 * @brief Build parameters schema from cached tool_definition_t
 *
 * Fallback for tools not found in tool_registry (shouldn't happen in normal operation).
 */
static struct json_object *build_parameters_schema_from_cached(const tool_definition_t *tool) {
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
      if (p->type == TOOL_PARAM_TYPE_ENUM && p->enum_count > 0) {
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

/**
 * @brief Build the parameters/input_schema JSON object
 *
 * Uses tool_registry for dynamic enum updates when available.
 */
static struct json_object *build_parameters_schema(const tool_definition_t *tool) {
   /* Check if this tool exists in tool_registry (supports dynamic params) */
   if (tool_registry_find(tool->name) != NULL) {
      return build_parameters_schema_from_treg(tool->name, tool->param_count);
   }

   /* Fall back to cached parameters */
   return build_parameters_schema_from_cached(tool);
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
 * Schema Generation - Filtered by Session Type
 * ============================================================================= */

/**
 * @brief Check if a tool is enabled for a given session type
 */
static bool is_tool_enabled_for_session(const tool_definition_t *t, bool is_remote) {
   if (!t->enabled) {
      return false; /* Capability not available */
   }
   return is_remote ? t->enabled_remote : t->enabled_local;
}

struct json_object *llm_tools_get_openai_format_filtered(bool is_remote_session) {
   if (!s_initialized || s_tool_count == 0) {
      return NULL;
   }

   /* Single pass: build array and count simultaneously */
   struct json_object *tools_array = json_object_new_array();
   int added = 0;

   for (int i = 0; i < s_tool_count; i++) {
      const tool_definition_t *t = &s_tools[i];
      if (!is_tool_enabled_for_session(t, is_remote_session)) {
         continue;
      }

      struct json_object *tool_obj = json_object_new_object();
      json_object_object_add(tool_obj, "type", json_object_new_string("function"));

      struct json_object *function = json_object_new_object();
      json_object_object_add(function, "name", json_object_new_string(t->name));
      json_object_object_add(function, "description", json_object_new_string(t->description));
      json_object_object_add(function, "parameters", build_parameters_schema(t));

      json_object_object_add(tool_obj, "function", function);
      json_object_array_add(tools_array, tool_obj);
      added++;
   }

   if (added == 0) {
      json_object_put(tools_array);
      return NULL;
   }

   return tools_array;
}

struct json_object *llm_tools_get_claude_format_filtered(bool is_remote_session) {
   if (!s_initialized || s_tool_count == 0) {
      return NULL;
   }

   /* Single pass: build array and count simultaneously */
   struct json_object *tools_array = json_object_new_array();
   int added = 0;

   for (int i = 0; i < s_tool_count; i++) {
      const tool_definition_t *t = &s_tools[i];
      if (!is_tool_enabled_for_session(t, is_remote_session)) {
         continue;
      }

      struct json_object *tool_obj = json_object_new_object();
      json_object_object_add(tool_obj, "name", json_object_new_string(t->name));
      json_object_object_add(tool_obj, "description", json_object_new_string(t->description));
      json_object_object_add(tool_obj, "input_schema", build_parameters_schema(t));

      json_object_array_add(tools_array, tool_obj);
      added++;
   }

   if (added == 0) {
      json_object_put(tools_array);
      return NULL;
   }

   return tools_array;
}

/* =============================================================================
 * Tool Configuration API
 * ============================================================================= */

int llm_tools_get_all(tool_info_t *out, int max_tools) {
   if (!out || max_tools <= 0 || !s_initialized) {
      return 0;
   }

   int count = 0;
   for (int i = 0; i < s_tool_count && count < max_tools; i++) {
      const tool_definition_t *t = &s_tools[i];
      tool_info_t *info = &out[count++];

      safe_strncpy(info->name, t->name, LLM_TOOLS_NAME_LEN);
      safe_strncpy(info->description, t->description, LLM_TOOLS_DESC_LEN);
      info->enabled = t->enabled;
      info->enabled_local = t->enabled_local;
      info->enabled_remote = t->enabled_remote;
      info->armor_feature = t->armor_feature;
   }

   return count;
}

int llm_tools_set_enabled(const char *tool_name, bool enabled_local, bool enabled_remote) {
   if (!tool_name || !s_initialized) {
      return 1; /* FAILURE - invalid args or not initialized */
   }

   pthread_mutex_lock(&s_tools_mutex);
   for (int i = 0; i < s_tool_count; i++) {
      if (strcmp(s_tools[i].name, tool_name) == 0) {
         s_tools[i].enabled_local = enabled_local;
         s_tools[i].enabled_remote = enabled_remote;

         /* Invalidate token estimate cache */
         s_token_estimate_local = -1;
         s_token_estimate_remote = -1;

         pthread_mutex_unlock(&s_tools_mutex);
         LOG_INFO("Tool '%s' enable state updated: local=%d, remote=%d", tool_name, enabled_local,
                  enabled_remote);
         return 0; /* SUCCESS */
      }
   }
   pthread_mutex_unlock(&s_tools_mutex);

   LOG_WARNING("Tool '%s' not found", tool_name);
   return 1; /* FAILURE - tool not found */
}

bool llm_tools_is_device_enabled(const char *device_name, bool is_remote) {
   if (!device_name || !s_initialized) {
      return false;
   }

   pthread_mutex_lock(&s_tools_mutex);

   /* Search tools by name - tools use device_string for device name mapping */
   for (int i = 0; i < s_tool_count; i++) {
      /* Check both the tool name and the device_string (underlying device) */
      if (strcmp(s_tools[i].name, device_name) == 0 ||
          (s_tools[i].device_name && strcmp(s_tools[i].device_name, device_name) == 0)) {
         /* Check if the tool is enabled at all */
         if (!s_tools[i].enabled) {
            pthread_mutex_unlock(&s_tools_mutex);
            return false;
         }

         /* Check session-specific enable state */
         bool enabled = is_remote ? s_tools[i].enabled_remote : s_tools[i].enabled_local;
         pthread_mutex_unlock(&s_tools_mutex);
         return enabled;
      }
   }

   pthread_mutex_unlock(&s_tools_mutex);

   /* Device not found in tools array. Only devices with tool blocks in
    * commands_config_nuevo.json become tools and should appear in prompts.
    * Plain MQTT devices (like armor_display without a tool block) are NOT
    * controllable via LLM and should return false for prompt filtering. */
   return false;
}

void llm_tools_apply_config(const char **local_list,
                            int local_count,
                            bool local_configured,
                            const char **remote_list,
                            int remote_count,
                            bool remote_configured) {
   if (!s_initialized) {
      LOG_WARNING("llm_tools_apply_config called before initialization - config ignored");
      return;
   }

   /*
    * WHITELIST SEMANTIC:
    * - If not configured: enable ALL tools (default behavior)
    * - If configured but empty: enable NONE (user explicitly disabled all)
    * - If configured with items: enable ONLY listed tools
    */
   bool enable_all_local = !local_configured;
   bool enable_all_remote = !remote_configured;

   /* Build lookup sets for O(n+m) instead of O(n*m) */
   bool local_set[LLM_TOOLS_MAX_TOOLS] = { 0 };
   bool remote_set[LLM_TOOLS_MAX_TOOLS] = { 0 };

   if (!enable_all_local) {
      for (int j = 0; j < local_count; j++) {
         for (int i = 0; i < s_tool_count; i++) {
            if (strcmp(s_tools[i].name, local_list[j]) == 0) {
               local_set[i] = true;
               break;
            }
         }
      }
   }

   if (!enable_all_remote) {
      for (int j = 0; j < remote_count; j++) {
         for (int i = 0; i < s_tool_count; i++) {
            if (strcmp(s_tools[i].name, remote_list[j]) == 0) {
               remote_set[i] = true;
               break;
            }
         }
      }
   }

   /* Single pass to apply with mutex protection */
   pthread_mutex_lock(&s_tools_mutex);
   for (int i = 0; i < s_tool_count; i++) {
      s_tools[i].enabled_local = enable_all_local || local_set[i];
      s_tools[i].enabled_remote = enable_all_remote || remote_set[i];
   }

   /* Invalidate token estimate cache */
   s_token_estimate_local = -1;
   s_token_estimate_remote = -1;
   pthread_mutex_unlock(&s_tools_mutex);

   LOG_INFO("Applied tool config: local=%d tools, remote=%d tools",
            llm_tools_get_enabled_count_filtered(false),
            llm_tools_get_enabled_count_filtered(true));
}

int llm_tools_get_enabled_count_filtered(bool is_remote_session) {
   if (!s_initialized) {
      return 0;
   }

   int count = 0;
   for (int i = 0; i < s_tool_count; i++) {
      if (is_tool_enabled_for_session(&s_tools[i], is_remote_session)) {
         count++;
      }
   }
   return count;
}

int llm_tools_estimate_tokens(bool is_remote_session) {
   /* Use cached value if available */
   int *cache = is_remote_session ? &s_token_estimate_remote : &s_token_estimate_local;
   if (*cache >= 0) {
      return *cache;
   }

   /* Compute and cache */
   struct json_object *tools = llm_tools_get_openai_format_filtered(is_remote_session);
   if (!tools) {
      *cache = 0;
      return 0;
   }

   const char *json_str = json_object_to_json_string(tools);
   *cache = strlen(json_str) / 4; /* Rough estimate: ~4 chars per token */

   json_object_put(tools);
   return *cache;
}

void llm_tools_invalidate_cache(void) {
   pthread_mutex_lock(&s_tools_mutex);
   s_token_estimate_local = -1;
   s_token_estimate_remote = -1;
   pthread_mutex_unlock(&s_tools_mutex);
   LOG_INFO("LLM tools schema cache invalidated");
}

/* =============================================================================
 * Tool Execution
 * ============================================================================= */

/**
 * @brief Execute a tool from tool_registry (new modular system)
 *
 * Extracts parameters from JSON arguments according to the tool's metadata,
 * then calls the tool's callback directly.
 */
static int llm_tools_execute_from_treg(const tool_call_t *call,
                                       const tool_metadata_t *meta,
                                       tool_result_t *result) {
   /* Parse arguments JSON */
   struct json_object *args = NULL;
   if (call->arguments[0] != '\0') {
      args = json_tokener_parse(call->arguments);
      if (!args) {
         snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Error: Invalid JSON arguments");
         result->success = false;
         return 1;
      }
   }

   /* Extract parameters based on tool metadata */
   char action_name[LLM_TOOLS_NAME_LEN] = "";
   char value_buf[LLM_TOOLS_ARGS_LEN] = "";
   char device_name[LLM_TOOLS_NAME_LEN] = "";

   for (int i = 0; i < meta->param_count; i++) {
      const treg_param_t *param = &meta->params[i];
      struct json_object *val_obj = NULL;

      if (args) {
         json_object_object_get_ex(args, param->name, &val_obj);
      }

      const char *val_str = val_obj ? json_object_get_string(val_obj) : NULL;

      switch (param->maps_to) {
         case TOOL_MAPS_TO_ACTION:
            if (val_str) {
               safe_strncpy(action_name, val_str, sizeof(action_name));
            }
            break;
         case TOOL_MAPS_TO_VALUE:
            if (val_str) {
               safe_strncpy(value_buf, val_str, sizeof(value_buf));
            }
            break;
         case TOOL_MAPS_TO_DEVICE:
            if (val_str) {
               safe_strncpy(device_name, val_str, sizeof(device_name));
            }
            break;
         case TOOL_MAPS_TO_CUSTOM:
            /* Custom fields appended to value with ::field_name::value format */
            if (val_str) {
               if (param->field_name && param->field_name[0]) {
                  /* Append with field name for parsing in callback */
                  size_t cur_len = strlen(value_buf);
                  size_t remaining = sizeof(value_buf) - cur_len;
                  snprintf(value_buf + cur_len, remaining, "::%s::%s", param->field_name, val_str);
               } else if (value_buf[0] == '\0') {
                  /* Fallback: store directly if value is empty */
                  safe_strncpy(value_buf, val_str, sizeof(value_buf));
               }
            }
            break;
      }
   }

   if (args) {
      json_object_put(args);
   }

   /* Resolve device mapping for meta-tools */
   const char *effective_device = meta->device_string;
   if (device_name[0] != '\0') {
      if (meta->device_map && meta->device_map_count > 0) {
         /* Use device_map to resolve the device name */
         const char *mapped = tool_registry_resolve_device(meta, device_name);
         if (mapped) {
            effective_device = mapped;
         }
      } else {
         /* No device_map - use device_name directly */
         effective_device = device_name;
      }
   }

   LOG_INFO("Executing tool '%s' (treg) -> device='%s', action='%s', value='%s'", call->name,
            effective_device, action_name, value_buf);

   /* Notify callback that tool execution is starting */
   notify_tool_execution(call->name, call->arguments, NULL, false);

   /* Special handling for sync_wait tools (e.g., viewing) */
   if (meta->sync_wait && strcmp(meta->name, "viewing") == 0) {
      result->success = execute_viewing_sync(action_name, value_buf, result);
      notify_tool_execution(call->name, call->arguments, result->result, result->success);
      return result->success ? 0 : 1;
   }

   /* MQTT-only tools must go through command_execute for proper MQTT publishing */
   if (meta->mqtt_only) {
      struct mosquitto *mosq = worker_pool_get_mosq();
      cmd_exec_result_t exec_result;

      /* Default action for ANALOG mqtt_only tools without explicit action */
      if (action_name[0] == '\0' && meta->device_type == TOOL_DEVICE_TYPE_ANALOG) {
         safe_strncpy(action_name, "set", sizeof(action_name));
      }

      int rc = command_execute(effective_device, action_name, value_buf, mosq, &exec_result);

      if (rc == 0 && exec_result.success) {
         if (exec_result.result) {
            safe_strncpy(result->result, exec_result.result, LLM_TOOLS_RESULT_LEN);
         } else {
            snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Command sent to %s", effective_device);
         }
         result->success = true;
      } else {
         if (exec_result.result) {
            safe_strncpy(result->result, exec_result.result, LLM_TOOLS_RESULT_LEN);
         } else {
            snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Error executing '%s'", call->name);
         }
         result->success = false;
      }

      result->skip_followup = meta->skip_followup || exec_result.skip_followup;
      notify_tool_execution(call->name, call->arguments, result->result, result->success);
      cmd_exec_result_free(&exec_result);
      return result->success ? 0 : 1;
   }

   /* Call the tool's callback directly */
   if (meta->callback) {
      int should_respond = 0;
      char *cb_result = meta->callback(action_name[0] ? action_name : "get",
                                       value_buf[0] ? value_buf : NULL, &should_respond);

      if (cb_result) {
         safe_strncpy(result->result, cb_result, LLM_TOOLS_RESULT_LEN);
         free(cb_result);
      } else {
         snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Tool '%s' completed", call->name);
      }
      result->success = true;
      result->skip_followup = meta->skip_followup;

      notify_tool_execution(call->name, call->arguments, result->result, result->success);
      return 0;
   }

   /* No callback - fallback to command_execute */
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
      result->skip_followup = meta->skip_followup || exec_result.skip_followup;
   } else {
      if (exec_result.result) {
         safe_strncpy(result->result, exec_result.result, LLM_TOOLS_RESULT_LEN);
      } else {
         snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Error executing '%s'", call->name);
      }
      result->success = false;
   }

   notify_tool_execution(call->name, call->arguments, result->result, result->success);
   cmd_exec_result_free(&exec_result);
   return result->success ? 0 : 1;
}

int llm_tools_execute(const tool_call_t *call, tool_result_t *result) {
   if (!call || !result) {
      return 1;
   }

   memset(result, 0, sizeof(*result));
   safe_strncpy(result->tool_call_id, call->id, LLM_TOOLS_ID_LEN);

   /* Look up tool in tool_registry */
   const tool_metadata_t *treg_meta = tool_registry_find(call->name);
   if (!treg_meta) {
      snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Error: Unknown tool '%s'", call->name);
      result->success = false;
      return 1;
   }

   return llm_tools_execute_from_treg(call, treg_meta, result);
}

int llm_tools_execute_all(const tool_call_list_t *calls, tool_result_list_t *results) {
   if (!calls || !results) {
      return 1;
   }

   results->count = 0;
   int failures = 0;
   int total_calls = (calls->count < LLM_TOOLS_MAX_PARALLEL_CALLS) ? calls->count
                                                                   : LLM_TOOLS_MAX_PARALLEL_CALLS;

   if (total_calls == 0) {
      return 0;
   }

   /* Send tool call state to connected clients (WebUI + satellites) */
   session_t *status_session = session_get_command_context();
   if (status_session) {
      for (int i = 0; i < total_calls; i++) {
         char tool_detail[128];
         snprintf(tool_detail, sizeof(tool_detail), "Calling %s...", calls->calls[i].name);
         webui_send_state_with_detail(status_session, "tool_call", tool_detail);
      }
   }

   /* Single tool - no threading overhead needed, skip timing */
   if (total_calls == 1) {
      if (llm_tools_execute(&calls->calls[0], &results->results[0]) != 0) {
         failures++;
      }
      results->count = 1;

      /* Transition from "tool_call" to "thinking" for the follow-up LLM call */
      if (status_session) {
         webui_send_state_with_detail(status_session, "thinking", "Processing results...");
      }

      return failures > 0 ? 1 : 0;
   }

   /* Multiple tools - measure parallel execution performance */
   struct timespec start_time, end_time;
   clock_gettime(CLOCK_MONOTONIC, &start_time);

   /* Multiple tools - partition into parallel-safe and sequential groups */
   int parallel_indices[LLM_TOOLS_MAX_PARALLEL_CALLS];
   int sequential_indices[LLM_TOOLS_MAX_PARALLEL_CALLS];
   int parallel_count = 0;
   int sequential_count = 0;

   for (int i = 0; i < total_calls; i++) {
      if (get_tool_parallel_safe(calls->calls[i].name)) {
         parallel_indices[parallel_count++] = i;
      } else {
         sequential_indices[sequential_count++] = i;
      }
   }

   LOG_INFO("Tool execution: %d parallel-safe, %d sequential", parallel_count, sequential_count);

   /* Execute parallel-safe tools concurrently */
   if (parallel_count > 0) {
      pthread_t threads[LLM_TOOLS_MAX_PARALLEL_CALLS];
      tool_exec_task_t tasks[LLM_TOOLS_MAX_PARALLEL_CALLS];
      bool thread_spawned[LLM_TOOLS_MAX_PARALLEL_CALLS] = { false };

      /* Configure thread attributes with reduced stack size (512KB vs 8MB default).
       * Tool execution uses libcurl and JSON parsing which need reasonable stack space. */
      pthread_attr_t thread_attr;
      pthread_attr_init(&thread_attr);
      pthread_attr_setstacksize(&thread_attr, 512 * 1024);

      /* Capture session context to propagate to spawned threads */
      session_t *current_session = session_get_command_context();

      /* Spawn threads for parallel tools */
      for (int i = 0; i < parallel_count; i++) {
         int idx = parallel_indices[i];
         tasks[i].call = &calls->calls[idx];
         tasks[i].result = &results->results[idx];
         tasks[i].session = current_session;
         tasks[i].return_code = 0;

         int rc = pthread_create(&threads[i], &thread_attr, tool_exec_thread, &tasks[i]);
         if (rc == 0) {
            thread_spawned[i] = true;
            LOG_INFO("Spawned thread %d for tool '%s'", i, calls->calls[idx].name);
         } else {
            /* Fallback to sequential if thread creation fails */
            LOG_WARNING("pthread_create failed for tool '%s' (error=%d), executing sequentially",
                        calls->calls[idx].name, rc);
            tasks[i].return_code = llm_tools_execute(tasks[i].call, tasks[i].result);
         }
      }

      /* Wait for all spawned threads */
      for (int i = 0; i < parallel_count; i++) {
         if (thread_spawned[i]) {
            pthread_join(threads[i], NULL);
         }
      }

      /* Collect results from parallel execution */
      for (int i = 0; i < parallel_count; i++) {
         if (tasks[i].return_code != 0) {
            failures++;
         }
      }

      pthread_attr_destroy(&thread_attr);
   }

   /* Execute sequential tools one at a time */
   for (int i = 0; i < sequential_count; i++) {
      int idx = sequential_indices[i];
      if (llm_tools_execute(&calls->calls[idx], &results->results[idx]) != 0) {
         failures++;
      }
   }

   results->count = total_calls;

   /* Transition from "tool_call" to "thinking" for the follow-up LLM call */
   if (status_session) {
      webui_send_state_with_detail(status_session, "thinking", "Processing results...");
   }

   clock_gettime(CLOCK_MONOTONIC, &end_time);
   long elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000 +
                     (end_time.tv_nsec - start_time.tv_nsec) / 1000000;
   LOG_INFO("Tool execution: %d tools completed in %ldms (%d parallel, %d sequential)", total_calls,
            elapsed_ms, parallel_count, sequential_count);

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

      const char *args_str = json_object_get_string(args_obj);
      if (args_str && strlen(args_str) >= LLM_TOOLS_ARGS_LEN) {
         LOG_WARNING("Tool '%s' arguments truncated from %zu to %d bytes", call->name,
                     strlen(args_str), LLM_TOOLS_ARGS_LEN - 1);
      }
      safe_strncpy(call->arguments, args_str ? args_str : "", LLM_TOOLS_ARGS_LEN);
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
 * Tool Suppression (Thread-Local)
 * ============================================================================= */

void llm_tools_suppress_push(void) {
   tl_suppress_count++;
}

void llm_tools_suppress_pop(void) {
   if (tl_suppress_count > 0) {
      tl_suppress_count--;
   }
}

bool llm_tools_suppressed(void) {
   return tl_suppress_count > 0;
}

void llm_tools_set_current_config(const llm_resolved_config_t *config) {
   tl_current_config = config;
}

const char *llm_get_current_thinking_mode(void) {
   /* Priority: thread-local config > global config > default */
   if (tl_current_config && tl_current_config->thinking_mode[0] != '\0') {
      return tl_current_config->thinking_mode;
   }
   if (g_config.llm.thinking.mode[0] != '\0') {
      return g_config.llm.thinking.mode;
   }
   return "auto";
}

const char *llm_get_current_reasoning_effort(void) {
   /* Priority: thread-local config > global config > default */
   if (tl_current_config && tl_current_config->reasoning_effort[0] != '\0') {
      return tl_current_config->reasoning_effort;
   }
   if (g_config.llm.thinking.reasoning_effort[0] != '\0') {
      return g_config.llm.thinking.reasoning_effort;
   }
   return "medium";
}

int llm_get_effective_budget_tokens(void) {
   /* Map reasoning_effort to configured budget using first-char for efficiency */
   const char *effort = llm_get_current_reasoning_effort();
   int budget;
   switch (effort[0]) {
      case 'l':
         budget = g_config.llm.thinking.budget_low;
         break;
      case 'h':
         budget = g_config.llm.thinking.budget_high;
         break;
      default:
         budget = g_config.llm.thinking.budget_medium;
         break;
   }

   /* Clamp to 50% of model's context size if we have session config */
   if (tl_current_config) {
      int context_size = llm_context_get_size(tl_current_config->type,
                                              tl_current_config->cloud_provider,
                                              tl_current_config->model);
      int max_budget = context_size / 2; /* 50% limit */

      if (budget > max_budget) {
         LOG_WARNING("Thinking budget %d exceeds 50%% of context (%d tokens), clamping to %d",
                     budget, context_size, max_budget);
         budget = max_budget;
      }
   }

   return budget;
}

bool llm_check_thinking_trigger(const char *text) {
   if (!text || text[0] == '\0') {
      return false;
   }

   /* Trigger phrases that should enable extended thinking (case-insensitive) */
   static const char *triggers[] = { "think about it",     "think carefully",  "reason through",
                                     "think step by step", "think it through", "let's think" };
   static const size_t trigger_count = sizeof(triggers) / sizeof(triggers[0]);

   for (size_t i = 0; i < trigger_count; i++) {
      if (strcasestr_portable(text, triggers[i]) != NULL) {
         return true;
      }
   }

   return false;
}

/* =============================================================================
 * Capability Checking
 * ============================================================================= */

bool llm_tools_enabled(const llm_resolved_config_t *config) {
   /* Check thread-local suppression first */
   if (tl_suppress_count > 0) {
      return false;
   }

   /* Check config option - only "native" mode enables native tool calling.
    * Priority: explicit config > thread-local config > global config */
   const llm_resolved_config_t *effective_config = config ? config : tl_current_config;
   const char *tool_mode = (effective_config && effective_config->tool_mode[0] != '\0')
                               ? effective_config->tool_mode
                               : g_config.llm.tools.mode;
   if (strcmp(tool_mode, "native") != 0) {
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
    * If mode is "native" and tools are initialized, we should use
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
