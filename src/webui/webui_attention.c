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
 * WebUI Watches (SAGE proactive attention) — per-user attention_rules CRUD.
 *
 * The visual counterpart to the conversational `attention` LLM tool
 * (src/tools/attention_tool.c).  Both surfaces drive the same attention_watch_*
 * core API and preserve its one-watch-per-metric model, so a watch created by
 * voice is editable in the panel and vice-versa.  Every handler is scoped to
 * conn->auth_user_id — the client payload is never trusted for identity, and
 * the core API is itself user-scoped (WHERE user_id = ?).
 */

#include "webui/webui_attention.h"

#include <json-c/json.h>
#include <math.h>
#include <string.h>

#include "config/dawn_config.h"
#include "core/attention/attention.h"
#include "dawn_error.h"
#include "logging.h"
#include "webui/webui_internal.h"

/* Enum <-> wire-string mapping + clamp_seconds are shared with the `attention`
 * tool via the attention core (sage_*_to_str / sage_*_from_str /
 * attention_clamp_seconds in attention.h) so the two surfaces can't drift. */

/* =============================================================================
 * Response helpers
 * ============================================================================= */

/* Send a bare {type, payload:{success[, error]}} reply and free it. */
static void respond_status(ws_connection_t *conn, const char *type, bool ok, const char *error) {
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string(type));
   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "success", json_object_new_boolean(ok));
   if (!ok && error) {
      json_object_object_add(payload, "error", json_object_new_string(error));
   }
   json_object_object_add(response, "payload", payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* Serialize one watch, enriched with catalog label/unit + the live reading so
 * the panel shows "now 480 ppm" without a second round-trip. */
static json_object *watch_to_json(const sage_watch_t *w) {
   json_object *o = json_object_new_object();
   json_object_object_add(o, "id", json_object_new_int64(w->id));
   json_object_object_add(o, "name", json_object_new_string(w->name));
   json_object_object_add(o, "metric", json_object_new_string(w->metric));
   const char *label = attention_catalog_label(w->metric);
   json_object_object_add(o, "label", json_object_new_string(label ? label : w->metric));
   json_object_object_add(o, "unit", json_object_new_string(attention_catalog_unit(w->metric)));
   json_object_object_add(o, "rule_type",
                          json_object_new_string(sage_rule_type_to_str(w->rule_type)));
   json_object_object_add(o, "direction",
                          json_object_new_string(sage_direction_to_str(w->direction)));
   /* threshold is a concrete number for stored rows (the template resolves the
    * catalog default); guard NAN anyway so we never emit invalid JSON. */
   if (isfinite(w->threshold)) {
      json_object_object_add(o, "threshold", json_object_new_double(w->threshold));
   }
   json_object_object_add(o, "absence_after_sec", json_object_new_int(w->absence_after_sec));
   json_object_object_add(o, "notify", json_object_new_string(sage_notify_to_str(w->notify)));
   json_object_object_add(o, "enabled", json_object_new_boolean(w->enabled));
   json_object_object_add(o, "source", json_object_new_string(w->source_tag));

   double cur = 0.0;
   bool have = attention_metric_current(w->metric, &cur);
   json_object_object_add(o, "has_current", json_object_new_boolean(have));
   if (have && isfinite(cur)) {
      json_object_object_add(o, "current", json_object_new_double(cur));
   }
   return o;
}

/* Find a user's watch by id (linear over the bounded per-user list). */
static bool find_watch_by_id(int user_id, int64_t id, sage_watch_t *out) {
   sage_watch_t watches[SAGE_MAX_WATCHES_PER_USER];
   int count = 0;
   if (attention_watch_list(user_id, watches, SAGE_MAX_WATCHES_PER_USER, &count) != SUCCESS) {
      return false;
   }
   for (int i = 0; i < count; i++) {
      if (watches[i].id == id) {
         *out = watches[i];
         return true;
      }
   }
   return false;
}

/* Apply the optional threshold/direction/notify overrides from a payload onto a
 * watch template (shared by add + update). */
static void apply_overrides(sage_watch_t *w, json_object *payload) {
   json_object *o = NULL;
   if (json_object_object_get_ex(payload, "direction", &o)) {
      w->direction = sage_direction_from_str(json_object_get_string(o), w->direction);
   }
   if (json_object_object_get_ex(payload, "notify", &o)) {
      w->notify = sage_notify_from_str(json_object_get_string(o), w->notify);
   }
   if (json_object_object_get_ex(payload, "threshold", &o)) {
      double t = json_object_get_double(o);
      if (isfinite(t)) {
         if (w->rule_type == SAGE_RULE_ABSENCE) {
            w->absence_after_sec = attention_clamp_seconds(t);
         } else {
            w->threshold = t;
         }
      }
   }
}

/* Extract a positive int64 watch id from @payload.  On a missing/non-positive
 * id it sends the error reply (@resp_type) and returns false. */
static bool require_watch_id(ws_connection_t *conn,
                             json_object *payload,
                             const char *resp_type,
                             int64_t *out) {
   json_object *id_obj = NULL;
   int64_t id = 0;
   if (json_object_object_get_ex(payload, "id", &id_obj)) {
      id = json_object_get_int64(id_obj);
   }
   if (id <= 0) {
      respond_status(conn, resp_type, false, "Missing watch id");
      return false;
   }
   *out = id;
   return true;
}

/* =============================================================================
 * Handlers
 * ============================================================================= */

void handle_watch_list(ws_connection_t *conn) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("watch_list_response"));
   json_object *payload = json_object_new_object();

   sage_watch_t watches[SAGE_MAX_WATCHES_PER_USER];
   int count = 0;
   int rc = attention_watch_list(conn->auth_user_id, watches, SAGE_MAX_WATCHES_PER_USER, &count);

   if (rc != SUCCESS) {
      json_object_object_add(payload, "success", json_object_new_boolean(0));
      json_object_object_add(payload, "error", json_object_new_string("Failed to list watches"));
   } else {
      json_object_object_add(payload, "success", json_object_new_boolean(1));
      /* Master switch — the panel surfaces a "proactive attention is off" note
       * (with a link to Settings) so a watch that can't fire is honest about it. */
      json_object_object_add(payload, "attention_enabled",
                             json_object_new_boolean(g_config.attention.enabled));

      json_object *arr = json_object_new_array();
      for (int i = 0; i < count; i++) {
         json_object_array_add(arr, watch_to_json(&watches[i]));
      }
      json_object_object_add(payload, "watches", arr);

      /* The metric catalog powers the add-watch dropdown (single source of
       * truth — same list the `attention` tool generates its schema from). */
      json_object *cat = json_object_new_array();
      int n = attention_catalog_count();
      for (int i = 0; i < n; i++) {
         const char *key = attention_catalog_key(i);
         if (!key) {
            continue;
         }
         json_object *c = json_object_new_object();
         json_object_object_add(c, "key", json_object_new_string(key));
         const char *label = attention_catalog_label(key);
         json_object_object_add(c, "label", json_object_new_string(label ? label : key));
         json_object_object_add(c, "unit", json_object_new_string(attention_catalog_unit(key)));
         json_object_array_add(cat, c);
      }
      json_object_object_add(payload, "catalog", cat);
   }

   json_object_object_add(response, "payload", payload);
   send_json_response(conn, response);
   json_object_put(response);
}

