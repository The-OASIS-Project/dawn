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
#include "core/command_router.h"
#include "core/worker_pool.h"
#include "dawn.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_interface.h"
#include "logging.h"
#include "mosquitto_comms.h"

/* Forward declarations for utility functions from mosquitto_comms.c */
extern unsigned char *read_file(const char *filename, size_t *length);
extern char *base64_encode(const unsigned char *buffer, size_t length);

/* Timeout for MQTT command responses (10 seconds) */
#define VIEWING_MQTT_TIMEOUT_MS 10000

/* =============================================================================
 * Static Tool Definitions
 * ============================================================================= */

static tool_definition_t s_tools[LLM_TOOLS_MAX_TOOLS];
static int s_tool_count = 0;
static bool s_initialized = false;

/* =============================================================================
 * Pending Vision Data (for viewing tool)
 * ============================================================================= */

static char *s_pending_vision_image = NULL;
static size_t s_pending_vision_size = 0;
static pthread_mutex_t s_pending_vision_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Forward declaration for atomic pending vision update */
static void set_pending_vision_locked(char *image, size_t size);

/* =============================================================================
 * String Safety Helpers
 * ============================================================================= */

/**
 * @brief Safe string copy with guaranteed null-termination
 *
 * Unlike strncpy, this always null-terminates the destination buffer.
 *
 * @param dest Destination buffer
 * @param src Source string
 * @param size Size of destination buffer
 */
static void safe_strncpy(char *dest, const char *src, size_t size) {
   if (size == 0) {
      return;
   }
   strncpy(dest, src, size - 1);
   dest[size - 1] = '\0';
}

/* =============================================================================
 * Validation Helpers
 * ============================================================================= */

/* Allowed HUD control elements */
static const char *VALID_HUD_ELEMENTS[] = { "armor_display", "detect", "map", "info" };
#define VALID_HUD_ELEMENT_COUNT (sizeof(VALID_HUD_ELEMENTS) / sizeof(VALID_HUD_ELEMENTS[0]))

/* Allowed recording modes */
static const char *VALID_RECORDING_MODES[] = { "record", "stream", "record_and_stream" };
#define VALID_RECORDING_MODE_COUNT \
   (sizeof(VALID_RECORDING_MODES) / sizeof(VALID_RECORDING_MODES[0]))

/**
 * @brief Validate a string value against an allowed list
 */
static bool validate_enum_value(const char *value, const char **allowed, size_t count) {
   if (!value) {
      return false;
   }
   for (size_t i = 0; i < count; i++) {
      if (strcmp(value, allowed[i]) == 0) {
         return true;
      }
   }
   return false;
}

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

/* Helper to add a tool parameter */
static void add_param(tool_definition_t *tool,
                      const char *name,
                      const char *desc,
                      tool_param_type_t type,
                      bool required) {
   if (tool->param_count >= LLM_TOOLS_MAX_PARAMS) {
      return;
   }
   tool_param_t *p = &tool->parameters[tool->param_count++];
   safe_strncpy(p->name, name, LLM_TOOLS_NAME_LEN);
   safe_strncpy(p->description, desc, sizeof(p->description));
   p->type = type;
   p->required = required;
   p->enum_count = 0;
}

/* Helper to add enum values to the last parameter */
static void add_enum_values(tool_definition_t *tool, const char *values[], int count) {
   if (tool->param_count == 0) {
      return;
   }
   tool_param_t *p = &tool->parameters[tool->param_count - 1];
   p->type = TOOL_PARAM_ENUM;
   for (int i = 0; i < count && i < LLM_TOOLS_MAX_ENUM_VALUES; i++) {
      safe_strncpy(p->enum_values[i], values[i], sizeof(p->enum_values[i]));
      p->enum_count++;
   }
}

/* Register a new tool */
static tool_definition_t *register_tool(const char *name, const char *desc, const char *device) {
   if (s_tool_count >= LLM_TOOLS_MAX_TOOLS) {
      LOG_ERROR("Maximum tool count (%d) reached", LLM_TOOLS_MAX_TOOLS);
      return NULL;
   }
   tool_definition_t *t = &s_tools[s_tool_count++];
   memset(t, 0, sizeof(*t));
   safe_strncpy(t->name, name, LLM_TOOLS_NAME_LEN);
   safe_strncpy(t->description, desc, LLM_TOOLS_DESC_LEN);
   t->device_name = device;
   t->enabled = true;
   return t;
}

