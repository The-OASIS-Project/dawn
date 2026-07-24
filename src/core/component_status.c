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
 * Component Status (Keepalive) Implementation for Dawn
 *
 * Implements OCP v1.3 Component Status protocol:
 * - Publishes dawn/status with LWT and heartbeat
 * - Subscribes to hud/status for Mirage presence
 * - Enables/disables armor tools based on HUD availability
 */

#include "core/component_status.h"

#include <json-c/json.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "core/ocp_helpers.h"
#include "core/session_manager.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "tools/hud_discovery.h"
#include "version.h"

/* =============================================================================
 * Module State
 * ============================================================================= */

static pthread_mutex_t s_status_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool s_initialized = false;

/* Heartbeat thread */
static pthread_t s_heartbeat_thread;
static _Atomic bool s_heartbeat_running = false;  // atomic: written on shutdown, read in loop
static struct mosquitto *s_mosq = NULL;

/* HUD status tracking */
static bool s_hud_online = false;
static time_t s_hud_last_seen = 0;

/* =============================================================================
 * Internal Helpers
 * ============================================================================= */

/**
 * @brief Build a status message JSON
 */
static char *build_status_message(const char *status) {
   struct json_object *msg = json_object_new_object();

   json_object_object_add(msg, "device", json_object_new_string("dawn"));
   json_object_object_add(msg, "msg_type", json_object_new_string("status"));
   json_object_object_add(msg, "status", json_object_new_string(status));
   json_object_object_add(msg, "timestamp", json_object_new_int64(ocp_get_timestamp_ms()));

#ifdef VERSION_STRING
   json_object_object_add(msg, "version", json_object_new_string(VERSION_STRING));
#endif

   /* Add capabilities */
   struct json_object *caps = json_object_new_array();
   json_object_array_add(caps, json_object_new_string("voice"));
   json_object_array_add(caps, json_object_new_string("tools"));
   json_object_object_add(msg, "capabilities", caps);

   const char *json_str = json_object_to_json_string(msg);
   char *result = strdup(json_str);
   json_object_put(msg);

   return result;
}

/**
 * @brief Publish status message
 */
static int publish_status(struct mosquitto *mosq, const char *status, bool retain) {
   char *payload = build_status_message(status);
   if (!payload) {
      return 1;
   }

   int rc = mosquitto_publish(mosq, NULL, STATUS_TOPIC_DAWN, (int)strlen(payload), payload, 1,
                              retain);
   if (rc != MOSQ_ERR_SUCCESS) {
      OLOG_ERROR("Failed to publish dawn status: %s", mosquitto_strerror(rc));
   } else {
      OLOG_INFO("Published dawn status: %s", status);
   }

   free(payload);
   return rc == MOSQ_ERR_SUCCESS ? 0 : 1;
}

/**
 * @brief Check HUD timeout and update state
 */
static void check_hud_timeout(void) {
   pthread_mutex_lock(&s_status_mutex);

   if (s_hud_online && s_hud_last_seen > 0) {
      time_t now = time(NULL);
      if ((now - s_hud_last_seen) > STATUS_TIMEOUT_SEC) {
         OLOG_WARNING("HUD heartbeat timeout (%ld seconds), marking offline",
                      (long)(now - s_hud_last_seen));
         s_hud_online = false;

         /* Refresh tool availability (will disable armor tools) */
         pthread_mutex_unlock(&s_status_mutex);
         llm_tools_refresh();
         llm_tools_invalidate_cache();
         invalidate_system_instructions();
         /* Update every active session so the LLM sees the new availability
          * on its next turn without losing conversation context. */
         session_manager_refresh_all_prompts();
         return;
      }
   }

   pthread_mutex_unlock(&s_status_mutex);
}

/**
 * @brief Heartbeat thread function
 */
static void *heartbeat_thread_func(void *arg) {
   (void)arg;

   OLOG_INFO("Heartbeat thread started (interval: %ds)", STATUS_HEARTBEAT_INTERVAL_SEC);

   while (s_heartbeat_running) {
      /* Sleep in smaller increments to allow faster shutdown */
      for (int i = 0; i < STATUS_HEARTBEAT_INTERVAL_SEC && s_heartbeat_running; i++) {
         sleep(1);
      }

      if (!s_heartbeat_running) {
         break;
      }

      /* Publish heartbeat */
      if (s_mosq) {
         publish_status(s_mosq, "online", true);
      }

      /* Check for HUD timeout */
      check_hud_timeout();
   }

   OLOG_INFO("Heartbeat thread stopped");
   return NULL;
}

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

int component_status_set_lwt(struct mosquitto *mosq) {
   if (!mosq) {
      return 1;
   }

   /* Build offline message for LWT (timestamp=0 indicates LWT origin) */
   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "device", json_object_new_string("dawn"));
   json_object_object_add(msg, "msg_type", json_object_new_string("status"));
   json_object_object_add(msg, "status", json_object_new_string("offline"));
   json_object_object_add(msg, "timestamp", json_object_new_int64(0));

   const char *payload = json_object_to_json_string(msg);

   int rc = mosquitto_will_set(mosq, STATUS_TOPIC_DAWN, (int)strlen(payload), payload, 1, true);
   json_object_put(msg);

   if (rc != MOSQ_ERR_SUCCESS) {
      OLOG_ERROR("Failed to set LWT: %s", mosquitto_strerror(rc));
      return 1;
   }

   OLOG_INFO("LWT configured for %s", STATUS_TOPIC_DAWN);
   return 0;
}