void handle_watch_add(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *o = NULL;
   const char *metric = NULL;
   if (json_object_object_get_ex(payload, "metric", &o)) {
      metric = json_object_get_string(o);
   }
   if (!metric || !metric[0]) {
      respond_status(conn, "watch_add_response", false, "Missing metric");
      return;
   }

   sage_watch_t w;
   if (attention_watch_template(metric, conn->auth_user_id, &w) != SUCCESS) {
      respond_status(conn, "watch_add_response", false, "Unknown metric");
      return;
   }
   apply_overrides(&w, payload);

   /* One watch per metric (mirrors the tool's do_watch): update the existing
    * row if the user already watches this metric, so the two surfaces agree. */
   sage_watch_t existing;
   bool updating = (attention_watch_find_by_metric(conn->auth_user_id, metric, &existing) ==
                    SUCCESS);
   int rc = updating ? attention_watch_update(conn->auth_user_id, existing.id, &w)
                     : attention_watch_add(&w, NULL);
   if (rc != SUCCESS) {
      respond_status(conn, "watch_add_response", false,
                     "Couldn't save the watch (you may be at the watch limit)");
      return;
   }
   if (updating && !existing.enabled) {
      attention_watch_set_enabled(conn->auth_user_id, existing.id, true);
   }
   respond_status(conn, "watch_add_response", true, NULL);
}

void handle_watch_update(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   int64_t id = 0;
   if (!require_watch_id(conn, payload, "watch_update_response", &id)) {
      return;
   }

   sage_watch_t w;
   if (!find_watch_by_id(conn->auth_user_id, id, &w)) {
      respond_status(conn, "watch_update_response", false, "Watch not found");
      return;
   }
   apply_overrides(&w, payload);

   if (attention_watch_update(conn->auth_user_id, id, &w) != SUCCESS) {
      respond_status(conn, "watch_update_response", false, "Couldn't update the watch");
      return;
   }
   respond_status(conn, "watch_update_response", true, NULL);
}

void handle_watch_set_enabled(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   int64_t id = 0;
   if (!require_watch_id(conn, payload, "watch_set_enabled_response", &id)) {
      return;
   }
   json_object *en_obj = NULL;
   bool enabled = true;
   if (json_object_object_get_ex(payload, "enabled", &en_obj)) {
      enabled = json_object_get_boolean(en_obj);
   }

   if (attention_watch_set_enabled(conn->auth_user_id, id, enabled) != SUCCESS) {
      respond_status(conn, "watch_set_enabled_response", false, "Couldn't update the watch");
      return;
   }
   respond_status(conn, "watch_set_enabled_response", true, NULL);
}

void handle_watch_remove(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   int64_t id = 0;
   if (!require_watch_id(conn, payload, "watch_remove_response", &id)) {
      return;
   }

   if (attention_watch_remove(conn->auth_user_id, id) != SUCCESS) {
      respond_status(conn, "watch_remove_response", false, "Couldn't remove the watch");
      return;
   }
   respond_status(conn, "watch_remove_response", true, NULL);
}

void handle_watch_readings_subscribe(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   /* Opt in/out of the 1 Hz live-gauge stream.  The panel subscribes when it's
    * shown and unsubscribes when hidden, so the stream only flows while someone is
    * actually looking at it.  Default OFF when the flag is absent: a malformed or
    * partial payload must not silently start a recurring stream (the real client
    * always sends an explicit `enabled`). */
   bool enabled = false;
   json_object *o = NULL;
   if (payload && json_object_object_get_ex(payload, "enabled", &o)) {
      enabled = json_object_get_boolean(o);
   }
   conn->watch_readings_subscribed = enabled;
   respond_status(conn, "watch_readings_subscribe_response", true, NULL);
}
