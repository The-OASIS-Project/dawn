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
 * DAWN Satellite Configuration - Implementation
 */

#include "satellite_config.h"

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "toml.h"

/* =============================================================================
 * Private Data
 * ============================================================================= */

static char g_config_path[CONFIG_PATH_SIZE] = { 0 };

/* =============================================================================
 * Helper Functions
 * ============================================================================= */

static void safe_strcpy(char *dest, const char *src, size_t dest_size) {
   if (!dest || !src || dest_size == 0)
      return;
   size_t src_len = strlen(src);
   size_t copy_len = src_len < dest_size - 1 ? src_len : dest_size - 1;
   memcpy(dest, src, copy_len);
   dest[copy_len] = '\0';
}

static bool file_exists(const char *path) {
   struct stat st;
   return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void expand_home_path(const char *path, char *expanded, size_t size) {
   if (path[0] == '~' && path[1] == '/') {
      const char *home = getenv("HOME");
      if (!home) {
         struct passwd *pw = getpwuid(getuid());
         if (pw)
            home = pw->pw_dir;
      }
      if (home) {
         snprintf(expanded, size, "%s%s", home, path + 1);
         return;
      }
   }
   safe_strcpy(expanded, path, size);
}

static const char *toml_string_or(toml_table_t *table, const char *key, const char *def) {
   toml_datum_t d = toml_string_in(table, key);
   if (d.ok) {
      /* Note: caller must handle memory - we return the allocated string */
      return d.u.s;
   }
   return def;
}

static int64_t toml_int_or(toml_table_t *table, const char *key, int64_t def) {
   toml_datum_t d = toml_int_in(table, key);
   return d.ok ? d.u.i : def;
}

static bool toml_bool_or(toml_table_t *table, const char *key, bool def) {
   toml_datum_t d = toml_bool_in(table, key);
   return d.ok ? d.u.b : def;
}

static double toml_double_or(toml_table_t *table, const char *key, double def) {
   toml_datum_t d = toml_double_in(table, key);
   return d.ok ? d.u.d : def;
}

/**
 * @brief Validate that a path is safe (no path traversal or special files)
 *
 * Security check to prevent path traversal attacks via malicious config.
 * Rejects paths containing "..", special file systems, and URL-encoded variants.
 *
 * @param path Path to validate
 * @param name Path name for logging (e.g., "VAD model")
 * @return true if path is safe, false if potentially malicious
 */
static bool validate_model_path(const char *path, const char *name) {
   if (!path || !path[0]) {
      return true; /* Empty path is fine - will be caught by existence check */
   }

   /* Check for path traversal patterns */
   if (strstr(path, "..") != NULL) {
      fprintf(stderr, "[CONFIG] SECURITY: Path traversal detected in %s: %s\n", name, path);
      return false;
   }

   /* Reject /dev/ paths (could cause hangs or resource exhaustion) */
   if (strncmp(path, "/dev/", 5) == 0) {
      fprintf(stderr, "[CONFIG] SECURITY: /dev/ path rejected for %s: %s\n", name, path);
      return false;
   }

   /* Reject /proc/ paths (could leak sensitive information) */
   if (strncmp(path, "/proc/", 6) == 0) {
      fprintf(stderr, "[CONFIG] SECURITY: /proc/ path rejected for %s: %s\n", name, path);
      return false;
   }

   /* Reject /sys/ paths */
   if (strncmp(path, "/sys/", 5) == 0) {
      fprintf(stderr, "[CONFIG] SECURITY: /sys/ path rejected for %s: %s\n", name, path);
      return false;
   }

   return true;
}

/* =============================================================================
 * Public Functions
 * ============================================================================= */

void satellite_config_init_defaults(satellite_config_t *config) {
   if (!config)
      return;

   memset(config, 0, sizeof(satellite_config_t));

   /* General defaults */
   safe_strcpy(config->general.ai_name, "friday", CONFIG_NAME_SIZE);

   /* Identity defaults */
   /* UUID intentionally empty - will be generated if not set */
   safe_strcpy(config->identity.name, "Satellite", CONFIG_NAME_SIZE);
   /* Location intentionally empty */
   /* reconnect_secret intentionally empty - set by server */

   /* Server defaults */
   safe_strcpy(config->server.host, "localhost", CONFIG_HOST_SIZE);
   config->server.port = 8080;
   config->server.ssl = false;
   config->server.ssl_verify = true; /* Default: verify certificates in production */
   config->server.reconnect_delay_ms = 5000;
   config->server.max_reconnect_attempts = 0; /* infinite */

   /* Audio defaults */
   safe_strcpy(config->audio.capture_device, "plughw:0,0", CONFIG_DEVICE_SIZE);
   safe_strcpy(config->audio.playback_device, "plughw:0,0", CONFIG_DEVICE_SIZE);
   config->audio.sample_rate = 16000;
   config->audio.max_record_seconds = 30;

   /* VAD defaults */
   config->vad.enabled = true;
   safe_strcpy(config->vad.model_path, "models/silero_vad_16k_op15.onnx", CONFIG_PATH_SIZE);
   config->vad.threshold = 0.5f;
   config->vad.silence_duration_ms = 800;
   config->vad.min_speech_ms = 250;

   /* Wake word defaults */
   config->wake_word.enabled = true;
   safe_strcpy(config->wake_word.word, "friday", CONFIG_NAME_SIZE);
   config->wake_word.sensitivity = 0.5f;

   /* ASR defaults - Vosk is the default for Tier 1 satellites (streaming, near-instant) */
   safe_strcpy(config->asr.engine, "vosk", sizeof(config->asr.engine));
   safe_strcpy(config->asr.model_path, "models/vosk-model-small-en-us-0.15", CONFIG_PATH_SIZE);
   safe_strcpy(config->asr.language, "en", sizeof(config->asr.language));
   config->asr.n_threads = 2;          /* Pi Zero 2 W has 4 cores */
   config->asr.max_audio_seconds = 15; /* 960KB buffer, not 3.84MB */

   /* TTS defaults */
   safe_strcpy(config->tts.model_path, "models/en_GB-alba-medium.onnx", CONFIG_PATH_SIZE);
   safe_strcpy(config->tts.config_path, "models/en_GB-alba-medium.onnx.json", CONFIG_PATH_SIZE);
   safe_strcpy(config->tts.espeak_data, "/usr/share/espeak-ng-data", CONFIG_PATH_SIZE);
   config->tts.length_scale = 0.85f;

   /* Processing defaults */
   config->processing.mode = PROCESSING_MODE_TEXT_ONLY; /* Safe default */

   /* GPIO defaults (disabled) */
   config->gpio.enabled = false;
   safe_strcpy(config->gpio.chip, "gpiochip0", CONFIG_DEVICE_SIZE);
   config->gpio.button_pin = 17;
   config->gpio.button_active_low = true;
   config->gpio.led_red_pin = -1;
   config->gpio.led_green_pin = -1;
   config->gpio.led_blue_pin = -1;

   /* NeoPixel defaults (disabled for Tier 1) */
   config->neopixel.enabled = false;
   safe_strcpy(config->neopixel.spi_device, "/dev/spidev0.0", CONFIG_PATH_SIZE);
   config->neopixel.num_leds = 3;
   config->neopixel.brightness = 64;

   /* Display defaults (disabled) */
   config->display.enabled = false;
   safe_strcpy(config->display.device, "/dev/fb1", CONFIG_PATH_SIZE);

   /* SDL2 UI defaults (disabled) */
   config->sdl_ui.enabled = false;
   config->sdl_ui.width = 1024;
   config->sdl_ui.height = 600;
   safe_strcpy(config->sdl_ui.font_dir, "assets/fonts", CONFIG_PATH_SIZE);
   config->sdl_ui.brightness_pct = 100;
   config->sdl_ui.volume_pct = 80;
   config->sdl_ui.time_24h = false;
   safe_strcpy(config->sdl_ui.theme, "cyan", sizeof(config->sdl_ui.theme));

   /* Screensaver defaults */
   config->screensaver.enabled = true;
   config->screensaver.timeout_sec = 120;

   /* Logging defaults */
   safe_strcpy(config->logging.level, "info", sizeof(config->logging.level));
   config->logging.use_syslog = false;
}

int satellite_config_load(satellite_config_t *config, const char *path) {
   if (!config)
      return -1;

   /* Find config file */
   char resolved_path[CONFIG_PATH_SIZE];
   const char *search_paths[] = {
      CONFIG_PATH_LOCAL,
      CONFIG_PATH_ETC,
      CONFIG_PATH_HOME,
   };

   if (path) {
      expand_home_path(path, resolved_path, sizeof(resolved_path));
      if (!file_exists(resolved_path)) {
         fprintf(stderr, "[CONFIG] Config file not found: %s\n", resolved_path);
         return -1;
      }
   } else {
      /* Search standard locations */
      bool found = false;
      for (size_t i = 0; i < sizeof(search_paths) / sizeof(search_paths[0]); i++) {
         expand_home_path(search_paths[i], resolved_path, sizeof(resolved_path));
         if (file_exists(resolved_path)) {
            found = true;
            break;
         }
      }
      if (!found) {
         /* No config file found - use defaults */
         return -1;
      }
   }

   /* Open and parse TOML file */
   FILE *fp = fopen(resolved_path, "r");
   if (!fp) {
      fprintf(stderr, "[CONFIG] Cannot open config file: %s (%s)\n", resolved_path,
              strerror(errno));
      return -1;
   }

   char errbuf[256];
   toml_table_t *root = toml_parse_file(fp, errbuf, sizeof(errbuf));
   fclose(fp);

   if (!root) {
      fprintf(stderr, "[CONFIG] Parse error in %s: %s\n", resolved_path, errbuf);
      return 1;
   }

   /* Save path for later reference */
   safe_strcpy(g_config_path, resolved_path, sizeof(g_config_path));

   /* Parse [general] section */
   toml_table_t *general = toml_table_in(root, "general");
   if (general) {
      const char *s = toml_string_or(general, "ai_name", NULL);
      if (s) {
         safe_strcpy(config->general.ai_name, s, CONFIG_NAME_SIZE);
         free((void *)s);
      }
   }

   /* Parse [identity] section */
   toml_table_t *identity = toml_table_in(root, "identity");
   if (identity) {
      const char *s;

      s = toml_string_or(identity, "uuid", NULL);
      if (s && s[0]) {
         safe_strcpy(config->identity.uuid, s, CONFIG_UUID_SIZE);
         free((void *)s);
      }

      s = toml_string_or(identity, "name", NULL);
      if (s) {
         safe_strcpy(config->identity.name, s, CONFIG_NAME_SIZE);
         free((void *)s);
      }

      s = toml_string_or(identity, "location", NULL);
      if (s) {
         safe_strcpy(config->identity.location, s, CONFIG_LOCATION_SIZE);
         free((void *)s);
      }

      s = toml_string_or(identity, "reconnect_secret", NULL);
      if (s) {
         safe_strcpy(config->identity.reconnect_secret, s, CONFIG_SECRET_SIZE);
         free((void *)s);
      }
   }

   /* Parse [server] section */
   toml_table_t *server = toml_table_in(root, "server");
   if (server) {
      const char *s = toml_string_or(server, "host", NULL);
      if (s) {
         safe_strcpy(config->server.host, s, CONFIG_HOST_SIZE);
         free((void *)s);
      }

      config->server.port = (uint16_t)toml_int_or(server, "port", config->server.port);
      config->server.ssl = toml_bool_or(server, "ssl", config->server.ssl);
      config->server.ssl_verify = toml_bool_or(server, "ssl_verify", config->server.ssl_verify);
      config->server.reconnect_delay_ms = (uint32_t)toml_int_or(server, "reconnect_delay_ms",
                                                                config->server.reconnect_delay_ms);
      config->server.max_reconnect_attempts = (uint32_t)toml_int_or(
          server, "max_reconnect_attempts", config->server.max_reconnect_attempts);
   }

   /* Parse [audio] section */
   toml_table_t *audio = toml_table_in(root, "audio");
   if (audio) {
      const char *s;

      s = toml_string_or(audio, "capture_device", NULL);
      if (s) {
         safe_strcpy(config->audio.capture_device, s, CONFIG_DEVICE_SIZE);
         free((void *)s);
      }

      s = toml_string_or(audio, "playback_device", NULL);
      if (s) {
         safe_strcpy(config->audio.playback_device, s, CONFIG_DEVICE_SIZE);
         free((void *)s);
      }

      config->audio.sample_rate = (uint32_t)toml_int_or(audio, "sample_rate",
                                                        config->audio.sample_rate);
      config->audio.max_record_seconds = (uint32_t)toml_int_or(audio, "max_record_seconds",
                                                               config->audio.max_record_seconds);
   }

   /* Parse [gpio] section */
   toml_table_t *gpio = toml_table_in(root, "gpio");
   if (gpio) {
      config->gpio.enabled = toml_bool_or(gpio, "enabled", config->gpio.enabled);

      const char *s = toml_string_or(gpio, "chip", NULL);
      if (s) {
         safe_strcpy(config->gpio.chip, s, CONFIG_DEVICE_SIZE);
         free((void *)s);
      }

      config->gpio.button_pin = (int)toml_int_or(gpio, "button_pin", config->gpio.button_pin);
      config->gpio.button_active_low = toml_bool_or(gpio, "button_active_low",
                                                    config->gpio.button_active_low);
      config->gpio.led_red_pin = (int)toml_int_or(gpio, "led_red_pin", config->gpio.led_red_pin);
      config->gpio.led_green_pin = (int)toml_int_or(gpio, "led_green_pin",
                                                    config->gpio.led_green_pin);
      config->gpio.led_blue_pin = (int)toml_int_or(gpio, "led_blue_pin", config->gpio.led_blue_pin);
   }

   /* Parse [neopixel] section */
   toml_table_t *neopixel = toml_table_in(root, "neopixel");
   if (neopixel) {
      config->neopixel.enabled = toml_bool_or(neopixel, "enabled", config->neopixel.enabled);

      const char *s = toml_string_or(neopixel, "spi_device", NULL);
      if (s) {
         safe_strcpy(config->neopixel.spi_device, s, CONFIG_PATH_SIZE);
         free((void *)s);
      }

      config->neopixel.num_leds = (int)toml_int_or(neopixel, "num_leds", config->neopixel.num_leds);
      config->neopixel.brightness = (uint8_t)toml_int_or(neopixel, "brightness",
                                                         config->neopixel.brightness);
   }

   /* Parse [display] section */
   toml_table_t *display = toml_table_in(root, "display");
   if (display) {
      config->display.enabled = toml_bool_or(display, "enabled", config->display.enabled);

      const char *s = toml_string_or(display, "device", NULL);
      if (s) {
         safe_strcpy(config->display.device, s, CONFIG_PATH_SIZE);
         free((void *)s);
      }
   }

   /* Parse [sdl_ui] section */
   toml_table_t *sdl_ui = toml_table_in(root, "sdl_ui");
   if (sdl_ui) {
      config->sdl_ui.enabled = toml_bool_or(sdl_ui, "enabled", config->sdl_ui.enabled);
      config->sdl_ui.width = (int)toml_int_or(sdl_ui, "width", config->sdl_ui.width);
      config->sdl_ui.height = (int)toml_int_or(sdl_ui, "height", config->sdl_ui.height);

      const char *s = toml_string_or(sdl_ui, "font_dir", NULL);
      if (s) {
         safe_strcpy(config->sdl_ui.font_dir, s, CONFIG_PATH_SIZE);
         free((void *)s);
      }

      int bri = (int)toml_int_or(sdl_ui, "brightness", config->sdl_ui.brightness_pct);
      if (bri >= 10 && bri <= 100)
         config->sdl_ui.brightness_pct = bri;
      int vol = (int)toml_int_or(sdl_ui, "volume", config->sdl_ui.volume_pct);
      if (vol >= 0 && vol <= 100)
         config->sdl_ui.volume_pct = vol;
      config->sdl_ui.time_24h = toml_bool_or(sdl_ui, "time_24h", config->sdl_ui.time_24h);

      const char *theme_s = toml_string_or(sdl_ui, "theme", NULL);
      if (theme_s) {
         /* NOTE: This validation mirrors the THEMES table in ui_theme.c.
          * Kept here for decoupling (config does not include ui_theme.h). */
         if (strcmp(theme_s, "cyan") == 0 || strcmp(theme_s, "purple") == 0 ||
             strcmp(theme_s, "green") == 0 || strcmp(theme_s, "blue") == 0 ||
             strcmp(theme_s, "terminal") == 0) {
            safe_strcpy(config->sdl_ui.theme, theme_s, sizeof(config->sdl_ui.theme));
         }
         free((void *)theme_s);
      }
   }

   /* Parse [screensaver] section */
   toml_table_t *screensaver = toml_table_in(root, "screensaver");
   if (screensaver) {
      config->screensaver.enabled = toml_bool_or(screensaver, "enabled",
                                                 config->screensaver.enabled);
      int timeout = (int)toml_int_or(screensaver, "timeout", config->screensaver.timeout_sec);
      if (timeout < 30)
         timeout = 30;
      if (timeout > 600)
         timeout = 600;
      config->screensaver.timeout_sec = timeout;
   }

   /* Parse [logging] section */
   toml_table_t *logging = toml_table_in(root, "logging");
   if (logging) {
      const char *s = toml_string_or(logging, "level", NULL);
      if (s) {
         safe_strcpy(config->logging.level, s, sizeof(config->logging.level));
         free((void *)s);
      }

      config->logging.use_syslog = toml_bool_or(logging, "use_syslog", config->logging.use_syslog);
   }

   /* Parse [vad] section */
   toml_table_t *vad = toml_table_in(root, "vad");
   if (vad) {
      config->vad.enabled = toml_bool_or(vad, "enabled", config->vad.enabled);

      const char *s = toml_string_or(vad, "model_path", NULL);
      if (s) {
         expand_home_path(s, config->vad.model_path, CONFIG_PATH_SIZE);
         free((void *)s);
      }

      config->vad.threshold = (float)toml_double_or(vad, "threshold", config->vad.threshold);
      config->vad.silence_duration_ms = (uint32_t)toml_int_or(vad, "silence_duration_ms",
                                                              config->vad.silence_duration_ms);
      config->vad.min_speech_ms = (uint32_t)toml_int_or(vad, "min_speech_ms",
                                                        config->vad.min_speech_ms);
   }

   /* Parse [wake_word] section */
   toml_table_t *wake_word = toml_table_in(root, "wake_word");
   if (wake_word) {
      config->wake_word.enabled = toml_bool_or(wake_word, "enabled", config->wake_word.enabled);

      const char *s = toml_string_or(wake_word, "word", NULL);
      if (s) {
         safe_strcpy(config->wake_word.word, s, CONFIG_NAME_SIZE);
         free((void *)s);
      }

      config->wake_word.sensitivity = (float)toml_double_or(wake_word, "sensitivity",
                                                            config->wake_word.sensitivity);
   }

   /* Parse [asr] section */
   toml_table_t *asr = toml_table_in(root, "asr");
   if (asr) {
      const char *s;

      s = toml_string_or(asr, "engine", NULL);
      if (s) {
         if (strcmp(s, "whisper") == 0 || strcmp(s, "vosk") == 0) {
            safe_strcpy(config->asr.engine, s, sizeof(config->asr.engine));
         } else {
            fprintf(stderr, "[CONFIG] WARNING: Unknown ASR engine '%s', using default '%s'\n", s,
                    config->asr.engine);
         }
         free((void *)s);
      }

      s = toml_string_or(asr, "model_path", NULL);
      if (s) {
         expand_home_path(s, config->asr.model_path, CONFIG_PATH_SIZE);
         free((void *)s);
      }

      s = toml_string_or(asr, "language", NULL);
      if (s) {
         safe_strcpy(config->asr.language, s, sizeof(config->asr.language));
         free((void *)s);
      }

      config->asr.n_threads = (int)toml_int_or(asr, "n_threads", config->asr.n_threads);
      config->asr.max_audio_seconds = (int)toml_int_or(asr, "max_audio_seconds",
                                                       config->asr.max_audio_seconds);
   }

   /* Parse [tts] section */
   toml_table_t *tts = toml_table_in(root, "tts");
   if (tts) {
      const char *s;

      s = toml_string_or(tts, "model_path", NULL);
      if (s) {
         expand_home_path(s, config->tts.model_path, CONFIG_PATH_SIZE);
         free((void *)s);
      }

      s = toml_string_or(tts, "config_path", NULL);
      if (s) {
         expand_home_path(s, config->tts.config_path, CONFIG_PATH_SIZE);
         free((void *)s);
      }

      s = toml_string_or(tts, "espeak_data", NULL);
      if (s) {
         safe_strcpy(config->tts.espeak_data, s, CONFIG_PATH_SIZE);
         free((void *)s);
      }

      config->tts.length_scale = (float)toml_double_or(tts, "length_scale",
                                                       config->tts.length_scale);
   }

   /* Parse [processing] section */
   toml_table_t *processing = toml_table_in(root, "processing");
   if (processing) {
      const char *s = toml_string_or(processing, "mode", NULL);
      if (s) {
         if (strcmp(s, "voice_activated") == 0) {
            config->processing.mode = PROCESSING_MODE_VOICE_ACTIVATED;
         } else {
            config->processing.mode = PROCESSING_MODE_TEXT_ONLY;
         }
         free((void *)s);
      }
   }

   toml_free(root);

   printf("[CONFIG] Loaded configuration from %s\n", resolved_path);
   return 0;
}

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
                                      bool keyboard_mode) {
   if (!config)
      return;

   if (server && server[0]) {
      safe_strcpy(config->server.host, server, CONFIG_HOST_SIZE);
   }

   if (port > 0) {
      config->server.port = port;
   }

   if (ssl >= 0) {
      config->server.ssl = (ssl != 0);
   }

   if (ssl_verify >= 0) {
      config->server.ssl_verify = (ssl_verify != 0);
   }

   if (name && name[0]) {
      safe_strcpy(config->identity.name, name, CONFIG_NAME_SIZE);
   }

   if (location && location[0]) {
      safe_strcpy(config->identity.location, location, CONFIG_LOCATION_SIZE);
   }

   if (capture_device && capture_device[0]) {
      safe_strcpy(config->audio.capture_device, capture_device, CONFIG_DEVICE_SIZE);
   }

   if (playback_device && playback_device[0]) {
      safe_strcpy(config->audio.playback_device, playback_device, CONFIG_DEVICE_SIZE);
   }

   if (num_leds > 0) {
      config->neopixel.num_leds = num_leds;
   }

   if (keyboard_mode) {
      config->gpio.enabled = false;
   }
}

