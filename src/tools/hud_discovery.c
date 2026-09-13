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
 * HUD Discovery Implementation
 *
 * Handles MQTT-based discovery of HUD capabilities from Mirage.
 * Updates tool registry with discovered elements and modes.
 */

#include "tools/hud_discovery.h"

#include <json-c/json.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "core/ocp_helpers.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "tools/tool_registry.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Default Values
 * ============================================================================= */

/* Default HUD elements (empty - nothing guaranteed until discovery) */
static const char *s_default_elements[] = { NULL };
static const int s_default_element_count = 0;

/* Default HUD modes (used before discovery or on timeout) */
static const char *s_default_modes[] = { "default" };
static const int s_default_mode_count = 1;

/* =============================================================================
 * Module State
 * ============================================================================= */

static pthread_mutex_t s_discovery_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool s_initialized = false;

/* Discovery state for elements */
static char s_elements[HUD_DISCOVERY_MAX_ITEMS][TOOL_NAME_MAX];
static const char *s_element_ptrs[HUD_DISCOVERY_MAX_ITEMS];
static int s_element_count = 0;
static time_t s_elements_timestamp = 0;

/* Discovery state for modes */
static char s_modes[HUD_DISCOVERY_MAX_ITEMS][TOOL_NAME_MAX];
static const char *s_mode_ptrs[HUD_DISCOVERY_MAX_ITEMS];
static int s_mode_count = 0;
static time_t s_modes_timestamp = 0;

/* Discovery status */
static bool s_elements_received = false;
static bool s_modes_received = false;

/* =============================================================================
 * Internal Helpers
 * ============================================================================= */

/**
 * @brief Parse a JSON array of strings into local storage
 *
 * @param array JSON array object
 * @param storage 2D char array to store strings
 * @param ptrs Array of pointers for tool registry
 * @param max_items Maximum items to parse
 * @return Number of items parsed, or -1 on error
 */
static int parse_string_array(struct json_object *array,
                              char storage[][TOOL_NAME_MAX],
                              const char **ptrs,
                              int max_items) {
   if (!json_object_is_type(array, json_type_array)) {
      return FAILURE;
   }

   int count = (int)json_object_array_length(array);
   if (count > max_items) {
      OLOG_WARNING("HUD discovery: Truncating array from %d to %d items", count, max_items);
      count = max_items;
   }

   for (int i = 0; i < count; i++) {
      struct json_object *item = json_object_array_get_idx(array, i);
      if (item && json_object_is_type(item, json_type_string)) {
         const char *str = json_object_get_string(item);
         safe_strncpy(storage[i], str, TOOL_NAME_MAX);
         ptrs[i] = storage[i];
      } else {
         storage[i][0] = '\0';
         ptrs[i] = NULL;
      }
   }

   /* Clear remaining slots */
   for (int i = count; i < max_items; i++) {
      storage[i][0] = '\0';
      ptrs[i] = NULL;
   }

   return count;
}

/**
 * @brief Update hud_control tool with discovered elements
 */
/* Returns true on success; caller is responsible for running the post-update
 * tool-refresh + cache-invalidate + session-refresh sequence
 * (hud_capability_changed_unlocked) AFTER releasing s_discovery_mutex.  The
 * refresh in particular MUST run outside the mutex: llm_tools_refresh()
 * consults hud_control_is_available(), which re-locks s_discovery_mutex —
 * doing it here would self-deadlock the discovery (MQTT loop) thread. */
static bool update_hud_control_elements(void) {
   if (s_element_count <= 0) {
      return false;
   }

   int rc = tool_registry_update_param_enum("hud_control", "element", s_element_ptrs,
                                            s_element_count);
   if (rc == TREG_ENUM_RC_OK) {
      OLOG_INFO("HUD discovery: Updated hud_control with %d elements", s_element_count);
      return true;
   }
   OLOG_WARNING("HUD discovery: Failed to update hud_control elements (rc=%d)", rc);
   return false;
}

/**
 * @brief Update hud_mode tool with discovered modes
 */
/* Returns true on success; see update_hud_control_elements() for why the
 * post-update invalidate+refresh is deferred to the caller. */
static bool update_hud_mode_modes(void) {
   if (s_mode_count <= 0) {
      return false;
   }

   int rc = tool_registry_update_param_enum("hud_mode", "mode", s_mode_ptrs, s_mode_count);
   if (rc == TREG_ENUM_RC_OK) {
      OLOG_INFO("HUD discovery: Updated hud_mode with %d modes", s_mode_count);
      return true;
   }
   OLOG_WARNING("HUD discovery: Failed to update hud_mode modes (rc=%d)", rc);
   return false;
}

/* Cache-invalidate + session-refresh sequence run AFTER s_discovery_mutex is
 * released. Kept in one helper so elements / modes paths don't diverge. */
