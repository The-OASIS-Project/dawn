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
 * WebUI Home Assistant Handler - WebSocket handlers for HA admin panel
 */

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"
#include "tools/homeassistant_service.h"
#include "tools/homeassistant_tool.h"
#include "tools/homeassistant_ws.h"
#include "webui/webui_internal.h"

/* =============================================================================
 * ha_call_service write allowlist  (server-side authorization for the HA board)
 *
 * The interactive HA board is only permitted to invoke the exact (domain,
 * service) pairs its widgets send.  Everything else HA exposes — shell_command.*,
 * python_script.*, hassio.*, arbitrary automation.trigger, etc. — is refused at
 * the browser write path, so a compromised admin session can drive the board's
 * widgets but not run arbitrary HA services.  (The LLM tool reaches HA through
 * its own dedicated functions, NOT this generic verb, so the assistant keeps
 * full service access — this gate is the browser board only.)
 *
 * ┌──────────────────────────────────────────────────────────────────────────┐
 * │ ADDING A CONTROLLABLE DOMAIN?  This table is one of THREE places that must │
 * │ move together — keep them in sync:                                         │
 * │   1. HA_BOARD_SERVICES below            — permits the write (this file).    │
 * │   2. serialize_entity_attributes()      — emits the widget's values (below).│
 * │   3. the client widget/service map      — DAWN_UI_SIGNAL_MAP.md §9.4        │
 * │      (aurora), which the browser sends from.                                │
 * │ A service the UI sends but is absent here is refused with "Service not      │
 * │ permitted"; an attribute emitted here but not consumed there is ignored.    │
 * └──────────────────────────────────────────────────────────────────────────┘
 * ============================================================================= */
static const struct {
   const char *domain;
   const char *service;
} HA_BOARD_SERVICES[] = {
   { "light", "turn_on" },
   { "light", "turn_off" },
   { "switch", "turn_on" },
   { "switch", "turn_off" },
   { "input_boolean", "turn_on" },
   { "input_boolean", "turn_off" },
   { "fan", "turn_on" },
   { "fan", "turn_off" },
   { "fan", "set_percentage" },
   { "lock", "lock" },
   { "lock", "unlock" },
   { "cover", "open_cover" },
   { "cover", "close_cover" },
   { "cover", "set_cover_position" },
   { "climate", "set_hvac_mode" },
   { "climate", "set_temperature" },
   { "media_player", "media_play" },
   { "media_player", "media_pause" },
};

static bool ha_board_service_allowed(const char *domain, const char *service) {
   if (!domain || !service)
      return false;
   for (size_t i = 0; i < sizeof(HA_BOARD_SERVICES) / sizeof(HA_BOARD_SERVICES[0]); i++) {
      if (strcmp(domain, HA_BOARD_SERVICES[i].domain) == 0 &&
          strcmp(service, HA_BOARD_SERVICES[i].service) == 0) {
         return true;
      }
   }
   return false;
}

/* =============================================================================
 * Helper: per-entity rich attributes, emitted by domain
 *
 * Only the keys relevant to the entity's domain are emitted; absent keys signal
 * the UI to fall back to a plain on/off toggle or a display row.  Additive — an
 * older client ignores the object.
 *
 * SYNC: this is the read (widget-value) half of the board's per-domain policy;
 * the write (permitted-service) half is HA_BOARD_SERVICES above.  Add a
 * controllable domain to both (see the box on HA_BOARD_SERVICES).
 * ============================================================================= */
static void serialize_entity_attributes(struct json_object *obj, const ha_entity_t *ent) {
   struct json_object *attrs = NULL;

   switch (ent->domain) {
      case HA_DOMAIN_LIGHT:
         attrs = json_object_new_object();
         json_object_object_add(attrs, "brightness", json_object_new_int(ent->brightness));
         break;
      case HA_DOMAIN_FAN:
         attrs = json_object_new_object();
         json_object_object_add(attrs, "percentage", json_object_new_int(ent->fan_percentage));
         break;
      case HA_DOMAIN_COVER:
         attrs = json_object_new_object();
         json_object_object_add(attrs, "current_position",
                                json_object_new_int(ent->cover_position));
         break;
      case HA_DOMAIN_CLIMATE:
         attrs = json_object_new_object();
         if (ent->hvac_mode[0])
            json_object_object_add(attrs, "hvac_mode", json_object_new_string(ent->hvac_mode));
         if (ent->hvac_modes_count > 0) {
            struct json_object *modes = json_object_new_array();
            for (int m = 0; m < ent->hvac_modes_count; m++)
               json_object_array_add(modes, json_object_new_string(ent->hvac_modes[m]));
            json_object_object_add(attrs, "hvac_modes", modes);
         }
         json_object_object_add(attrs, "current_temperature",
                                json_object_new_double(ent->temperature));
         json_object_object_add(attrs, "temperature", json_object_new_double(ent->target_temp));
         break;
      case HA_DOMAIN_SENSOR:
      case HA_DOMAIN_BINARY_SENSOR:
         if (ent->unit_of_measurement[0] || ent->device_class[0]) {
            attrs = json_object_new_object();
            if (ent->unit_of_measurement[0])
               json_object_object_add(attrs, "unit_of_measurement",
                                      json_object_new_string(ent->unit_of_measurement));
            if (ent->device_class[0])
               json_object_object_add(attrs, "device_class",
                                      json_object_new_string(ent->device_class));
         }
         break;
      default:
         break; /* switch/lock/media_player/etc. → plain toggle, no attributes */
   }

   if (attrs)
      json_object_object_add(obj, "attributes", attrs);
}

