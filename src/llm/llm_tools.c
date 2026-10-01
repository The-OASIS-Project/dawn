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
#include "core/hash_util.h"
#include "core/ocp_helpers.h"
#include "core/research_allowlist.h"
#include "core/session_manager.h"
#include "core/session_prefix.h"
#include "core/worker_pool.h"
#include "dawn.h"
#include "dawn_error.h"
#include "llm/llm_capabilities.h"
#include "llm/llm_claude_format.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_context.h"
#include "llm/llm_context_text.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_interface.h"
#include "llm/llm_tools_internal.h"
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

tool_definition_t llm_tools_table[LLM_TOOLS_MAX_TOOLS];
int llm_tools_count = 0;
static int s_enabled_count = 0; /* Cached enabled count, updated by llm_tools_refresh() */
bool llm_tools_ready = false;

/* Thread safety for tool state modifications */
pthread_mutex_t llm_tools_mutex = PTHREAD_MUTEX_INITIALIZER;
_Atomic uint64_t llm_tools_generation;

/* Cached token estimates (-1 = needs recalculation) */
static int s_token_estimate_local = -1;
static int s_token_estimate_remote = -1;

/* Thread-local pointer to current resolved config.
 * Set by llm_tools_set_current_config() before LLM calls so that
 * llm_tools_enabled() can check session-specific suppress_tools. */
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

/* The tool loop finishes its batch's results itself (llm_tools_finish_result,
 * after its view stage): execute_all asks for the next execute's finish to
 * wait (tl_defer_request), and that execute alone defers it
 * (tl_defer_current); a call a tool makes from inside (a plan's steps)
 * finishes as always. */
static __thread bool tl_defer_request;
static __thread bool tl_defer_current;

/**
 * @brief Notify registered callback about tool execution
 */
static void notify_tool_execution(const char *tool_name,
                                  const char *tool_args,
                                  const char *result,
                                  bool success) {
   if (result != NULL && tl_defer_current) {
      return; /* the tool loop's batch: sent when the result is finished */
   }
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
   session_t *session;  /* Session context to propagate to spawned thread */
   uint64_t turn_token; /* ...and the turn it works for (its LLM settings) */
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
   "email",              /* Shared pending draft/trash state; and concurrent Gmail
                          * operations (e.g. two digests + a search in one turn)
                          * produced inconsistent reply-enrichment results — the
                          * per-op sent-searches interfere. A single email op is
                          * already internally serial, so this only serializes the
                          * multi-call case, at no cost to normal use. */
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
   for (int i = 0; i < llm_tools_count; i++) {
      if (strcmp(llm_tools_table[i].name, tool_name) == 0) {
         return llm_tools_table[i].parallel_safe;
      }
   }
   /* Unknown tool - assume sequential for safety */
   return false;
}

/* Set while this thread runs the tools the LLM called (llm_tools_execute_all
 * and its workers): a plan's steps run inside, so they carry it only when the
 * model started the plan (llm_tools_executing()). */
static __thread bool tl_executing;