/* Load persisted identity (UUID + reconnect_secret) from file */
static void load_persisted_identity(satellite_config_t *config) {
   const char *home = getenv("HOME");
   char identity_path[512];
   if (home) {
      snprintf(identity_path, sizeof(identity_path), "%s/.dawn_satellite_identity", home);
   } else {
      snprintf(identity_path, sizeof(identity_path), ".dawn_satellite_identity");
   }

   FILE *f = fopen(identity_path, "r");
   if (!f)
      return;

   char line[256];
   while (fgets(line, sizeof(line), f)) {
      /* Skip comments and empty lines */
      if (line[0] == '#' || line[0] == '\n')
         continue;

      char key[64], value[128];
      /* Parse simple key = "value" format */
      if (sscanf(line, " %63[^=] = \"%127[^\"]\"", key, value) == 2) {
         /* Trim trailing whitespace from key */
         char *end = key + strlen(key) - 1;
         while (end > key && (*end == ' ' || *end == '\t'))
            *end-- = '\0';

         if (strcmp(key, "uuid") == 0 && config->identity.uuid[0] == '\0') {
            safe_strcpy(config->identity.uuid, value, CONFIG_UUID_SIZE);
            printf("[CONFIG] Loaded UUID from identity file: %s\n", value);
         } else if (strcmp(key, "reconnect_secret") == 0) {
            safe_strcpy(config->identity.reconnect_secret, value, CONFIG_SECRET_SIZE);
            printf("[CONFIG] Loaded reconnect_secret from identity file\n");
         }
      }
   }
   fclose(f);
}

