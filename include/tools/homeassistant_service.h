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
 * Home Assistant Service - REST API integration for smart home control
 *
 * Thread Safety: All public functions are thread-safe via rwlock protection.
 * Uses per-request CURL handles for safe concurrent access.
 */

#ifndef HOMEASSISTANT_SERVICE_H
#define HOMEASSISTANT_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward decl so callers can pass service data without pulling json-c into this
 * widely-included header. */
struct json_object;

/* =============================================================================
 * Constants
 * ============================================================================= */
#define HA_MAX_ENTITIES 512
#define HA_MAX_ENTITY_ID 128
#define HA_MAX_FRIENDLY_NAME 128
#define HA_MAX_HVAC_MODES 12        /* climate hvac_modes option list cap */
#define HA_ENTITY_CACHE_TTL_SEC 300 /* 5 minutes */
#define HA_AREA_CACHE_TTL_SEC 3600  /* 1 hour — areas rarely change */
#define HA_API_TIMEOUT_SEC 30
#define HA_API_MAX_RETRIES 3

/* =============================================================================
 * Error Codes
 * ============================================================================= */
typedef enum {
   HA_OK = 0,
   HA_ERR_NOT_CONFIGURED,
   HA_ERR_NOT_CONNECTED,
   HA_ERR_NETWORK,
   HA_ERR_API,
   HA_ERR_ENTITY_NOT_FOUND,
   HA_ERR_INVALID_PARAM,
   HA_ERR_RATE_LIMITED,
   HA_ERR_MEMORY,
   HA_ERR_AMBIGUOUS /* the name could mean more than one entity, or a lock/cover too loosely */
} ha_error_t;

/* =============================================================================
 * Domain Types
 * ============================================================================= */
typedef enum {
   HA_DOMAIN_LIGHT,
   HA_DOMAIN_SWITCH,
   HA_DOMAIN_CLIMATE,
   HA_DOMAIN_LOCK,
   HA_DOMAIN_COVER,
   HA_DOMAIN_MEDIA_PLAYER,
   HA_DOMAIN_FAN,
   HA_DOMAIN_SCENE,
   HA_DOMAIN_SCRIPT,
   HA_DOMAIN_AUTOMATION,
   HA_DOMAIN_SENSOR,
   HA_DOMAIN_BINARY_SENSOR,
   HA_DOMAIN_INPUT_BOOLEAN,
   HA_DOMAIN_VACUUM,
   HA_DOMAIN_ALARM,
   HA_DOMAIN_UNKNOWN
} ha_domain_t;

/* =============================================================================
 * Data Structures
 * ============================================================================= */

/**
 * Entity information (cached from /api/states)
 */
typedef struct {
   char entity_id[HA_MAX_ENTITY_ID];
   char friendly_name[HA_MAX_FRIENDLY_NAME];
   char friendly_name_lower[HA_MAX_FRIENDLY_NAME]; /* Pre-computed for fuzzy match */
   char domain_str[32];
   char state[64];
   char area_name[64]; /* From area registry, may be empty */
   ha_domain_t domain;
   /* Domain-specific attributes */
   int brightness;      /* 0-255 (lights) */
   int color_temp;      /* mireds (lights) */
   int rgb_color[3];    /* R, G, B 0-255 (lights, from HA state) */
   double hs_color[2];  /* hue 0-360, saturation 0-100 (lights, from HA state) */
   char color_mode[16]; /* "hs", "color_temp", "xy", "rgb", "rgbw", etc. */
   double temperature;  /* current (climate/sensor) */
   double target_temp;  /* setpoint (climate) */
   char hvac_mode[32];
   int cover_position;                     /* 0-100 */
   int fan_percentage;                     /* 0-100 (fan speed) */
   char hvac_modes[HA_MAX_HVAC_MODES][32]; /* climate: available modes (dropdown options) */
   int hvac_modes_count;                   /* number of entries in hvac_modes */
   char unit_of_measurement[16];           /* sensor readout unit, e.g. "°F" */
   char device_class[32];                  /* sensor/binary_sensor class, e.g. "temperature" */
} ha_entity_t;

/**
 * Entity list (cached)
 */
typedef struct {
   ha_entity_t entities[HA_MAX_ENTITIES];
   int count;
   int64_t cached_at; /* Unix timestamp when cached */
} ha_entity_list_t;

/**
 * Service status (for WebUI display)
 */
typedef struct {
   bool configured;
   bool connected;
   int entity_count;
   char version[32];
   char url[512];
} ha_status_t;

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