bool llm_tools_executing(void) {
   return tl_executing;
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
   session_set_turn_token(task->turn_token);
   tl_executing = true; /* the LLM's call, on its worker */

   tl_defer_request = true; /* the loop finishes it (llm_tools_finish_result) */
   task->return_code = llm_tools_execute(task->call, task->result);
   tl_executing = false;

   /* Clear context before thread exit */
   session_set_turn_token(0);
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
   for (int i = 0; i < llm_tools_count; i++) {
      if (strcmp(llm_tools_table[i].name, name) == 0) {
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

   if (llm_tools_count >= LLM_TOOLS_MAX_TOOLS) {
      OLOG_ERROR("Maximum tool count (%d) reached, skipping '%s'", LLM_TOOLS_MAX_TOOLS, meta->name);
      return;
   }

   tool_definition_t *t = &llm_tools_table[llm_tools_count++];
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
   if (llm_tools_ready) {
      return;
   }

   llm_tools_count = 0;

   /* Generate tools from ALL registry entries (including dangerous tools that
    * default to disabled). This ensures they appear in the WebUI tools config
    * so users can opt in. The enabled_local/remote flags control LLM access. */
   tool_registry_foreach(generate_tool_from_treg, NULL);
   atomic_fetch_add(&llm_tools_generation, 1);

   llm_tools_ready = true;
   OLOG_INFO("Initialized %d LLM tools from tool_registry", llm_tools_count);

   /* Refresh availability based on current config */
   llm_tools_refresh();

   /* Log which tools are enabled */
   char enabled_list[512] = "";
   int offset = 0;
   for (int i = 0; i < llm_tools_count && offset < 500; i++) {
      if (llm_tools_table[i].enabled) {
         offset += snprintf(enabled_list + offset, 512 - offset, "%s%s", offset > 0 ? ", " : "",
                            llm_tools_table[i].name);
      }
   }
   OLOG_INFO("Enabled tools: %s", enabled_list);
}

/* =============================================================================
 * Tool Availability Refresh
 * ============================================================================= */

void llm_tools_refresh(void) {
   if (!llm_tools_ready) {
      return;
   }

   /* Check if HUD/helmet hardware is available via status keepalive */
   bool hud_available = component_status_is_hud_online();

   for (int i = 0; i < llm_tools_count; i++) {
      tool_definition_t *t = &llm_tools_table[i];

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

      /* Deep research is opt-in (costs real tokens/time): the [research] master
       * switch gates the NATIVE schema here.  is_available() alone is NOT enough
       * — it is consulted only in the armor block above and on the legacy
       * <command> path, so a non-armor tool must be gated by name like search /
       * memory or it stays advertised.  deep_research_callback also refuses at
       * execution as a backstop for any non-schema path. */
      if (strcmp(t->name, "deep_research") == 0) {
         t->enabled = g_config.research.enabled;
      }

      /* Stocks (Schwab) requires the OAuth client to be configured — gate the
       * native schema by name (is_available is consulted only for armor tools,
       * per the deep_research note above).  The callback refuses per-user with a
       * "run dawn-admin schwab auth" message when the account isn't linked. */
      if (strcmp(t->name, "stocks") == 0) {
         t->enabled = (g_secrets.schwab_client_id[0] != '\0');
      }
   }

   /* Update cached enabled count (total capability-enabled) */
   s_enabled_count = 0;
   for (int i = 0; i < llm_tools_count; i++) {
      if (llm_tools_table[i].enabled) {
         s_enabled_count++;
      }
   }

   OLOG_INFO("Refreshed tool availability: %d enabled (HUD %s)", s_enabled_count,
             hud_available ? "available" : "unavailable");
}

void llm_tools_cleanup(void) {
   llm_tools_filter_release();
   llm_tools_count = 0;
   llm_tools_ready = false;
   atomic_fetch_add(&llm_tools_generation, 1);
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
struct json_object *llm_tools_parameters_schema(const tool_definition_t *tool) {
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
const char *llm_tools_effective_description(const tool_definition_t *t) {
   const tool_metadata_t *meta = tool_registry_lookup(t->name);
   return (meta && meta->description) ? meta->description : t->description;
}

/* =============================================================================
 * Schema Generation - OpenAI Format
 * ============================================================================= */

struct json_object *llm_tools_get_openai_format(void) {
   if (!llm_tools_ready || llm_tools_get_enabled_count() == 0) {
      return NULL;
   }

   struct json_object *tools_array = json_object_new_array();

   for (int i = 0; i < llm_tools_count; i++) {
      const tool_definition_t *t = &llm_tools_table[i];
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
                             json_object_new_string(llm_tools_effective_description(t)));
      json_object_object_add(function, "parameters", llm_tools_parameters_schema(t));

      json_object_object_add(tool_obj, "function", function);
      json_object_array_add(tools_array, tool_obj);
   }

   return tools_array;
}

/* =============================================================================
 * Schema Generation - Claude Format
 * ============================================================================= */

struct json_object *llm_tools_get_claude_format(void) {
   if (!llm_tools_ready || llm_tools_get_enabled_count() == 0) {
      return NULL;
   }

   struct json_object *tools_array = json_object_new_array();

   for (int i = 0; i < llm_tools_count; i++) {
      const tool_definition_t *t = &llm_tools_table[i];
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
                             json_object_new_string(llm_tools_effective_description(t)));
      json_object_object_add(tool_obj, "input_schema", llm_tools_parameters_schema(t));

      json_object_array_add(tools_array, tool_obj);
   }

   return tools_array;
}

/* =============================================================================
 * Schema Generation - Filtered by Session Type
 * ============================================================================= */

/* =============================================================================
 * Tool Configuration API
 * ============================================================================= */

int llm_tools_get_all(tool_info_t *out, int max_tools) {
   if (!out || max_tools <= 0 || !llm_tools_ready) {
      return 0;
   }

   int count = 0;
   for (int i = 0; i < llm_tools_count && count < max_tools; i++) {
      const tool_definition_t *t = &llm_tools_table[i];
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
   if (!tool_name || !llm_tools_ready) {
      return 1; /* FAILURE - invalid args or not initialized */
   }

   pthread_mutex_lock(&llm_tools_mutex);
   for (int i = 0; i < llm_tools_count; i++) {
      if (strcmp(llm_tools_table[i].name, tool_name) == 0) {
         llm_tools_table[i].enabled_local = enabled_local;
         llm_tools_table[i].enabled_remote = enabled_remote;

         /* Invalidate token estimate cache */
         s_token_estimate_local = -1;
         s_token_estimate_remote = -1;

         pthread_mutex_unlock(&llm_tools_mutex);
         OLOG_INFO("Tool '%s' enable state updated: local=%d, remote=%d", tool_name, enabled_local,
                   enabled_remote);
         return 0; /* SUCCESS */
      }
   }
   pthread_mutex_unlock(&llm_tools_mutex);

   OLOG_WARNING("Tool '%s' not found", tool_name);
   return 1; /* FAILURE - tool not found */
}

bool llm_tools_is_device_enabled(const char *device_name, bool is_remote) {
   if (!device_name || !llm_tools_ready) {
      return false;
   }

   pthread_mutex_lock(&llm_tools_mutex);

   /* Search tools by name - tools use device_string for device name mapping */
   for (int i = 0; i < llm_tools_count; i++) {
      /* Check both the tool name and the device_string (underlying device) */
      if (strcmp(llm_tools_table[i].name, device_name) == 0 ||
          (llm_tools_table[i].device_name &&
           strcmp(llm_tools_table[i].device_name, device_name) == 0)) {
         /* Check if the tool is enabled at all */
         if (!llm_tools_table[i].enabled) {
            pthread_mutex_unlock(&llm_tools_mutex);
            return false;
         }

         /* Check session-specific enable state */
         bool enabled = is_remote ? llm_tools_table[i].enabled_remote
                                  : llm_tools_table[i].enabled_local;
         pthread_mutex_unlock(&llm_tools_mutex);
         return enabled;
      }
   }

   pthread_mutex_unlock(&llm_tools_mutex);

   /* Device not found in tools array. Only devices with tool blocks in
    * commands_config_nuevo.json become tools and should appear in prompts.
    * Plain MQTT devices (like armor_display without a tool block) are NOT
    * controllable via LLM and should return false for prompt filtering. */
   return false;
}

/* Mark set[i]=true for each registered tool named in names[0..count). */
static void build_tool_set(bool *set, const char names[][LLM_TOOL_NAME_MAX], int count) {
   for (int j = 0; j < count; j++) {
      for (int i = 0; i < llm_tools_count; i++) {
         if (strcmp(llm_tools_table[i].name, names[j]) == 0) {
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
   if (!llm_tools_ready) {
      OLOG_WARNING("llm_tools_apply_config called before initialization - config ignored");
      return;
   }
   if (!cfg)
      return;

   /* Build membership sets for each of the four lists (O(n*m), tiny m). Reads
    * llm_tools_count / llm_tools_table[].name OUTSIDE llm_tools_mutex: safe because this runs
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

   pthread_mutex_lock(&llm_tools_mutex);
   for (int i = 0; i < llm_tools_count; i++) {
      llm_tools_table[i].enabled_local = resolve_tool_enabled(llm_tools_table[i].dangerous,
                                                              en_local[i], dis_local[i],
                                                              cfg->local_enabled_configured,
                                                              cfg->local_disabled_configured);
      llm_tools_table[i].enabled_remote = resolve_tool_enabled(llm_tools_table[i].dangerous,
                                                               en_remote[i], dis_remote[i],
                                                               cfg->remote_enabled_configured,
                                                               cfg->remote_disabled_configured);
   }

   /* Invalidate token estimate cache */
   s_token_estimate_local = -1;
   s_token_estimate_remote = -1;
   pthread_mutex_unlock(&llm_tools_mutex);

   OLOG_INFO("Applied tool config: local=%d tools, remote=%d tools",
             llm_tools_get_enabled_count_filtered(false),
             llm_tools_get_enabled_count_filtered(true));
}

int llm_tools_get_enabled_count_filtered(bool is_remote_session) {
   if (!llm_tools_ready) {
      return 0;
   }

   int count = 0;
   for (int i = 0; i < llm_tools_count; i++) {
      if (llm_tools_enabled_for_session(&llm_tools_table[i], is_remote_session)) {
         count++;
      }
   }
   return count;
}

int llm_tools_estimate_tokens(bool is_remote_session) {
   /* The per-session job-tool mask (llm_tools_enabled_for_session) makes the built
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
   pthread_mutex_lock(&llm_tools_mutex);
   s_token_estimate_local = -1;
   s_token_estimate_remote = -1;
   pthread_mutex_unlock(&llm_tools_mutex);
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

/* One argument into the (action, value, device) call: the action and device
 * as sent, the value escaped when @p packed, a custom field appended as
 * ::field::value.  The names of the params that fill the action, value and
 * device are noted for the flat-args fallback.  FAILURE (with @p result set)
 * when a custom field doesn't fit. */
static int pack_param(const tool_metadata_t *meta,
                      const treg_param_t *param,
                      const char *val_str,
                      bool packed,
                      char *action_name,
                      size_t action_len,
                      char *value_buf,
                      size_t value_len,
                      char *device_name,
                      size_t device_len,
                      const char **action_param_name,
                      const char **value_param_name,
                      const char **device_param_name,
                      tool_result_t *result) {
   switch (param->maps_to) {
      case TOOL_MAPS_TO_ACTION:
         *action_param_name = param->name;
         if (val_str) {
            safe_strncpy(action_name, val_str, action_len);
         }
         return SUCCESS;
      case TOOL_MAPS_TO_VALUE:
         *value_param_name = param->name;
         if (val_str) {
            if (packed) {
               tool_value_escape(val_str, strlen(val_str), value_buf, value_len);
            } else {
               safe_strncpy(value_buf, val_str, value_len);
            }
         }
         return SUCCESS;
      case TOOL_MAPS_TO_DEVICE:
         *device_param_name = param->name;
         if (val_str) {
            safe_strncpy(device_name, val_str, device_len);
         }
         return SUCCESS;
      case TOOL_MAPS_TO_CUSTOM:
         break;
   }
   /* A custom field, appended as ::field_name::value.  An ARRAY's val_str is
    * its serialized JSON (json_object_get_string on a non-string). */
   if (!val_str) {
      return SUCCESS;
   }
   if (!param->field_name || !param->field_name[0]) {
      if (value_buf[0] == '\0') {
         /* No field name: the value itself, escaped like a base so it can't
          * pose as the fields appended after it. */
         if (packed) {
            tool_value_escape(val_str, strlen(val_str), value_buf, value_len);
         } else {
            safe_strncpy(value_buf, val_str, value_len);
         }
      }
      return SUCCESS;
   }
   /* A cut-off field would reach the callback as a different value (a
    * truncated array parses as invalid JSON), so it is refused instead. */
   const size_t cur_len = strlen(value_buf);
   const size_t remaining = value_len - cur_len;
   const size_t name_len = strlen(param->field_name);
   const size_t val_len = tool_value_escape(val_str, strlen(val_str), NULL, 0);
   /* 4 = the "::" x2 delimiters; the strict >= leaves room for the NUL. */
   if (name_len + val_len + 4 >= remaining) {
      OLOG_ERROR("Tool '%s' param '%s' too large to encode (%zu >= %zu)", meta->name,
                 param->field_name, name_len + val_len + 4, remaining);
      snprintf(result->result, LLM_TOOLS_RESULT_LEN,
               "Error: too many items for '%s' — try fewer at a time", param->field_name);
      result->success = false;
      return FAILURE;
   }
   char *at = value_buf + cur_len;
   memcpy(at, "::", 2);
   memcpy(at + 2, param->field_name, name_len);
   memcpy(at + 2 + name_len, "::", 2);
   tool_value_escape(val_str, strlen(val_str), at + 4 + name_len, remaining - 4 - name_len);
   return SUCCESS;
}

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
      /* Name the budget and the recovery ACTION.  The old wording — "split the
       * input into smaller pieces (e.g. save a long document in sections)" — was
       * true but unusable twice over: it gave no size to aim at, so the retry was
       * a guess that overshot again; and "in sections" reads as "make more
       * documents", so a long report came back as 'Part 1'/'Part 2' instead of
       * one document. Observed live: two discarded ~7k-token generations before a
       * third happened to fit. Everything here is generic — 'append' is a common
       * action name, and a tool without one simply ignores the hint. */
      snprintf(result->result, LLM_TOOLS_RESULT_LEN,
               "Error: the arguments to '%s' totalled over %d bytes and were cut off, so the call "
               "did NOT run and nothing was saved. That limit covers the whole argument object — "
               "the field names and the JSON escaping, not just your text — so aim for roughly "
               "half of it per call. Do not retry the same call: send the first part now, then "
               "add each remaining part with this tool's 'append' action, targeting the SAME "
               "label. Creating 'Part 1'/'Part 2' entries instead leaves the content split across "
               "separate records.",
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

   /* A tool with CUSTOM params gets its value packed as base::field::value;
    * each part is escaped (tool_value_escape) so its own "::" can't be read as a
    * separator.  A tool without them gets its value as sent. */
   bool packed = false;
   for (int i = 0; i < meta->param_count; i++) {
      if (meta->params[i].maps_to == TOOL_MAPS_TO_CUSTOM && meta->params[i].field_name &&
          meta->params[i].field_name[0]) {
         packed = true;
         break;
      }
   }

   /* The base first, then the custom fields after it, whatever the params'
    * declared order. */
   for (int pass = 0; pass < 2; pass++) {
      for (int i = 0; i < meta->param_count; i++) {
         const treg_param_t *param = &meta->params[i];
         if ((param->maps_to == TOOL_MAPS_TO_CUSTOM) != (pass == 1)) {
            continue;
         }
         struct json_object *val_obj = NULL;
         if (args) {
            json_object_object_get_ex(args, param->name, &val_obj);
         }
         const char *val_str = val_obj ? json_object_get_string(val_obj) : NULL;
         if (pack_param(meta, param, val_str, packed, action_name, sizeof(action_name), value_buf,
                        sizeof(value_buf), device_name, sizeof(device_name), &action_param_name,
                        &value_param_name, &device_param_name, result) != SUCCESS) {
            json_object_put(args);
            return 1;
         }
      }
   }

   /* Fallback: if value_buf is empty and the LLM sent a flat JSON (no "arguments" key),
    * collect all non-extracted fields into a JSON object as the value.
    * This handles LLMs that flatten {"action":"create","type":"timer",...}
    * instead of nesting {"action":"create","arguments":"{\"type\":\"timer\",...}"}. */
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
         if (packed) {
            tool_value_escape(remaining_str, strlen(remaining_str), value_buf, sizeof(value_buf));
         } else {
            safe_strncpy(value_buf, remaining_str, sizeof(value_buf));
         }
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

      /* Capture the tool's self-reported hard-failure mark BEFORE stripping it: `success` stays
       * true (the marked text still flows to the LLM as description), but the pill must red. This
       * is the ONE site where a structurally-fine call is still a confirmed failure.
       *
       * ASSUMPTION (currently holds; document so it isn't silently load-bearing): marker capture
       * lives ONLY here on the direct-callback path. The other dispatch paths — command_execute
       * fallback (below), mqtt_only, viewing — do NOT capture the mark, and command_execute
       * additionally strips it and forces success=true (command_executor.c), so a marker returned
       * through those paths is unrecoverable and renders NEUTRAL, not red. Safe (a MISSED red,
       * never a false one) and unreachable today: every marker-emitting tool
       * (attention/stat/suit/search/ weather) is a modular direct-callback tool that hits THIS
       * site. If a legacy MQTT/command device ever adopts TOOL_RESULT_ERROR_MARK, surface the
       * verdict in cmd_exec_result_t and OR it in on those paths too. */
      bool marked = tool_result_is_error(cb_result);

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
      result->is_error = marked; /* structurally OK, but the tool flagged a hard failure */
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

/* Whether a string in @p obj (its keys too) carries @p hex. */
static bool json_carries_secret(struct json_object *obj, const char *hex, int depth) {
   if (!obj) {
      return false;
   }
   if (depth > 16) {
      return true; /* deeper than any real call's arguments: refused */
   }
   switch (json_object_get_type(obj)) {
      case json_type_string:
         return llm_context_carries_secret(json_object_get_string(obj), hex);
      case json_type_array:
         for (size_t i = 0; i < json_object_array_length(obj); i++) {
            if (json_carries_secret(json_object_array_get_idx(obj, i), hex, depth + 1)) {
               return true;
            }
         }
         return false;
      case json_type_object: {
         json_object_object_foreach(obj, key, val) {
            if (llm_context_carries_secret(key, hex) || json_carries_secret(val, hex, depth + 1)) {
               return true;
            }
         }
         return false;
      }
      default:
         return false;
   }
}

/* Whether @p call's arguments carry the conversation's tag secret: in the
 * text as sent, or in any decoded string of it. */
static bool call_carries_tag(const tool_call_t *call) {
   session_t *ctx = session_get_command_context();
   char tag[LLM_CONTEXT_TAG_MAX];
   char hex[9];
   if (!ctx || !call->arguments || !session_prefix_tag(ctx, tag, sizeof(tag)) ||
       !llm_context_tag_secret(tag, hex)) {
      return false;
   }
   if (llm_context_carries_secret(call->arguments, hex)) {
      return true;
   }
   struct json_object *args = json_tokener_parse(call->arguments);
   const bool carried = json_carries_secret(args, hex, 0);
   json_object_put(args);
   return carried;
}

/* @p result neutralized (llm_context_neutralize) in place, and the
 * conversation's tag secret masked in it; a result that can't be is replaced
 * by an error rather than passed on as it came. */
static void neutralize_result(const char *tool, tool_result_t *result) {
   session_t *ctx = session_get_command_context();
   if (result->result_extended) {
      result->result_extended = session_prefix_mask_secret(ctx, llm_context_neutralize_owned(
                                                                    result->result_extended));
      if (!result->result_extended) {
         snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Error: out of memory reading the result");
         result->success = false;
         OLOG_ERROR("Tool '%s': out of memory neutralizing its result", tool);
         return;
      }
   }
   char *safe = session_prefix_mask_secret(ctx, llm_context_neutralize(result->result));
   if (safe) {
      safe_strncpy(result->result, safe, LLM_TOOLS_RESULT_LEN);
      utf8_trim_incomplete(result->result);
      free(safe);
   } else {
      snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Error: out of memory reading the result");
      result->success = false;
      OLOG_ERROR("Tool '%s': out of memory neutralizing its result", tool);
   }
}

static int execute_one(const tool_call_t *call, tool_result_t *result) {
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
      result->is_error = true;
      return 1;
   }

   /* The conversation's tag is what marks DAWN's own framing: a call that
    * carries its secret out (a URL, a search, a message; split, spaced or
    * encoded) is refused, so text the model was led to read can't learn it
    * through a tool.  A tripwire: what comes in is defused regardless. */
   if (call_carries_tag(call)) {
      snprintf(result->result, LLM_TOOLS_RESULT_LEN,
               "Refused: the call's arguments carry DAWN's conversation tag, which never leaves "
               "the conversation. Make the call without it.");
      result->success = false;
      result->is_error = true;
      OLOG_WARNING("Refused tool '%s': its arguments carry the conversation tag", call->name);
      notify_tool_execution(call->name, "(withheld: carried the conversation tag)", result->result,
                            false);
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
      pthread_mutex_lock(&llm_tools_mutex);
      for (int i = 0; i < llm_tools_count; i++) {
         if (strcmp(llm_tools_table[i].name, call->name) == 0) {
            enabled = llm_tools_enabled_for_session(&llm_tools_table[i], is_remote);
            break;
         }
      }
      pthread_mutex_unlock(&llm_tools_mutex);

      if (!enabled) {
         snprintf(result->result, LLM_TOOLS_RESULT_LEN,
                  "Tool '%s' is not currently available (capability offline, misconfigured, or "
                  "disabled for this %s session). Let the user know the feature can't be used "
                  "right now.",
                  call->name, is_remote ? "remote" : "local");
         result->success = false;
         /* A refused (unavailable) tool confirmedly did NOT run → red. It's a refusal, not a
          * malfunction, so this is the one path where taste could differ (flip to false for
          * neutral); reds by default because the call did not execute. */
         result->is_error = true;
         result->should_respond = true;
         OLOG_WARNING("Refused tool '%s' — not enabled for %s session", call->name,
                      is_remote ? "remote" : "local");
         notify_tool_execution(call->name, call->arguments, result->result, false);
         return 1;
      }
   }

   int rc = llm_tools_execute_from_treg(call, treg_meta, result);
   /* Backstop: every from_treg path sets `success` explicitly, so folding `!success` in here
    * retroactively covers ALL of them (structural failures — bad args, invalid JSON, encode
    * overflow, command-exec failure) with one line. The direct-callback site sets `is_error`
    * itself for the marked-but-structurally-OK case (success stays true), which this OR preserves.
    */
   result->is_error = result->is_error || !result->success;
   return rc;
}

int llm_tools_execute(const tool_call_t *call, tool_result_t *result) {
   const bool outer = tl_defer_current;
   tl_defer_current = tl_defer_request;
   tl_defer_request = false;
   const int rc = execute_one(call, result);
   /* A result is text from anywhere (a page, a message, a document, an MCP
    * server, the call's own name in an error): what imitates DAWN's framing
    * or carries a tag is defused, on every path.  A batch's result is
    * neutralized when finished, after its view stage (a view is made from the
    * result as it came; what the model sees is neutralized last). */
   if (result && call && !tl_defer_current) {
      neutralize_result(call->name, result);
      result->finished = true;
   }
   tl_defer_current = outer;
   return rc;
}

/* The loop's own call: finished later (llm_tools_finish_result). */
static int execute_deferred(const tool_call_t *call, tool_result_t *result) {
   tl_defer_request = true;
   return llm_tools_execute(call, result);
}

void llm_tools_result_set_content(tool_result_t *result, char *text) {
   if (!result || !text) {
      free(text);
      return;
   }
   free(result->result_extended);
   result->result_extended = NULL;
   const size_t len = strlen(text);
   if (len < LLM_TOOLS_RESULT_LEN) {
      memcpy(result->result, text, len + 1);
      free(text);
   } else {
      result->result_extended = text;
      safe_strncpy(result->result, text, LLM_TOOLS_RESULT_LEN);
      utf8_trim_incomplete(result->result);
   }
}

/* @p header in front of @p result's content; without the memory for it, a
 * fixed notice instead, so a shortened result never reads as whole. */
static void prepend_header(tool_result_t *result, const char *header) {
   const char *body = tool_result_content(result);
   const size_t hlen = strlen(header);
   const size_t blen = strlen(body);
   char *framed = malloc(hlen + blen + 1);
   if (!framed) {
      OLOG_ERROR("Tool result: out of memory framing a view; its header is replaced");
      free(result->result_extended);
      result->result_extended = NULL;
      snprintf(result->result, LLM_TOOLS_RESULT_LEN,
               "[Tool result shortened: the rest was left out.]");
      return;
   }
   memcpy(framed, header, hlen);
   memcpy(framed + hlen, body, blen + 1);
   llm_tools_result_set_content(result, framed);
}

void llm_tools_finish_result(const tool_call_t *call, tool_result_t *result, const char *header) {
   if (!call || !result || result->finished) {
      return;
   }
   neutralize_result(call->name, result);
   /* DAWN's own frame goes on after the result is neutralized (the
    * neutralizer defuses an imitation of it in the result). */
   if (header && header[0]) {
      prepend_header(result, header);
   }
   result->is_error = result->is_error || !result->success;
   result->finished = true;
   /* A call to no tool was never announced, so it isn't completed either. */
   if (!tool_registry_find(call->name)) {
      return;
   }
   const bool outer = tl_defer_current;
   tl_defer_current = false;
   notify_tool_execution(call->name,
                         call_carries_tag(call) ? "(withheld: carried the conversation tag)"
                                                : call->arguments,
                         tool_result_content(result), result->success);
   tl_defer_current = outer;
}

static int execute_all_impl(const tool_call_list_t *calls, tool_result_list_t *results) {
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
      if (execute_deferred(&calls->calls[0], &results->results[0]) != 0) {
         failures++;
      }
      results->count = 1;
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
         tasks[i].turn_token = session_turn_token();
         tasks[i].return_code = 0;

         int rc = pthread_create(&threads[i], &thread_attr, tool_exec_thread, &tasks[i]);
         if (rc == 0) {
            thread_spawned[i] = true;
            OLOG_INFO("Spawned thread %d for tool '%s'", i, calls->calls[idx].name);
         } else {
            /* Fallback to sequential if thread creation fails */
            OLOG_WARNING("pthread_create failed for tool '%s' (error=%d), executing sequentially",
                         calls->calls[idx].name, rc);
            tasks[i].return_code = execute_deferred(tasks[i].call, tasks[i].result);
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
      if (execute_deferred(&calls->calls[idx], &results->results[idx]) != 0) {
         failures++;
      }
   }

   results->count = total_calls;

   clock_gettime(CLOCK_MONOTONIC, &end_time);
   long elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000 +
                     (end_time.tv_nsec - start_time.tv_nsec) / 1000000;
   OLOG_INFO("Tool execution: %d tools completed in %ldms (%d parallel, %d sequential)",
             total_calls, elapsed_ms, parallel_count, sequential_count);

   return failures > 0 ? 1 : 0;
}

int llm_tools_execute_all(const tool_call_list_t *calls,
                          tool_result_list_t *results,
                          llm_tools_batch_finish_fn finish,
                          void *userdata) {
   /* The LLM's own calls (a plan it starts runs its steps in here too). */
   const bool outer = tl_executing;
   tl_executing = true;
   const int rc = execute_all_impl(calls, results);
   tl_executing = outer;
   if (!calls || !results) {
      return rc;
   }
   if (finish) {
      finish(calls, results, userdata);
   }
   /* Nothing leaves here unfinished: what the stage missed is finished as it
    * came, never passed on raw. */
   for (int i = 0; i < results->count; i++) {
      if (!results->results[i].finished) {
         if (finish) {
            OLOG_ERROR("Tool '%s': its result wasn't finished by the batch; finishing it",
                       calls->calls[i].name);
         }
         llm_tools_finish_result(&calls->calls[i], &results->results[i], NULL);
      }
   }
   /* Transition from "tool_call" to "thinking" for the follow-up LLM call,
    * after the results were announced. */
   session_t *status_session = session_get_command_context();
   if (status_session && results->count > 0) {
      webui_send_state_with_detail(status_session, "thinking", "Processing results...");
   }
   return rc;
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
 * Current config and thinking (Thread-Local)
 * ============================================================================= */

void llm_tools_set_current_config(const llm_resolved_config_t *config) {
   tl_current_config = config;
}

const char *llm_get_current_thinking_mode(void) {
   /* Priority: thread-local config > global config > default.  The value is
    * what was picked; requests resolve it against the model
    * (llm_thinking_resolve_current).  Legacy "auto" means reasoning on. */
   if (tl_current_config && tl_current_config->thinking_mode[0] != '\0') {
      return tl_current_config->thinking_mode;
   }
   if (g_config.llm.thinking.mode[0] != '\0') {
      return g_config.llm.thinking.mode;
   }
   return LLM_THINKING_MODE_DEFAULT;
}

const char *llm_get_current_reasoning_effort(void) {
   /* Priority: thread-local config > global config > default */
   if (tl_current_config && tl_current_config->reasoning_effort[0] != '\0') {
      return tl_current_config->reasoning_effort;
   }
   if (g_config.llm.thinking.reasoning_effort[0] != '\0') {
      return g_config.llm.thinking.reasoning_effort;
   }
   return LLM_REASONING_EFFORT_DEFAULT;
}

int llm_budget_tokens_for_effort(const char *effort) {
   /* The level's size (the capability module owns the levels), capped for
    * the current model below. */
   int budget = llm_thinking_budget_size(effort);

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
   if (llm_tools_suppressed()) {
      return false;
   }

   /* Per-call suppression: internal callers (extraction, compaction,
    * silent-observe, briefings) force tools off via the resolved config.
    * Priority: explicit config > thread-local config. */
   const llm_resolved_config_t *effective_config = config ? config : tl_current_config;
   if (effective_config && effective_config->suppress_tools) {
      return false;
   }

   /* Global on/off switch for native tool calling. */
   if (!g_config.llm.tools.enabled) {
      return false;
   }

   /* Check that tools system is initialized with enabled tools */
   if (!llm_tools_ready || llm_tools_get_enabled_count() == 0) {
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
   if (!llm_tools_ready) {
      return 0;
   }
   return s_enabled_count;
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
      if (response->blocks) {
         json_object_put(response->blocks);
         response->blocks = NULL;
      }
      if (response->response_id) {
         free(response->response_id);
         response->response_id = NULL;
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
