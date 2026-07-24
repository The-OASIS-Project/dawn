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
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/command_executor.h"
#include "core/component_status.h"
#include "core/ocp_helpers.h"
#include "core/session_manager.h"
#include "core/worker_pool.h"
#include "dawn.h"
#include "dawn_error.h"
#include "llm/llm_claude_format.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_context.h"
#include "llm/llm_interface.h"
#include "logging.h"
#include "mosquitto_comms.h"
#include "tools/hud_discovery.h"
#include "tools/tool_registry.h"
#include "utils/string_utils.h"
#include "webui/webui_server.h"

/* =============================================================================
 * File I/O and Base64 Utilities (used by vision image handling)
 * ============================================================================= */

static unsigned char *read_file(const char *filename, size_t *length) {
   *length = 0;
   FILE *file = fopen(filename, "rb");
   if (!file) {
      OLOG_ERROR("File opening failed: %s", filename);
      return NULL;
   }

   fseek(file, 0, SEEK_END);
   long size = ftell(file);
   if (size == -1) {
      OLOG_ERROR("Failed to determine file size: %s", filename);
      fclose(file);
      return NULL;
   }
   *length = (size_t)size;

   /* Reject files over 20 MB to prevent OOM on embedded targets */
#define MAX_IMAGE_FILE_SIZE (20 * 1024 * 1024)
   if (*length > MAX_IMAGE_FILE_SIZE) {
      OLOG_ERROR("File too large (%zu bytes, max %d): %s", *length, MAX_IMAGE_FILE_SIZE, filename);
      fclose(file);
      *length = 0;
      return NULL;
   }

   OLOG_INFO("Reading file: %zu bytes", *length);
   fseek(file, 0, SEEK_SET);

   unsigned char *content = malloc(*length);
   if (!content) {
      OLOG_ERROR("Memory allocation failed");
      fclose(file);
      return NULL;
   }

   size_t read_length = fread(content, 1, *length, file);
   if (*length != read_length) {
      OLOG_ERROR("Failed to read the total size. Expected: %zu, Read: %zu", *length, read_length);
      free(content);
      fclose(file);
      return NULL;
   }

   fclose(file);
   return content;
}

/* Thin wrapper over the shared encoder (src/core/ocp_helpers.c) — kept as a
 * static alias so existing call sites stay unchanged. */
static char *base64_encode(const unsigned char *buffer, size_t length) {
   return ocp_base64_encode(buffer, length);
}

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
   "execute_plan",       /* Plan executor modifies state via sub-tool calls */
   "phone",              /* Shared pending confirmation state + delete rate bucket */
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

   OLOG_INFO("Thread started for tool '%s'", task->call->name);

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
      OLOG_WARNING("Rejected relative path: %s", path);
      return false;
   }

   /* Reject path traversal attempts (check before resolving) */
   if (strstr(path, "..") != NULL) {
      OLOG_WARNING("Rejected path with traversal: %s", path);
      return false;
   }

   /* Resolve symlinks to get canonical path - prevents symlink attacks */
   char resolved[PATH_MAX];
   if (realpath(path, resolved) == NULL) {
      OLOG_WARNING("Could not resolve path (file may not exist): %s", path);
      return false;
   }

   /* Allowed directories for viewing response files */
   /* Build allowed prefixes using $HOME instead of hardcoded /home/jetson */
   const char *home = getenv("HOME");
   if (!home || home[0] == '\0') {
      OLOG_WARNING("$HOME not set — rejecting file view request for security");
      return false;
   }
   char home_recordings[PATH_MAX];
   char home_oasis[PATH_MAX];
   snprintf(home_recordings, sizeof(home_recordings), "%s/recordings/", home);
   snprintf(home_oasis, sizeof(home_oasis), "%s/oasis/", home);
   const char *allowed_prefixes[] = { home_recordings, "/tmp/", home_oasis };
   size_t prefix_count = sizeof(allowed_prefixes) / sizeof(allowed_prefixes[0]);

   for (size_t i = 0; i < prefix_count; i++) {
      if (strncmp(resolved, allowed_prefixes[i], strlen(allowed_prefixes[i])) == 0) {
         return true;
      }
   }

   OLOG_WARNING("Rejected path outside allowed directories: %s (resolved: %s)", path, resolved);
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
      OLOG_INFO("Vision data is inline base64 (%zu bytes)", strlen(data));
      base64_image = strdup(data);
      if (!base64_image) {
         if (error_buf) {
            snprintf(error_buf, error_len, "Error: Memory allocation failed");
         }
         return false;
      }
   } else {
      /* Data is a file path - validate, read, and encode */
      OLOG_INFO("Vision data is file path: %s", data);

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
      OLOG_INFO("Image file encoded: %zu bytes base64", strlen(base64_image));
   }

   /* Defense-in-depth: MQTT-sourced captures (e.g. the `viewing` camera tool) have
    * no upstream size cap the way WebUI uploads do — MIRAGE (or a device
    * impersonating it, if the broker is compromised) fully controls these bytes.
    * Reuse the existing upload cap so a captured image can't grow the persisted
    * conversation_history entry (see llm_tools_add_results_openai/claude)
    * without bound. */
   size_t max_bytes = (size_t)g_config.vision.max_image_size_kb * 1024;
   if (strlen(base64_image) > max_bytes) {
      OLOG_WARNING("Vision image exceeds max_image_size_kb (%d KB, %zu bytes received) — rejecting",
                   g_config.vision.max_image_size_kb, strlen(base64_image));
      free(base64_image);
      if (error_buf) {
         snprintf(error_buf, error_len, "Error: Image exceeds maximum size (%d KB)",
                  g_config.vision.max_image_size_kb);
      }
      return false;
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
      OLOG_INFO("Vision image stored in tool result: %zu bytes", vision_size);
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
      OLOG_ERROR("Maximum tool count (%d) reached, skipping '%s'", LLM_TOOLS_MAX_TOOLS, meta->name);
      return;
   }

   tool_definition_t *t = &s_tools[s_tool_count++];
   memset(t, 0, sizeof(*t));

   safe_strncpy(t->name, meta->name, LLM_TOOLS_NAME_LEN);
   safe_strncpy(t->description, meta->description, LLM_TOOLS_DESC_LEN);
   /* A long description with a multi-byte UTF-8 char near the 512-byte cut
    * (e.g. an em-dash in an MCP tool's docs) truncates mid-codepoint, producing
    * invalid UTF-8 that breaks the whole get_tools_config WebSocket frame and
    * the LLM API request body.  Repair the trailing partial sequence in place. */
   sanitize_utf8_for_json(t->description);
   t->device_name = meta->device_string;
   t->enabled = true; /* Will be updated by llm_tools_refresh() */
   t->armor_feature = (meta->capabilities & TOOL_CAP_ARMOR_FEATURE) != 0;
   t->dangerous = (meta->capabilities & TOOL_CAP_DANGEROUS) != 0;
   t->parallel_safe = is_tool_parallel_safe(meta->name);

   /* Dangerous tools require explicit opt-in — default to disabled */
   if (t->dangerous) {
      t->enabled_local = false;
      t->enabled_remote = false;
   } else {
      t->enabled_local = true;
      t->enabled_remote = meta->default_remote;
   }

   /* Copy parameters.  Params past LLM_TOOLS_MAX_PARAMS are dropped from the LLM-callable
    * schema — warn loudly rather than silently truncate (this masked replaced_by/with_source/
    * as_of/include_historical on the 12-param memory tool when the cap was 8). */
   if (meta->param_count > LLM_TOOLS_MAX_PARAMS) {
      OLOG_WARNING("Tool '%s' declares %d params but LLM_TOOLS_MAX_PARAMS=%d — %d param(s) dropped "
                   "from the LLM schema (the LLM cannot call them). Raise LLM_TOOLS_MAX_PARAMS.",
                   meta->name ? meta->name : "?", meta->param_count, LLM_TOOLS_MAX_PARAMS,
                   meta->param_count - LLM_TOOLS_MAX_PARAMS);
   }
   for (int i = 0; i < meta->param_count && i < LLM_TOOLS_MAX_PARAMS; i++) {
      const treg_param_t *src = &meta->params[i];
      tool_param_t *dst = &t->parameters[t->param_count++];

      safe_strncpy(dst->name, src->name, LLM_TOOLS_NAME_LEN);
      safe_strncpy(dst->description, src->description ? src->description : "",
                   sizeof(dst->description));
      sanitize_utf8_for_json(dst->description); /* repair any mid-codepoint truncation */
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

   /* Generate tools from ALL registry entries (including dangerous tools that
    * default to disabled). This ensures they appear in the WebUI tools config
    * so users can opt in. The enabled_local/remote flags control LLM access. */
   tool_registry_foreach(generate_tool_from_treg, NULL);

   s_initialized = true;
   OLOG_INFO("Initialized %d LLM tools from tool_registry", s_tool_count);

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
   OLOG_INFO("Enabled tools: %s", enabled_list);
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

         /* Also honor the tool's own availability gate (hud_control requires
          * discovered elements; hud_mode requires >1 mode).  hud_available
          * flips true when the HUD keepalive comes online, but discovery
          * responses land a beat later — without this the native schema could
          * ship hud_control with an empty `element` enum (emitted as an
          * unconstrained string) in that window and the LLM fabricates element
          * names.  Discovery re-runs llm_tools_refresh() once the enum is
          * populated, so this re-enables the tool automatically. */
         const tool_metadata_t *meta = tool_registry_find(t->name);
         if (meta != NULL && meta->is_available != NULL && !meta->is_available()) {
            t->enabled = false;
         }
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

   OLOG_INFO("Refreshed tool availability: %d enabled (HUD %s)", s_enabled_count,
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
      case TOOL_PARAM_TYPE_ARRAY:
         return "array";
      default:
         return "string";
   }
}