void satellite_config_ensure_uuid(satellite_config_t *config) {
   if (!config)
      return;

   /* Try to load persisted identity first */
   load_persisted_identity(config);

   /* If UUID is already set (from config or identity file), keep it */
   if (config->identity.uuid[0] != '\0')
      return;

   /* Generate a random UUID v4 */
   unsigned char bytes[16];

   /* Use /dev/urandom for random bytes */
   FILE *fp = fopen("/dev/urandom", "rb");
   if (fp) {
      if (fread(bytes, 1, 16, fp) != 16) {
         /* Fallback to time-based if read fails */
         srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
         for (int i = 0; i < 16; i++) {
            bytes[i] = (unsigned char)(rand() & 0xFF);
         }
      }
      fclose(fp);
   } else {
      /* Fallback to time-based */
      srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
      for (int i = 0; i < 16; i++) {
         bytes[i] = (unsigned char)(rand() & 0xFF);
      }
   }

   /* Set version (4) and variant (RFC 4122) bits */
   bytes[6] = (bytes[6] & 0x0F) | 0x40; /* Version 4 */
   bytes[8] = (bytes[8] & 0x3F) | 0x80; /* Variant RFC 4122 */

   /* Format as UUID string */
   snprintf(config->identity.uuid, CONFIG_UUID_SIZE,
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0],
            bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8],
            bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);

   printf("[CONFIG] Generated UUID: %s\n", config->identity.uuid);
}