/* Serialize one entity into `obj` (entity_id/friendly_name/domain/state/area? +
 * domain-switched attributes). Shared by the full list and the realtime delta. */
static void serialize_one_entity(struct json_object *obj, const ha_entity_t *ent) {
   json_object_object_add(obj, "entity_id", json_object_new_string(ent->entity_id));
   json_object_object_add(obj, "friendly_name", json_object_new_string(ent->friendly_name));
   json_object_object_add(obj, "domain", json_object_new_string(ent->domain_str));
   json_object_object_add(obj, "state", json_object_new_string(ent->state));
   if (ent->area_name[0]) {
      json_object_object_add(obj, "area", json_object_new_string(ent->area_name));
   }
   serialize_entity_attributes(obj, ent);
}

/* =============================================================================
 * Helper: serialize entity list into a JSON payload
 * ============================================================================= */
static void serialize_entity_list(struct json_object *payload, const ha_entity_list_t *entities) {
   json_object_object_add(payload, "success", json_object_new_boolean(1));
   json_object_object_add(payload, "count", json_object_new_int(entities->count));
   json_object_object_add(payload, "cached_at", json_object_new_int64(entities->cached_at));

   struct json_object *arr = json_object_new_array();
   for (int i = 0; i < entities->count; i++) {
      struct json_object *obj = json_object_new_object();
      serialize_one_entity(obj, &entities->entities[i]);
      json_object_array_add(arr, obj);
   }
   json_object_object_add(payload, "entities", arr);
}

/* =============================================================================
 * Strong override of the ha_broadcast_state_changed weak hook (homeassistant_ws.c):
 * turn a coalesced batch of state_changed events into a delta frame and push it
 * to the admin board. The delta reuses the SAME per-domain shape as the full
 * list so the client's widget-value contract is unchanged. Runs on the WS
 * listener thread; reads the freshly-applied cache (copy-out under the service
 * lock) and fans out to admins only (HA state is admin-gated).
 * ============================================================================= */
void ha_broadcast_state_changed(struct json_object *dirty_map) {
   if (!dirty_map) {
      return;
   }
   struct json_object *arr = json_object_new_array();
   json_object_object_foreach(dirty_map, entity_id, val) {
      if (!val || json_object_is_type(val, json_type_null)) {
         struct json_object *e = json_object_new_object();
         json_object_object_add(e, "entity_id", json_object_new_string(entity_id));
         json_object_object_add(e, "removed", json_object_new_boolean(1));
         json_object_array_add(arr, e);
         continue;
      }
      ha_entity_t ent;
      if (!homeassistant_copy_entity(entity_id, &ent)) {
         continue; /* not in cache (unsupported domain / not applied) — skip */
      }
      struct json_object *e = json_object_new_object();
      serialize_one_entity(e, &ent); /* adds entity_id + fields + attributes */
      json_object_array_add(arr, e);
   }

   if (json_object_array_length(arr) == 0) {
      json_object_put(arr);
      return;
   }

   struct json_object *root = json_object_new_object();
   json_object_object_add(root, "type", json_object_new_string("ha_state_changed"));
   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "entities", arr);
   json_object_object_add(root, "payload", payload);
   broadcast_json_to_admins(root, /*browsers_only=*/true); /* takes ownership */
}

/* =============================================================================
 * Helper: snapshot the entity cache (optionally forcing a fresh re-poll) into a
 * heap buffer under the service read lock, then serialize it into `payload`.
 * On failure, writes success:false + error instead.  The heap copy is what makes
 * the read race-safe — we never serialize the live cache while a worker rewrites
 * it.  The buffer is large (~sizeof(ha_entity_list_t)), hence malloc not stack.
 * ============================================================================= */