/**
 * @brief Initialize Home Assistant service
 *
 * @param url Base URL (e.g., "http://192.168.1.100:8123")
 * @param token Long-Lived Access Token
 * @return HA_OK on success
 */
ha_error_t homeassistant_init(const char *url, const char *token);

/**
 * @brief Clean up Home Assistant service
 *
 * Zeroes token from memory for security.
 */
void homeassistant_cleanup(void);

/**
 * @brief Check if Home Assistant is configured
 */
bool homeassistant_is_configured(void);

/**
 * @brief Check if Home Assistant is connected
 */
bool homeassistant_is_connected(void);

/**
 * @brief Test connection to Home Assistant
 *
 * Calls GET /api/ and verifies response.
 */
ha_error_t homeassistant_test_connection(void);

/**
 * @brief Get current status for WebUI
 */
ha_error_t homeassistant_get_status(ha_status_t *status);

/**
 * @brief Copy the configured base URL + token into caller buffers under the
 *        service read lock.
 *
 * For the realtime WS listener, which re-reads credentials at each connect so a
 * token/URL rotation is picked up without caching a stale copy. The caller MUST
 * secure_zero the token buffer after use.
 *
 * @return HA_OK, or HA_ERR_NOT_CONFIGURED if url/token are unset (buffers emptied).
 */
ha_error_t homeassistant_copy_credentials(char *url_out,
                                          size_t url_len,
                                          char *token_out,
                                          size_t token_len);

/* =============================================================================
 * Entity Discovery
 * ============================================================================= */

/**
 * @brief Get list of entities (cached)
 *
 * Returns cached list if still valid (< 5 minutes old).
 */
ha_error_t homeassistant_list_entities(const ha_entity_list_t **list);

/**
 * @brief Force refresh entity list
 */
ha_error_t homeassistant_refresh_entities(const ha_entity_list_t **list);

/**
 * @brief Rebuild the entity cache from a WS get_states result array (realtime
 *        seed). Takes the write lock internally, reuses the shared per-entity
 *        parser, and does NOT touch the REST-owned connected flag.
 * @param states_array A json_object array of HA state objects.
 * @return HA_OK, or HA_ERR_INVALID_PARAM if not an array.
 */
ha_error_t homeassistant_cache_replace_from_states(struct json_object *states_array);

/**
 * @brief Rebuild the area cache from HA's WS registries (realtime area data,
 *        replacing the REST /api/template scrape). Joins entity -> (direct
 *        area_id, else device-inherited area_id) -> area name. Takes the write
 *        lock internally.
 * @param area_reg   config/area_registry/list result array.
 * @param entity_reg config/entity_registry/list result array.
 * @param device_reg config/device_registry/list result array.
 * @return HA_OK, HA_ERR_INVALID_PARAM (non-array), or HA_ERR_MEMORY.
 */
ha_error_t homeassistant_cache_replace_areas(struct json_object *area_reg,
                                             struct json_object *entity_reg,
                                             struct json_object *device_reg);

/**
 * @brief Apply a batch of state_changed events to the entity cache under ONE
 *        write lock (realtime live updates). Batched, not per-event, so an event
 *        flood doesn't storm the lock the REST command path also takes.
 * @param dirty_map A json object mapping entity_id -> new_state (JSON null value
 *                  removes the entity). Upsert semantics; unknown-domain entities
 *                  ignored; a malformed event never corrupts an existing slot.
 */
ha_error_t homeassistant_cache_apply_batch(struct json_object *dirty_map);

/**
 * @brief Copy one cached entity by id under the read lock (for delta broadcast).
 * @return true if found (out filled), false otherwise.
 */
bool homeassistant_copy_entity(const char *entity_id, ha_entity_t *out);

/**
 * @brief Copy the entity cache into caller-owned memory under the read lock.
 *
 * Race-safe alternative to the bare-pointer accessors: the caller serializes
 * from its own snapshot, never the live cache (which a worker thread can rewrite
 * in place). @p out is large (~sizeof(ha_entity_list_t)) — heap-allocate it.
 *
 * @param out           Destination buffer (filled on HA_OK).
 * @param force_refresh When true, re-poll /api/states first (entity-only — the
 *                      area cache keeps its own TTL, no area round-trip).
 * @return HA_OK, or a not-configured/not-connected/transport error.
 */
ha_error_t homeassistant_snapshot_entities(ha_entity_list_t *out, bool force_refresh);