/**
 * @brief Add the JSON-schema "type" (and, for arrays, "items") to a property
 *
 * Scalar types add a single "type" string. TOOL_PARAM_TYPE_ARRAY additionally
 * adds "items":{"type":"string"} so the provider advertises a string array.
 * Shared by both schema builders so the array shape stays consistent.
 */
static void add_param_type_to_prop(struct json_object *prop, tool_param_type_t type) {
   json_object_object_add(prop, "type", json_object_new_string(param_type_to_json_type(type)));
   if (type == TOOL_PARAM_TYPE_ARRAY) {
      struct json_object *items = json_object_new_object();
      json_object_object_add(items, "type", json_object_new_string("string"));
      json_object_object_add(prop, "items", items);
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
      add_param_type_to_prop(prop, (tool_param_type_t)p->type);
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
      add_param_type_to_prop(prop, p->type);
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

/**
 * @brief Full tool description as sent to the LLM.
 *
 * The cached tool_definition_t.description is capped at LLM_TOOLS_DESC_LEN (512),
 * which silently truncated long tool descriptions (e.g. scheduler at ~5.8 KB).
 * Params already read their full text live from the registry when the tools JSON
 * is built (build_parameters_schema_from_treg); this makes the tool-level
 * description do the same, so nothing is clipped in the request the model
 * receives.  The tools array is rebuilt per LLM request (no cached schema), so
 * this is an O(1) hash lookup per enabled tool per request — negligible against
 * the per-tool json-c allocations already on that path.  Falls back to the cached
 * copy only for tools not in the registry.  The registry always holds valid
 * UTF-8 (static literals by construction; MCP descriptions are
 * codepoint-safe-capped at ingest in mcp_schema_wrap_description).
 */
static const char *tool_effective_description(const tool_definition_t *t) {
   const tool_metadata_t *meta = tool_registry_lookup(t->name);
   return (meta && meta->description) ? meta->description : t->description;
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
      json_object_object_add(function, "description",
                             json_object_new_string(tool_effective_description(t)));
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
      json_object_object_add(tool_obj, "description",
                             json_object_new_string(tool_effective_description(t)));
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
   /* Headless background-job workers must not fan out into more jobs — hide the
    * job-spawn tool from a SESSION_TYPE_JOB context's schema so the model never
    * sees it.  (handle_spawn also hard-refuses a job-context caller as a backstop
    * for any non-schema path, e.g. a legacy <command> tag.)  The session lookup
    * runs only for the "job" tool, so it costs nothing for every other tool. */
   if (strcmp(t->name, "job") == 0) {
      session_t *ctx = session_get_command_context();
      if (ctx != NULL && ctx->type == SESSION_TYPE_JOB) {
         return false;
      }
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
      json_object_object_add(function, "description",
                             json_object_new_string(tool_effective_description(t)));
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
      json_object_object_add(tool_obj, "description",
                             json_object_new_string(tool_effective_description(t)));
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
      info->dangerous = t->dangerous;
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
         OLOG_INFO("Tool '%s' enable state updated: local=%d, remote=%d", tool_name, enabled_local,
                   enabled_remote);
         return 0; /* SUCCESS */
      }
   }
   pthread_mutex_unlock(&s_tools_mutex);

   OLOG_WARNING("Tool '%s' not found", tool_name);
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

/* Mark set[i]=true for each registered tool named in names[0..count). */
static void build_tool_set(bool *set, const char names[][LLM_TOOL_NAME_MAX], int count) {
   for (int j = 0; j < count; j++) {
      for (int i = 0; i < s_tool_count; i++) {
         if (strcmp(s_tools[i].name, names[j]) == 0) {
            set[i] = true;
            break;
         }
      }
   }
}

/* Resolve one tool's enabled state for one surface (local OR remote).
 *
 * - DANGEROUS tools are NEVER auto-enabled: they require explicit membership in
 *   the enable (whitelist) list under either model.
 * - Otherwise, if a DISABLE list is configured → BLOCKLIST: on unless listed
 *   (so a newly-added tool is on by default). Takes precedence over the enable
 *   list for non-dangerous tools.
 * - Else if an ENABLE list is configured → WHITELIST (legacy): on only if listed.
 * - Else (this surface unconfigured) → on (all non-dangerous enabled).
 */
static bool resolve_tool_enabled(bool dangerous,
                                 bool in_enable,
                                 bool in_disable,
                                 bool enable_cfg,
                                 bool disable_cfg) {
   if (dangerous)
      return in_enable;
   if (disable_cfg)
      return !in_disable;
   if (enable_cfg)
      return in_enable;
   return true;
}

void llm_tools_apply_config(const llm_tools_config_t *cfg) {
   if (!s_initialized) {
      OLOG_WARNING("llm_tools_apply_config called before initialization - config ignored");
      return;
   }
   if (!cfg)
      return;

   /* Build membership sets for each of the four lists (O(n*m), tiny m). Reads
    * s_tool_count / s_tools[].name OUTSIDE s_tools_mutex: safe because this runs
    * once at startup (single caller in llm_interface.c, before worker/session
    * threads exist) and name/count are init-time-immutable. A future runtime
    * re-apply from a request thread would need to take the lock here. */
   bool en_local[LLM_TOOLS_MAX_TOOLS] = { 0 };
   bool en_remote[LLM_TOOLS_MAX_TOOLS] = { 0 };
   bool dis_local[LLM_TOOLS_MAX_TOOLS] = { 0 };
   bool dis_remote[LLM_TOOLS_MAX_TOOLS] = { 0 };
   build_tool_set(en_local, cfg->local_enabled, cfg->local_enabled_count);
   build_tool_set(en_remote, cfg->remote_enabled, cfg->remote_enabled_count);
   build_tool_set(dis_local, cfg->local_disabled, cfg->local_disabled_count);
   build_tool_set(dis_remote, cfg->remote_disabled, cfg->remote_disabled_count);

   pthread_mutex_lock(&s_tools_mutex);
   for (int i = 0; i < s_tool_count; i++) {
      s_tools[i].enabled_local = resolve_tool_enabled(s_tools[i].dangerous, en_local[i],
                                                      dis_local[i], cfg->local_enabled_configured,
                                                      cfg->local_disabled_configured);
      s_tools[i].enabled_remote = resolve_tool_enabled(s_tools[i].dangerous, en_remote[i],
                                                       dis_remote[i],
                                                       cfg->remote_enabled_configured,
                                                       cfg->remote_disabled_configured);
   }

   /* Invalidate token estimate cache */
   s_token_estimate_local = -1;
   s_token_estimate_remote = -1;
   pthread_mutex_unlock(&s_tools_mutex);

   OLOG_INFO("Applied tool config: local=%d tools, remote=%d tools",
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
   /* The per-session job-tool mask (is_tool_enabled_for_session) makes the built
    * schema session-dependent, but this estimate cache is process-global.  If this
    * ever runs in a SESSION_TYPE_JOB context, compute fresh and do NOT read or
    * write the shared cache — otherwise a job worker's one-tool-lighter schema
    * would poison the estimate served to every other surface until the next config
    * reload.  (No job-context caller exists today; this keeps it that way safely.) */
   session_t *ctx = session_get_command_context();
   bool job_ctx = (ctx != NULL && ctx->type == SESSION_TYPE_JOB);

   int *cache = is_remote_session ? &s_token_estimate_remote : &s_token_estimate_local;
   if (!job_ctx && *cache >= 0) {
      return *cache;
   }

   struct json_object *tools = llm_tools_get_openai_format_filtered(is_remote_session);
   if (!tools) {
      if (!job_ctx) {
         *cache = 0;
      }
      return 0;
   }

   const char *json_str = json_object_to_json_string(tools);
   int est = (int)(strlen(json_str) / 4); /* Rough estimate: ~4 chars per token */
   json_object_put(tools);

   if (!job_ctx) {
      *cache = est;
   }
   return est;
}

void llm_tools_invalidate_cache(void) {
   pthread_mutex_lock(&s_tools_mutex);
   s_token_estimate_local = -1;
   s_token_estimate_remote = -1;
   pthread_mutex_unlock(&s_tools_mutex);
   OLOG_INFO("LLM tools schema cache invalidated");
}

/* Raw JSON args of the in-flight tool call, exposed to the callback for the
 * duration of its invocation (see llm_tools.h). Thread-local, like the session
 * command context. */
static __thread const char *s_current_raw_args = NULL;

const char *llm_tools_current_raw_args(void) {
   return s_current_raw_args;
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
   /* Refuse to execute on truncated arguments: the provider's tool-call args
    * exceeded LLM_TOOLS_ARGS_LEN and were clipped, so any field could be cut
    * mid-value (e.g. a document_manage save_text body).  Acting on partial args
    * silently stores corrupt data — return a clear, actionable error instead of
    * the generic "Invalid JSON" the truncation would otherwise produce. */
   if (call->args_truncated) {
      snprintf(
          result->result, LLM_TOOLS_RESULT_LEN,
          "Error: the arguments to '%s' were too long (over %d bytes) and were cut off, so the "
          "call was not run. Split the input into smaller pieces (e.g. save a long document in "
          "sections) and try again.",
          call->name, LLM_TOOLS_ARGS_LEN - 1);
      result->success = false;
      return 1;
   }

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

   /* Track which param names map to action/device so we can build a fallback value */
   const char *action_param_name = NULL;
   const char *device_param_name = NULL;
   const char *value_param_name = NULL;

   for (int i = 0; i < meta->param_count; i++) {
      const treg_param_t *param = &meta->params[i];
      struct json_object *val_obj = NULL;

      if (args) {
         json_object_object_get_ex(args, param->name, &val_obj);
      }

      const char *val_str = val_obj ? json_object_get_string(val_obj) : NULL;

      switch (param->maps_to) {
         case TOOL_MAPS_TO_ACTION:
            action_param_name = param->name;
            if (val_str) {
               safe_strncpy(action_name, val_str, sizeof(action_name));
            }
            break;
         case TOOL_MAPS_TO_VALUE:
            value_param_name = param->name;
            if (val_str) {
               safe_strncpy(value_buf, val_str, sizeof(value_buf));
            }
            break;
         case TOOL_MAPS_TO_DEVICE:
            device_param_name = param->name;
            if (val_str) {
               safe_strncpy(device_name, val_str, sizeof(device_name));
            }
            break;
         case TOOL_MAPS_TO_CUSTOM:
            /* Custom fields appended to value with ::field_name::value format.
             * For TOOL_PARAM_TYPE_ARRAY, val_str is the array's serialized JSON
             * (json_object_get_string returns the JSON repr for non-strings) and
             * MUST be the terminal slot — see the ARRAY contract in tool_registry.h. */
            if (val_str) {
               if (param->field_name && param->field_name[0]) {
                  /* Append with field name for parsing in callback. Guard against
                   * silent truncation: a truncated array slot would parse as invalid
                   * JSON downstream with no diagnostic, so reject explicitly. */
                  size_t cur_len = strlen(value_buf);
                  size_t remaining = sizeof(value_buf) - cur_len;
                  /* 4 = the four delimiter bytes ("::" x2); the NUL is covered by
                   * the strict `need >= remaining` test (leaves >=1 byte). */
                  size_t need = strlen(param->field_name) + strlen(val_str) + 4;
                  if (need >= remaining) {
                     OLOG_ERROR("Tool '%s' param '%s' too large to encode (%zu >= %zu)", meta->name,
                                param->field_name, need, remaining);
                     snprintf(result->result, LLM_TOOLS_RESULT_LEN,
                              "Error: too many items for '%s' — try fewer at a time",
                              param->field_name);
                     result->success = false;
                     json_object_put(args);
                     return 1;
                  }
                  snprintf(value_buf + cur_len, remaining, "::%s::%s", param->field_name, val_str);
               } else if (value_buf[0] == '\0') {
                  /* Fallback: store directly if value is empty */
                  safe_strncpy(value_buf, val_str, sizeof(value_buf));
               }
            }
            break;
      }
   }

   /* Fallback: if value_buf is empty and the LLM sent a flat JSON (no "details" key),
    * collect all non-extracted fields into a JSON object as the value.
    * This handles LLMs that flatten {"action":"create","type":"timer",...}
    * instead of nesting {"action":"create","details":"{\"type\":\"timer\",...}"}. */
   if (value_buf[0] == '\0' && args && value_param_name) {
      struct json_object *remaining = json_object_new_object();
      json_object_object_foreach(args, key, val) {
         /* Skip keys already extracted as action/device/value params */
         if ((action_param_name && strcmp(key, action_param_name) == 0) ||
             (device_param_name && strcmp(key, device_param_name) == 0) ||
             (value_param_name && strcmp(key, value_param_name) == 0))
            continue;
         json_object_object_add(remaining, key, json_object_get(val));
      }
      if (json_object_object_length(remaining) > 0) {
         const char *remaining_str = json_object_to_json_string(remaining);
         safe_strncpy(value_buf, remaining_str, sizeof(value_buf));
         OLOG_INFO("Tool '%s': LLM sent flat args, reconstructed value from remaining fields",
                   call->name);
      }
      json_object_put(remaining);
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

   OLOG_INFO("Executing tool '%s' (treg) -> device='%s', action='%s', value='%s'", call->name,
             effective_device, action_name, value_buf);

   /* Notify callback that tool execution is starting */
   notify_tool_execution(call->name, call->arguments, NULL, false);

   /* Special handling for sync_wait tools (e.g., viewing) */
   if (meta->sync_wait && strcmp(meta->name, "viewing") == 0) {
      result->success = execute_viewing_sync(action_name, value_buf, result);
      notify_tool_execution(call->name, call->arguments, result->result, result->success);
      return result->success ? 0 : 1;
   }

   /* MQTT-only tools publish directly using the already-resolved outer tool's
    * metadata. We deliberately skip command_execute()'s device-name registry
    * lookup here because effective_device can be a pass-through value (e.g.,
    * "altitude" for hud_control, "record" for recording) that is not itself
    * a registered tool — MIRAGE's topic-hud handler parses the device field
    * directly to route to display elements or recording modes. */
   if (meta->mqtt_only) {
      struct mosquitto *mosq = worker_pool_get_mosq();
      cmd_exec_result_t exec_result;

      /* Default action for ANALOG mqtt_only tools without explicit action */
      if (action_name[0] == '\0' && meta->device_type == TOOL_DEVICE_TYPE_ANALOG) {
         safe_strncpy(action_name, "set", sizeof(action_name));
      }

      int rc = command_execute_mqtt_direct(meta, effective_device, action_name, value_buf, mosq,
                                           &exec_result);

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
      result->should_respond = exec_result.should_respond;
      notify_tool_execution(call->name, call->arguments, result->result, result->success);
      cmd_exec_result_free(&exec_result);
      return result->success ? 0 : 1;
   }

   /* Call the tool's callback directly */
   if (meta->callback) {
      int should_respond = 0;
      /* Expose the raw LLM args to the callback (thread-local) so bridged tools
       * can recover the original typed JSON that (action, value) packing flattens
       * lossily. Cleared immediately after the call. */
      s_current_raw_args = call->arguments;
      char *cb_result = meta->callback(action_name[0] ? action_name : "get",
                                       value_buf[0] ? value_buf : NULL, &should_respond);
      s_current_raw_args = NULL;

      /* Strip the opt-in tool error-marker (this native-tool path invokes the
       * callback directly, bypassing command_execute's strip). */
      tool_result_strip_error_mark(cb_result);

      if (cb_result) {
         size_t cb_len = strlen(cb_result);
         if (cb_len >= LLM_TOOLS_RESULT_LEN) {
            /* Large result — store in result_extended, copy truncated preview to result[] */
            result->result_extended = cb_result; /* Transfer ownership */
            safe_strncpy(result->result, cb_result, LLM_TOOLS_RESULT_LEN);
            OLOG_INFO("Tool '%s' result stored in result_extended (%zu bytes)", call->name, cb_len);
         } else {
            safe_strncpy(result->result, cb_result, LLM_TOOLS_RESULT_LEN);
            free(cb_result);
         }
      } else {
         snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Tool '%s' completed", call->name);
      }
      result->success = true;
      result->skip_followup = meta->skip_followup;
      result->should_respond = (should_respond != 0);

      notify_tool_execution(call->name, call->arguments, tool_result_content(result),
                            result->success);
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
      result->should_respond = exec_result.should_respond;
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
   result->should_respond = true; /* Default: respond unless callback says otherwise */

   /* Look up tool in tool_registry */
   const tool_metadata_t *treg_meta = tool_registry_find(call->name);
   if (!treg_meta) {
      snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Error: Unknown tool '%s'", call->name);
      result->success = false;
      return 1;
   }

   /* Session-level availability guard. The schema sent to the LLM already
    * excludes unavailable tools, but LLMs sometimes hallucinate a call from
    * conversation history — e.g., when a previous turn successfully used a
    * tool that has since been disabled (HUD going offline, admin toggling
    * it off, etc.). Without this check we'd blindly execute the hallucinated
    * call because tool_registry_find() only looks up by name. Fail soft with
    * a descriptive error so the LLM can tell the user instead of silently
    * MQTT-publishing into a dead endpoint.
    *
    * When session context is absent (e.g., internal / system-driven tool
    * calls) we allow execution — those paths aren't gated by session flags. */
   session_t *ctx = session_get_command_context();
   if (ctx) {
      bool is_remote = (ctx->type != SESSION_TYPE_LOCAL);
      bool enabled = true;
      pthread_mutex_lock(&s_tools_mutex);
      for (int i = 0; i < s_tool_count; i++) {
         if (strcmp(s_tools[i].name, call->name) == 0) {
            enabled = is_tool_enabled_for_session(&s_tools[i], is_remote);
            break;
         }
      }
      pthread_mutex_unlock(&s_tools_mutex);

      if (!enabled) {
         snprintf(result->result, LLM_TOOLS_RESULT_LEN,
                  "Tool '%s' is not currently available (capability offline, misconfigured, or "
                  "disabled for this %s session). Let the user know the feature can't be used "
                  "right now.",
                  call->name, is_remote ? "remote" : "local");
         result->success = false;
         result->should_respond = true;
         OLOG_WARNING("Refused tool '%s' — not enabled for %s session", call->name,
                      is_remote ? "remote" : "local");
         notify_tool_execution(call->name, call->arguments, result->result, false);
         return 1;
      }
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

   OLOG_INFO("Tool execution: %d parallel-safe, %d sequential", parallel_count, sequential_count);

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
            OLOG_INFO("Spawned thread %d for tool '%s'", i, calls->calls[idx].name);
         } else {
            /* Fallback to sequential if thread creation fails */
            OLOG_WARNING("pthread_create failed for tool '%s' (error=%d), executing sequentially",
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
   OLOG_INFO("Tool execution: %d tools completed in %ldms (%d parallel, %d sequential)",
             total_calls, elapsed_ms, parallel_count, sequential_count);

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

   /* For single result, return it if should_respond is set */
   if (results->count == 1) {
      if (!results->results[0].should_respond) {
         return NULL; /* Tool handled its own output */
      }
      return strdup(tool_result_content(&results->results[0]));
   }

   /* For multiple results, concatenate only should_respond=true results */
   size_t total_len = 0;
   int respondable = 0;
   for (int i = 0; i < results->count; i++) {
      if (results->results[i].should_respond) {
         total_len += strlen(tool_result_content(&results->results[i])) + 2; /* +2 for newline */
         respondable++;
      }
   }

   if (respondable == 0) {
      return NULL; /* All tools handled their own output */
   }

   char *response = malloc(total_len + 1);
   if (!response) {
      return NULL;
   }

   /* Use pointer offset instead of strcat to avoid O(n²) */
   char *ptr = response;
   int written = 0;
   for (int i = 0; i < results->count; i++) {
      if (!results->results[i].should_respond) {
         continue;
      }
      const char *content = tool_result_content(&results->results[i]);
      size_t len = strlen(content);
      memcpy(ptr, content, len);
      ptr += len;
      written++;
      if (written < respondable) {
         *ptr++ = '\n';
      }
   }
   *ptr = '\0';

   return response;
}

/* =============================================================================
 * Tool Result Formatting for Conversation History
 * ============================================================================= */

/**
 * @brief Build a standalone user message holding exactly one captured-vision image
 *
 * OpenAI-shape: {"role":"user","content":[{"type":"image_url","image_url":{"url":...}}]}
 * No accompanying text. Persists a tool-captured image (e.g. the `viewing`
 * camera tool) into conversation history so follow-up turns can still see
 * it, instead of the image only being visible for the single turn it was
 * captured in. is_capture_image_message() recognizes this exact
 * single-part shape for retention pruning.
 */
static struct json_object *build_openai_capture_image_message(const char *base64_data) {
   const char *media_type = llm_claude_detect_image_mime_type(base64_data);
   const char *prefix_fmt = "data:%s;base64,";
   size_t data_uri_len = strlen(prefix_fmt) + strlen(media_type) + strlen(base64_data) + 1;
   char *data_uri = malloc(data_uri_len);
   if (!data_uri) {
      OLOG_ERROR("Failed to allocate data URI for captured image (%zu bytes) — skipping persist",
                 data_uri_len);
      return NULL;
   }
   snprintf(data_uri, data_uri_len, "data:%s;base64,%s", media_type, base64_data);

   struct json_object *image_url_obj = json_object_new_object();
   json_object_object_add(image_url_obj, "url", json_object_new_string(data_uri));
   free(data_uri);

   struct json_object *image_obj = json_object_new_object();
   json_object_object_add(image_obj, "type", json_object_new_string("image_url"));
   json_object_object_add(image_obj, "image_url", image_url_obj);

   struct json_object *content_array = json_object_new_array();
   json_object_array_add(content_array, image_obj);

   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "role", json_object_new_string("user"));
   json_object_object_add(msg, "content", content_array);
   return msg;
}

/**
 * @brief Build a standalone user message holding exactly one captured-vision image (Claude shape)
 *
 * Mirrors build_openai_capture_image_message() but wraps the image with
 * llm_claude_create_image_block(), matching what convert_to_claude_format()
 * already produces ephemerally for a caller-supplied vision image.
 */
static struct json_object *build_claude_capture_image_message(const char *base64_data) {
   struct json_object *content_array = json_object_new_array();
   json_object_array_add(content_array, llm_claude_create_image_block(base64_data));

   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "role", json_object_new_string("user"));
   json_object_object_add(msg, "content", content_array);
   return msg;
}

/**
 * @brief True if msg is a lone captured-vision-image message
 *
 * Recognizes the exact shape built above: a "user" message whose content is
 * a single-element array containing only an image part ("image_url" for
 * OpenAI shape, "image" for Claude shape). This is distinct from a
 * WebUI-uploaded image (session_add_message_with_images() always pairs an
 * image with a text part, so its content array has length >= 2), so
 * retention pruning only ever touches ambient tool captures, never images
 * the user deliberately attached to a message.
 */
static bool is_capture_image_message(struct json_object *msg) {
   struct json_object *role_obj = NULL;
   struct json_object *content_obj = NULL;

   if (!msg || !json_object_object_get_ex(msg, "role", &role_obj) ||
       strcmp(json_object_get_string(role_obj), "user") != 0) {
      return false;
   }
   if (!json_object_object_get_ex(msg, "content", &content_obj) ||
       !json_object_is_type(content_obj, json_type_array) ||
       json_object_array_length(content_obj) != 1) {
      return false;
   }

   struct json_object *part = json_object_array_get_idx(content_obj, 0);
   struct json_object *type_obj = NULL;
   if (!part || !json_object_object_get_ex(part, "type", &type_obj)) {
      return false;
   }

   const char *type = json_object_get_string(type_obj);
   return type && (strcmp(type, "image_url") == 0 || strcmp(type, "image") == 0);
}

/**
 * @brief Keep only the most recent N tool-captured images in history
 *
 * Scans newest-to-oldest; the first retention_count capture-image messages
 * found are left intact, anything older is collapsed to a short text
 * placeholder. retention_count <= 0 means unlimited (no-op) — bounding is
 * then left entirely to normal context compaction.
 */
static void evict_old_capture_images(struct json_object *history, int retention_count) {
   if (!history || retention_count <= 0) {
      return;
   }

   int live = 0;
   for (int i = json_object_array_length(history) - 1; i >= 0; i--) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      if (!is_capture_image_message(msg)) {
         continue;
      }
      live++;
      if (live > retention_count) {
         json_object_object_add(msg, "content",
                                json_object_new_string(
                                    "[earlier camera capture - image no longer retained]"));
      }
   }
}

/**
 * @brief Persist the first tool result carrying a captured image, if any
 *
 * Appends an image-only message via the given builder and prunes older
 * captures per [vision] capture_history_count. Matches the prior ephemeral
 * behavior of surfacing at most one captured image per tool iteration.
 */
static void persist_capture_image_if_present(struct json_object *history,
                                             const tool_result_list_t *results,
                                             struct json_object *(*build_message)(const char *)) {
   for (int i = 0; i < results->count; i++) {
      const tool_result_t *r = &results->results[i];
      if (r->vision_image && r->vision_image_size > 0) {
         struct json_object *msg = build_message(r->vision_image);
         if (msg) {
            json_object_array_add(history, msg);
            evict_old_capture_images(history, g_config.vision.capture_history_count);
         }
         return;
      }
   }
}

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
      json_object_object_add(msg, "content", json_object_new_string(tool_result_content(r)));

      json_object_array_add(history, msg);
   }

   /* A tool that returned a captured image (e.g. `viewing`) only shows the
    * model that image for the turn it was captured in unless we persist it
    * here — see docs/arch/subsystems/llm.md vision notes. */
   persist_capture_image_if_present(history, results, build_openai_capture_image_message);

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
      json_object_object_add(block, "content", json_object_new_string(tool_result_content(r)));

      json_object_array_add(content_array, block);
   }

   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "role", json_object_new_string("user"));
   json_object_object_add(msg, "content", content_array);

   json_object_array_add(history, msg);

   persist_capture_image_if_present(history, results, build_claude_capture_image_message);

   return 0;
}