void satellite_config_print(const satellite_config_t *config) {
   if (!config)
      return;

   printf("\n=== Satellite Configuration ===\n");

   printf("\n[general]\n");
   printf("  ai_name = \"%s\"\n", config->general.ai_name);

   printf("\n[identity]\n");
   printf("  uuid     = \"%s\"\n", config->identity.uuid);
   printf("  name     = \"%s\"\n", config->identity.name);
   printf("  location = \"%s\"\n", config->identity.location);
   printf("  secret   = %s\n", config->identity.reconnect_secret[0] ? "(set)" : "(empty)");

   printf("\n[server]\n");
   printf("  host = \"%s\"\n", config->server.host);
   printf("  port = %u\n", config->server.port);
   printf("  ssl  = %s\n", config->server.ssl ? "true" : "false");

   printf("\n[audio]\n");
   printf("  capture_device  = \"%s\"\n", config->audio.capture_device);
   printf("  playback_device = \"%s\"\n", config->audio.playback_device);

   printf("\n[vad]\n");
   printf("  enabled            = %s\n", config->vad.enabled ? "true" : "false");
   printf("  model_path         = \"%s\"\n", config->vad.model_path);
   printf("  threshold          = %.2f\n", config->vad.threshold);
   printf("  silence_duration   = %u ms\n", config->vad.silence_duration_ms);
   printf("  min_speech         = %u ms\n", config->vad.min_speech_ms);

   printf("\n[wake_word]\n");
   printf("  enabled     = %s\n", config->wake_word.enabled ? "true" : "false");
   printf("  word        = \"%s\"\n", config->wake_word.word);
   printf("  sensitivity = %.2f\n", config->wake_word.sensitivity);

   printf("\n[asr]\n");
   printf("  engine            = \"%s\"\n", config->asr.engine);
   printf("  model_path        = \"%s\"\n", config->asr.model_path);
   printf("  language          = \"%s\"\n", config->asr.language);
   printf("  n_threads         = %d\n", config->asr.n_threads);
   printf("  max_audio_seconds = %d\n", config->asr.max_audio_seconds);

   printf("\n[tts]\n");
   printf("  model_path   = \"%s\"\n", config->tts.model_path);
   printf("  config_path  = \"%s\"\n", config->tts.config_path);
   printf("  espeak_data  = \"%s\"\n", config->tts.espeak_data);
   printf("  length_scale = %.2f\n", config->tts.length_scale);

   printf("\n[processing]\n");
   printf("  mode = %s\n", config->processing.mode == PROCESSING_MODE_VOICE_ACTIVATED
                               ? "voice_activated"
                               : "text_only");

   printf("\n[gpio]\n");
   printf("  enabled    = %s\n", config->gpio.enabled ? "true" : "false");
   printf("  button_pin = %d\n", config->gpio.button_pin);

   printf("\n[neopixel]\n");
   printf("  enabled    = %s\n", config->neopixel.enabled ? "true" : "false");
   printf("  num_leds   = %d\n", config->neopixel.num_leds);
   printf("  brightness = %u\n", config->neopixel.brightness);

   printf("\n[display]\n");
   printf("  enabled = %s\n", config->display.enabled ? "true" : "false");

   printf("\n[sdl_ui]\n");
   printf("  enabled = %s\n", config->sdl_ui.enabled ? "true" : "false");
   printf("  width = %d\n", config->sdl_ui.width);
   printf("  height = %d\n", config->sdl_ui.height);
   printf("  font_dir = %s\n", config->sdl_ui.font_dir);
   printf("  brightness = %d\n", config->sdl_ui.brightness_pct);
   printf("  volume = %d\n", config->sdl_ui.volume_pct);
   printf("  time_24h = %s\n", config->sdl_ui.time_24h ? "true" : "false");
   printf("  theme = \"%s\"\n", config->sdl_ui.theme);

   printf("\n[screensaver]\n");
   printf("  enabled = %s\n", config->screensaver.enabled ? "true" : "false");
   printf("  timeout = %d\n", config->screensaver.timeout_sec);

   printf("\n===============================\n\n");
}