static void hud_capability_changed_unlocked(void) {
   /* Refresh tool availability first — enables/disables armor tools now that the
    * discovered enum changed.  MUST run outside s_discovery_mutex:
    * llm_tools_refresh() consults each armor tool's is_available() callback, and
    * hud_control_is_available()/hud_mode_is_available() re-lock s_discovery_mutex
    * (via the element/mode count getters).  Calling it while the discovery mutex
    * is held self-deadlocks the MQTT loop thread that drives discovery. */
   llm_tools_refresh();
   /* Schema cache is regenerated with the new enum values on next tool call */
   llm_tools_invalidate_cache();
   /* Rebuild system-prompt hint so it reflects the new availability */
   invalidate_system_instructions();
   /* Propagate the refreshed prompt to every active session.
    * Takes session_manager_rwlock (read) then per-session locks + DB I/O;
    * must run OUTSIDE s_discovery_mutex. */
   session_manager_refresh_all_prompts();
}

/**
 * @brief Process elements discovery message
 */
static void process_elements_discovery(struct json_object *root) {
   struct json_object *elements_array = NULL;

   if (!json_object_object_get_ex(root, "elements", &elements_array)) {
      OLOG_WARNING("HUD discovery: Elements message missing 'elements' field");
      return;
   }

   pthread_mutex_lock(&s_discovery_mutex);

   bool capability_changed = false;
   int count = parse_string_array(elements_array, s_elements, s_element_ptrs,
                                  HUD_DISCOVERY_MAX_ITEMS);
   if (count > 0) {
      s_element_count = count;
      s_elements_timestamp = time(NULL);
      s_elements_received = true;

      capability_changed = update_hud_control_elements();
   }

   pthread_mutex_unlock(&s_discovery_mutex);

   /* Post-update refresh runs OUTSIDE s_discovery_mutex — it acquires
    * session_manager_rwlock + per-session history_mutex and may do DB I/O
    * via the registered user-prompt builder. */
   if (capability_changed) {
      hud_capability_changed_unlocked();
   }
}

/**
 * @brief Process modes discovery message
 */
static void process_modes_discovery(struct json_object *root) {
   struct json_object *modes_array = NULL;

   /* Check for "huds" field (HUD screens/modes) */
   if (!json_object_object_get_ex(root, "huds", &modes_array)) {
      OLOG_WARNING("HUD discovery: Modes message missing 'huds' field");
      return;
   }

   pthread_mutex_lock(&s_discovery_mutex);

   bool capability_changed = false;
   int count = parse_string_array(modes_array, s_modes, s_mode_ptrs, HUD_DISCOVERY_MAX_ITEMS);
   if (count > 0) {
      s_mode_count = count;
      s_modes_timestamp = time(NULL);
      s_modes_received = true;

      capability_changed = update_hud_mode_modes();
   }

   pthread_mutex_unlock(&s_discovery_mutex);

   if (capability_changed) {
      hud_capability_changed_unlocked();
   }
}

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

int hud_discovery_init(struct mosquitto *mosq) {
   if (!mosq) {
      return 1;
   }

   pthread_mutex_lock(&s_discovery_mutex);

   if (s_initialized) {
      pthread_mutex_unlock(&s_discovery_mutex);
      return 0;
   }

   /* Clear state */
   memset(s_elements, 0, sizeof(s_elements));
   memset(s_modes, 0, sizeof(s_modes));
   s_element_count = 0;
   s_mode_count = 0;
   s_elements_timestamp = 0;
   s_modes_timestamp = 0;
   s_elements_received = false;
   s_modes_received = false;

   /* Apply defaults immediately (will be overwritten by discovery) */
   for (int i = 0; i < s_default_element_count; i++) {
      safe_strncpy(s_elements[i], s_default_elements[i], TOOL_NAME_MAX);
      s_element_ptrs[i] = s_elements[i];
   }
   s_element_count = s_default_element_count;

   for (int i = 0; i < s_default_mode_count; i++) {
      safe_strncpy(s_modes[i], s_default_modes[i], TOOL_NAME_MAX);
      s_mode_ptrs[i] = s_modes[i];
   }
   s_mode_count = s_default_mode_count;

   s_initialized = true;

   pthread_mutex_unlock(&s_discovery_mutex);

   /* Subscribe to discovery topics */
   int rc = mosquitto_subscribe(mosq, NULL, HUD_DISCOVERY_TOPIC_WILDCARD, 0);
   if (rc != MOSQ_ERR_SUCCESS) {
      OLOG_ERROR("HUD discovery: Failed to subscribe to %s: %s", HUD_DISCOVERY_TOPIC_WILDCARD,
                 mosquitto_strerror(rc));
      return 1;
   }
   OLOG_INFO("HUD discovery: Subscribed to %s", HUD_DISCOVERY_TOPIC_WILDCARD);

   /* Discovery request is triggered by component_status when HUD comes online */

   return 0;
}

void hud_discovery_shutdown(void) {
   pthread_mutex_lock(&s_discovery_mutex);

   memset(s_elements, 0, sizeof(s_elements));
   memset(s_modes, 0, sizeof(s_modes));
   s_element_count = 0;
   s_mode_count = 0;
   s_elements_timestamp = 0;
   s_modes_timestamp = 0;
   s_elements_received = false;
   s_modes_received = false;
   s_initialized = false;

   pthread_mutex_unlock(&s_discovery_mutex);

   OLOG_INFO("HUD discovery: Shutdown complete");
}

