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
 * WebUI Admin Satellite Handlers - Satellite management endpoints (admin-only)
 *
 * Provides list/update/delete operations for satellite-to-user mappings.
 * Changes are DB-only and take effect on satellite's next reconnect.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/ota_db.h"
#include "core/rate_limiter.h"
#include "logging.h"
#include "utils/string_utils.h"
#include "webui/webui_internal.h"

#ifdef DAWN_ENABLE_HOMEASSISTANT_TOOL
#include "tools/homeassistant_service.h"
#endif

/* Maximum satellites we expect (for stack-allocated collection buffer) */
#define MAX_SATELLITE_MAPPINGS 64

/* =============================================================================
 * Shared: Serialize satellite_mapping_t to json_object
 * ============================================================================= */

static json_object *satellite_mapping_to_json(const satellite_mapping_t *m, bool online) {
   json_object *sat = json_object_new_object();
   json_object_object_add(sat, "uuid", json_object_new_string(m->uuid));
   json_object_object_add(sat, "name", json_object_new_string(m->name));
   json_object_object_add(sat, "location", json_object_new_string(m->location));
   json_object_object_add(sat, "ha_area", json_object_new_string(m->ha_area));
   json_object_object_add(sat, "user_id", json_object_new_int(m->user_id));
   json_object_object_add(sat, "tier", json_object_new_int(m->tier));
   json_object_object_add(sat, "enabled", json_object_new_boolean(m->enabled));
   json_object_object_add(sat, "last_seen", json_object_new_int64((int64_t)m->last_seen));
   json_object_object_add(sat, "created_at", json_object_new_int64((int64_t)m->created_at));
   json_object_object_add(sat, "online", json_object_new_boolean(online));

   /* OTA fleet visibility (Phase 1): reported firmware version + any in-flight
    * update state.  Looked up here (outside the satellite_db lock — callers
    * serialize from a collected array or post-update) so the panel can show
    * which devices are behind.  Absent row → unknown/idle defaults. */
   ota_device_state_t ota;
   if (ota_db_get(m->uuid, &ota) == AUTH_DB_SUCCESS) {
      json_object_object_add(sat, "firmware_version", json_object_new_string(ota.current_version));
      json_object_object_add(sat, "ota_state", json_object_new_string(ota.state));
      json_object_object_add(sat, "ota_target_version", json_object_new_string(ota.target_version));
   } else {
      json_object_object_add(sat, "firmware_version", json_object_new_string(""));
      json_object_object_add(sat, "ota_state", json_object_new_string("idle"));
      json_object_object_add(sat, "ota_target_version", json_object_new_string(""));
   }
   return sat;
}

/* =============================================================================
 * Callback: Collect satellite mappings into array (no online-status lookup)
 *
 * This avoids calling webui_is_satellite_online() while the DB mutex is held,
 * preventing a db_mutex -> conn_registry_mutex lock ordering dependency.
 * ============================================================================= */

typedef struct {
   satellite_mapping_t *mappings;
   int count;
   int capacity;
} satellite_collect_ctx_t;

static int collect_satellites_callback(const satellite_mapping_t *mapping, void *ctx) {
   satellite_collect_ctx_t *collect = (satellite_collect_ctx_t *)ctx;
   if (collect->count >= collect->capacity) {
      OLOG_WARNING("Satellite list truncated at %d entries", collect->capacity);
      return 1; /* Stop iteration */
   }
   collect->mappings[collect->count] = *mapping;
   collect->count++;
   return 0;
}

/* =============================================================================
 * Callback: Build minimal user list for dropdown
 * ============================================================================= */

static int list_users_minimal_callback(const auth_user_summary_t *user, void *ctx) {
   json_object *array = (json_object *)ctx;
   json_object *u = json_object_new_object();
   json_object_object_add(u, "id", json_object_new_int(user->id));
   json_object_object_add(u, "display_name", json_object_new_string(user->username));
   json_object_array_add(array, u);
   return 0;
}

/* =============================================================================
 * Handlers
 * ============================================================================= */