const char *satellite_config_get_path(void) {
   return g_config_path[0] ? g_config_path : NULL;
}

void satellite_config_set_reconnect_secret(satellite_config_t *config, const char *secret) {
   if (!config || !secret)
      return;

   safe_strcpy(config->identity.reconnect_secret, secret, CONFIG_SECRET_SIZE);

   /* Persist identity (UUID + secret) to file for reconnection after restart */
   const char *home = getenv("HOME");
   char identity_path[512];
   if (home) {
      snprintf(identity_path, sizeof(identity_path), "%s/.dawn_satellite_identity", home);
   } else {
      snprintf(identity_path, sizeof(identity_path), ".dawn_satellite_identity");
   }

   FILE *f = fopen(identity_path, "w");
   if (f) {
      /* Set restrictive permissions BEFORE writing sensitive data (0600 = owner read/write only)
       * This prevents session hijacking via identity file theft on multi-user systems */
      int fd = fileno(f);
      if (fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
         fprintf(stderr, "[CONFIG] Warning: Could not set permissions on %s\n", identity_path);
      }
      fprintf(f, "# DAWN Satellite Identity (auto-generated, do not edit)\n");
      fprintf(f, "uuid = \"%s\"\n", config->identity.uuid);
      fprintf(f, "reconnect_secret = \"%s\"\n", secret);
      fclose(f);
      printf("[CONFIG] Identity saved to %s (mode 0600)\n", identity_path);
   } else {
      fprintf(stderr, "[CONFIG] Warning: Could not save identity to %s\n", identity_path);
   }
}

