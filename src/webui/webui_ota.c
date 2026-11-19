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
 * OTA WebSocket transport — see webui_ota.h.
 */

#include "webui/webui_ota.h"

#include <json-c/json.h>
#include <string.h>

#include "auth/auth_db.h"
#include "core/ota.h"
#include "core/ota_db.h"
#include "core/ota_manifest.h" /* ota_platform_t */
#include "core/ota_rollout.h"
#include "core/session_manager.h"
#include "logging.h"

/* Resolve the satellite uuid from a registered satellite connection. */
static const char *conn_satellite_uuid(ws_connection_t *conn) {
   if (!conn || !conn->is_satellite || !conn->session) {
      return NULL;
   }
   return conn->session->identity.uuid;
}

void handle_ota_status(ws_connection_t *conn, struct json_object *payload) {
   const char *uuid = conn_satellite_uuid(conn);
   if (!uuid || !payload) {
      return;
   }
   struct json_object *state_obj = NULL;
   if (!json_object_object_get_ex(payload, "state", &state_obj)) {
      send_error_impl(conn->wsi, "INVALID_MESSAGE", "ota_status missing 'state'");
      return;
   }
   const char *state = json_object_get_string(state_obj);
   /* Satellite-supplied: reject unknown state tokens (they would be stored
    * verbatim and dodge the in-flight predicate).  Free-text fields (error/reason
    * /version) are width-clamped at the DB chokepoint in ota_db.c. */
   if (!ota_state_is_valid(state)) {
      send_error_impl(conn->wsi, "INVALID_MESSAGE", "ota_status invalid 'state'");
      return;
   }
   const char *error = NULL;
   struct json_object *err_obj = NULL;
   if (json_object_object_get_ex(payload, "error", &err_obj)) {
      error = json_object_get_string(err_obj);
   }
   /* set_state moves 'failed' out of the in-flight predicate, so a failed
    * device is automatically eligible for a re-push (no manual unlock). */
   ota_report_status(uuid, state, error);
   OLOG_INFO("OTA: %s status=%s%s%s", uuid, state, error ? " error=" : "", error ? error : "");
}

void handle_ota_ack(ws_connection_t *conn, struct json_object *payload) {
   const char *uuid = conn_satellite_uuid(conn);
   if (!uuid) {
      return;
   }
   (void)payload;
   /* Informational: the authoritative downloading transition happens when the
    * device consumes its download token. */
   OLOG_INFO("OTA: %s acknowledged offer", uuid);
}

void handle_ota_reject(ws_connection_t *conn, struct json_object *payload) {
   const char *uuid = conn_satellite_uuid(conn);
   if (!uuid) {
      return;
   }
   const char *reason = "rejected";
   struct json_object *reason_obj = NULL;
   if (payload && json_object_object_get_ex(payload, "reason", &reason_obj)) {
      reason = json_object_get_string(reason_obj);
   }
   ota_report_status(uuid, OTA_STATE_FAILED, reason);
   OLOG_WARNING("OTA: %s rejected offer: %s", uuid, reason);
}

/* Build + queue the ota_offer JSON for a (held) satellite session. */
static void send_offer(session_t *session, const ota_offer_t *offer) {
   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "type", json_object_new_string("ota_offer"));
   struct json_object *p = json_object_new_object();
   json_object_object_add(p, "platform", json_object_new_string(offer->platform_str));
   json_object_object_add(p, "version", json_object_new_string(offer->version));
   json_object_object_add(p, "url_path", json_object_new_string(offer->url_path));
   json_object_object_add(p, "token", json_object_new_string(offer->token));
   json_object_object_add(p, "image_size", json_object_new_int64((int64_t)offer->image_size));
   json_object_object_add(p, "sha256", json_object_new_string(offer->sha256_hex));
   json_object_object_add(p, "manifest", json_object_new_string(offer->manifest_hex));
   json_object_object_add(p, "sig", json_object_new_string(offer->sig_hex));
   json_object_object_add(p, "allow_downgrade", json_object_new_boolean(offer->allow_downgrade));
   json_object_object_add(msg, "payload", p);

   ws_response_t resp = { 0 };
   resp.session = session;
   resp.type = WS_RESP_JSON;
   resp.generic_json.json = strdup(json_object_to_json_string(msg));
   queue_response(&resp);

   json_object_put(msg);
}