void handle_list_satellites(ws_connection_t *conn) {
   if (!conn_require_admin(conn))
      return;

   /* Collect mappings from DB (releases DB lock before online-status check) */
   satellite_mapping_t mappings[MAX_SATELLITE_MAPPINGS];
   satellite_collect_ctx_t ctx = { .mappings = mappings,
                                   .count = 0,
                                   .capacity = MAX_SATELLITE_MAPPINGS };
   satellite_db_list(collect_satellites_callback, &ctx);

   /* Build JSON array with online status (now outside DB lock) */
   json_object *sat_array = json_object_new_array();
   for (int i = 0; i < ctx.count; i++) {
      bool online = webui_is_satellite_online(mappings[i].uuid);
      json_object_array_add(sat_array, satellite_mapping_to_json(&mappings[i], online));
   }

   /* Build minimal user list for dropdown */
   json_object *users_array = json_object_new_array();
   auth_db_list_users(list_users_minimal_callback, users_array);

   /* Build response */
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("list_satellites_response"));

   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "satellites", sat_array);
   json_object_object_add(payload, "users", users_array);

   /* Include HA area names for dropdown if available */
#ifdef DAWN_ENABLE_HOMEASSISTANT_TOOL
   if (homeassistant_is_configured()) {
      char area_names[128][64];
      int area_count = homeassistant_list_areas(area_names, 128);
      if (area_count > 0) {
         json_object *areas_array = json_object_new_array();
         for (int i = 0; i < area_count; i++) {
            json_object_array_add(areas_array, json_object_new_string(area_names[i]));
         }
         json_object_object_add(payload, "ha_areas", areas_array);
      }
   }
#endif

   json_object_object_add(response, "payload", payload);
   send_json_response(conn, response);
   json_object_put(response);
}

void handle_update_satellite(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_admin(conn))
      return;

   struct json_object *uuid_obj;
   if (!json_object_object_get_ex(payload, "uuid", &uuid_obj)) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Missing 'uuid'");
      return;
   }

   const char *uuid = json_object_get_string(uuid_obj);
   if (!uuid || strlen(uuid) != 36) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Invalid UUID");
      return;
   }

   /* Get current mapping */
   satellite_mapping_t mapping;
   int rc = satellite_db_get(uuid, &mapping);
   if (rc == AUTH_DB_NOT_FOUND) {
      send_error_impl(conn->wsi, "NOT_FOUND", "Satellite not found");
      return;
   } else if (rc != AUTH_DB_SUCCESS) {
      send_error_impl(conn->wsi, "DB_ERROR", "Database error");
      return;
   }

   /* Apply requested updates */
   struct json_object *user_id_obj, *ha_area_obj;
   if (json_object_object_get_ex(payload, "user_id", &user_id_obj)) {
      int new_user_id = json_object_get_int(user_id_obj);
      satellite_db_update_user(uuid, new_user_id);
      mapping.user_id = new_user_id;
   }

   if (json_object_object_get_ex(payload, "ha_area", &ha_area_obj)) {
      const char *ha_area = json_object_get_string(ha_area_obj);
      if (ha_area) {
         /* Sanitize: allowlist alphanumeric, spaces, hyphens, underscores */
         char safe[SATELLITE_LOCATION_MAX];
         safe_strscpy(safe, ha_area);
         for (char *p = safe; *p; p++) {
            if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == ' ' || *p == '-' || *p == '_'))
               *p = '_';
         }
         satellite_db_update_location(uuid, mapping.location, safe);
         safe_strscpy(mapping.ha_area, safe);
      }
   }

   /* Force-reconnect so satellite picks up new config immediately */
   webui_force_disconnect_satellite(uuid);

   /* Build response — satellite will show offline briefly until it reconnects */
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("update_satellite_response"));

   json_object *resp_payload = json_object_new_object();
   json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   json_object_object_add(resp_payload, "satellite", satellite_mapping_to_json(&mapping, false));

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);

   OLOG_INFO("Admin: Updated satellite %s (%s) user_id=%d ha_area='%s'", mapping.name, uuid,
             mapping.user_id, mapping.ha_area);
}

