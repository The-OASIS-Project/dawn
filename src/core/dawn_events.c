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
 * the project author(s).
 *
 * dawn/events: the local voice interface's OCP events.
 */

#include "core/dawn_events.h"

#include <json-c/json.h>
#include <mosquitto.h>
#include <string.h>

#include "core/ocp_helpers.h"
#include "core/worker_pool.h"
#include "logging.h"

void dawn_events_publish(const char *event, struct json_object *fields) {
   struct mosquitto *mosq = worker_pool_get_mosq();
   if (mosq == NULL || event == NULL) {
      json_object_put(fields);
      return;
   }
   struct json_object *msg = fields ? fields : json_object_new_object();
   if (msg == NULL) {
      return;
   }
   json_object_object_add(msg, "device", json_object_new_string("dawn"));
   json_object_object_add(msg, "msg_type", json_object_new_string("event"));
   json_object_object_add(msg, "event", json_object_new_string(event));
   json_object_object_add(msg, "surface", json_object_new_string("local"));
   json_object_object_add(msg, "timestamp", json_object_new_int64(ocp_get_timestamp_ms()));

   const char *payload = json_object_to_json_string(msg);
   int rc = mosquitto_publish(mosq, NULL, DAWN_EVENTS_TOPIC, (int)strlen(payload), payload, 0,
                              false);
   /* A broker that dropped refuses every publish until it's back: not worth a
    * line per sentence. */
   if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_NO_CONN) {
      OLOG_WARNING("dawn/events: %s not published: %s", event, mosquitto_strerror(rc));
   }
   json_object_put(msg);
}