int webui_ota_push(const char *uuid, const char *version, bool allow_downgrade) {
   if (!uuid || !uuid[0] || !version || !version[0]) {
      return AUTH_DB_FAILURE;
   }

   /* tier → platform (tier 1 = RPi, tier 2 = ESP32). */
   satellite_mapping_t sat;
   if (satellite_db_get(uuid, &sat) != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }
   ota_platform_t platform = (sat.tier == 2) ? OTA_PLATFORM_ESP32 : OTA_PLATFORM_RPI;

   /* Require the device online before reserving an offer (avoids an orphaned
    * in-flight row for an unreachable device). */
   session_t *session = session_find_by_uuid(uuid);
   if (!session || session->disconnected) {
      if (session) {
         session_release(session);
      }
      return AUTH_DB_NOT_FOUND;
   }
   /* Only offer to a device that advertised OTA support at registration
    * (DAP2 capabilities.ota).  Offering to one that didn't opt in would, if its
    * firmware ignores the offer, wedge the single-flight 'offered' row until the
    * download token expires. */
   if (!session_capabilities(session).ota) {
      session_release(session);
      OLOG_WARNING("OTA: %s does not advertise OTA capability — push refused", uuid);
      return AUTH_DB_FAILURE;
   }

   ota_offer_t offer;
   int rc = ota_begin_push(uuid, platform, version, allow_downgrade, &offer);
   if (rc != SUCCESS) {
      session_release(session);
      return (rc == AUTH_DB_LOCKED) ? AUTH_DB_LOCKED : AUTH_DB_FAILURE;
   }

   send_offer(session, &offer);
   session_release(session);
   OLOG_INFO("OTA: pushed %s to %s", version, uuid);
   return AUTH_DB_SUCCESS;
}

/* ===== Admin (WebUI) handlers ===== */

void handle_ota_push(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_admin(conn)) {
      return;
   }
   struct json_object *u = NULL, *v = NULL, *d = NULL;
   if (!payload || !json_object_object_get_ex(payload, "uuid", &u) ||
       !json_object_object_get_ex(payload, "version", &v)) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "ota_push requires uuid + version");
      return;
   }
   const char *uuid = json_object_get_string(u);
   const char *version = json_object_get_string(v);
   bool allow_downgrade = false;
   if (json_object_object_get_ex(payload, "allow_downgrade", &d)) {
      allow_downgrade = json_object_get_boolean(d);
   }

   int rc = webui_ota_push(uuid, version, allow_downgrade);
   const char *err = NULL;
   switch (rc) {
      case AUTH_DB_SUCCESS:
         break;
      case AUTH_DB_LOCKED:
         err = "device is already mid-update";
         break;
      case AUTH_DB_NOT_FOUND:
         err = "device is offline";
         break;
      default:
         err = "no matching release, or push failed";
         break;
   }
   if (err) {
      send_error_impl(conn->wsi, "OTA_ERROR", err);
      return;
   }

   struct json_object *resp = json_object_new_object();
   json_object_object_add(resp, "type", json_object_new_string("ota_push_response"));
   struct json_object *p = json_object_new_object();
   json_object_object_add(p, "success", json_object_new_boolean(1));
   json_object_object_add(p, "uuid", json_object_new_string(uuid));
   json_object_object_add(p, "version", json_object_new_string(version));
   json_object_object_add(resp, "payload", p);
   send_json_response(conn, resp);
   json_object_put(resp);
}