void handle_delete_satellite(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_admin(conn))
      return;

   struct json_object *uuid_obj;
   if (!json_object_object_get_ex(payload, "uuid", &uuid_obj)) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Missing 'uuid'");
      return;
   }

   const char *uuid = json_object_get_string(uuid_obj);
   if (!uuid || strlen(uuid) != 36) {
      send_error_impl(conn->wsi, "INVALID_PARAM", "Invalid UUID");
      return;
   }

   /* The local pseudo-satellite represents the daemon's own speaker — deleting
    * it doesn't make sense. Admin can still disable or reassign it. */
   if (satellite_is_local_pseudo(uuid)) {
      send_error_impl(conn->wsi, "FORBIDDEN",
                      "Local device cannot be deleted; reassign to another user instead");
      return;
   }

   /* Force-disconnect if online before removing mapping */
   webui_force_disconnect_satellite(uuid);

   int rc = satellite_db_delete(uuid);
   if (rc == AUTH_DB_NOT_FOUND) {
      send_error_impl(conn->wsi, "NOT_FOUND", "Satellite not found");
      return;
   } else if (rc != AUTH_DB_SUCCESS) {
      send_error_impl(conn->wsi, "DB_ERROR", "Failed to delete satellite");
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("delete_satellite_response"));

   json_object *resp_payload = json_object_new_object();
   json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   json_object_object_add(resp_payload, "uuid", json_object_new_string(uuid));

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);

   OLOG_INFO("Admin: Deleted satellite mapping %s", uuid);
}

/* =============================================================================
 * Registration-key fetch (admin-only, QR-code pairing surface)
 *
 * Surfaces secrets->satellite_registration_key to the admin WebUI client so it
 * can render a QR code for Android satellite pairing.  The key value never
 * appears in any log line — only `[set]`/`[not set]` and a session-token hash
 * for correlation.  Rate-limited per source IP to limit the blast radius of a
 * compromised admin session.
 * ============================================================================= */

/* Two stacked core/rate_limiter instances express the dual-window contract.
 * Migrated from a per-file FNV-bucket reimplementation 2026-05-19 — see
 * docs/TODO.md "Extract shared webui_rate_limit.{c,h}".  Linear-scan + LRU
 * eviction in the underlying primitive eliminates the FNV-collision-bypass
 * surface entirely; the truncation fail-closed contract lives in
 * rate_limiter_check itself.
 *
 * IMPORTANT: these two limiters must be checked together via
 * key_fetch_rate_check() to enforce the dual-window contract.  Direct
 * rate_limiter_check on either alone bypasses one of the two windows. */
/* Asymmetric slot counts: hour-window state is the higher-cardinality-attack
 * surface (an attacker cycling >N distinct /64s within an hour can LRU-evict
 * a legitimate admin's burned hour-budget).  Sizing hour at 256 raises the
 * floor 4x with ~12 KB extra .bss; the min window's 60s natural rollover
 * limits the same attack to one window per minute, so 64 slots remains
 * adequate there. */
#define KEY_FETCH_MIN_RATE_SLOTS 64
#define KEY_FETCH_HOUR_RATE_SLOTS 256
#define KEY_FETCH_PER_MIN_LIMIT 5
#define KEY_FETCH_PER_HOUR_LIMIT 30
#define KEY_FETCH_MIN_WINDOW_SEC 60
#define KEY_FETCH_HOUR_WINDOW_SEC 3600

/* Registration-key hex length (32 bytes -> 64 hex chars + NUL).  Used as a
 * well-formed-ness gate before emitting the key to the client so that a
 * malformed secrets.toml entry surfaces as a structured error instead of being
 * propagated downstream to the Android pairing flow. */
#define SATELLITE_REG_KEY_HEX_LEN 65

static rate_limit_entry_t s_keyfetch_min_entries[KEY_FETCH_MIN_RATE_SLOTS];
static rate_limiter_t s_keyfetch_min_limiter = RATE_LIMITER_STATIC_INIT(s_keyfetch_min_entries,
                                                                        KEY_FETCH_MIN_RATE_SLOTS,
                                                                        KEY_FETCH_PER_MIN_LIMIT,
                                                                        KEY_FETCH_MIN_WINDOW_SEC);