/**
 * @brief Execute a command via MQTT for hardware devices
 *
 * Sends a JSON command to the MQTT broker for hardware devices (HUD, faceplate,
 * recording, etc.) that don't have direct callbacks.
 *
 * Uses validate_device_in_config() to:
 * 1. Validate the device exists in commands_config_nuevo.json (security)
 * 2. Get the correct MQTT topic for the device (hud, helmet, dawn, etc.)
 *
 * @param device Device name (e.g., "hud", "faceplate", "record")
 * @param action Action to perform (e.g., "enable", "disable", "set")
 * @param value Optional value for the action
 * @param result Output buffer for result message
 * @param result_len Size of result buffer
 * @return true on success, false on failure
 */
static bool execute_mqtt_command(const char *device,
                                 const char *action,
                                 const char *value,
                                 char *result,
                                 size_t result_len) {
   struct mosquitto *mosq = worker_pool_get_mosq();
   if (!mosq) {
      snprintf(result, result_len, "Error: MQTT not available");
      return false;
   }

   /* Validate device and get correct topic from commands_config_nuevo.json */
   char topic[64] = "dawn";
   if (!validate_device_in_config(device, topic, sizeof(topic))) {
      LOG_WARNING("Tool command rejected: Unknown device '%s' not in config", device);
      snprintf(result, result_len, "Error: Unknown device '%s'", device);
      return false;
   }

   /* Build the JSON command */
   struct json_object *cmd = json_object_new_object();
   json_object_object_add(cmd, "device", json_object_new_string(device));
   json_object_object_add(cmd, "action", json_object_new_string(action));
   if (value && value[0] != '\0') {
      json_object_object_add(cmd, "value", json_object_new_string(value));
   }

   const char *cmd_str = json_object_to_json_string(cmd);

   /* Publish to the device's configured topic */
   int rc = mosquitto_publish(mosq, NULL, topic, strlen(cmd_str), cmd_str, 0, false);
   if (rc != MOSQ_ERR_SUCCESS) {
      snprintf(result, result_len, "Error: Failed to publish command: %s", mosquitto_strerror(rc));
      json_object_put(cmd);
      return false;
   }

   LOG_INFO("MQTT command executed: topic=%s cmd=%s", topic, cmd_str);
   snprintf(result, result_len, "Command sent: %s %s%s%s", device, action, value[0] ? " " : "",
            value);

   json_object_put(cmd);
   return true;
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
 * Execute viewing command with synchronous MQTT wait using command_router.
 *
 * Sends viewing command to HUD with request_id, waits for OCP response.
 * Response may contain either:
 * - Inline base64 image data (data.content field)
 * - File path reference (value field)
 *
 * @param action Action name (e.g., "look")
 * @param query Query string (e.g., "what do you see?")
 * @param result Output buffer for result message
 * @param result_len Size of result buffer
 * @return true on success (image captured and stored), false on failure
 */
static bool execute_viewing_sync(const char *action,
                                 const char *query,
                                 char *result,
                                 size_t result_len) {
   struct mosquitto *mosq = worker_pool_get_mosq();
   if (!mosq) {
      snprintf(result, result_len, "Error: MQTT not available");
      return false;
   }

   /* Check if vision is enabled */
   if (!is_vision_enabled_for_current_llm()) {
      snprintf(result, result_len,
               "Vision is not enabled for the current AI model. "
               "Switch to cloud LLM or enable vision for local LLM in the config.");
      return false;
   }

   /* Get topic from config */
   char topic[64] = "dawn";
   if (!validate_device_in_config("viewing", topic, sizeof(topic))) {
      snprintf(result, result_len, "Error: viewing device not configured");
      return false;
   }

   /* Register with command_router for response */
   pending_request_t *req = command_router_register(99); /* Use worker_id 99 for tool calls */
   if (!req) {
      snprintf(result, result_len, "Error: Could not register for response");
      return false;
   }
   const char *request_id = command_router_get_id(req);

   /* Build the JSON command with request_id (OCP format) */
   struct json_object *cmd = json_object_new_object();
   json_object_object_add(cmd, "device", json_object_new_string("viewing"));
   json_object_object_add(cmd, "action", json_object_new_string(action));
   if (query && query[0] != '\0') {
      json_object_object_add(cmd, "value", json_object_new_string(query));
   }
   json_object_object_add(cmd, "request_id", json_object_new_string(request_id));

   const char *cmd_str = json_object_to_json_string(cmd);

   /* Publish to HUD */
   int rc = mosquitto_publish(mosq, NULL, topic, strlen(cmd_str), cmd_str, 0, false);
   json_object_put(cmd);

   if (rc != MOSQ_ERR_SUCCESS) {
      command_router_cancel(req);
      snprintf(result, result_len, "Error: Failed to publish: %s", mosquitto_strerror(rc));
      return false;
   }

   LOG_INFO("Viewing command sent (request_id=%s), waiting for response...", request_id);

   /* Wait for response via command_router */
   char *response = command_router_wait(req, VIEWING_MQTT_TIMEOUT_MS);
   if (!response || response[0] == '\0') {
      snprintf(result, result_len, "Error: Timeout waiting for image capture");
      if (response) {
         free(response);
      }
      return false;
   }

   LOG_INFO("Viewing response received: %.100s%s", response, strlen(response) > 100 ? "..." : "");

   /* Check if response is an OCP error */
   if (strncmp(response, "ERROR:", 6) == 0) {
      /* OCP error response - pass error message to LLM */
      snprintf(result, result_len, "%s", response);
      free(response);
      return false;
   }

   /* Check if response is inline base64 data or a file path */
   char *base64_image = NULL;
   if (is_base64_image_data(response)) {
      /* Response is already base64 image data (inline mode) */
      LOG_INFO("Response is inline base64 image data (%zu bytes)", strlen(response));
      base64_image = response; /* Take ownership */
   } else {
      /* Response is a file path - validate and read */
      LOG_INFO("Response is file path, reading: %s", response);

      /* Validate path for security before reading */
      if (!validate_file_path(response)) {
         snprintf(result, result_len, "Error: Invalid or unsafe file path");
         free(response);
         return false;
      }

      size_t file_size = 0;
      unsigned char *file_content = read_file(response, &file_size);
      free(response);

      if (!file_content) {
         snprintf(result, result_len, "Error: Could not read image file");
         return false;
      }

      base64_image = base64_encode(file_content, file_size);
      free(file_content);

      if (!base64_image) {
         snprintf(result, result_len, "Error: Could not encode image");
         return false;
      }
      LOG_INFO("Image file encoded: %zu bytes base64", strlen(base64_image));
   }

   /* Store in pending vision for follow-up call (atomic operation) */
   size_t image_size = strlen(base64_image) + 1;
   set_pending_vision_locked(base64_image, image_size);

   LOG_INFO("Vision image stored for LLM follow-up: %zu bytes", image_size);

   snprintf(result, result_len,
            "Image captured successfully. Please describe what you see in detail.");
   return true;
}

/* =============================================================================
 * Tool Initialization
 * ============================================================================= */

void llm_tools_init(void) {
   if (s_initialized) {
      return;
   }

   s_tool_count = 0;
   tool_definition_t *t;

   /* ----- weather ----- */
   t = register_tool("weather",
                     "Get weather forecast for a location. Returns current conditions, "
                     "temperature, humidity, and forecast.",
                     "weather");
   if (t) {
      add_param(t, "location", "City and state/country (e.g., 'Atlanta, Georgia')",
                TOOL_PARAM_STRING, true);
   }

   /* ----- search ----- */
   t = register_tool("search",
                     "Search the web for information. Returns relevant search results "
                     "that can be used to answer questions about current events, facts, etc.",
                     "search");
   if (t) {
      add_param(t, "query", "The search query", TOOL_PARAM_STRING, true);
      add_param(t, "category",
                "Search category: 'web' for general search, 'news' for current events, "
                "'science' for scientific topics, 'social' for social media posts and discussions",
                TOOL_PARAM_ENUM, false);
      const char *categories[] = { "web", "news", "science", "social" };
      add_enum_values(t, categories, 4);
   }

   /* ----- calculator ----- */
   t = register_tool("calculator",
                     "Perform calculations: evaluate math expressions, convert units, "
                     "convert number bases, or generate random numbers.",
                     "calculator");
   if (t) {
      add_param(t, "action", "The type of calculation to perform", TOOL_PARAM_ENUM, true);
      const char *calc_actions[] = { "evaluate", "convert", "base", "random" };
      add_enum_values(t, calc_actions, 4);
      add_param(t, "value",
                "For 'evaluate': math expression (e.g., 'sqrt(144) + 2^8'). "
                "For 'convert': unit conversion (e.g., '5 miles to km'). "
                "For 'base': number base conversion (e.g., '255 to hex'). "
                "For 'random': range (e.g., '1 to 100').",
                TOOL_PARAM_STRING, true);
   }

   /* ----- url ----- */
   t = register_tool("url",
                     "Fetch and extract readable content from a URL. "
                     "Returns the main text content of a webpage.",
                     "url");
   if (t) {
      add_param(t, "url", "The URL to fetch (must start with http:// or https://)",
                TOOL_PARAM_STRING, true);
   }

   /* ----- smartthings ----- */
   t = register_tool("smartthings",
                     "Control SmartThings smart home devices. Can list devices, check status, "
                     "and control lights, switches, locks, and thermostats.",
                     "smartthings");
   if (t) {
      add_param(t, "action", "The action to perform", TOOL_PARAM_ENUM, true);
      const char *actions[] = { "list",  "status",      "on",   "off",   "brightness",
                                "color", "temperature", "lock", "unlock" };
      add_enum_values(t, actions, 9);
      add_param(t, "device", "Device name (required for all actions except 'list')",
                TOOL_PARAM_STRING, false);
      add_param(t, "value",
                "Value for the action (brightness: 0-100, color: hex code, temperature: degrees)",
                TOOL_PARAM_STRING, false);
   }

   /* ----- date ----- */
   t = register_tool("date", "Get the current date. Returns today's date formatted for the user.",
                     "date");
   /* No parameters needed */

   /* ----- time ----- */
   t = register_tool("time", "Get the current time. Returns the current local time.", "time");
   /* No parameters needed */

   /* ----- llm_status ----- */
   t = register_tool("llm_status",
                     "Get information about the current LLM configuration. "
                     "Returns whether using local or cloud LLM and the model name.",
                     "llm");
   /* No parameters needed */

   /* ----- volume ----- */
   t = register_tool("volume", "Set the audio volume level for TTS and music playback.", "volume");
   if (t) {
      add_param(t, "level", "Volume level from 0 (silent) to 100 (maximum)", TOOL_PARAM_INTEGER,
                true);
   }

   /* ----- music ----- */
   t = register_tool("music",
                     "Control music playback. Can play, pause, stop, skip tracks, "
                     "or play a specific song/artist.",
                     "music");
   if (t) {
      add_param(t, "action", "The playback action", TOOL_PARAM_ENUM, true);
      const char *music_actions[] = { "play", "pause", "stop", "next", "previous", "shuffle" };
      add_enum_values(t, music_actions, 6);
      add_param(t, "query", "Song/artist name to play (for 'play' action)", TOOL_PARAM_STRING,
                false);
   }

   /* ----- switch_llm ----- */
   t = register_tool("switch_llm",
                     "Switch between local and cloud LLM, or change the cloud provider. "
                     "Use 'local' to use the local LLM server, 'cloud' to use cloud AI, "
                     "or specify a provider name like 'openai' or 'claude'.",
                     "switch_llm");
   if (t) {
      add_param(t, "target", "The LLM target to switch to", TOOL_PARAM_ENUM, true);
      const char *llm_targets[] = { "local", "cloud", "openai", "claude" };
      add_enum_values(t, llm_targets, 4);
   }

   /* ----- reset_conversation ----- */
   t = register_tool("reset_conversation",
                     "Clear the conversation history and start fresh. Use when the user "
                     "wants to change topics completely or start a new conversation.",
                     "reset conversation");
   /* No parameters needed */

   /* ----- viewing ----- */
   t = register_tool("viewing",
                     "Analyze what the camera sees. Takes a photo and describes the scene, "
                     "identifies objects, reads text, or answers questions about the view.",
                     "viewing");
   if (t) {
      add_param(t, "query",
                "What to look for or question about the view (e.g., 'what do you see?', "
                "'read the text', 'is anyone there?')",
                TOOL_PARAM_STRING, false);
   }

   /* ----- shutdown ----- */
   t = register_tool("shutdown",
                     "Shutdown or restart the system. Only use when explicitly requested.",
                     "shutdown");
   if (t) {
      add_param(t, "action", "The shutdown action", TOOL_PARAM_ENUM, true);
      const char *shutdown_actions[] = { "shutdown", "restart", "cancel" };
      add_enum_values(t, shutdown_actions, 3);
   }

   /* ----- hud_control ----- */
   t = register_tool("hud_control",
                     "Control HUD (Heads-Up Display) elements. Enable or disable display overlays "
                     "like the armor display, minimap, object detection, or info panel.",
                     "hud_control");
   if (t) {
      add_param(t, "element", "The HUD element to control", TOOL_PARAM_ENUM, true);
      const char *hud_elements[] = { "armor_display", "detect", "map", "info" };
      add_enum_values(t, hud_elements, 4);
      add_param(t, "action", "Whether to show or hide the element", TOOL_PARAM_ENUM, true);
      const char *hud_actions[] = { "enable", "disable" };
      add_enum_values(t, hud_actions, 2);
   }

   /* ----- hud_mode ----- */
   t = register_tool("hud_mode",
                     "Switch the HUD display mode. Available modes: default, environmental, armor.",
                     "hud");
   if (t) {
      add_param(t, "mode", "The HUD mode to switch to", TOOL_PARAM_ENUM, true);
      const char *hud_modes[] = { "default", "environmental", "armor" };
      add_enum_values(t, hud_modes, 3);
   }

   /* ----- visual_offset ----- */
   t = register_tool("visual_offset",
                     "Adjust the 3D visual offset for stereoscopic display alignment.",
                     "visual offset");
   if (t) {
      add_param(t, "pixels", "Offset in pixels (positive or negative)", TOOL_PARAM_INTEGER, true);
   }

   /* ----- faceplate ----- */
   t = register_tool("faceplate",
                     "Control the helmet faceplate/visor. Open or close the faceplate.",
                     "faceplate");
   if (t) {
      add_param(t, "action", "Whether to open or close the faceplate", TOOL_PARAM_ENUM, true);
      const char *faceplate_actions[] = { "enable", "disable" };
      add_enum_values(t, faceplate_actions, 2);
   }

   /* ----- recording ----- */
   t = register_tool("recording",
                     "Control video recording and streaming. Start or stop recording, streaming, "
                     "or both simultaneously.",
                     "recording");
   if (t) {
      add_param(t, "mode", "What to control", TOOL_PARAM_ENUM, true);
      const char *rec_modes[] = { "record", "stream", "record_and_stream" };
      add_enum_values(t, rec_modes, 3);
      add_param(t, "action", "Whether to start or stop", TOOL_PARAM_ENUM, true);
      const char *rec_actions[] = { "enable", "disable" };
      add_enum_values(t, rec_actions, 2);
   }

   /* ----- voice_amplifier ----- */
   t = register_tool(
       "voice_amplifier",
       "Control the voice amplifier/PA system for projecting voice through external speakers.",
       "voice amplifier");
   if (t) {
      add_param(t, "action", "Whether to enable or disable voice amplification", TOOL_PARAM_ENUM,
                true);
      const char *va_actions[] = { "enable", "disable" };
      add_enum_values(t, va_actions, 2);
   }

   /* ----- audio_device ----- */
   t = register_tool("audio_device", "Switch audio input or output devices.", "audio_device");
   if (t) {
      add_param(t, "type", "The type of audio device to change", TOOL_PARAM_ENUM, true);
      const char *audio_types[] = { "capture", "playback" };
      add_enum_values(t, audio_types, 2);
      add_param(t, "device", "Device name (e.g., 'microphone', 'headphones', 'speakers')",
                TOOL_PARAM_STRING, true);
   }

   s_initialized = true;
   LOG_INFO("Initialized %d LLM tools", s_tool_count);

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

   LOG_INFO("Refreshed tool availability: %d enabled", llm_tools_get_enabled_count());
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
      case TOOL_PARAM_STRING:
      case TOOL_PARAM_ENUM:
         return "string";
      case TOOL_PARAM_INTEGER:
         return "integer";
      case TOOL_PARAM_NUMBER:
         return "number";
      case TOOL_PARAM_BOOLEAN:
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
      if (p->type == TOOL_PARAM_ENUM && p->enum_count > 0) {
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

/**
 * @brief Find a tool definition by name
 *
 * @param tool_name The tool name to look up
 * @return Pointer to tool definition, or NULL if not found
 */
static const tool_definition_t *find_tool_by_name(const char *tool_name) {
   if (!tool_name || !s_initialized) {
      return NULL;
   }

   for (int i = 0; i < s_tool_count; i++) {
      if (strcmp(s_tools[i].name, tool_name) == 0) {
         return &s_tools[i];
      }
   }

   return NULL;
}

int llm_tools_execute(const tool_call_t *call, tool_result_t *result) {
   if (!call || !result) {
      return 1;
   }

   memset(result, 0, sizeof(*result));
   safe_strncpy(result->tool_call_id, call->id, LLM_TOOLS_ID_LEN);

   /* Handle switch_llm specially - it routes to different callbacks */
   if (strcmp(call->name, "switch_llm") == 0) {
      struct json_object *args = json_tokener_parse(call->arguments);
      struct json_object *target_obj;
      const char *target = "cloud"; /* default */

      if (args && json_object_object_get_ex(args, "target", &target_obj)) {
         target = json_object_get_string(target_obj);
      }

      LOG_INFO("Executing switch_llm with target='%s'", target);

      int should_respond = 1;
      char *callback_result = NULL;
      device_callback_fn local_cb = get_device_callback("local llm");
      device_callback_fn cloud_cb = get_device_callback("cloud llm");
      device_callback_fn provider_cb = get_device_callback("cloud provider");

      if (strcmp(target, "local") == 0) {
         if (local_cb)
            callback_result = local_cb("switch", "", &should_respond);
      } else if (strcmp(target, "cloud") == 0) {
         if (cloud_cb)
            callback_result = cloud_cb("switch", "", &should_respond);
      } else if (strcmp(target, "openai") == 0) {
         /* Set provider AND enable cloud mode */
         if (provider_cb) {
            char *provider_result = provider_cb("set", "openai", &should_respond);
            if (provider_result) {
               free(provider_result);
            }
         }
         if (cloud_cb)
            callback_result = cloud_cb("switch", "", &should_respond);
      } else if (strcmp(target, "claude") == 0) {
         /* Set provider AND enable cloud mode */
         if (provider_cb) {
            char *provider_result = provider_cb("set", "claude", &should_respond);
            if (provider_result) {
               free(provider_result);
            }
         }
         if (cloud_cb)
            callback_result = cloud_cb("switch", "", &should_respond);
      } else {
         snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Unknown LLM target: %s", target);
         result->success = false;
         if (args)
            json_object_put(args);
         return 1;
      }

      if (callback_result) {
         safe_strncpy(result->result, callback_result, LLM_TOOLS_RESULT_LEN);
         free(callback_result);
      } else {
         snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Switched to %s", target);
      }
      result->success = true;
      result->skip_followup = true; /* LLM was switched - don't make follow-up call */

      if (args)
         json_object_put(args);
      return 0;
   }

   /* Find the tool definition to get its device_name */
   const tool_definition_t *tool = find_tool_by_name(call->name);
   if (!tool) {
      snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Error: Unknown tool '%s'", call->name);
      result->success = false;
      return 1;
   }

   /* Look up the callback using the tool's device_name via shared lookup */
   device_callback_fn callback = get_device_callback(tool->device_name);
   if (!callback) {
      /* Tool has no direct callback - it may use MQTT (handled via device_override) */
      /* This is OK for hardware tools like hud_control, faceplate, etc. */
      callback = NULL;
   }

   /* Parse arguments JSON */
   struct json_object *args = json_tokener_parse(call->arguments);
   if (!args && call->arguments[0] != '\0') {
      snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Error: Invalid JSON arguments");
      result->success = false;
      return 1;
   }

   /* Extract action and value based on tool type */
   char action_name[64] = "get"; /* Default action */
   char value_buf[4096] = "";
   char device_override[64] = ""; /* For tools that dynamically select device */

   /* Tool-specific argument extraction */
   if (strcmp(call->name, "weather") == 0) {
      struct json_object *loc;
      if (json_object_object_get_ex(args, "location", &loc)) {
         safe_strncpy(value_buf, json_object_get_string(loc), sizeof(value_buf));
      }
   } else if (strcmp(call->name, "search") == 0) {
      struct json_object *query, *category;
      if (json_object_object_get_ex(args, "query", &query)) {
         safe_strncpy(value_buf, json_object_get_string(query), sizeof(value_buf));
      }
      if (json_object_object_get_ex(args, "category", &category)) {
         safe_strncpy(action_name, json_object_get_string(category), sizeof(action_name));
      } else {
         strcpy(action_name, "web");
      }
   } else if (strcmp(call->name, "calculator") == 0) {
      struct json_object *act, *val;
      if (json_object_object_get_ex(args, "action", &act)) {
         safe_strncpy(action_name, json_object_get_string(act), sizeof(action_name));
      } else {
         strcpy(action_name, "evaluate"); /* Default action */
      }
      if (json_object_object_get_ex(args, "value", &val)) {
         safe_strncpy(value_buf, json_object_get_string(val), sizeof(value_buf));
      }
   } else if (strcmp(call->name, "url") == 0) {
      struct json_object *url;
      if (json_object_object_get_ex(args, "url", &url)) {
         safe_strncpy(value_buf, json_object_get_string(url), sizeof(value_buf));
      }
      strcpy(action_name, "fetch");
   } else if (strcmp(call->name, "smartthings") == 0) {
      struct json_object *act, *dev, *val;
      if (json_object_object_get_ex(args, "action", &act)) {
         safe_strncpy(action_name, json_object_get_string(act), sizeof(action_name));
      }
      if (json_object_object_get_ex(args, "device", &dev)) {
         safe_strncpy(value_buf, json_object_get_string(dev), sizeof(value_buf));
      }
      /* Append value if present (e.g., "device value") */
      if (json_object_object_get_ex(args, "value", &val)) {
         size_t len = strlen(value_buf);
         if (len > 0 && len < sizeof(value_buf) - 2) {
            value_buf[len] = ' ';
            safe_strncpy(value_buf + len + 1, json_object_get_string(val),
                         sizeof(value_buf) - len - 1);
         }
      }
   } else if (strcmp(call->name, "volume") == 0) {
      struct json_object *level;
      if (json_object_object_get_ex(args, "level", &level)) {
         snprintf(value_buf, sizeof(value_buf), "%d", json_object_get_int(level));
      }
      strcpy(action_name, "set");
   } else if (strcmp(call->name, "music") == 0) {
      struct json_object *act, *query;
      if (json_object_object_get_ex(args, "action", &act)) {
         safe_strncpy(action_name, json_object_get_string(act), sizeof(action_name));
      }
      if (json_object_object_get_ex(args, "query", &query)) {
         safe_strncpy(value_buf, json_object_get_string(query), sizeof(value_buf));
      }
   } else if (strcmp(call->name, "viewing") == 0) {
      strcpy(device_override, "viewing"); /* Route through MQTT to HUD */
      struct json_object *query;
      strcpy(action_name, "look"); /* Default viewing action */
      if (json_object_object_get_ex(args, "query", &query)) {
         safe_strncpy(value_buf, json_object_get_string(query), sizeof(value_buf));
      }
   } else if (strcmp(call->name, "shutdown") == 0) {
      struct json_object *act;
      if (json_object_object_get_ex(args, "action", &act)) {
         safe_strncpy(action_name, json_object_get_string(act), sizeof(action_name));
      } else {
         strcpy(action_name, "shutdown"); /* Default action */
      }
   } else if (strcmp(call->name, "hud_control") == 0) {
      /* HUD control: element (armor_display, detect, map, info) + action (enable/disable) */
      struct json_object *elem, *act;
      if (json_object_object_get_ex(args, "element", &elem)) {
         const char *elem_str = json_object_get_string(elem);
         /* Validate element against allowed values */
         if (!validate_enum_value(elem_str, VALID_HUD_ELEMENTS, VALID_HUD_ELEMENT_COUNT)) {
            LOG_WARNING("Invalid HUD element: %s", elem_str);
            snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Error: Invalid HUD element '%s'",
                     elem_str);
            result->success = false;
            json_object_put(args);
            return 1;
         }
         safe_strncpy(device_override, elem_str, sizeof(device_override));
      }
      if (json_object_object_get_ex(args, "action", &act)) {
         safe_strncpy(action_name, json_object_get_string(act), sizeof(action_name));
      }
   } else if (strcmp(call->name, "hud_mode") == 0) {
      /* HUD mode: maps to "hud" device with set action */
      strcpy(device_override, "hud");
      strcpy(action_name, "set");
      struct json_object *mode;
      if (json_object_object_get_ex(args, "mode", &mode)) {
         safe_strncpy(value_buf, json_object_get_string(mode), sizeof(value_buf));
      }
   } else if (strcmp(call->name, "visual_offset") == 0) {
      /* Visual offset: set action with pixel value */
      strcpy(device_override, "visual offset");
      strcpy(action_name, "set");
      struct json_object *pixels;
      if (json_object_object_get_ex(args, "pixels", &pixels)) {
         snprintf(value_buf, sizeof(value_buf), "%d", json_object_get_int(pixels));
      }
   } else if (strcmp(call->name, "faceplate") == 0) {
      strcpy(device_override, "faceplate");
      struct json_object *act;
      if (json_object_object_get_ex(args, "action", &act)) {
         safe_strncpy(action_name, json_object_get_string(act), sizeof(action_name));
      }
   } else if (strcmp(call->name, "recording") == 0) {
      /* Recording: mode (record, stream, record_and_stream) + action (enable/disable) */
      struct json_object *mode, *act;
      if (json_object_object_get_ex(args, "mode", &mode)) {
         const char *mode_str = json_object_get_string(mode);
         /* Validate mode against allowed values */
         if (!validate_enum_value(mode_str, VALID_RECORDING_MODES, VALID_RECORDING_MODE_COUNT)) {
            LOG_WARNING("Invalid recording mode: %s", mode_str);
            snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Error: Invalid recording mode '%s'",
                     mode_str);
            result->success = false;
            json_object_put(args);
            return 1;
         }
         /* Map mode to device name - copy since args will be freed */
         if (strcmp(mode_str, "record_and_stream") == 0) {
            strcpy(device_override, "record and stream");
         } else {
            safe_strncpy(device_override, mode_str, sizeof(device_override));
         }
      }
      if (json_object_object_get_ex(args, "action", &act)) {
         safe_strncpy(action_name, json_object_get_string(act), sizeof(action_name));
      }
   } else if (strcmp(call->name, "voice_amplifier") == 0) {
      strcpy(device_override, "voice amplifier");
      struct json_object *act;
      if (json_object_object_get_ex(args, "action", &act)) {
         safe_strncpy(action_name, json_object_get_string(act), sizeof(action_name));
      }
   } else if (strcmp(call->name, "audio_device") == 0) {
      /* Audio device: type (capture/playback) + device name */
      struct json_object *type_obj, *dev;
      strcpy(action_name, "set");
      if (json_object_object_get_ex(args, "type", &type_obj)) {
         const char *type_str = json_object_get_string(type_obj);
         if (strcmp(type_str, "capture") == 0) {
            strcpy(device_override, "audio capture device");
         } else {
            strcpy(device_override, "audio playback device");
         }
      }
      if (json_object_object_get_ex(args, "device", &dev)) {
         safe_strncpy(value_buf, json_object_get_string(dev), sizeof(value_buf));
      }
   }
   /* date, time, llm_status, reset_conversation don't need arguments */

   if (args) {
      json_object_put(args);
   }

   /* If device was overridden, this is a hardware device controlled via MQTT */
   if (device_override[0] != '\0') {
      LOG_INFO("Executing tool '%s' -> device='%s', action='%s', value='%s'", call->name,
               device_override, action_name, value_buf);

      /* Viewing uses synchronous MQTT to wait for image capture */
      if (strcmp(call->name, "viewing") == 0) {
         result->success = execute_viewing_sync(action_name, value_buf, result->result,
                                                LLM_TOOLS_RESULT_LEN);
         /* Don't skip follow-up - LLM needs to describe the image */
         return result->success ? 0 : 1;
      }

      bool mqtt_success = execute_mqtt_command(device_override, action_name, value_buf,
                                               result->result, LLM_TOOLS_RESULT_LEN);
      result->success = mqtt_success;

      return mqtt_success ? 0 : 1;
   }

   /* Check that we have a callback to invoke */
   if (!callback) {
      snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Error: No callback for tool '%s'",
               call->name);
      result->success = false;
      return 1;
   }

   /* Use the direct callback */
   LOG_INFO("Executing tool '%s' with action='%s', value='%s'", call->name, action_name, value_buf);

   /* Invoke the callback */
   int should_respond = 1;
   char *callback_result = callback(action_name, value_buf, &should_respond);

   if (callback_result) {
      safe_strncpy(result->result, callback_result, LLM_TOOLS_RESULT_LEN);
      free(callback_result);
      result->success = true;
   } else {
      snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Tool '%s' completed (no output)", call->name);
      result->success = true;
   }

   return 0;
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

   response[0] = '\0';
   for (int i = 0; i < results->count; i++) {
      strcat(response, results->results[i].result);
      if (i < results->count - 1) {
         strcat(response, "\n");
      }
   }

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

   int count = 0;
   for (int i = 0; i < s_tool_count; i++) {
      if (s_tools[i].enabled) {
         count++;
      }
   }
   return count;
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

   /* Check for pending vision data (from viewing tool) */
   if (llm_tools_has_pending_vision()) {
      ctx->has_pending_vision = true;
      ctx->pending_vision = llm_tools_get_pending_vision(&ctx->pending_vision_size);
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

/**
 * @brief Atomically set pending vision data
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
