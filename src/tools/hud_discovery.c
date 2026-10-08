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

/* The changes discovery made to the sets (and so to every conversation's
 * standing directions) in the current window, and the newest set held back
 * past the bound: applied by the next discovery message once the window
 * turns (the helmet re-announces on reconnect and on request). */
static time_t s_window_start = 0;
static int s_window_changes = 0;
static bool s_window_logged = false;
static bool s_dropped_logged = false;

/* =============================================================================
 * Internal Helpers
 * ============================================================================= */

bool hud_discovery_name_ok(const char *name) {
   size_t n = 0;
   for (; name && name[n]; n++) {
      const char c = name[n];
      if (n >= HUD_DISCOVERY_NAME_MAX ||
          !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == ' ' || c == '-')) {
         return false;
      }
   }
   return n > 0;
}

/**
 * @brief The names a discovery message lists, into @p storage: only those
 *        hud_discovery_name_ok() accepts (they reach every conversation's
 *        standing directions), each once, up to @p max_items
 * @return How many (0 when @p array isn't an array)
 */
static int parse_string_array(struct json_object *array,
                              char storage[][TOOL_NAME_MAX],
                              int max_items) {
   if (!json_object_is_type(array, json_type_array)) {
      return 0;
   }
   const int total = (int)json_object_array_length(array);
   int count = 0;
   int dropped = 0;
   for (int i = 0; i < total; i++) {
      struct json_object *item = json_object_array_get_idx(array, i);
      const char *str = json_object_is_type(item, json_type_string) ? json_object_get_string(item)
                                                                    : NULL;
      if (!hud_discovery_name_ok(str)) {
         dropped++;
         continue;
      }
      bool seen = false;
      for (int j = 0; j < count && !seen; j++) {
         /* hud_discovery_name_ok(NULL) is false */
         // NOLINTNEXTLINE(clang-analyzer-core.NonNullParamChecker)
         seen = strcmp(storage[j], str) == 0;
      }
      if (seen) {
         continue;
      }
      if (count == max_items) {
         OLOG_WARNING("HUD discovery: more than %d names; the rest left out", max_items);
         break;
      }
      safe_strncpy(storage[count++], str, TOOL_NAME_MAX);
   }
   if (dropped > 0 && !s_dropped_logged) {
      s_dropped_logged = true;
      OLOG_WARNING("HUD discovery: %d name(s) left out: a name is 1-%d letters, digits, spaces, "
                   "'_' or '-' (further ones are not logged)",
                   dropped, HUD_DISCOVERY_NAME_MAX);
   }
   return count;
}

/* Whether the first @p n of @p list are @p storage's @p count, in order. */
static bool same_set(char storage[][TOOL_NAME_MAX], int count, const char *list[], int n) {
   if (count != n) {
      return false;
   }
   for (int i = 0; i < n; i++) {
      if (!list[i] || strcmp(list[i], storage[i]) != 0) {
         return false;
      }
   }
   return true;
}

/* Whether a change may be applied now (the window's bound); counts it.
 * Caller holds the mutex. */
static bool change_allowed_locked(time_t now) {
   if (now - s_window_start >= HUD_DISCOVERY_CHANGE_WINDOW_SEC) {
      s_window_start = now;
      s_window_changes = 0;
      s_window_logged = false;
   }
   if (s_window_changes >= HUD_DISCOVERY_CHANGES_PER_WINDOW) {
      if (!s_window_logged) {
         s_window_logged = true;
         OLOG_WARNING("HUD discovery: the HUD's elements or modes changed %d times this hour; "
                      "the sets in force stay until the hour is up",
                      s_window_changes);
      }
      return false;
   }
   s_window_changes++;
   return true;
}

/* Apply a discovered set to @p dest (@p ptrs, *@p count, *@p stamp,
 * *@p received).  The same set again only refreshes its time; a different
 * one is a capability change, within the window's bound.  Caller holds the
 * mutex.  @return true when the set changed. */
static bool apply_set_locked(char parsed[][TOOL_NAME_MAX],
                             int n,
                             char dest[][TOOL_NAME_MAX],
                             const char **ptrs,
                             int *count,
                             time_t *stamp,
                             bool *received) {
   const time_t now = time(NULL);
   if (same_set(parsed, n, ptrs, *count)) {
      *stamp = now;
      *received = true;
      return false;
   }
   if (!change_allowed_locked(now)) {
      return false;
   }
   for (int i = 0; i < HUD_DISCOVERY_MAX_ITEMS; i++) {
      if (i < n) {
         safe_strncpy(dest[i], parsed[i], TOOL_NAME_MAX);
         ptrs[i] = dest[i];
      } else {
         dest[i][0] = '\0';
         ptrs[i] = NULL;
      }
   }
   *count = n;
   *stamp = now;
   *received = true;
   return true;
}

/* The discovered elements and modes are not written into the tools' schemas: a
 * conversation freezes its tool schemas, and a value list that changes as the
 * helmet connects would change them every time.  The live sets reach the model
 * in the turn's standing directions (hud_discovery_describe), and a call naming
 * one not discovered is refused (hud_tools.c).  What changed is announced by
 * the caller (hud_capability_changed_unlocked), outside s_discovery_mutex. */

