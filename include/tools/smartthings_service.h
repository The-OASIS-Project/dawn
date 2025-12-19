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
 * SmartThings Service - Samsung SmartThings API integration
 *
 * This module provides voice-controllable smart home automation through
 * the SmartThings REST API. Uses OAuth2 for authentication with automatic
 * token refresh.
 *
 * Thread Safety: All public functions are thread-safe. Token refresh uses
 * a rwlock to allow concurrent API reads.
 */

#ifndef SMARTTHINGS_SERVICE_H
#define SMARTTHINGS_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */
#define ST_MAX_DEVICES 64
#define ST_MAX_DEVICE_NAME 128
#define ST_MAX_DEVICE_ID 64
#define ST_MAX_CAPABILITIES 16
#define ST_DEVICE_CACHE_TTL_SEC 300 /* 5 minutes */

/* SmartThings API base URL */
#define ST_API_BASE_URL "https://api.smartthings.com/v1"

/* OAuth2 endpoints */
#define ST_AUTH_URL "https://api.smartthings.com/oauth/authorize"
#define ST_TOKEN_URL "https://api.smartthings.com/oauth/token"

/* OAuth scopes needed for device control */
#define ST_OAUTH_SCOPES "r:devices:* x:devices:*"

/* =============================================================================
 * Error Codes
 * ============================================================================= */
typedef enum {
   ST_OK = 0,
   ST_ERR_NOT_CONFIGURED,     /* Client credentials not set */
   ST_ERR_NOT_AUTHENTICATED,  /* No valid access token */
   ST_ERR_TOKEN_EXPIRED,      /* Token refresh failed */
   ST_ERR_NETWORK,            /* Network/HTTP error */
   ST_ERR_API,                /* SmartThings API error */
   ST_ERR_DEVICE_NOT_FOUND,   /* Device not found by name */
   ST_ERR_INVALID_CAPABILITY, /* Device doesn't have capability */
   ST_ERR_RATE_LIMITED,       /* API rate limit exceeded */
   ST_ERR_INVALID_PARAM,      /* Invalid parameter */
   ST_ERR_MEMORY              /* Memory allocation failure */
} st_error_t;

/* =============================================================================
 * Device Capabilities (bitmask)
 * ============================================================================= */
typedef enum {
   ST_CAP_NONE = 0,
   ST_CAP_SWITCH = (1 << 0),        /* on/off */
   ST_CAP_SWITCH_LEVEL = (1 << 1),  /* dimmer 0-100 */
   ST_CAP_COLOR_CONTROL = (1 << 2), /* hue/saturation */
   ST_CAP_COLOR_TEMP = (1 << 3),    /* color temperature */
   ST_CAP_THERMOSTAT = (1 << 4),    /* temperature setpoint */
   ST_CAP_LOCK = (1 << 5),          /* lock/unlock */
   ST_CAP_MOTION = (1 << 6),        /* motion sensor (read-only) */
   ST_CAP_CONTACT = (1 << 7),       /* contact sensor (read-only) */
   ST_CAP_TEMPERATURE = (1 << 8),   /* temperature sensor (read-only) */
   ST_CAP_HUMIDITY = (1 << 9),      /* humidity sensor (read-only) */
   ST_CAP_BATTERY = (1 << 10),      /* battery level (read-only) */
   ST_CAP_POWER_METER = (1 << 11),  /* power consumption (read-only) */
   ST_CAP_PRESENCE = (1 << 12),     /* presence sensor (read-only) */
   ST_CAP_WINDOW_SHADE = (1 << 13), /* window shade position */
   ST_CAP_FAN_SPEED = (1 << 14)     /* fan speed control */
} st_capability_t;

/* =============================================================================
 * Data Structures
 * ============================================================================= */

/**
 * Device state (current values from status query)
 */
typedef struct {
   bool switch_on;     /* Current switch state */
   int level;          /* Current level (0-100) */
   int hue;            /* Current hue (0-100) */
   int saturation;     /* Current saturation (0-100) */
   int color_temp;     /* Current color temp (kelvin) */
   double temperature; /* Current temperature reading */
   double humidity;    /* Current humidity reading */
   int battery;        /* Battery level (0-100) */
   bool motion_active; /* Motion detected */
   bool contact_open;  /* Contact open (door/window) */
   bool locked;        /* Lock state */
   bool present;       /* Presence state */
   int shade_level;    /* Window shade level (0-100) */
   int fan_speed;      /* Fan speed (0-4 typically) */
   double power;       /* Power consumption (watts) */
} st_device_state_t;