static rate_limit_entry_t s_keyfetch_hour_entries[KEY_FETCH_HOUR_RATE_SLOTS];
static rate_limiter_t s_keyfetch_hour_limiter = RATE_LIMITER_STATIC_INIT(s_keyfetch_hour_entries,
                                                                         KEY_FETCH_HOUR_RATE_SLOTS,
                                                                         KEY_FETCH_PER_HOUR_LIMIT,
                                                                         KEY_FETCH_HOUR_WINDOW_SEC);

/* FNV-1a hash used ONLY for the session-token correlation hash at the
 * `session_hash = fnv1a_hash_str(conn->auth_session_token)` call in
 * handle_get_satellite_registration_key below.  Bucketing has moved to the
 * shared core/rate_limiter primitive (linear-scan + LRU).  Do not reuse this
 * for new rate-limit buckets. */
static uint32_t fnv1a_hash_str(const char *s) {
   uint32_t h = 2166136261u;
   if (!s)
      return h;
   for (; *s; s++) {
      h ^= (unsigned char)*s;
      h *= 16777619u;
   }
   return h;
}

/* Rate-limit result enum.  Distinguishes which window tripped so the audit log
 * can record the actual cause — useful for telling a noisy legitimate admin
 * apart from a sustained brute-force flood. */
typedef enum {
   KEY_FETCH_RATE_OK = 0,
   KEY_FETCH_RATE_DENY_MIN,
   KEY_FETCH_RATE_DENY_HOUR,
   KEY_FETCH_RATE_DENY_TRUNCATED, /* client_ip too long for the underlying limiter — fail closed */
} key_fetch_rate_result_t;

/* Returns OK if the fetch is allowed (and increments both window counters),
 * otherwise the specific DENY_* reason.
 *
 * Check order is min-first / hour-second by design.  Denials don't increment
 * the rejected limiter's counter (rate_limiter_check returns BEFORE the
 * count++), so the order doesn't decide which counter "burns" — both paths
 * preserve budgets equivalently on a denial.  What the order DOES decide:
 *   (a) hot-path latency — min is the tighter window (5/60s vs 30/3600s) so
 *       most denials trigger there; checking it first short-circuits the
 *       second mutex acquire + scan.
 *   (b) LRU tiebreak — rate_limiter_check updates last_access on every match
 *       (allowed or denied), so a min-denied request would refresh the min
 *       limiter's LRU position only.  Checking min first means the hour
 *       limiter's LRU position only moves on requests that genuinely
 *       progressed past min.
 *
 * Truncation fail-closed lives in rate_limiter_check itself; we surface it
 * here as a distinct DENY_TRUNCATED reason for audit-log fidelity (the
 * underlying primitive returns a plain bool).  We pre-probe the truncation
 * case via strnlen so the audit log gets the dedicated reason instead of a
 * generic min/hour denial. */
static key_fetch_rate_result_t key_fetch_rate_check(const char *client_ip) {
   if (!client_ip || !client_ip[0])
      return KEY_FETCH_RATE_OK;

   /* IPv6 with zone IDs (e.g. fe80::1%eth0) or X-Forwarded-For chains may
    * exceed the limiter's per-slot buffer.  rate_limiter_check would
    * fail-closed and return true on either limiter; classify here as the
    * dedicated TRUNCATED reason for audit-log fidelity. */
   if (strnlen(client_ip, RATE_LIMIT_IP_SIZE) >= RATE_LIMIT_IP_SIZE)
      return KEY_FETCH_RATE_DENY_TRUNCATED;

   /* /64 normalization for IPv6 prevents per-address bypass within a single
    * network; IPv4 passes through unchanged. */
   char normalized_ip[RATE_LIMIT_IP_SIZE];
   rate_limiter_normalize_ip(client_ip, normalized_ip, sizeof(normalized_ip));

   if (rate_limiter_check(&s_keyfetch_min_limiter, normalized_ip))
      return KEY_FETCH_RATE_DENY_MIN;
   if (rate_limiter_check(&s_keyfetch_hour_limiter, normalized_ip))
      return KEY_FETCH_RATE_DENY_HOUR;

   return KEY_FETCH_RATE_OK;
}