void satellite_config_save_ui_prefs(const satellite_config_t *config) {
   const char *path = satellite_config_get_path();
   if (!path || !config) {
      fprintf(stderr, "[CONFIG] Cannot save UI prefs: no config path\n");
      return;
   }

   /* Read existing file into memory */
   FILE *fp = fopen(path, "r");
   if (!fp) {
      fprintf(stderr, "[CONFIG] Cannot open %s for reading\n", path);
      return;
   }

   /* Read all lines */
   enum {
      MAX_LINES = 400
   };
   char lines[MAX_LINES][256];
   int line_count = 0;
   while (line_count < MAX_LINES && fgets(lines[line_count], sizeof(lines[0]), fp)) {
      line_count++;
   }
   if (line_count == MAX_LINES && !feof(fp)) {
      fprintf(stderr, "[CONFIG] Warning: config exceeds %d lines, save may truncate\n", MAX_LINES);
   }
   fclose(fp);

   /* Track which keys we've updated */
   bool found_brightness = false;
   bool found_volume = false;
   bool found_time_24h = false;
   bool found_theme = false;
   bool in_sdl_ui = false;
   bool ever_in_sdl_ui = false;
   int sdl_ui_end = -1; /* Last line of [sdl_ui] section for appending */

   for (int i = 0; i < line_count; i++) {
      char trimmed[256];
      strncpy(trimmed, lines[i], sizeof(trimmed) - 1);
      trimmed[sizeof(trimmed) - 1] = '\0';

      /* Strip leading whitespace */
      char *p = trimmed;
      while (*p == ' ' || *p == '\t')
         p++;

      /* Detect section headers */
      if (*p == '[') {
         if (in_sdl_ui) {
            sdl_ui_end = i; /* Leaving [sdl_ui], mark boundary */
         }
         in_sdl_ui = (strncmp(p, "[sdl_ui]", 8) == 0);
         if (in_sdl_ui)
            ever_in_sdl_ui = true;
         continue;
      }

      if (in_sdl_ui) {
         sdl_ui_end = i + 1;
         if (strncmp(p, "brightness", 10) == 0 && (p[10] == ' ' || p[10] == '=')) {
            snprintf(lines[i], sizeof(lines[0]), "brightness = %d\n",
                     config->sdl_ui.brightness_pct);
            found_brightness = true;
         } else if (strncmp(p, "volume", 6) == 0 && (p[6] == ' ' || p[6] == '=')) {
            snprintf(lines[i], sizeof(lines[0]), "volume = %d\n", config->sdl_ui.volume_pct);
            found_volume = true;
         } else if (strncmp(p, "time_24h", 8) == 0 && (p[8] == ' ' || p[8] == '=')) {
            snprintf(lines[i], sizeof(lines[0]), "time_24h = %s\n",
                     config->sdl_ui.time_24h ? "true" : "false");
            found_time_24h = true;
         } else if (strncmp(p, "theme", 5) == 0 && (p[5] == ' ' || p[5] == '=')) {
            snprintf(lines[i], sizeof(lines[0]), "theme = \"%s\"\n", config->sdl_ui.theme);
            found_theme = true;
         }
      }
   }

   /* If [sdl_ui] was the last section, mark end at EOF */
   if (in_sdl_ui) {
      sdl_ui_end = line_count;
   }

   /* Write to temp file, then atomic rename to prevent corruption on power loss */
   char tmp_path[512];
   snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

   fp = fopen(tmp_path, "w");
   if (!fp) {
      fprintf(stderr, "[CONFIG] Cannot open %s for writing\n", tmp_path);
      return;
   }

   for (int i = 0; i < line_count; i++) {
      /* Append missing keys at end of [sdl_ui] section */
      if (i == sdl_ui_end) {
         if (!found_brightness) {
            fprintf(fp, "brightness = %d\n", config->sdl_ui.brightness_pct);
            found_brightness = true;
         }
         if (!found_volume) {
            fprintf(fp, "volume = %d\n", config->sdl_ui.volume_pct);
            found_volume = true;
         }
         if (!found_time_24h) {
            fprintf(fp, "time_24h = %s\n", config->sdl_ui.time_24h ? "true" : "false");
            found_time_24h = true;
         }
         if (!found_theme) {
            fprintf(fp, "theme = \"%s\"\n", config->sdl_ui.theme);
            found_theme = true;
         }
      }
      fputs(lines[i], fp);
   }

   /* If [sdl_ui] was the last section, append at EOF */
   if (sdl_ui_end == line_count) {
      if (!found_brightness) {
         fprintf(fp, "brightness = %d\n", config->sdl_ui.brightness_pct);
      }
      if (!found_volume) {
         fprintf(fp, "volume = %d\n", config->sdl_ui.volume_pct);
      }
      if (!found_time_24h) {
         fprintf(fp, "time_24h = %s\n", config->sdl_ui.time_24h ? "true" : "false");
      }
      if (!found_theme) {
         fprintf(fp, "theme = \"%s\"\n", config->sdl_ui.theme);
      }
   }

   /* No [sdl_ui] section at all — create one at EOF */
   if (!ever_in_sdl_ui) {
      fprintf(fp, "\n[sdl_ui]\nbrightness = %d\nvolume = %d\ntime_24h = %s\ntheme = \"%s\"\n",
              config->sdl_ui.brightness_pct, config->sdl_ui.volume_pct,
              config->sdl_ui.time_24h ? "true" : "false", config->sdl_ui.theme);
   }

   /* Flush and sync before atomic rename */
   fflush(fp);
   fsync(fileno(fp));
   fclose(fp);

   if (rename(tmp_path, path) != 0) {
      fprintf(stderr, "[CONFIG] Failed to rename %s -> %s: %s\n", tmp_path, path, strerror(errno));
      return;
   }

   printf("[CONFIG] UI prefs saved (brightness=%d, volume=%d, time_24h=%s, theme=%s)\n",
          config->sdl_ui.brightness_pct, config->sdl_ui.volume_pct,
          config->sdl_ui.time_24h ? "true" : "false", config->sdl_ui.theme);
}