/**
 * Single device information
 */
typedef struct {
   char id[ST_MAX_DEVICE_ID];      /* SmartThings device ID (UUID) */
   char name[ST_MAX_DEVICE_NAME];  /* User-friendly device name */
   char label[ST_MAX_DEVICE_NAME]; /* Device label (often same as name) */
   char room[ST_MAX_DEVICE_NAME];  /* Room assignment */
   uint32_t capabilities;          /* Bitmask of st_capability_t */
   st_device_state_t state;        /* Current device state */
} st_device_t;

/**
 * Device list (returned by list_devices)
 */
typedef struct {
   st_device_t devices[ST_MAX_DEVICES];
   int count;
   int64_t cached_at; /* Unix timestamp when cached */
} st_device_list_t;

/**
 * Authentication mode
 */
typedef enum {
   ST_AUTH_MODE_NONE = 0, /* Not configured */
   ST_AUTH_MODE_PAT,      /* Personal Access Token */
   ST_AUTH_MODE_OAUTH2    /* OAuth2 with refresh tokens */
} st_auth_mode_t;

/**
 * Service status (for WebUI display)
 */
typedef struct {
   bool has_tokens;          /* Tokens are present */
   bool tokens_valid;        /* Tokens haven't expired */
   int64_t token_expiry;     /* Unix timestamp of access token expiry */
   int devices_count;        /* Number of discovered devices */
   st_auth_mode_t auth_mode; /* Current authentication mode */
} st_status_t;

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

/**
 * @brief Initialize SmartThings service
 *
 * Loads tokens from ~/.config/dawn/smartthings_tokens.json if present.
 * Call this once at startup after config is loaded.
 *
 * @return ST_OK on success, ST_ERR_NOT_CONFIGURED if no client credentials
 */
st_error_t smartthings_init(void);

/**
 * @brief Clean up SmartThings service
 *
 * Frees resources and clears cached data.
 */
void smartthings_cleanup(void);

/**
 * @brief Check if SmartThings is configured
 *
 * @return true if client_id and client_secret are set
 */
bool smartthings_is_configured(void);

/**
 * @brief Check if SmartThings is authenticated
 *
 * @return true if valid tokens are present
 */
bool smartthings_is_authenticated(void);

/**
 * @brief Get current status for WebUI
 *
 * @param status Output status struct
 * @return ST_OK on success
 */
st_error_t smartthings_get_status(st_status_t *status);

/* =============================================================================
 * OAuth2 Functions
 * ============================================================================= */

/**
 * @brief Generate OAuth authorization URL
 *
 * User must visit this URL to authorize DAWN to access their SmartThings.
 * After authorization, SmartThings redirects to callback with auth code.
 *
 * @param redirect_uri The callback URI (e.g., "https://localhost:3000/smartthings/callback")
 * @param url_buf Buffer to store generated URL
 * @param buf_size Size of url_buf
 * @return ST_OK on success, ST_ERR_NOT_CONFIGURED if no client_id
 */
st_error_t smartthings_get_auth_url(const char *redirect_uri, char *url_buf, size_t buf_size);

/**
 * @brief Exchange authorization code for tokens
 *
 * Called after user authorizes and is redirected back with auth code.
 * Stores tokens to ~/.config/dawn/smartthings_tokens.json
 *
 * @param auth_code The authorization code from redirect
 * @param redirect_uri Must match the one used in get_auth_url
 * @param state The state parameter from redirect (CSRF protection), can be NULL
 * @return ST_OK on success, ST_ERR_INVALID_PARAM if state doesn't match
 */
st_error_t smartthings_exchange_code(const char *auth_code,
                                     const char *redirect_uri,
                                     const char *state);

/**
 * @brief Disconnect (clear stored tokens)
 *
 * Removes tokens from memory and disk.
 *
 * @return ST_OK on success
 */
st_error_t smartthings_disconnect(void);

/* =============================================================================
 * Device Discovery
 * ============================================================================= */

/**
 * @brief Get list of all devices
 *
 * Returns cached list if still valid (< 5 minutes old).
 *
 * @param list Output device list (caller should not free)
 * @return ST_OK on success
 */
st_error_t smartthings_list_devices(const st_device_list_t **list);

/**
 * @brief Force refresh device list
 *
 * Ignores cache and fetches fresh list from API.
 *
 * @param list Output device list
 * @return ST_OK on success
 */
