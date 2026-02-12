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
#define CONFIG_SECRET_SIZE 65 /* 32 bytes hex-encoded + null */

/* Processing modes */
typedef enum {
   PROCESSING_MODE_TEXT_ONLY = 0,   /* Current behavior: keyboard input */
   PROCESSING_MODE_VOICE_ACTIVATED, /* VAD + wake word + ASR + TTS */
} processing_mode_t;

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
typedef struct satellite_config {
   /* General settings */
   struct {
      char ai_name[CONFIG_NAME_SIZE]; /* Must match server dawn.toml (e.g., "friday") */
   } general;

   /* Identity - unique per satellite */
   struct {
      char uuid[CONFIG_UUID_SIZE];
      char name[CONFIG_NAME_SIZE];
      char location[CONFIG_LOCATION_SIZE];
      char reconnect_secret[CONFIG_SECRET_SIZE]; /* Session secret for secure reconnection */
   } identity;

   /* Server connection */
   struct {
      char host[CONFIG_HOST_SIZE];
      uint16_t port;
      bool ssl;
      bool ssl_verify; /* Verify SSL certificates (default: true for production) */
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

   /* Voice Activity Detection (VAD) */
   struct {
      bool enabled;
      char model_path[CONFIG_PATH_SIZE];
      float threshold;              /* 0.0-1.0, higher = stricter */
      uint32_t silence_duration_ms; /* Silence to trigger end-of-speech */
      uint32_t min_speech_ms;       /* Minimum speech before accepting */
   } vad;

   /* Wake Word Detection */
   struct {
      bool enabled;
      char word[CONFIG_NAME_SIZE]; /* Wake word (e.g., "friday") */
      float sensitivity;           /* 0.0-1.0, higher = more sensitive */
   } wake_word;

   /* Automatic Speech Recognition (ASR) */
   struct {
      char engine[16]; /* "whisper" or "vosk" */
      char model_path[CONFIG_PATH_SIZE];
      char language[8];      /* e.g., "en" */
      int n_threads;         /* Processing threads */
      int max_audio_seconds; /* Max buffer size (15s recommended for efficiency) */
   } asr;

   /* Text-to-Speech (TTS) */
   struct {
      char model_path[CONFIG_PATH_SIZE];
      char config_path[CONFIG_PATH_SIZE];
      char espeak_data[CONFIG_PATH_SIZE];
      float length_scale; /* Speech speed (0.85 = faster) */
   } tts;

   /* Processing mode */
   struct {
      processing_mode_t mode;
   } processing;

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

   /* Display configuration (legacy SPI framebuffer) */
   struct {
      bool enabled;
      char device[CONFIG_PATH_SIZE];
   } display;

   /* SDL2 touchscreen UI */
   struct {
      bool enabled;
      int width;
      int height;
      char font_dir[CONFIG_PATH_SIZE];
      int brightness_pct; /* 10-100, persisted across restarts */
      int volume_pct;     /* 0-100, persisted across restarts */
      bool time_24h;      /* 12h/24h time format, persisted across restarts */
      char theme[16];     /* Theme name: cyan/purple/green/blue/terminal */
   } sdl_ui;

   /* Screensaver / ambient mode */
   struct {
      bool enabled;
      int timeout_sec; /* 30-600, default 120 */
   } screensaver;

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
 * @param ssl_verify SSL cert verification (-1 to skip, 0=disable, 1=enable)
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
                                      int ssl_verify,
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

/**
 * @brief Save reconnect secret to identity config
 *
 * Stores the session secret received from server during registration.
 * This secret is required for secure reconnection without session hijacking.
 *
 * @param config Pointer to config structure
 * @param secret Session secret (64 hex chars)
 */
void satellite_config_set_reconnect_secret(satellite_config_t *config, const char *secret);

/**
 * @brief Save UI preferences (brightness, volume) to TOML config file
 *
 * Updates the [sdl_ui] section in-place if keys exist, otherwise appends them.
 *
 * @param config Pointer to config structure with current values
 */
void satellite_config_save_ui_prefs(const satellite_config_t *config);

/**
 * @brief Check if a model path exists and is readable
 *
 * @param path Path to check
 * @return true if path exists and is readable
 */
bool satellite_config_path_valid(const char *path);

/**
 * @brief Validate all model paths and disable features with missing models
 *
 * Checks each model path (VAD, ASR, TTS) and disables the corresponding
 * feature if the model file is missing or unreadable. Logs warnings for
 * missing models.
 *
 * @param config Pointer to config structure
 */
void satellite_config_validate_paths(satellite_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* SATELLITE_CONFIG_H */