static const char *key_fetch_rate_reason(key_fetch_rate_result_t r) {
   switch (r) {
      case KEY_FETCH_RATE_DENY_MIN:
         return "min";
      case KEY_FETCH_RATE_DENY_HOUR:
         return "hour";
      case KEY_FETCH_RATE_DENY_TRUNCATED:
         return "ip_truncated";
      default:
         return "unknown";
   }
}

void handle_get_satellite_registration_key(ws_connection_t *conn) {
   if (!conn_require_admin(conn))
      return;

   key_fetch_rate_result_t rl = key_fetch_rate_check(conn->client_ip);
   if (rl != KEY_FETCH_RATE_OK) {
      const char *limit = key_fetch_rate_reason(rl);
      OLOG_WARNING("WebUI: SATELLITE_KEY_FETCH rate-limit exceeded admin=%s remote=%s limit=%s",
                   conn->username, conn->client_ip, limit);
      char details[96];
      snprintf(details, sizeof(details), "satellite_key_fetch_rate_limited limit=%s", limit);
      auth_db_log_event("SECURITY_EVENT", conn->username, conn->client_ip, details);
      send_error_impl(conn->wsi, "rate_limited", "Too many key fetches. Wait a minute and retry.");
      return;
   }

   const secrets_config_t *secrets = config_get_secrets();
   const dawn_config_t *cfg = config_get();

   bool key_set = (secrets && secrets->satellite_registration_key[0] != '\0');
   bool tls_enabled = (cfg && cfg->webui.https);

   /* Refuse to surface a malformed key — anything other than exactly 64 hex
    * chars (SATELLITE_REG_KEY_HEX_LEN - 1) indicates a corrupted/truncated
    * secrets entry that would propagate as an opaque "registration failed" on
    * the Android side.  Surface it here as a structured server-side error so
    * the operator can fix secrets.toml. */
   if (key_set) {
      size_t klen = strnlen(secrets->satellite_registration_key, SATELLITE_REG_KEY_HEX_LEN);
      if (klen != SATELLITE_REG_KEY_HEX_LEN - 1) {
         OLOG_WARNING("WebUI: SATELLITE_KEY_FETCH refused — malformed key (len=%zu, expected %d)",
                      klen, SATELLITE_REG_KEY_HEX_LEN - 1);
         send_error_impl(conn->wsi, "malformed_key",
                         "Configured registration key is not 64 hex characters. "
                         "Regenerate with ./generate_ssl_cert.sh --gen-key and update secrets.");
         return;
      }
   }

   /* 8-hex-digit FNV-1a hash of the auth session token: enough to correlate
    * multiple fetches from the same session in the audit log without ever
    * surfacing the raw token.  Not cryptographic.  Empty/missing token
    * (shouldn't happen post-conn_require_admin, but be defensive) yields 0 so
    * the misleading "FNV basis-only" value isn't logged as a real correlation
    * identifier. */
   uint32_t session_hash = conn->auth_session_token[0] ? fnv1a_hash_str(conn->auth_session_token)
                                                       : 0u;

   /* Audit log — must NEVER contain the key value. */
   OLOG_INFO("WebUI: SATELLITE_KEY_FETCH admin=%s remote=%s session_hash=%08x key=%s",
             conn->username, conn->client_ip, session_hash, key_set ? "[set]" : "[not set]");

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("satellite_registration_key_response"));

   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "key_set", json_object_new_boolean(key_set));
   json_object_object_add(
       payload, "key", json_object_new_string(key_set ? secrets->satellite_registration_key : ""));
   /* Client uses this to refuse emitting a `ws://` pairing payload when the
    * daemon is serving TLS (security M5 — don't downgrade the PSK channel). */
   json_object_object_add(payload, "tls_enabled", json_object_new_boolean(tls_enabled));

   json_object_object_add(response, "payload", payload);
   send_json_response(conn, response);
   json_object_put(response);
}
