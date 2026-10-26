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
 * - Subscribes to hud/status (Mirage) and reachy/status (an expressive body)
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
#include "prompts.h"
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

/* The peers whose presence DAWN follows (under s_status_mutex). */
typedef enum {
   PEER_HUD,
   PEER_REACHY,
   PEER_COUNT
} peer_id_t;

typedef struct {
   const char *topic;
   const char *name;
   bool online;
   time_t last_seen;
} peer_t;

/* The body has been online since DAWN started: only then is its coming back news
 * (at start-up the standing directions say it). */
static bool s_reachy_seen = false;

static peer_t s_peers[PEER_COUNT] = {
   [PEER_HUD] = { STATUS_TOPIC_HUD, "HUD", false, 0 },
   [PEER_REACHY] = { STATUS_TOPIC_REACHY, "Reachy", false, 0 },
};

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
 * @brief The HUD came or went: the armor tools follow it
 */
static void hud_changed(void) {
   llm_tools_refresh();
   llm_tools_invalidate_cache();
   /* Each conversation's next turn tells the model what is unavailable
    * now (its standing directions). */
   invalidate_system_instructions();
}

/**
 * @brief The robot body came or went: the voice surface's next turn is told
 *
 * The standing directions change too, but only by leaving the expression
 * directive out, which the model doesn't read as "the body is gone"; this says
 * it once, in the next turn's context.
 */
static void reachy_changed(bool online, bool was_seen) {
   if (online && !was_seen) {
      return;
   }
   session_post_notice(session_get_local(), online ? REACHY_BACK_NOTICE : REACHY_GONE_NOTICE);
}

/**
 * @brief Mark peers whose heartbeat stopped offline
 *
 * Only the HUD has anything to do when that happens; the rest are read when
 * needed (the prompt builder asks every turn).
 */
static void check_peer_timeouts(void) {
   bool hud_lost = false;
   bool reachy_lost = false;
   time_t now = time(NULL);

   pthread_mutex_lock(&s_status_mutex);
   for (int i = 0; i < PEER_COUNT; i++) {
      peer_t *peer = &s_peers[i];
      if (peer->online && peer->last_seen > 0 && (now - peer->last_seen) > STATUS_TIMEOUT_SEC) {
         OLOG_WARNING("%s heartbeat timeout (%ld seconds), marking offline", peer->name,
                      (long)(now - peer->last_seen));
         peer->online = false;
         hud_lost = hud_lost || i == PEER_HUD;
         reachy_lost = reachy_lost || i == PEER_REACHY;
      }
   }
   pthread_mutex_unlock(&s_status_mutex);

   if (hud_lost) {
      hud_changed();
   }
   if (reachy_lost) {
      reachy_changed(false, true);
   }
}

static bool peer_is_online(peer_id_t id) {
   pthread_mutex_lock(&s_status_mutex);
   const peer_t *peer = &s_peers[id];
   bool online = peer->online &&
                 !(peer->last_seen > 0 && (time(NULL) - peer->last_seen) > STATUS_TIMEOUT_SEC);
   pthread_mutex_unlock(&s_status_mutex);
   return online;
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

      check_peer_timeouts();
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

   /* Every connect, not just the first: the client's session is clean, so the
    * broker forgets the subscription when the connection drops, and a dropped
    * connection publishes our "offline" will, which stands until we say
    * "online" again. */
   for (int i = 0; i < PEER_COUNT; i++) {
      int rc = mosquitto_subscribe(mosq, NULL, s_peers[i].topic, 1);
      if (rc != MOSQ_ERR_SUCCESS) {
         OLOG_ERROR("Failed to subscribe to %s: %s", s_peers[i].topic, mosquitto_strerror(rc));
         pthread_mutex_unlock(&s_status_mutex);
         return 1;
      }
      OLOG_INFO("Subscribed to %s", s_peers[i].topic);
   }

   if (publish_status(mosq, "online", true) != 0) {
      pthread_mutex_unlock(&s_status_mutex);
      return 1;
   }

   if (s_initialized) {
      pthread_mutex_unlock(&s_status_mutex);
      return 0;
   }

   s_mosq = mosq;

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
   s_reachy_seen = false;
   for (int i = 0; i < PEER_COUNT; i++) {
      s_peers[i].online = false;
      s_peers[i].last_seen = 0;
   }
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

   int id = 0;
   while (id < PEER_COUNT && strcmp(topic, s_peers[id].topic) != 0) {
      id++;
   }
   if (id == PEER_COUNT) {
      return;
   }
   const char *name = s_peers[id].name;

   /* Parse JSON */
   struct json_object *root = json_tokener_parse(payload);
   if (!root) {
      OLOG_WARNING("Failed to parse %s status message", name);
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
      OLOG_WARNING("%s status message missing 'status' field", name);
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
   bool was_online = s_peers[id].online;
   bool was_seen = s_reachy_seen;
   if (id == PEER_REACHY && is_online) {
      s_reachy_seen = true;
   }
   s_peers[id].online = is_online;
   if (is_online) {
      s_peers[id].last_seen = time(NULL);
   }
   pthread_mutex_unlock(&s_status_mutex);

   if (is_online && !was_online) {
      OLOG_INFO("%s is now ONLINE (timestamp=%ld)", name, (long)timestamp);
   } else if (!is_online && was_online) {
      OLOG_INFO("%s is now OFFLINE (timestamp=%ld, LWT=%s)", name, (long)timestamp,
                timestamp == 0 ? "yes" : "no");
   }

   if (id == PEER_REACHY && is_online != was_online) {
      reachy_changed(is_online, was_seen);
   }
   if (id != PEER_HUD || is_online == was_online) {
      return;
   }
   /* Request discovery now that HUD is online */
   if (is_online && s_mosq) {
      hud_discovery_request_update(s_mosq);
   }
   hud_changed();
}

/* =============================================================================
 * State Queries
 * ============================================================================= */

bool component_status_is_hud_online(void) {
   return peer_is_online(PEER_HUD);
}

bool component_status_is_reachy_online(void) {
   return peer_is_online(PEER_REACHY);
}

int component_status_get_hud_age(void) {
   pthread_mutex_lock(&s_status_mutex);

   int age = -1;
   if (s_peers[PEER_HUD].last_seen > 0) {
      age = (int)(time(NULL) - s_peers[PEER_HUD].last_seen);
   }

   pthread_mutex_unlock(&s_status_mutex);
   return age;
}