static void serialize_snapshot_into(struct json_object *payload, bool force_refresh) {
   ha_entity_list_t *snap = malloc(sizeof(*snap));
   if (!snap) {
      json_object_object_add(payload, "success", json_object_new_boolean(0));
      json_object_object_add(payload, "error", json_object_new_string("Out of memory"));
      return;
   }

   ha_error_t err = homeassistant_snapshot_entities(snap, force_refresh);
   if (err == HA_OK) {
      serialize_entity_list(payload, snap);
   } else {
      json_object_object_add(payload, "success", json_object_new_boolean(0));
      json_object_object_add(payload, "error",
                             json_object_new_string(homeassistant_error_str(err)));
   }
   free(snap);
}

/* =============================================================================
 * Handler: ha_status — returns configured, connected, entity_count, version
 * ============================================================================= */
void handle_ha_status(ws_connection_t *conn) {
   if (!conn_require_admin(conn))
      return;

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("ha_status_response"));
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "configured",
                          json_object_new_boolean(homeassistant_is_configured()));
   json_object_object_add(payload, "connected",
                          json_object_new_boolean(homeassistant_is_connected()));

   ha_status_t status;
   if (homeassistant_get_status(&status) == HA_OK) {
      json_object_object_add(payload, "entity_count", json_object_new_int(status.entity_count));
      json_object_object_add(payload, "version", json_object_new_string(status.version));
      json_object_object_add(payload, "url", json_object_new_string(status.url));
   }
   json_object_object_add(payload, "led_hue_correction",
                          json_object_new_int(homeassistant_tool_get_hue_correction()));

   json_object_object_add(response, "payload", payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Handler: ha_test_connection — test HA connection
 * ============================================================================= */
void handle_ha_test_connection(ws_connection_t *conn) {
   if (!conn_require_admin(conn))
      return;

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("ha_test_connection_response"));
   struct json_object *payload = json_object_new_object();

   ha_error_t err = homeassistant_test_connection();
   json_object_object_add(payload, "success", json_object_new_boolean(err == HA_OK));
   if (err != HA_OK) {
      json_object_object_add(payload, "error",
                             json_object_new_string(homeassistant_error_str(err)));
   } else {
      ha_status_t status;
      homeassistant_get_status(&status);
      json_object_object_add(payload, "version", json_object_new_string(status.version));
   }

   json_object_object_add(response, "payload", payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Handler: ha_list_entities — serializes cached entity list
 * ============================================================================= */
void handle_ha_list_entities(ws_connection_t *conn) {
   if (!conn_require_admin(conn))
      return;

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("ha_entities_response"));
   struct json_object *payload = json_object_new_object();

   if (!homeassistant_is_connected()) {
      json_object_object_add(payload, "success", json_object_new_boolean(0));
      json_object_object_add(payload, "error", json_object_new_string("Not connected"));
   } else {
      serialize_snapshot_into(payload, /*force_refresh=*/false);
   }

   json_object_object_add(response, "payload", payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Helper: re-poll HA truth and broadcast a fresh ha_entities_response to the
 * acting admin's browser sessions.  Server-authoritative reconciliation after a
 * successful control call: the client never decides when to re-poll.  Scoped to
 * the acting user (HA data is admin-only, so this avoids leaking entity state to
 * other users' sessions).  Best-effort — a refresh failure just leaves the 30s
 * poll as the backstop.
 * ============================================================================= */
static void broadcast_fresh_entities(int user_id) {
   ha_entity_list_t *snap = malloc(sizeof(*snap));
   if (!snap)
      return;
   /* Entity-only re-poll under the read lock; bail silently on failure (the 30s
    * board poll is the backstop — we do NOT fan an error frame to every tab). */
   if (homeassistant_snapshot_entities(snap, /*force_refresh=*/true) != HA_OK) {
      free(snap);
      return;
   }

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("ha_entities_response"));
   struct json_object *payload = json_object_new_object();
   serialize_entity_list(payload, snap);
   json_object_object_add(response, "payload", payload);
   free(snap);

   /* Takes ownership of 'response'. */
   webui_broadcast_json_to_user(user_id, response, /*browsers_only=*/true);
}

/* =============================================================================
 * Handler: ha_call_service — generic HA service call (the interactive board's
 * write path).  Admin-only, like the read verbs.
 * ============================================================================= */
void handle_ha_call_service(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_admin(conn))
      return;

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("ha_call_service_response"));
   struct json_object *resp_payload = json_object_new_object();

   struct json_object *jval = NULL;
   const char *entity_id = NULL;
   const char *service = NULL;
   const char *domain = NULL;
   struct json_object *data_in = NULL;

   if (payload && json_object_object_get_ex(payload, "entity_id", &jval))
      entity_id = json_object_get_string(jval);
   if (payload && json_object_object_get_ex(payload, "service", &jval))
      service = json_object_get_string(jval);
   if (payload && json_object_object_get_ex(payload, "domain", &jval))
      domain = json_object_get_string(jval);
   if (payload && json_object_object_get_ex(payload, "data", &jval) &&
       json_object_is_type(jval, json_type_object))
      data_in = jval;

   /* Resolve the effective service domain: explicit payload domain, else the
    * entity_id prefix ("light.kitchen" -> "light").  NULL if underivable. */
   char domain_buf[32];
   const char *eff_domain = domain;
   if ((!eff_domain || !eff_domain[0]) && entity_id) {
      const char *dot = strchr(entity_id, '.');
      if (dot && (size_t)(dot - entity_id) < sizeof(domain_buf)) {
         memcpy(domain_buf, entity_id, (size_t)(dot - entity_id));
         domain_buf[dot - entity_id] = '\0';
         eff_domain = domain_buf;
      } else {
         eff_domain = NULL;
      }
   }

   /* called_ok gates the reconcile: it means "a service call actually succeeded",
    * which the initial/validation-rejected state cannot be mistaken for (an
    * `err == HA_OK` gate would fire the blocking re-poll on rejected input). */
   bool called_ok = false;
   if (!service || !service[0] || !entity_id || !entity_id[0]) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("entity_id and service are required"));
   } else if (!homeassistant_is_connected()) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "entity_id", json_object_new_string(entity_id));
      json_object_object_add(resp_payload, "error", json_object_new_string("Not connected"));
   } else if (!ha_board_service_allowed(eff_domain, service)) {
      /* Server-side write allowlist — see HA_BOARD_SERVICES. */
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "entity_id", json_object_new_string(entity_id));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Service not permitted"));
   } else {
      /* Deep-copy the service data into a tree we own: homeassistant_call_service
       * takes ownership, and the request payload tree is freed by the dispatcher.
       * A copy (not a refcount bump) is required because call_service_json mutates
       * the object (adds entity_id) — bumping would corrupt the shared payload. */
      struct json_object *data_owned = NULL;
      bool data_copy_failed = false;
      if (data_in) {
         const char *ds = json_object_to_json_string(data_in);
         data_owned = ds ? json_tokener_parse(ds) : NULL;
         if (!data_owned)
            data_copy_failed = true;
      }

      if (data_copy_failed) {
         /* Fail rather than silently invoke the service with no parameters. */
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "entity_id", json_object_new_string(entity_id));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Invalid service data"));
      } else {
         ha_error_t err = homeassistant_call_service(eff_domain, service, entity_id, data_owned);
         called_ok = (err == HA_OK);

         json_object_object_add(resp_payload, "success", json_object_new_boolean(err == HA_OK));
         json_object_object_add(resp_payload, "entity_id", json_object_new_string(entity_id));
         if (err != HA_OK) {
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string(homeassistant_error_str(err)));
         } else {
            json_object_object_add(resp_payload, "error", NULL);
         }
      }
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);

   /* Server-authoritative reconcile: on success, re-poll HA and push fresh truth
    * to the acting admin's browsers so the board reflects reality without the UI
    * orchestrating a re-poll. When realtime is live, SKIP the blocking full
    * re-poll — HA emits a state_changed for the controlled entity and the WS
    * listener already pushes that delta to all admin boards. */
   if (called_ok && !homeassistant_ws_is_connected())
      broadcast_fresh_entities(conn->auth_user_id);
}

/* =============================================================================
 * Handler: ha_refresh_entities — force cache refresh
 * ============================================================================= */
void handle_ha_refresh_entities(ws_connection_t *conn) {
   if (!conn_require_admin(conn))
      return;

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("ha_entities_response"));
   struct json_object *payload = json_object_new_object();

   if (!homeassistant_is_connected()) {
      json_object_object_add(payload, "success", json_object_new_boolean(0));
      json_object_object_add(payload, "error", json_object_new_string("Not connected"));
   } else {
      serialize_snapshot_into(payload, /*force_refresh=*/true);
   }

   json_object_object_add(response, "payload", payload);
   send_json_response(conn, response);
   json_object_put(response);
}