void handle_ota_push_all(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_admin(conn)) {
      return;
   }
   struct json_object *pl = NULL, *v = NULL, *d = NULL;
   if (!payload || !json_object_object_get_ex(payload, "platform", &pl) ||
       !json_object_object_get_ex(payload, "version", &v)) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "ota_push_all requires platform + version");
      return;
   }
   const char *platform_s = json_object_get_string(pl);
   const char *version = json_object_get_string(v);
   bool allow_downgrade = false;
   if (json_object_object_get_ex(payload, "allow_downgrade", &d)) {
      allow_downgrade = json_object_get_boolean(d);
   }

   ota_platform_t platform;
   int tier;
   if (platform_s && strcmp(platform_s, "esp32") == 0) {
      platform = OTA_PLATFORM_ESP32;
      tier = 2;
   } else if (platform_s && strcmp(platform_s, "rpi") == 0) {
      platform = OTA_PLATFORM_RPI;
      tier = 1;
   } else {
      send_error_impl(conn->wsi, "INVALID_PARAM", "platform must be 'rpi' or 'esp32'");
      return;
   }

   char summary[256];
   int rc = ota_rollout_start(platform, tier, version, allow_downgrade, summary, sizeof(summary));
   if (rc != SUCCESS) {
      send_error_impl(conn->wsi, "OTA_ERROR",
                      summary[0] ? summary : "rollout could not be started");
      return;
   }
   struct json_object *resp = json_object_new_object();
   json_object_object_add(resp, "type", json_object_new_string("ota_push_all_response"));
   struct json_object *p = json_object_new_object();
   json_object_object_add(p, "success", json_object_new_boolean(1));
   json_object_object_add(p, "summary", json_object_new_string(summary));
   json_object_object_add(resp, "payload", p);
   send_json_response(conn, resp);
   json_object_put(resp);
}

void handle_ota_rollout_status(ws_connection_t *conn) {
   if (!conn_require_admin(conn)) {
      return;
   }
   char status[512];
   if (ota_rollout_status(status, sizeof(status)) != SUCCESS) {
      status[0] = '\0';
   }
   struct json_object *resp = json_object_new_object();
   json_object_object_add(resp, "type", json_object_new_string("ota_rollout_status_response"));
   struct json_object *p = json_object_new_object();
   json_object_object_add(p, "status", json_object_new_string(status));
   json_object_object_add(resp, "payload", p);
   send_json_response(conn, resp);
   json_object_put(resp);
}

void handle_ota_rollout_abort(ws_connection_t *conn) {
   if (!conn_require_admin(conn)) {
      return;
   }
   bool aborted = (ota_rollout_abort() == SUCCESS);
   struct json_object *resp = json_object_new_object();
   json_object_object_add(resp, "type", json_object_new_string("ota_rollout_abort_response"));
   struct json_object *p = json_object_new_object();
   json_object_object_add(p, "aborted", json_object_new_boolean(aborted));
   json_object_object_add(resp, "payload", p);
   send_json_response(conn, resp);
   json_object_put(resp);
}

void handle_ota_list(ws_connection_t *conn) {
   if (!conn_require_admin(conn)) {
      return;
   }
   struct json_object *resp = json_object_new_object();
   json_object_object_add(resp, "type", json_object_new_string("ota_list_response"));
   struct json_object *p = json_object_new_object();
   json_object_object_add(p, "enabled", json_object_new_boolean(ota_enabled()));

   struct json_object *arr = json_object_new_array();
   int n = ota_release_count();
   for (int i = 0; i < n; i++) {
      ota_platform_t plat = OTA_PLATFORM_UNKNOWN;
      int tier = 0;
      char ver[OTA_VERSION_MAX] = "";
      if (ota_release_info(i, &plat, &tier, ver, sizeof(ver)) != SUCCESS) {
         continue;
      }
      struct json_object *r = json_object_new_object();
      json_object_object_add(r, "platform",
                             json_object_new_string(plat == OTA_PLATFORM_ESP32 ? "esp32" : "rpi"));
      json_object_object_add(r, "tier", json_object_new_int(tier));
      json_object_object_add(r, "version", json_object_new_string(ver));
      json_object_array_add(arr, r);
   }
   json_object_object_add(p, "releases", arr);
   json_object_object_add(resp, "payload", p);
   send_json_response(conn, resp);
   json_object_put(resp);
}