/* =============================================================================
 * Response Parsing
 * ============================================================================= */

int llm_tools_parse_openai_response(struct json_object *response, tool_call_list_t *out) {
   if (!response || !out) {
      return FAILURE;
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
      call->args_truncated = (args_str && strlen(args_str) >= LLM_TOOLS_ARGS_LEN);
      if (call->args_truncated) {
         OLOG_WARNING("Tool '%s' arguments truncated from %zu to %d bytes", call->name,
                      strlen(args_str), LLM_TOOLS_ARGS_LEN - 1);
      }
      safe_strncpy(call->arguments, args_str ? args_str : "", LLM_TOOLS_ARGS_LEN);
   }

   return out->count > 0 ? 0 : 1;
}

int llm_tools_parse_claude_response(struct json_object *response, tool_call_list_t *out) {
   if (!response || !out) {
      return FAILURE;
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
      call->args_truncated = (input_str && strlen(input_str) >= LLM_TOOLS_ARGS_LEN);
      if (call->args_truncated) {
         OLOG_WARNING("Tool '%s' arguments truncated from %zu to %d bytes", call->name,
                      strlen(input_str), LLM_TOOLS_ARGS_LEN - 1);
      }
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
   /* Priority: thread-local config > global config > default.
    * "auto" is treated as a synonym of "enabled" by all providers (see
    * llm_openai.c, llm_openai_responses.c, llm_claude_format.c) and remains
    * accepted from legacy DB rows / older clients. The default is "enabled"
    * since the WebUI dropdown only emits disabled/enabled. */
   if (tl_current_config && tl_current_config->thinking_mode[0] != '\0') {
      return tl_current_config->thinking_mode;
   }
   if (g_config.llm.thinking.mode[0] != '\0') {
      return g_config.llm.thinking.mode;
   }
   return "enabled";
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
   /* Map reasoning_effort to configured budget using first-char for efficiency.
    *
    * Effort vocabulary across providers:
    *   none    — gpt-5.4 / gpt-5.1+ only ("don't reason"). For Claude (which has
    *             no API "none"), map to budget_low so the thinking block at least
    *             has a minimum viable budget if "none" leaks through.
    *   low     — Claude budget_low.    OpenAI/Gemini pass through.
    *   medium  — Claude budget_medium. OpenAI/Gemini pass through. Default.
    *   high    — Claude budget_high.   OpenAI/Gemini pass through.
    *   xhigh   — Claude budget_xhigh.  OpenAI gpt-5.2/5.4 pass through; older OpenAI
    *             and Gemini get clamped to "high" at request build time. */
   const char *effort = llm_get_current_reasoning_effort();
   int budget;
   switch (effort[0]) {
      case 'n': /* none */
      case 'l': /* low */
         budget = g_config.llm.thinking.budget_low;
         break;
      case 'x': /* xhigh */
         budget = g_config.llm.thinking.budget_xhigh;
         break;
      case 'h': /* high */
         budget = g_config.llm.thinking.budget_high;
         break;
      default: /* medium */
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
         OLOG_WARNING("Thinking budget %d exceeds 50%% of context (%d tokens), clamping to %d",
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

/* Append "name" (with ", " separator after the first entry) to a bucket,
 * clamping on truncation so repeated calls can't underflow remaining space.
 * Returns true if the name was fully written, false on truncation or error. */
static bool hint_bucket_append(char *bucket, size_t bucket_size, int *offset, const char *name) {
   if (!bucket || !offset || !name || bucket_size == 0) {
      return false;
   }
   int off = *offset;
   if (off < 0 || (size_t)off >= bucket_size - 1) {
      return false; /* Bucket full */
   }

   if (off > 0) {
      int w = snprintf(bucket + off, bucket_size - off, ", ");
      if (w < 0 || (size_t)w >= bucket_size - off) {
         *offset = (int)bucket_size - 1; /* Clamp */
         return false;
      }
      off += w;
   }

   int w = snprintf(bucket + off, bucket_size - off, "%s", name);
   if (w < 0) {
      return false;
   }
   if ((size_t)w >= bucket_size - off) {
      /* Truncated — clamp so further appends see no remaining space */
      *offset = (int)bucket_size - 1;
      return false;
   }
   off += w;
   *offset = off;
   return true;
}

int llm_tools_build_disabled_hint(bool is_remote, char *buffer, size_t buffer_size) {
   if (!s_initialized || !buffer || buffer_size == 0) {
      return 0;
   }

   /* Bucket tools into two lists for the target session:
    *   unavailable[]      - capability not available (hardware/config not met)
    *   session_disabled[] - capability works, but admin-disabled for this session
    */
   char unavailable[512] = "";
   char session_disabled[512] = "";
   int unavail_off = 0;
   int disabled_off = 0;
   int unavail_count = 0;
   int disabled_count = 0;

   /* Hold s_tools_mutex across the scan so enable-flag writers
    * (llm_tools_set_enabled, llm_tools_refresh) don't mutate mid-read. */
   pthread_mutex_lock(&s_tools_mutex);
   for (int i = 0; i < s_tool_count; i++) {
      const tool_definition_t *t = &s_tools[i];
      bool session_flag = is_remote ? t->enabled_remote : t->enabled_local;

      /* Fully usable in this session - omit from hint */
      if (t->enabled && session_flag) {
         continue;
      }

      if (!t->enabled) {
         /* Capability not available (e.g., HUD hardware offline, API key missing) */
         if (hint_bucket_append(unavailable, sizeof(unavailable), &unavail_off, t->name)) {
            unavail_count++;
         }
      } else {
         /* Capability works but disabled for this session type */
         if (hint_bucket_append(session_disabled, sizeof(session_disabled), &disabled_off,
                                t->name)) {
            disabled_count++;
         }
      }
   }
   pthread_mutex_unlock(&s_tools_mutex);

   if (unavail_count == 0 && disabled_count == 0) {
      buffer[0] = '\0';
      return 0;
   }

   int len = 0;
   const char *session_label = is_remote ? "remote" : "local";

   if (unavail_count > 0) {
      int w = snprintf(buffer + len, buffer_size - len,
                       "\nNote: The following tools are installed but not currently "
                       "available (hardware offline or not configured): %s. If the user "
                       "asks about these capabilities, let them know the feature exists "
                       "but is not reachable right now.\n",
                       unavailable);
      if (w > 0 && (size_t)w < buffer_size - len) {
         len += w;
      } else if (w > 0) {
         len = (int)buffer_size - 1; /* Clamp on truncation */
      }
   }

   if (disabled_count > 0 && (size_t)len < buffer_size - 1) {
      int w = snprintf(buffer + len, buffer_size - len,
                       "\nNote: The following tools are disabled by the administrator "
                       "for this %s session: %s. If the user asks about these "
                       "capabilities, let them know the feature exists but is not "
                       "enabled in this context.\n",
                       session_label, session_disabled);
      if (w > 0 && (size_t)w < buffer_size - len) {
         len += w;
      } else if (w > 0) {
         len = (int)buffer_size - 1;
      }
   }

   return len;
}

struct json_object *llm_history_strip_provider_state(struct json_object *history) {
   if (!history || json_object_get_type(history) != json_type_array) {
      return NULL;
   }

   /* Deep-copy via json-c's deep_copy (single pass, no serializer round-trip)
    * then strip transient fields from each message. */
   struct json_object *copy = NULL;
   if (json_object_deep_copy(history, &copy, NULL) != 0 || !copy) {
      if (copy)
         json_object_put(copy);
      return NULL;
   }
   if (json_object_get_type(copy) != json_type_array) {
      json_object_put(copy);
      return NULL;
   }

   int n = json_object_array_length(copy);
   for (int i = 0; i < n; i++) {
      struct json_object *msg = json_object_array_get_idx(copy, i);
      if (msg && json_object_get_type(msg) == json_type_object) {
         json_object_object_del(msg, "_provider_state");
      }
   }
   return copy;
}

#define VISION_STRIP_TEXT_BUF_MAX 8192

static bool is_vision_content_block(struct json_object *block) {
   struct json_object *type_obj;
   if (!json_object_object_get_ex(block, "type", &type_obj)) {
      return false;
   }
   const char *type_str = json_object_get_string(type_obj);
   return (strcmp(type_str, "image_url") == 0 || strcmp(type_str, "image") == 0);
}

struct json_object *llm_history_strip_vision_content(struct json_object *history) {
   if (!history || json_object_get_type(history) != json_type_array) {
      return NULL;
   }

   int len = json_object_array_length(history);

   bool has_vision = false;
   for (int i = 0; i < len && !has_vision; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *content_obj;
      if (json_object_object_get_ex(msg, "content", &content_obj) &&
          json_object_get_type(content_obj) == json_type_array) {
         int arr_len = json_object_array_length(content_obj);
         for (int j = 0; j < arr_len; j++) {
            struct json_object *elem = json_object_array_get_idx(content_obj, j);
            if (is_vision_content_block(elem)) {
               has_vision = true;
               break;
            }
         }
      }
   }

   if (!has_vision) {
      return json_object_get(history);
   }

   OLOG_INFO("Stripping vision content from history");

   struct json_object *sanitized = json_object_new_array();

   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *content_obj;

      if (json_object_object_get_ex(msg, "content", &content_obj) &&
          json_object_get_type(content_obj) == json_type_array) {
         int arr_len = json_object_array_length(content_obj);
         char text_buffer[VISION_STRIP_TEXT_BUF_MAX] = "";
         size_t text_len = 0;
         bool found_image = false;

         for (int j = 0; j < arr_len; j++) {
            struct json_object *elem = json_object_array_get_idx(content_obj, j);
            struct json_object *type_obj;
            if (json_object_object_get_ex(elem, "type", &type_obj)) {
               const char *type_str = json_object_get_string(type_obj);
               if (strcmp(type_str, "text") == 0) {
                  struct json_object *text_obj;
                  if (json_object_object_get_ex(elem, "text", &text_obj)) {
                     const char *text = json_object_get_string(text_obj);
                     if (text && text_len < sizeof(text_buffer) - 1) {
                        if (text_len > 0) {
                           text_buffer[text_len++] = ' ';
                        }
                        size_t copy_len = strlen(text);
                        if (text_len + copy_len >= sizeof(text_buffer)) {
                           copy_len = sizeof(text_buffer) - text_len - 1;
                        }
                        memcpy(text_buffer + text_len, text, copy_len);
                        text_len += copy_len;
                        text_buffer[text_len] = '\0';
                     }
                  }
               } else if (is_vision_content_block(elem)) {
                  found_image = true;
               }
            }
         }

         struct json_object *new_msg = json_object_new_object();
         struct json_object *role_obj;
         if (json_object_object_get_ex(msg, "role", &role_obj)) {
            json_object_object_add(new_msg, "role", json_object_get(role_obj));
         }

         if (found_image) {
            if (text_len > 0) {
               char combined[VISION_STRIP_TEXT_BUF_MAX + 128];
               snprintf(combined, sizeof(combined), "%s [An image was shared earlier]",
                        text_buffer);
               json_object_object_add(new_msg, "content", json_object_new_string(combined));
            } else {
               json_object_object_add(new_msg, "content",
                                      json_object_new_string("[An image was shared here]"));
            }
         } else {
            json_object_object_add(new_msg, "content", json_object_new_string(text_buffer));
         }

         json_object_array_add(sanitized, new_msg);
      } else {
         json_object_array_add(sanitized, json_object_get(msg));
      }
   }

   return sanitized;
}

void llm_tool_response_free(llm_tool_response_t *response) {
   if (response) {
      if (response->text) {
         free(response->text);
         response->text = NULL;
      }
      if (response->thinking_content) {
         free(response->thinking_content);
         response->thinking_content = NULL;
      }
      if (response->thinking_signature) {
         free(response->thinking_signature);
         response->thinking_signature = NULL;
      }
      if (response->response_id) {
         free(response->response_id);
         response->response_id = NULL;
      }
      if (response->provider_state_json) {
         free(response->provider_state_json);
         response->provider_state_json = NULL;
      }
   }
}

/* =============================================================================
 * Duplicate Tool Call Detection
 * ============================================================================= */

/* Maximum messages to check for duplicate tool calls (performance optimization) */
#define DUPLICATE_CHECK_LOOKBACK 10

/**
 * @brief Check OpenAI-format history for duplicate tool call
 */
static bool is_duplicate_in_openai_history(struct json_object *history,
                                           const char *tool_name,
                                           const char *tool_args,
                                           int min_idx) {
   int len = json_object_array_length(history);

   for (int i = len - 1; i >= min_idx; i--) {
      json_object *msg = json_object_array_get_idx(history, i);
      if (!msg)
         continue;

      json_object *role_obj;
      if (!json_object_object_get_ex(msg, "role", &role_obj))
         continue;

      const char *role = json_object_get_string(role_obj);
      if (!role || strcmp(role, "assistant") != 0)
         continue;

      json_object *tool_calls;
      if (!json_object_object_get_ex(msg, "tool_calls", &tool_calls))
         continue;
      if (!json_object_is_type(tool_calls, json_type_array))
         continue;

      int tc_len = json_object_array_length(tool_calls);
      for (int j = 0; j < tc_len; j++) {
         json_object *tc = json_object_array_get_idx(tool_calls, j);
         if (!tc)
            continue;

         json_object *func;
         if (!json_object_object_get_ex(tc, "function", &func))
            continue;

         json_object *name_obj;
         if (!json_object_object_get_ex(func, "name", &name_obj))
            continue;

         const char *prev_name = json_object_get_string(name_obj);
         if (!prev_name || strcmp(prev_name, tool_name) != 0)
            continue;

         json_object *args_obj;
         if (json_object_object_get_ex(func, "arguments", &args_obj)) {
            const char *prev_args = json_object_get_string(args_obj);
            bool args_match = false;
            if ((!prev_args || prev_args[0] == '\0') && (!tool_args || tool_args[0] == '\0')) {
               args_match = true;
            } else if (prev_args && tool_args && strcmp(prev_args, tool_args) == 0) {
               args_match = true;
            }

            if (args_match) {
               return true;
            }
         }
      }
   }
   return false;
}

/**
 * @brief Check Claude-format history for duplicate tool call
 */
static bool is_duplicate_in_claude_history(struct json_object *history,
                                           const char *tool_name,
                                           const char *tool_args,
                                           int min_idx) {
   int len = json_object_array_length(history);

   for (int i = len - 1; i >= min_idx; i--) {
      json_object *msg = json_object_array_get_idx(history, i);
      if (!msg)
         continue;

      json_object *role_obj;
      if (!json_object_object_get_ex(msg, "role", &role_obj))
         continue;

      const char *role = json_object_get_string(role_obj);
      if (!role || strcmp(role, "assistant") != 0)
         continue;

      json_object *content_obj;
      if (!json_object_object_get_ex(msg, "content", &content_obj))
         continue;
      if (!json_object_is_type(content_obj, json_type_array))
         continue;

      int arr_len = json_object_array_length(content_obj);
      for (int j = 0; j < arr_len; j++) {
         json_object *block = json_object_array_get_idx(content_obj, j);
         if (!block)
            continue;

         json_object *type_obj;
         if (!json_object_object_get_ex(block, "type", &type_obj))
            continue;

         const char *type_str = json_object_get_string(type_obj);
         if (!type_str || strcmp(type_str, "tool_use") != 0)
            continue;

         json_object *name_obj;
         if (!json_object_object_get_ex(block, "name", &name_obj))
            continue;

         const char *prev_name = json_object_get_string(name_obj);
         if (!prev_name || strcmp(prev_name, tool_name) != 0)
            continue;

         /* Claude stores input as object, compare JSON string representation */
         json_object *input_obj;
         if (json_object_object_get_ex(block, "input", &input_obj)) {
            const char *prev_args = json_object_to_json_string(input_obj);
            bool args_match = false;
            if ((!prev_args || prev_args[0] == '\0') && (!tool_args || tool_args[0] == '\0')) {
               args_match = true;
            } else if (prev_args && tool_args && strcmp(prev_args, tool_args) == 0) {
               args_match = true;
            }

            if (args_match) {
               return true;
            }
         }
      }
   }
   return false;
}

/* Index of the last real user message — the start of the current turn.  A repeat
 * of a tool call from an EARLIER turn is legitimate (the user asked again, or the
 * underlying data changed between turns); only a repeat within THIS turn is the
 * runaway loop the duplicate check guards against.  Claude tool-result messages
 * are role "user" too (content array with a tool_result block) — they are not a
 * turn boundary, so they're skipped.  Returns 0 when no real user message found. */
static int last_real_user_msg_index(struct json_object *history, llm_history_format_t format) {
   int len = json_object_array_length(history);
   for (int i = len - 1; i >= 0; i--) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *role_obj;
      if (!msg || !json_object_object_get_ex(msg, "role", &role_obj))
         continue;
      if (strcmp(json_object_get_string(role_obj), "user") != 0)
         continue;
      if (format == LLM_HISTORY_CLAUDE) {
         struct json_object *content;
         if (json_object_object_get_ex(msg, "content", &content) &&
             json_object_is_type(content, json_type_array)) {
            bool is_tool_result = false;
            int n = json_object_array_length(content);
            for (int j = 0; j < n; j++) {
               struct json_object *blk = json_object_array_get_idx(content, j);
               struct json_object *type_obj;
               if (blk && json_object_object_get_ex(blk, "type", &type_obj) &&
                   strcmp(json_object_get_string(type_obj), "tool_result") == 0) {
                  is_tool_result = true;
                  break;
               }
            }
            if (is_tool_result)
               continue; /* Claude tool result, not a turn boundary */
         }
      }
      return i;
   }
   return 0;
}

bool llm_tools_is_duplicate_call(struct json_object *history,
                                 const char *tool_name,
                                 const char *tool_args,
                                 llm_history_format_t format) {
   if (!history || !tool_name)
      return false;

   /* Non-deterministic actions are exempt: an identical-args repeat is a feature
    * (e.g. calculator "random" — "pick another number"), not an infinite loop.
    * The registry owns both the args key that carries the action (usually
    * "action", but e.g. switch_llm uses "target") and which action values a tool
    * declares repeatable. */
   if (tool_args && tool_args[0] != '\0') {
      const char *action_key = tool_registry_get_action_param_name(tool_name);
      if (action_key) {
         struct json_object *parsed = json_tokener_parse(tool_args);
         if (parsed) {
            struct json_object *action_obj;
            if (json_object_object_get_ex(parsed, action_key, &action_obj)) {
               const char *action = json_object_get_string(action_obj);
               if (action && tool_registry_action_is_repeatable(tool_name, action)) {
                  json_object_put(parsed);
                  return false;
               }
            }
            json_object_put(parsed);
         }
      }
   }

   int len = json_object_array_length(history);
   int min_idx = len - DUPLICATE_CHECK_LOOKBACK;
   if (min_idx < 0) {
      min_idx = 0;
   }
   /* Confine the scan to the current turn so a user-requested repeat (or a
    * re-read of data that changed since the last turn) isn't blocked as a dup —
    * only same-turn loops are caught. */
   int turn_start = last_real_user_msg_index(history, format);
   if (turn_start > min_idx) {
      min_idx = turn_start;
   }

   bool is_dup;
   if (format == LLM_HISTORY_CLAUDE) {
      is_dup = is_duplicate_in_claude_history(history, tool_name, tool_args, min_idx);
   } else {
      is_dup = is_duplicate_in_openai_history(history, tool_name, tool_args, min_idx);
   }

   if (is_dup) {
      OLOG_INFO("Duplicate tool call detected: %s with args %s", tool_name,
                tool_args ? tool_args : "(none)");
   }
   return is_dup;
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

   /* Check if all tools set should_respond=false (callback handled output).
    * Unlike skip_followup, this is history-safe — results can be appended.
    * Note: only evaluated when skip_followup is false, making the two paths
    * mutually exclusive in llm_tool_iteration_loop(). */
   if (!ctx->skip_followup && results && results->count > 0) {
      bool all_silent = true;
      for (int i = 0; i < results->count; i++) {
         if (results->results[i].should_respond) {
            all_silent = false;
            break;
         }
      }
      ctx->all_silent = all_silent;
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