st_error_t smartthings_refresh_devices(const st_device_list_t **list);

/**
 * @brief Find device by name (fuzzy match)
 *
 * Searches device names and labels, returns best match.
 *
 * @param friendly_name Name to search (e.g., "living room light")
 * @param device Output device pointer (points into cache, do not free)
 * @return ST_OK on success, ST_ERR_DEVICE_NOT_FOUND if no match
 */
st_error_t smartthings_find_device(const char *friendly_name, const st_device_t **device);

/**
 * @brief Get device status
 *
 * Queries current state of a device.
 *
 * @param device_id SmartThings device ID
 * @param state Output state struct
 * @return ST_OK on success
 */
st_error_t smartthings_get_device_status(const char *device_id, st_device_state_t *state);

/* =============================================================================
 * Device Control Functions
 * ============================================================================= */

/**
 * @brief Turn device on
 *
 * @param device_id SmartThings device ID
 * @return ST_OK on success
 */
st_error_t smartthings_switch_on(const char *device_id);

/**
 * @brief Turn device off
 *
 * @param device_id SmartThings device ID
 * @return ST_OK on success
 */
st_error_t smartthings_switch_off(const char *device_id);

/**
 * @brief Set dimmer level
 *
 * @param device_id SmartThings device ID
 * @param level Brightness level (0-100)
 * @return ST_OK on success
 */
st_error_t smartthings_set_level(const char *device_id, int level);

/**
 * @brief Set color (HSV)
 *
 * @param device_id SmartThings device ID
 * @param hue Hue (0-100, mapped to 0-360 internally)
 * @param saturation Saturation (0-100)
 * @return ST_OK on success
 */
st_error_t smartthings_set_color(const char *device_id, int hue, int saturation);

/**
 * @brief Set color temperature
 *
 * @param device_id SmartThings device ID
 * @param kelvin Color temperature in kelvin (typically 2700-6500)
 * @return ST_OK on success
 */
st_error_t smartthings_set_color_temp(const char *device_id, int kelvin);

/**
 * @brief Lock a lock device
 *
 * @param device_id SmartThings device ID
 * @return ST_OK on success
 */
st_error_t smartthings_lock(const char *device_id);

/**
 * @brief Unlock a lock device
 *
 * @param device_id SmartThings device ID
 * @return ST_OK on success
 */
st_error_t smartthings_unlock(const char *device_id);

/**
 * @brief Set thermostat cooling setpoint
 *
 * @param device_id SmartThings device ID
 * @param temp_f Temperature in Fahrenheit
 * @return ST_OK on success
 */
st_error_t smartthings_set_thermostat(const char *device_id, double temp_f);

/**
 * @brief Set window shade position
 *
 * @param device_id SmartThings device ID
 * @param level Position (0=closed, 100=open)
 * @return ST_OK on success
 */
st_error_t smartthings_set_shade_level(const char *device_id, int level);

/**
 * @brief Set fan speed
 *
 * @param device_id SmartThings device ID
 * @param speed Fan speed (0-4 typically, 0=off)
 * @return ST_OK on success
 */
st_error_t smartthings_set_fan_speed(const char *device_id, int speed);

/* =============================================================================
 * Utility Functions
 * ============================================================================= */

/**
 * @brief Get error message for error code
 *
 * @param err Error code
 * @return Human-readable error message
 */
const char *smartthings_error_str(st_error_t err);

/**
 * @brief Get capability name string
 *
 * @param cap Capability flag
 * @return Human-readable capability name
 */
const char *smartthings_capability_str(st_capability_t cap);

/**
 * @brief Get authentication mode string
 *
 * @param mode Auth mode enum value
 * @return Human-readable auth mode ("none", "pat", "oauth2")
 */
const char *smartthings_auth_mode_str(st_auth_mode_t mode);

/**
 * @brief Format device info as JSON string
 *
 * For use in LLM responses and WebUI.
 *
 * @param device Device to format
 * @param buf Output buffer
 * @param buf_size Buffer size
 * @return Length written, or -1 on error
 */
int smartthings_device_to_json(const st_device_t *device, char *buf, size_t buf_size);

/**
 * @brief Format device list as JSON string
 *
 * @param list Device list to format
 * @param buf Output buffer
 * @param buf_size Buffer size
 * @return Length written, or -1 on error
 */
int smartthings_list_to_json(const st_device_list_t *list, char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* SMARTTHINGS_SERVICE_H */