/* =============================================================================
 * Message Handling
 * ============================================================================= */

void hud_discovery_handle_message(const char *topic, const char *payload, int payloadlen) {
   if (!topic || !payload || payloadlen <= 0) {
      return;
   }

   /* Parse JSON payload */
   struct json_object *root = json_tokener_parse(payload);
   if (!root) {
      OLOG_WARNING("HUD discovery: Failed to parse JSON from %s", topic);
      return;
   }

   /* Validate msg_type is "discovery" */
   struct json_object *msg_type_obj = NULL;
   if (json_object_object_get_ex(root, "msg_type", &msg_type_obj)) {
      const char *msg_type = json_object_get_string(msg_type_obj);
      if (!msg_type || strcmp(msg_type, "discovery") != 0) {
         /* Not a discovery message, ignore */
         json_object_put(root);
         return;
      }
   }

   /* Route based on topic */
   if (strcmp(topic, HUD_DISCOVERY_TOPIC_ELEMENTS) == 0) {
      process_elements_discovery(root);
   } else if (strcmp(topic, HUD_DISCOVERY_TOPIC_MODES) == 0) {
      process_modes_discovery(root);
   } else {
      /* Ignore unknown discovery subtopics silently */
   }

   json_object_put(root);
}

/* =============================================================================
 * State Queries
 * ============================================================================= */

bool hud_discovery_is_valid(void) {
   pthread_mutex_lock(&s_discovery_mutex);
   bool valid = s_initialized && (s_elements_received || s_modes_received);
   pthread_mutex_unlock(&s_discovery_mutex);
   return valid;
}

bool hud_discovery_is_stale(void) {
   pthread_mutex_lock(&s_discovery_mutex);

   time_t now = time(NULL);
   bool stale = true;

   if (s_elements_received && s_elements_timestamp > 0) {
      if ((now - s_elements_timestamp) < HUD_DISCOVERY_STALE_THRESHOLD) {
         stale = false;
      }
   }

   if (s_modes_received && s_modes_timestamp > 0) {
      if ((now - s_modes_timestamp) < HUD_DISCOVERY_STALE_THRESHOLD) {
         stale = false;
      }
   }

   pthread_mutex_unlock(&s_discovery_mutex);
   return stale;
}

int hud_discovery_get_element_count(void) {
   pthread_mutex_lock(&s_discovery_mutex);
   int count = s_element_count;
   pthread_mutex_unlock(&s_discovery_mutex);
   return count;
}

int hud_discovery_get_mode_count(void) {
   pthread_mutex_lock(&s_discovery_mutex);
   int count = s_mode_count;
   pthread_mutex_unlock(&s_discovery_mutex);
   return count;
}

/* =============================================================================
 * Manual Control
 * ============================================================================= */

void hud_discovery_request_update(struct mosquitto *mosq) {
   if (!mosq) {
      return;
   }

   /* Build OCP-compliant discovery request */
   struct json_object *request = json_object_new_object();
   json_object_object_add(request, "device", json_object_new_string("dawn"));
   json_object_object_add(request, "msg_type", json_object_new_string("discovery_request"));
   json_object_object_add(request, "timestamp", json_object_new_int64(ocp_get_timestamp_ms()));

   const char *payload = json_object_to_json_string(request);

   int rc = mosquitto_publish(mosq, NULL, HUD_DISCOVERY_TOPIC_REQUEST, (int)strlen(payload),
                              payload, 0, false);
   if (rc != MOSQ_ERR_SUCCESS) {
      OLOG_WARNING("HUD discovery: Failed to publish request: %s", mosquitto_strerror(rc));
   } else {
      OLOG_INFO("HUD discovery: Sent discovery request");
   }

   json_object_put(request);
}

void hud_discovery_apply_defaults(void) {
   pthread_mutex_lock(&s_discovery_mutex);

   /* Apply default elements */
   for (int i = 0; i < s_default_element_count; i++) {
      safe_strncpy(s_elements[i], s_default_elements[i], TOOL_NAME_MAX);
      s_element_ptrs[i] = s_elements[i];
   }
   s_element_count = s_default_element_count;

   /* Apply default modes */
   for (int i = 0; i < s_default_mode_count; i++) {
      safe_strncpy(s_modes[i], s_default_modes[i], TOOL_NAME_MAX);
      s_mode_ptrs[i] = s_modes[i];
   }
   s_mode_count = s_default_mode_count;

   pthread_mutex_unlock(&s_discovery_mutex);

   /* Update tool registry with defaults (mutex already released above) */
   update_hud_control_elements();
   update_hud_mode_modes();

   /* Refresh availability + rebuild schema/prompts — runs outside the mutex. */
   hud_capability_changed_unlocked();

   OLOG_INFO("HUD discovery: Applied default values");
}