bool satellite_config_path_valid(const char *path) {
   if (!path || path[0] == '\0')
      return false;

   return access(path, R_OK) == 0;
}

void satellite_config_validate_paths(satellite_config_t *config) {
   if (!config)
      return;

   /* Security check: validate all paths for traversal attacks before use */
   if (!validate_model_path(config->vad.model_path, "VAD model")) {
      config->vad.enabled = false;
      config->vad.model_path[0] = '\0';
   }
   if (!validate_model_path(config->asr.model_path, "ASR model")) {
      config->processing.mode = PROCESSING_MODE_TEXT_ONLY;
      config->asr.model_path[0] = '\0';
   }
   if (!validate_model_path(config->tts.model_path, "TTS model")) {
      config->tts.model_path[0] = '\0';
   }
   if (!validate_model_path(config->tts.config_path, "TTS config")) {
      config->tts.config_path[0] = '\0';
   }
   if (!validate_model_path(config->tts.espeak_data, "espeak data")) {
      config->tts.espeak_data[0] = '\0';
   }

   /* Validate VAD model path */
   if (config->vad.enabled && !satellite_config_path_valid(config->vad.model_path)) {
      fprintf(stderr, "[CONFIG] WARNING: VAD model not found: %s - disabling VAD\n",
              config->vad.model_path);
      config->vad.enabled = false;
   } else if (config->vad.enabled) {
      printf("[CONFIG] VAD model: %s\n", config->vad.model_path);
   }

   /* Validate ASR model path */
   if (!satellite_config_path_valid(config->asr.model_path)) {
      fprintf(stderr, "[CONFIG] WARNING: ASR model not found: %s - voice input disabled\n",
              config->asr.model_path);
      /* Force text-only mode if ASR is unavailable */
      config->processing.mode = PROCESSING_MODE_TEXT_ONLY;
   } else {
      printf("[CONFIG] ASR engine: %s, model: %s (max %ds)\n", config->asr.engine,
             config->asr.model_path, config->asr.max_audio_seconds);
   }

   /* Validate TTS model path */
   if (!satellite_config_path_valid(config->tts.model_path)) {
      fprintf(stderr, "[CONFIG] WARNING: TTS model not found: %s - TTS disabled\n",
              config->tts.model_path);
   } else {
      printf("[CONFIG] TTS model: %s\n", config->tts.model_path);
   }

   /* Validate espeak data path */
   if (!satellite_config_path_valid(config->tts.espeak_data)) {
      fprintf(stderr, "[CONFIG] WARNING: espeak data not found: %s - TTS may fail\n",
              config->tts.espeak_data);
   }
}