int component_status_init(struct mosquitto *mosq) {
   if (!mosq) {
      return 1;
   }

   pthread_mutex_lock(&s_status_mutex);

   if (s_initialized) {
      pthread_mutex_unlock(&s_status_mutex);
      return 0;
   }

   s_mosq = mosq;

   /* Subscribe to HUD status */
   int rc = mosquitto_subscribe(mosq, NULL, STATUS_TOPIC_HUD, 1);
   if (rc != MOSQ_ERR_SUCCESS) {
      OLOG_ERROR("Failed to subscribe to %s: %s", STATUS_TOPIC_HUD, mosquitto_strerror(rc));
      pthread_mutex_unlock(&s_status_mutex);
      return 1;
   }
   OLOG_INFO("Subscribed to %s", STATUS_TOPIC_HUD);

   /* Publish online status */
   if (publish_status(mosq, "online", true) != 0) {
      pthread_mutex_unlock(&s_status_mutex);
      return 1;
   }

   /* Start heartbeat thread */
   s_heartbeat_running = true;
   if (pthread_create(&s_heartbeat_thread, NULL, heartbeat_thread_func, NULL) != 0) {
      OLOG_ERROR("Failed to create heartbeat thread");
      s_heartbeat_running = false;
      pthread_mutex_unlock(&s_status_mutex);
      return 1;
   }

   s_initialized = true;
   pthread_mutex_unlock(&s_status_mutex);

   OLOG_INFO("Component status initialized");
   return 0;
}

void component_status_publish_offline(struct mosquitto *mosq) {
   if (mosq) {
      publish_status(mosq, "offline", true);
   }
}

void component_status_shutdown(void) {
   pthread_mutex_lock(&s_status_mutex);

   if (!s_initialized) {
      pthread_mutex_unlock(&s_status_mutex);
      return;
   }

   /* Stop heartbeat thread */
   s_heartbeat_running = false;
   pthread_mutex_unlock(&s_status_mutex);

   /* Wait for thread to finish (outside mutex to avoid deadlock) */
   pthread_join(s_heartbeat_thread, NULL);

   pthread_mutex_lock(&s_status_mutex);
   s_initialized = false;
   s_mosq = NULL;
   s_hud_online = false;
   s_hud_last_seen = 0;
   pthread_mutex_unlock(&s_status_mutex);

   OLOG_INFO("Component status shutdown complete");
}

/* =============================================================================
 * Message Handling
 * ============================================================================= */

void component_status_handle_message(const char *topic, const char *payload, int payloadlen) {
   if (!topic || !payload || payloadlen <= 0) {
      return;
   }

   /* Only handle hud/status */
   if (strcmp(topic, STATUS_TOPIC_HUD) != 0) {
      return;
   }

   /* Parse JSON */
   struct json_object *root = json_tokener_parse(payload);
   if (!root) {
      OLOG_WARNING("Failed to parse HUD status message");
      return;
   }

   /* Validate msg_type */
   struct json_object *msg_type_obj = NULL;
   if (json_object_object_get_ex(root, "msg_type", &msg_type_obj)) {
      const char *msg_type = json_object_get_string(msg_type_obj);
      if (!msg_type || strcmp(msg_type, "status") != 0) {
         json_object_put(root);
         return;
      }
   }

   /* Extract status */
   struct json_object *status_obj = NULL;
   if (!json_object_object_get_ex(root, "status", &status_obj)) {
      OLOG_WARNING("HUD status message missing 'status' field");
      json_object_put(root);
      return;
   }

   const char *status = json_object_get_string(status_obj);
   bool is_online = (status && strcmp(status, "online") == 0);

   /* Extract timestamp for debugging */
   struct json_object *ts_obj = NULL;
   int64_t timestamp = 0;
   if (json_object_object_get_ex(root, "timestamp", &ts_obj)) {
      timestamp = json_object_get_int64(ts_obj);
   }

   json_object_put(root);

   /* Update state */
   pthread_mutex_lock(&s_status_mutex);
   bool was_online = s_hud_online;
   s_hud_online = is_online;
   if (is_online) {
      s_hud_last_seen = time(NULL);
   }
   pthread_mutex_unlock(&s_status_mutex);

   /* Log state change */
   if (is_online && !was_online) {
      OLOG_INFO("HUD is now ONLINE (timestamp=%ld)", (long)timestamp);

      /* Request discovery now that HUD is online */
      if (s_mosq) {
         hud_discovery_request_update(s_mosq);
      }
   } else if (!is_online && was_online) {
      OLOG_INFO("HUD is now OFFLINE (timestamp=%ld, LWT=%s)", (long)timestamp,
                timestamp == 0 ? "yes" : "no");
   }

   /* Refresh tool availability */
   if (is_online != was_online) {
      llm_tools_refresh();
      llm_tools_invalidate_cache();
      invalidate_system_instructions();
      /* Update every active session so the LLM sees the new availability
       * on its next turn without losing conversation context. */
      session_manager_refresh_all_prompts();
   }
}

/* =============================================================================
 * State Queries
 * ============================================================================= */

bool component_status_is_hud_online(void) {
   pthread_mutex_lock(&s_status_mutex);

   bool online = s_hud_online;

   /* Also check for timeout */
   if (online && s_hud_last_seen > 0) {
      time_t now = time(NULL);
      if ((now - s_hud_last_seen) > STATUS_TIMEOUT_SEC) {
         online = false;
      }
   }

   pthread_mutex_unlock(&s_status_mutex);
   return online;
}

int component_status_get_hud_age(void) {
   pthread_mutex_lock(&s_status_mutex);

   int age = -1;
   if (s_hud_last_seen > 0) {
      age = (int)(time(NULL) - s_hud_last_seen);
   }

   pthread_mutex_unlock(&s_status_mutex);
   return age;
}
