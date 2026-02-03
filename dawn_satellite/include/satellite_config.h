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
 * DAWN Satellite Configuration
 *
 * TOML-based configuration for satellite devices. Supports a base config
 * for hardware settings (same across identical builds) and per-device
 * identity settings.
 */

#ifndef SATELLITE_CONFIG_H
#define SATELLITE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define CONFIG_UUID_SIZE 37
#define CONFIG_NAME_SIZE 64
#define CONFIG_LOCATION_SIZE 64
#define CONFIG_HOST_SIZE 256
#define CONFIG_DEVICE_SIZE 64
#define CONFIG_PATH_SIZE 256

/* Default config file locations (searched in order) */
#define CONFIG_PATH_LOCAL "./satellite.toml"
#define CONFIG_PATH_ETC "/etc/dawn/satellite.toml"
#define CONFIG_PATH_HOME "~/.config/dawn/satellite.toml"

/* =============================================================================
 * Configuration Structure
 * ============================================================================= */

/**
 * @brief Complete satellite configuration
 */
typedef struct {
   /* Identity - unique per satellite */
   struct {
      char uuid[CONFIG_UUID_SIZE];
      char name[CONFIG_NAME_SIZE];
      char location[CONFIG_LOCATION_SIZE];
   } identity;

   /* Server connection */
   struct {
      char host[CONFIG_HOST_SIZE];
      uint16_t port;
      bool ssl;
      uint32_t reconnect_delay_ms;
      uint32_t max_reconnect_attempts;
   } server;

   /* Audio configuration */
   struct {
      char capture_device[CONFIG_DEVICE_SIZE];
      char playback_device[CONFIG_DEVICE_SIZE];
      uint32_t sample_rate;
      uint32_t max_record_seconds;
   } audio;

   /* GPIO button/LED configuration */
   struct {
      bool enabled;
      char chip[CONFIG_DEVICE_SIZE];
      int button_pin;
      bool button_active_low;
      int led_red_pin;
      int led_green_pin;
      int led_blue_pin;
   } gpio;

   /* NeoPixel LED configuration */
   struct {
      bool enabled;
      char spi_device[CONFIG_PATH_SIZE];
      int num_leds;
      uint8_t brightness;
   } neopixel;

   /* Display configuration */
   struct {
      bool enabled;
      char device[CONFIG_PATH_SIZE];
   } display;

   /* Logging configuration */
   struct {
      char level[16];
      bool use_syslog;
   } logging;

} satellite_config_t;

/* =============================================================================
 * Functions
 * ============================================================================= */

/**
 * @brief Initialize config with defaults
 *
 * Sets all fields to sensible default values.
 *
 * @param config Pointer to config structure
 */
void satellite_config_init_defaults(satellite_config_t *config);

/**
 * @brief Load configuration from TOML file
 *
 * Searches for config file in standard locations if path is NULL:
 *   1. ./satellite.toml (current directory)
 *   2. /etc/dawn/satellite.toml
 *   3. ~/.config/dawn/satellite.toml
 *
 * @param config Pointer to config structure (should be initialized with defaults)
 * @param path Config file path (NULL to search standard locations)
 * @return 0 on success, -1 if no config found, positive on parse error
 */
int satellite_config_load(satellite_config_t *config, const char *path);

/**
 * @brief Apply command-line overrides to config
 *
 * Command-line arguments take precedence over config file values.
 * Pass NULL for any value that shouldn't override.
 *
 * @param config Pointer to config structure
 * @param server Server hostname override
 * @param port Port override (0 to skip)
 * @param ssl SSL override (-1 to skip, 0=off, 1=on)
 * @param name Satellite name override
 * @param location Location override
 * @param capture_device Capture device override
 * @param playback_device Playback device override
 * @param num_leds Number of LEDs override (0 to skip)
 * @param keyboard_mode If true, disable GPIO
 */
void satellite_config_apply_overrides(satellite_config_t *config,
                                      const char *server,
                                      uint16_t port,
                                      int ssl,
                                      const char *name,
                                      const char *location,
                                      const char *capture_device,
                                      const char *playback_device,
                                      int num_leds,
                                      bool keyboard_mode);

/**
 * @brief Generate a new UUID if not set
 *
 * If config->identity.uuid is empty, generates a new random UUID.
 *
 * @param config Pointer to config structure
 */
void satellite_config_ensure_uuid(satellite_config_t *config);

/**
 * @brief Print configuration summary
 *
 * Outputs current configuration to stdout for debugging.
 *
 * @param config Pointer to config structure
 */
void satellite_config_print(const satellite_config_t *config);

/**
 * @brief Get config file path that was loaded
 *
 * @return Path to loaded config file, or NULL if none loaded
 */
const char *satellite_config_get_path(void);

#ifdef __cplusplus
}
#endif

#endif /* SATELLITE_CONFIG_H */