/* Cache-invalidate + session-refresh sequence run AFTER s_discovery_mutex is
 * released. Kept in one helper so elements / modes paths don't diverge. */
static void hud_capability_changed_unlocked(void) {
   /* Refresh tool availability first — enables/disables armor tools now that the
    * discovered sets changed.  MUST run outside s_discovery_mutex:
    * llm_tools_refresh() consults each armor tool's is_available() callback, and
    * hud_control_is_available()/hud_mode_is_available() re-lock s_discovery_mutex
    * (via the element/mode count getters).  Calling it while the discovery mutex
    * is held self-deadlocks the MQTT loop thread that drives discovery. */
   llm_tools_refresh();
   llm_tools_invalidate_cache();
   /* Each conversation's next turn tells the model what is available now
    * (its standing directions: hud_discovery_describe). */
   invalidate_system_instructions();
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
   char parsed[HUD_DISCOVERY_MAX_ITEMS][TOOL_NAME_MAX];
   const int count = parse_string_array(elements_array, parsed, HUD_DISCOVERY_MAX_ITEMS);
   if (count <= 0) {
      return;
   }

   pthread_mutex_lock(&s_discovery_mutex);
   const bool capability_changed = apply_set_locked(parsed, count, s_elements, s_element_ptrs,
                                                    &s_element_count, &s_elements_timestamp,
                                                    &s_elements_received);
   pthread_mutex_unlock(&s_discovery_mutex);

   /* Post-update refresh runs OUTSIDE s_discovery_mutex: llm_tools_refresh()
    * re-locks it through the armor tools' availability callbacks. */
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
   char parsed[HUD_DISCOVERY_MAX_ITEMS][TOOL_NAME_MAX];
   const int count = parse_string_array(modes_array, parsed, HUD_DISCOVERY_MAX_ITEMS);
   if (count <= 0) {
      return;
   }

   pthread_mutex_lock(&s_discovery_mutex);
   const bool capability_changed = apply_set_locked(parsed, count, s_modes, s_mode_ptrs,
                                                    &s_mode_count, &s_modes_timestamp,
                                                    &s_modes_received);
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
   s_window_start = 0;
   s_window_changes = 0;
   s_window_logged = false;
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

/* Whether @p name is one of the first @p n of @p list.  Caller holds the mutex. */
static bool listed_locked(const char *list[], int n, const char *name) {
   for (int i = 0; name && name[0] && i < n; i++) {
      if (list[i] && strcmp(list[i], name) == 0) {
         return true;
      }
   }
   return false;
}

bool hud_discovery_has_element(const char *name) {
   pthread_mutex_lock(&s_discovery_mutex);
   const bool found = listed_locked(s_element_ptrs, s_element_count, name);
   pthread_mutex_unlock(&s_discovery_mutex);
   return found;
}

bool hud_discovery_has_mode(const char *name) {
   pthread_mutex_lock(&s_discovery_mutex);
   const bool found = listed_locked(s_mode_ptrs, s_mode_count, name);
   pthread_mutex_unlock(&s_discovery_mutex);
   return found;
}

/* Append "<label>: a, b." for the first @p n of @p list to @p out at *len. */
static void describe_list_locked(char *out,
                                 size_t size,
                                 size_t *len,
                                 const char *label,
                                 const char *list[],
                                 int n) {
   if (n <= 0 || *len >= size) {
      return;
   }
   /* Each name quoted, as data: a name holds no quote (hud_discovery_name_ok). */
   int w = snprintf(out + *len, size - *len, "%s%s: ", *len ? " " : "", label);
   for (int i = 0; w > 0 && i < n; i++) {
      *len = *len + (size_t)w < size ? *len + (size_t)w : size - 1;
      w = snprintf(out + *len, size - *len, "%s\"%s\"", i ? ", " : "", list[i] ? list[i] : "");
   }
   if (w > 0) {
      *len = *len + (size_t)w < size ? *len + (size_t)w : size - 1;
      w = snprintf(out + *len, size - *len, ".");
      *len = *len + (size_t)w < size ? *len + (size_t)w : size - 1;
   }
}

size_t hud_discovery_describe(char *out, size_t size) {
   if (!out || size == 0) {
      return 0;
   }
   out[0] = '\0';
   size_t len = 0;
   pthread_mutex_lock(&s_discovery_mutex);
   describe_list_locked(out, size, &len, "HUD elements available now (hud_control)", s_element_ptrs,
                        s_element_count);
   /* One mode is no choice (hud_mode stays unavailable). */
   if (s_mode_count > 1) {
      describe_list_locked(out, size, &len, "HUD modes available now (hud_mode)", s_mode_ptrs,
                           s_mode_count);
   }
   pthread_mutex_unlock(&s_discovery_mutex);
   return len;
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

   /* Refresh availability + rebuild schema/prompts — runs outside the mutex. */
   hud_capability_changed_unlocked();

   OLOG_INFO("HUD discovery: Applied default values");
}