/**
 * @brief Find the one entity a name means, with domain-aware fuzzy matching
 *
 * Never picks among equals: two entities tied for the best match, or a lock
 * or cover matched by less than its whole name (or a name containing what was
 * said), are HA_ERR_AMBIGUOUS with the candidates listed.
 *
 * @param name Friendly name, entity_id, or partial match
 * @param domain_hint Filter to specific domain (HA_DOMAIN_UNKNOWN for any)
 * @param out The entity, copied from the cache (HA_OK)
 * @param candidates "Name (entity_id); ..." for HA_ERR_AMBIGUOUS (may be NULL)
 * @param candidates_len Size of @p candidates
 * @return HA_OK, HA_ERR_ENTITY_NOT_FOUND, HA_ERR_AMBIGUOUS, or a connection error
 */
ha_error_t homeassistant_find_entity(const char *name,
                                     ha_domain_t domain_hint,
                                     ha_entity_t *out,
                                     char *candidates,
                                     size_t candidates_len);

/**
 * @brief Get fresh state for a specific entity
 */
ha_error_t homeassistant_get_entity_state(const char *entity_id, ha_entity_t *out);

/* =============================================================================
 * Device Control Functions
 * ============================================================================= */

ha_error_t homeassistant_turn_on(const char *entity_id);
ha_error_t homeassistant_turn_off(const char *entity_id);
ha_error_t homeassistant_toggle(const char *entity_id);
ha_error_t homeassistant_set_brightness(const char *entity_id, int pct);
ha_error_t homeassistant_set_color(const char *entity_id, int r, int g, int b);
ha_error_t homeassistant_set_hs_color(const char *entity_id, double hue, double saturation);

/**
 * @brief Convert RGB (0-255) to hue (0-360) and saturation (0-100)
 */
void homeassistant_rgb_to_hs(int r, int g, int b, double *out_hue, double *out_sat);
ha_error_t homeassistant_set_color_temp(const char *entity_id, int kelvin);
ha_error_t homeassistant_set_temperature(const char *entity_id, double temp_f);
ha_error_t homeassistant_lock(const char *entity_id);
ha_error_t homeassistant_unlock(const char *entity_id);
ha_error_t homeassistant_open_cover(const char *entity_id);
ha_error_t homeassistant_close_cover(const char *entity_id);
ha_error_t homeassistant_activate_scene(const char *entity_id);
ha_error_t homeassistant_run_script(const char *entity_id);
ha_error_t homeassistant_trigger_automation(const char *entity_id);

/**
 * @brief Generic Home Assistant service call (mirrors HA's own service model).
 *
 * Thin public wrapper over the internal service-call path so a WebUI/tool caller
 * can invoke any current or future HA service without a dedicated function.
 *
 * @param domain    Service domain (e.g. "light"). If NULL/empty, derived from the
 *                  entity_id prefix (e.g. "light.kitchen" → "light").
 * @param service   Service name (e.g. "turn_on"). Required.
 * @param entity_id Target entity (e.g. "light.kitchen"). Required for entity services.
 * @param data      Optional per-service data object (e.g. {"brightness":180}).
 *                  Ownership is TAKEN by this call and freed internally on every
 *                  path (including validation failure) — the caller must not reuse
 *                  or free it after the call.
 * @return HA_OK on success; HA_ERR_INVALID_PARAM on a malformed domain/service/
 *         entity_id; otherwise the underlying transport error.
 */
ha_error_t homeassistant_call_service(const char *domain,
                                      const char *service,
                                      const char *entity_id,
                                      struct json_object *data);

/* =============================================================================
 * Utility Functions
 * ============================================================================= */

/**
 * @brief Get list of unique area names from the area cache
 *
 * Copies area name strings into caller-provided buffers (safe after return).
 *
 * @param areas Output array of area name buffers (each 64 bytes)
 * @param max_areas Maximum number of areas to return
 * @return Number of unique areas written, or 0 if unavailable
 */
int homeassistant_list_areas(char areas[][64], int max_areas);

/**
 * @brief Get error message for error code
 */
const char *homeassistant_error_str(ha_error_t err);

/**
 * @brief Parse domain from entity_id string (e.g., "light.kitchen" → HA_DOMAIN_LIGHT)
 */
ha_domain_t homeassistant_parse_domain(const char *entity_id);

/**
 * @brief Get human-readable domain name
 */
const char *homeassistant_domain_str(ha_domain_t domain);

#ifdef __cplusplus
}
#endif

#endif /* HOMEASSISTANT_SERVICE_H */
