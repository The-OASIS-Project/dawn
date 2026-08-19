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
 * Home Assistant Tool - Smart home device control via Home Assistant REST API
 *
 * Supports: list, status, on, off, toggle, brightness, color, color_temp,
 *           temperature, lock, unlock, open, close, scene, script, automation
 */

#include "tools/homeassistant_tool.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/buf_printf.h"
#include "logging.h"
#include "tools/homeassistant_service.h"
#include "tools/homeassistant_ws.h"
#include "tools/toml.h"
#include "tools/tool_registry.h"

/* ========== Forward Declarations ========== */

static char *ha_tool_callback(const char *action, char *value, int *should_respond);
static bool ha_tool_is_available(void);
static int ha_tool_init(void);
static void ha_tool_cleanup(void);
static void ha_parse_config(toml_table_t *table, void *config);
static void ha_write_config(void *fp, const void *config);

/* ========== Tool-Owned Config ========== */

typedef struct {
   bool enabled;
   char url[256];
   int led_hue_correction; /* degrees to shift magenta region toward red (0-60) */
   bool realtime;          /* subscribe to HA's WS state_changed stream for live updates */
   bool insecure_tls;      /* wss:// only — accept self-signed / skip hostname (self-hosted HA) */
} ha_tool_config_t;

static ha_tool_config_t s_config = { .enabled = true,
                                     .url = "",
                                     .led_hue_correction = 20,
                                     .realtime = true,
                                     .insecure_tls = false };
static pthread_mutex_t s_reconfig_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ========== Config Parser ========== */

static void ha_parse_config(toml_table_t *table, void *config) {
   ha_tool_config_t *cfg = (ha_tool_config_t *)config;

   if (!table) {
      return;
   }

   toml_datum_t enabled = toml_bool_in(table, "enabled");
   if (enabled.ok) {
      cfg->enabled = enabled.u.b;
   }

   toml_datum_t url = toml_string_in(table, "url");
   if (url.ok) {
      strncpy(cfg->url, url.u.s, sizeof(cfg->url) - 1);
      cfg->url[sizeof(cfg->url) - 1] = '\0';
      free(url.u.s);
   }

   toml_datum_t hue_corr = toml_int_in(table, "led_hue_correction");
   if (hue_corr.ok) {
      int val = (int)hue_corr.u.i;
      if (val < 0)
         val = 0;
      if (val > 60)
         val = 60;
      cfg->led_hue_correction = val;
   }

   toml_datum_t realtime = toml_bool_in(table, "realtime");
   if (realtime.ok) {
      cfg->realtime = realtime.u.b;
   }

   toml_datum_t insecure_tls = toml_bool_in(table, "insecure_tls");
   if (insecure_tls.ok) {
      cfg->insecure_tls = insecure_tls.u.b;
   }
}

/* ========== Config Writer ========== */

static void ha_write_config(void *fp, const void *config) {
   const ha_tool_config_t *cfg = (const ha_tool_config_t *)config;
   FILE *f = (FILE *)fp;
   fprintf(f, "enabled = %s\n", cfg->enabled ? "true" : "false");
   if (cfg->url[0]) {
      /* Verify URL is safe for unescaped TOML string (no quotes/newlines) */
      bool safe = true;
      for (const char *p = cfg->url; *p; p++) {
         if (*p == '"' || *p == '\\' || *p == '\n' || *p == '\r') {
            safe = false;
            break;
         }
      }
      if (safe) {
         fprintf(f, "url = \"%s\"\n", cfg->url);
      }
   }
   fprintf(f, "led_hue_correction = %d\n", cfg->led_hue_correction);
   fprintf(f, "realtime = %s\n", cfg->realtime ? "true" : "false");
   fprintf(f, "insecure_tls = %s\n", cfg->insecure_tls ? "true" : "false");
}

/* ========== Secret Requirements ========== */

static const tool_secret_requirement_t ha_secrets[] = {
   { .secret_name = "home_assistant_token", .required = false },
   { .secret_name = NULL } /* Sentinel */
};

/* ========== Tool Parameter Definition ========== */

static const treg_param_t ha_params[] = {
   {
       .name = "action",
       .description = "Home Assistant action: 'list' (all entities), 'status' (entity state), "
                      "'on' (turn on), 'off' (turn off), 'toggle', 'brightness' (set level 0-100), "
                      "'color' (set color), 'color_temp' (set color temperature), "
                      "'temperature' (thermostat), 'lock', 'unlock', 'open' (cover), "
                      "'close' (cover), 'scene', 'script', 'automation'",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "list", "status", "on", "off", "toggle", "brightness", "color",
                        "color_temp", "temperature", "lock", "unlock", "open", "close", "scene",
                        "script", "automation" },
       .enum_count = 16,
   },
   {
       .name = "device",
       .description =
           "Entity friendly_name OR entity_id as registered in Home Assistant — call "
           "action='list' first to discover available entities; do not invent names. "
           "For brightness/color_temp/temperature: 'name value' (e.g., 'kitchen light 75' "
           "where brightness is 0-100%, color_temp is 1000-12000K, temperature is "
           "40-100°F). For color: 'name color' — color may be a name (red, blue, warm) or "
           "hex (#FF6B35).",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

/* ========== Tool Metadata ========== */

static const tool_metadata_t ha_metadata = {
   .name = "home_assistant",
   .device_string = "home assistant",
   .topic = "dawn",
   .aliases = { "hass", "smarthome", "iot" },
   .alias_count = 3,

   .description = "Control Home Assistant smart home entities. Actions: list (show all), "
                  "status (get state), on/off/toggle (power), brightness (0-100%), "
                  "color (hex #RRGGBB or name), color_temp (kelvin), temperature (thermostat), "
                  "lock/unlock, open/close (covers), scene/script/automation (activate).",
   .params = ha_params,
   .param_count = 2,

   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_NETWORK | TOOL_CAP_SECRETS | TOOL_CAP_SCHEDULABLE,
   .skip_followup = false,
   .default_remote = true,

   .config = &s_config,
   .config_size = sizeof(s_config),
   .config_parser = ha_parse_config,
   .config_writer = ha_write_config,
   .config_section = "home_assistant",

   .secret_requirements = ha_secrets,

   .is_available = ha_tool_is_available,
   .init = ha_tool_init,
   .cleanup = ha_tool_cleanup,
   .callback = ha_tool_callback,
};

/* ========== Lifecycle ========== */

static bool ha_tool_is_available(void) {
   return homeassistant_is_connected();
}

static int ha_tool_init(void) {
   const char *token = tool_registry_get_secret("home_assistant", "home_assistant_token");
   if (!s_config.url[0] || !token) {
      OLOG_INFO("Home Assistant tool: Not configured (url or token missing)");
      return 0; /* Not configured, not an error */
   }
   int rc = homeassistant_init(s_config.url, token) == HA_OK ? 0 : 1;
   if (rc == 0 && s_config.realtime) {
      /* Read/event plane: subscribe to HA's state_changed stream. Command plane
       * stays on REST regardless (see homeassistant_ws.h). No-op stub unless
       * built with DAWN_ENABLE_HA_REALTIME. */
      homeassistant_ws_start(s_config.insecure_tls);
   }
   return rc;
}

static void ha_tool_cleanup(void) {
   homeassistant_ws_stop();
   homeassistant_cleanup();
}

int homeassistant_tool_update_config(const char *url, int enabled, int led_hue_correction) {
   if (url) {
      /* Validate URL scheme (SSRF prevention) */
      if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
         OLOG_ERROR("Home Assistant: Invalid URL scheme (must be http:// or https://): %.32s...",
                    url);
         return 1;
      }
      strncpy(s_config.url, url, sizeof(s_config.url) - 1);
      s_config.url[sizeof(s_config.url) - 1] = '\0';
   }
   if (enabled >= 0) {
      s_config.enabled = (bool)enabled;
   }
   if (led_hue_correction >= 0) {
      if (led_hue_correction > 60)
         led_hue_correction = 60;
      s_config.led_hue_correction = led_hue_correction;
   }

   /* If only hue correction changed, no service reinit needed */
   if (!url && enabled < 0)
      return 0;

   /* Serialize cleanup+init so concurrent callers don't see partially-initialized state */
   pthread_mutex_lock(&s_reconfig_mutex);

   /* Stop the realtime listener before reconfig so it reconnects with the new
    * URL/token (it re-reads credentials at each connect). Idempotent. */
   homeassistant_ws_stop();
   homeassistant_cleanup();

   int result = 0;
   bool inited = false;
   if (!s_config.enabled || !s_config.url[0]) {
      OLOG_INFO("Home Assistant: Disabled or URL not configured");
   } else {
      const char *token = tool_registry_get_secret("home_assistant", "home_assistant_token");
      if (!token) {
         OLOG_INFO("Home Assistant: Token not configured");
      } else {
         result = homeassistant_init(s_config.url, token) == HA_OK ? 0 : 1;
         inited = (result == 0);
      }
   }
   if (inited && s_config.realtime) {
      homeassistant_ws_start(s_config.insecure_tls);
   }

   pthread_mutex_unlock(&s_reconfig_mutex);
   return result;
}

/* ========== Color Name Mappings (RGB) ========== */

typedef struct {
   const char *name;
   int r, g, b;
} color_map_t;

static const color_map_t color_names[] = {
   { "red", 255, 0, 0 },      { "orange", 255, 165, 0 }, { "yellow", 255, 255, 0 },
   { "green", 0, 255, 0 },    { "cyan", 0, 255, 255 },   { "blue", 0, 0, 255 },
   { "purple", 128, 0, 128 }, { "pink", 255, 192, 203 }, { "white", 255, 255, 255 },
   { "warm", 255, 180, 100 }, { "cool", 200, 220, 255 },
};
static const int color_count = sizeof(color_names) / sizeof(color_names[0]);

/* ========== Helper Functions ========== */

/* Copy entity fields to local buffers to avoid stale cache pointers.
 * The entity pointer from homeassistant_find_entity() points into the shared
 * cache which can be refreshed by another thread at any time. */
#define ENTITY_LOCAL_COPY(entity)                                                         \
   char entity_id_buf[HA_MAX_ENTITY_ID];                                                  \
   char friendly_name_buf[HA_MAX_FRIENDLY_NAME];                                          \
   do {                                                                                   \
      strncpy(entity_id_buf, (entity)->entity_id, sizeof(entity_id_buf) - 1);             \
      entity_id_buf[sizeof(entity_id_buf) - 1] = '\0';                                    \
      strncpy(friendly_name_buf, (entity)->friendly_name, sizeof(friendly_name_buf) - 1); \
      friendly_name_buf[sizeof(friendly_name_buf) - 1] = '\0';                            \
   } while (0)

static char *make_error_msg(const char *fmt, const char *arg) {
   char *msg = malloc(256);
   if (msg) {
      snprintf(msg, 256, fmt, arg);
   }
   return msg ? msg : strdup("Error");
}

static char *make_success_msg(const char *fmt, const char *arg) {
   char *msg = malloc(256);
   if (msg) {
      snprintf(msg, 256, fmt, arg);
   }
   return msg ? msg : strdup("Success");
}

static char *make_success_msg_int(const char *fmt, const char *arg, int val) {
   char *msg = malloc(256);
   if (msg) {
      snprintf(msg, 256, fmt, arg, val);
   }
   return msg ? msg : strdup("Success");
}

static char *make_success_msg_double(const char *fmt, const char *arg, double val) {
   char *msg = malloc(256);
   if (msg) {
      snprintf(msg, 256, fmt, arg, val);
   }
   return msg ? msg : strdup("Success");
}

/**
 * @brief Parse "device_name value" from the value string
 *
 * Splits on last space: everything before is device name, last token is the value.
 */
static bool parse_device_and_value(const char *value,
                                   char *device_out,
                                   size_t device_size,
                                   const char **value_part) {
   if (!value || !value[0])
      return false;

   const char *last_space = strrchr(value, ' ');
   if (!last_space || !last_space[1])
      return false;

   size_t name_len = (size_t)(last_space - value);
   if (name_len >= device_size)
      name_len = device_size - 1;

   memcpy(device_out, value, name_len);
   device_out[name_len] = '\0';
   *value_part = last_space + 1;
   return true;
}

/* ========== Domain Display Helpers ========== */

static const char *domain_display_name(ha_domain_t domain) {
   switch (domain) {
      case HA_DOMAIN_LIGHT:
         return "Lights";
      case HA_DOMAIN_SWITCH:
         return "Switches";
      case HA_DOMAIN_CLIMATE:
         return "Climate";
      case HA_DOMAIN_LOCK:
         return "Locks";
      case HA_DOMAIN_COVER:
         return "Covers";
      case HA_DOMAIN_MEDIA_PLAYER:
         return "Media Players";
      case HA_DOMAIN_FAN:
         return "Fans";
      case HA_DOMAIN_SCENE:
         return "Scenes";
      case HA_DOMAIN_SCRIPT:
         return "Scripts";
      case HA_DOMAIN_AUTOMATION:
         return "Automations";
      case HA_DOMAIN_SENSOR:
         return "Sensors";
      case HA_DOMAIN_BINARY_SENSOR:
         return "Binary Sensors";
      case HA_DOMAIN_INPUT_BOOLEAN:
         return "Input Booleans";
      case HA_DOMAIN_VACUUM:
         return "Vacuums";
      case HA_DOMAIN_ALARM:
         return "Alarms";
      default:
         return "Other";
   }
}

/* ========== LED Hue Correction ========== */

/**
 * @brief Convert RGB to HS and apply LED hue correction
 *
 * LED bulbs render magenta/pink (hue ~300°) as blue/purple due to phosphor
 * wavelength offsets from sRGB assumptions.  This applies a raised-cosine
 * weighted shift centered on 300° with ±90° falloff, nudging magenta toward
 * red so it renders correctly on real hardware.
 */
static void rgb_to_corrected_hs(int r,
                                int g,
                                int b,
                                int correction,
                                double *out_hue,
                                double *out_sat) {
   double hue, sat;
   homeassistant_rgb_to_hs(r, g, b, &hue, &sat);
   *out_sat = sat;

   /* Apply hue correction: raised-cosine taper centered at 300° (magenta) */
   if (correction != 0 && *out_sat > 1.0) {
      double dist = fabs(hue - 300.0);
      if (dist > 180.0)
         dist = 360.0 - dist;
      if (dist < 90.0) {
         double weight = 0.5 + 0.5 * cos(dist * M_PI / 90.0);
         hue += (double)correction * weight;
         if (hue >= 360.0)
            hue -= 360.0;
         if (hue < 0.0)
            hue += 360.0;
      }
   }
   *out_hue = hue;
}

/* ========== Action Handlers ========== */

static char *handle_list(void) {
   const ha_entity_list_t *entities;
   ha_error_t err = homeassistant_list_entities(&entities);
   if (err != HA_OK) {
      return make_error_msg("Failed to list entities: %s", homeassistant_error_str(err));
   }

   /* Pre-allocate buffer: entity_count * ~100 bytes each */
   size_t buf_size = (size_t)entities->count * 100 + 512;
   if (buf_size > 65536)
      buf_size = 65536;
   char *buf = malloc(buf_size);
   if (!buf)
      return strdup("Memory allocation failed");

   size_t len = 0;
   size_t rem = buf_size;

   BUF_PRINTF(buf, len, rem, "Found %d Home Assistant entities:\n", entities->count);

   /* Group by domain — iterate domain enum values */
   for (int d = 0; d <= HA_DOMAIN_UNKNOWN; d++) {
      ha_domain_t domain = (ha_domain_t)d;
      if (domain == HA_DOMAIN_UNKNOWN)
         continue;

      /* Count entities in this domain */
      int domain_count = 0;
      for (int i = 0; i < entities->count; i++) {
         if (entities->entities[i].domain == domain)
            domain_count++;
      }
      if (domain_count == 0)
         continue;

      BUF_PRINTF(buf, len, rem, "\n%s (%d):\n", domain_display_name(domain), domain_count);

      for (int i = 0; i < entities->count && rem > 100; i++) {
         const ha_entity_t *ent = &entities->entities[i];
         if (ent->domain != domain)
            continue;

         BUF_PRINTF(buf, len, rem, "- %s (%s", ent->friendly_name, ent->entity_id);
         if (ent->area_name[0])
            BUF_PRINTF(buf, len, rem, ", %s", ent->area_name);
         BUF_PRINTF(buf, len, rem, ") - %s", ent->state);

         /* Domain-specific details */
         if (domain == HA_DOMAIN_LIGHT) {
            if (ent->brightness > 0)
               BUF_PRINTF(buf, len, rem, ", %d%%", ent->brightness * 100 / 255);
            if (ent->rgb_color[0] || ent->rgb_color[1] || ent->rgb_color[2])
               BUF_PRINTF(buf, len, rem, ", #%02X%02X%02X", ent->rgb_color[0], ent->rgb_color[1],
                          ent->rgb_color[2]);
         } else if (domain == HA_DOMAIN_CLIMATE) {
            if (ent->temperature != 0)
               BUF_PRINTF(buf, len, rem,
                          ", %.0f\xC2\xB0"
                          "F",
                          ent->temperature);
            if (ent->target_temp != 0)
               BUF_PRINTF(buf, len, rem,
                          " (target: %.0f\xC2\xB0"
                          "F)",
                          ent->target_temp);
         } else if (domain == HA_DOMAIN_COVER && ent->cover_position >= 0) {
            BUF_PRINTF(buf, len, rem, ", %d%%", ent->cover_position);
         }

         BUF_PRINTF(buf, len, rem, "\n");
      }
   }

   return buf;
}

static char *handle_status(const char *value) {
   if (!value || !value[0])
      return strdup("Please specify an entity name.");

   const ha_entity_t *entity;
   ha_error_t err = homeassistant_find_entity(value, HA_DOMAIN_UNKNOWN, &entity);
   if (err != HA_OK) {
      return make_error_msg("Entity '%s' not found", value);
   }
   ENTITY_LOCAL_COPY(entity);

   /* Get fresh state */
   ha_entity_t fresh;
   err = homeassistant_get_entity_state(entity_id_buf, &fresh);
   if (err != HA_OK) {
      return make_error_msg("Failed to get status: %s", homeassistant_error_str(err));
   }

   char *buf = malloc(1024);
   if (!buf)
      return strdup("Memory allocation failed");

   size_t len = 0;
   size_t rem = 1024;

   BUF_PRINTF(buf, len, rem, "Status of '%s' (%s):\n", fresh.friendly_name, fresh.entity_id);
   BUF_PRINTF(buf, len, rem, "- State: %s\n", fresh.state);

   if (fresh.brightness > 0)
      BUF_PRINTF(buf, len, rem, "- Brightness: %d%%\n", fresh.brightness * 100 / 255);
   if (fresh.color_mode[0])
      BUF_PRINTF(buf, len, rem, "- Color mode: %s\n", fresh.color_mode);
   if (fresh.rgb_color[0] || fresh.rgb_color[1] || fresh.rgb_color[2])
      BUF_PRINTF(buf, len, rem, "- Color: #%02X%02X%02X (RGB %d,%d,%d)\n", fresh.rgb_color[0],
                 fresh.rgb_color[1], fresh.rgb_color[2], fresh.rgb_color[0], fresh.rgb_color[1],
                 fresh.rgb_color[2]);
   if (fresh.hs_color[1] > 0.0)
      BUF_PRINTF(buf, len, rem, "- HS color: hue %.0f°, saturation %.0f%%\n", fresh.hs_color[0],
                 fresh.hs_color[1]);
   if (fresh.color_temp > 0)
      BUF_PRINTF(buf, len, rem, "- Color temp: %dK\n", 1000000 / fresh.color_temp);
   if (fresh.temperature != 0)
      BUF_PRINTF(buf, len, rem,
                 "- Temperature: %.1f\xC2\xB0"
                 "F\n",
                 fresh.temperature);
   if (fresh.target_temp != 0)
      BUF_PRINTF(buf, len, rem,
                 "- Target: %.1f\xC2\xB0"
                 "F\n",
                 fresh.target_temp);
   if (fresh.hvac_mode[0])
      BUF_PRINTF(buf, len, rem, "- HVAC mode: %s\n", fresh.hvac_mode);

   return buf;
}

static char *handle_on(const char *value) {
   if (!value || !value[0])
      return strdup("Please specify an entity name.");

   const ha_entity_t *entity;
   ha_error_t err = homeassistant_find_entity(value, HA_DOMAIN_UNKNOWN, &entity);
   if (err != HA_OK)
      return make_error_msg("Entity '%s' not found", value);
   ENTITY_LOCAL_COPY(entity);

   err = homeassistant_turn_on(entity_id_buf);
   if (err != HA_OK)
      return make_error_msg("Failed to turn on: %s", homeassistant_error_str(err));

   return make_success_msg("Turned on '%s'", friendly_name_buf);
}

static char *handle_off(const char *value) {
   if (!value || !value[0])
      return strdup("Please specify an entity name.");

   const ha_entity_t *entity;
   ha_error_t err = homeassistant_find_entity(value, HA_DOMAIN_UNKNOWN, &entity);
   if (err != HA_OK)
      return make_error_msg("Entity '%s' not found", value);
   ENTITY_LOCAL_COPY(entity);

   err = homeassistant_turn_off(entity_id_buf);
   if (err != HA_OK)
      return make_error_msg("Failed to turn off: %s", homeassistant_error_str(err));

   return make_success_msg("Turned off '%s'", friendly_name_buf);
}

static char *handle_toggle(const char *value) {
   if (!value || !value[0])
      return strdup("Please specify an entity name.");

   const ha_entity_t *entity;
   ha_error_t err = homeassistant_find_entity(value, HA_DOMAIN_UNKNOWN, &entity);
   if (err != HA_OK)
      return make_error_msg("Entity '%s' not found", value);
   ENTITY_LOCAL_COPY(entity);

   err = homeassistant_toggle(entity_id_buf);
   if (err != HA_OK)
      return make_error_msg("Failed to toggle: %s", homeassistant_error_str(err));

   return make_success_msg("Toggled '%s'", friendly_name_buf);
}

static char *handle_brightness(const char *value) {
   if (!value || !value[0])
      return strdup("Please specify entity name and brightness level (e.g., 'lamp 75').");

   char device_name[128];
   const char *val_part;
   if (!parse_device_and_value(value, device_name, sizeof(device_name), &val_part)) {
      return strdup("Please specify entity name and brightness (0-100).");
   }

   int level = atoi(val_part);
   if (level < 0 || level > 100) {
      return strdup("Brightness must be 0-100.");
   }

   const ha_entity_t *entity;
   ha_error_t err = homeassistant_find_entity(device_name, HA_DOMAIN_LIGHT, &entity);
   if (err != HA_OK)
      return make_error_msg("Entity '%s' not found", device_name);
   ENTITY_LOCAL_COPY(entity);

   err = homeassistant_set_brightness(entity_id_buf, level);
   if (err != HA_OK)
      return make_error_msg("Failed to set brightness: %s", homeassistant_error_str(err));

   return make_success_msg_int("Set '%s' brightness to %d%%", friendly_name_buf, level);
}

static char *handle_color(const char *value) {
   if (!value || !value[0])
      return strdup("Please specify entity name and color (e.g., 'lamp red').");

   char device_name[128];
   const char *color_part;
   if (!parse_device_and_value(value, device_name, sizeof(device_name), &color_part)) {
      return strdup("Please specify entity name and color.");
   }

   int r = -1, g = -1, b = -1;

   /* Try named color first */
   for (int i = 0; i < color_count; i++) {
      if (strcasecmp(color_part, color_names[i].name) == 0) {
         r = color_names[i].r;
         g = color_names[i].g;
         b = color_names[i].b;
         break;
      }
   }

   /* Try hex color: #RRGGBB or RRGGBB */
   if (r < 0) {
      const char *hex = color_part;
      if (hex[0] == '#')
         hex++;
      unsigned int hr, hg, hb;
      if (strlen(hex) == 6 && sscanf(hex, "%02x%02x%02x", &hr, &hg, &hb) == 3) {
         r = (int)hr;
         g = (int)hg;
         b = (int)hb;
      }
   }

   if (r < 0) {
      return strdup(
          "Unknown color. Use a hex code (#FF6B35) or name: red, orange, yellow, green, cyan, "
          "blue, purple, pink, white, warm, cool");
   }

   const ha_entity_t *entity;
   ha_error_t err = homeassistant_find_entity(device_name, HA_DOMAIN_LIGHT, &entity);
   if (err != HA_OK)
      return make_error_msg("Entity '%s' not found", device_name);
   ENTITY_LOCAL_COPY(entity);

   /* Convert RGB to HS with LED hue correction applied */
   double hue, sat;
   rgb_to_corrected_hs(r, g, b, s_config.led_hue_correction, &hue, &sat);
   err = homeassistant_set_hs_color(entity_id_buf, hue, sat);
   if (err != HA_OK)
      return make_error_msg("Failed to set color: %s", homeassistant_error_str(err));

   return make_success_msg("Set '%s' color", friendly_name_buf);
}

static char *handle_color_temp(const char *value) {
   if (!value || !value[0])
      return strdup("Please specify entity name and color temp in kelvin (e.g., 'lamp 3000').");

   char device_name[128];
   const char *val_part;
   if (!parse_device_and_value(value, device_name, sizeof(device_name), &val_part)) {
      return strdup("Please specify entity name and color temperature (kelvin).");
   }

   int kelvin = atoi(val_part);
   if (kelvin < 1000 || kelvin > 12000) {
      return strdup("Color temperature must be 1000-12000K.");
   }

   const ha_entity_t *entity;
   ha_error_t err = homeassistant_find_entity(device_name, HA_DOMAIN_LIGHT, &entity);
   if (err != HA_OK)
      return make_error_msg("Entity '%s' not found", device_name);
   ENTITY_LOCAL_COPY(entity);

   err = homeassistant_set_color_temp(entity_id_buf, kelvin);
   if (err != HA_OK)
      return make_error_msg("Failed to set color temp: %s", homeassistant_error_str(err));

   return make_success_msg_int("Set '%s' color temperature to %dK", friendly_name_buf, kelvin);
}

static char *handle_temperature(const char *value) {
   if (!value || !value[0])
      return strdup("Please specify device name and temperature (e.g., 'thermostat 72').");

   char device_name[128];
   const char *val_part;
   if (!parse_device_and_value(value, device_name, sizeof(device_name), &val_part)) {
      return strdup("Please specify device name and temperature.");
   }

   double temp = atof(val_part);
   if (temp < 40 || temp > 100) {
      return strdup("Please specify a valid temperature (40-100F).");
   }

   const ha_entity_t *entity;
   ha_error_t err = homeassistant_find_entity(device_name, HA_DOMAIN_CLIMATE, &entity);
   if (err != HA_OK)
      return make_error_msg("Entity '%s' not found", device_name);
   ENTITY_LOCAL_COPY(entity);

   err = homeassistant_set_temperature(entity_id_buf, temp);
   if (err != HA_OK)
      return make_error_msg("Failed to set temperature: %s", homeassistant_error_str(err));

   return make_success_msg_double("Set '%s' to %.0f\xC2\xB0"
                                  "F",
                                  friendly_name_buf, temp);
}

static char *handle_lock(const char *value) {
   if (!value || !value[0])
      return strdup("Please specify a lock device name.");

   const ha_entity_t *entity;
   ha_error_t err = homeassistant_find_entity(value, HA_DOMAIN_LOCK, &entity);
   if (err != HA_OK)
      return make_error_msg("Lock '%s' not found", value);
   ENTITY_LOCAL_COPY(entity);

   err = homeassistant_lock(entity_id_buf);
   if (err != HA_OK)
      return make_error_msg("Failed to lock: %s", homeassistant_error_str(err));

   return make_success_msg("Locked '%s'", friendly_name_buf);
}

static char *handle_unlock(const char *value) {
   if (!value || !value[0])
      return strdup("Please specify a lock device name.");

   const ha_entity_t *entity;
   ha_error_t err = homeassistant_find_entity(value, HA_DOMAIN_LOCK, &entity);
   if (err != HA_OK)
      return make_error_msg("Lock '%s' not found", value);
   ENTITY_LOCAL_COPY(entity);

   err = homeassistant_unlock(entity_id_buf);
   if (err != HA_OK)
      return make_error_msg("Failed to unlock: %s", homeassistant_error_str(err));

   return make_success_msg("Unlocked '%s'", friendly_name_buf);
}

static char *handle_open(const char *value) {
   if (!value || !value[0])
      return strdup("Please specify a cover device name.");

   const ha_entity_t *entity;
   ha_error_t err = homeassistant_find_entity(value, HA_DOMAIN_COVER, &entity);
   if (err != HA_OK)
      return make_error_msg("Cover '%s' not found", value);
   ENTITY_LOCAL_COPY(entity);

   err = homeassistant_open_cover(entity_id_buf);
   if (err != HA_OK)
      return make_error_msg("Failed to open: %s", homeassistant_error_str(err));

   return make_success_msg("Opened '%s'", friendly_name_buf);
}

static char *handle_close(const char *value) {
   if (!value || !value[0])
      return strdup("Please specify a cover device name.");

   const ha_entity_t *entity;
   ha_error_t err = homeassistant_find_entity(value, HA_DOMAIN_COVER, &entity);
   if (err != HA_OK)
      return make_error_msg("Cover '%s' not found", value);
   ENTITY_LOCAL_COPY(entity);

   err = homeassistant_close_cover(entity_id_buf);
   if (err != HA_OK)
      return make_error_msg("Failed to close: %s", homeassistant_error_str(err));

   return make_success_msg("Closed '%s'", friendly_name_buf);
}

static char *handle_scene(const char *value) {
   if (!value || !value[0])
      return strdup("Please specify a scene name.");

   const ha_entity_t *entity;
   ha_error_t err = homeassistant_find_entity(value, HA_DOMAIN_SCENE, &entity);
   if (err != HA_OK)
      return make_error_msg("Scene '%s' not found", value);
   ENTITY_LOCAL_COPY(entity);

   err = homeassistant_activate_scene(entity_id_buf);
   if (err != HA_OK)
      return make_error_msg("Failed to activate scene: %s", homeassistant_error_str(err));

   return make_success_msg("Activated scene '%s'", friendly_name_buf);
}

static char *handle_script(const char *value) {
   if (!value || !value[0])
      return strdup("Please specify a script name.");

   const ha_entity_t *entity;
   ha_error_t err = homeassistant_find_entity(value, HA_DOMAIN_SCRIPT, &entity);
   if (err != HA_OK)
      return make_error_msg("Script '%s' not found", value);
   ENTITY_LOCAL_COPY(entity);

   err = homeassistant_run_script(entity_id_buf);
   if (err != HA_OK)
      return make_error_msg("Failed to run script: %s", homeassistant_error_str(err));

   return make_success_msg("Ran script '%s'", friendly_name_buf);
}

static char *handle_automation(const char *value) {
   if (!value || !value[0])
      return strdup("Please specify an automation name.");

   const ha_entity_t *entity;
   ha_error_t err = homeassistant_find_entity(value, HA_DOMAIN_AUTOMATION, &entity);
   if (err != HA_OK)
      return make_error_msg("Automation '%s' not found", value);
   ENTITY_LOCAL_COPY(entity);

   err = homeassistant_trigger_automation(entity_id_buf);
   if (err != HA_OK)
      return make_error_msg("Failed to trigger automation: %s", homeassistant_error_str(err));

   return make_success_msg("Triggered automation '%s'", friendly_name_buf);
}

/* ========== Callback Implementation ========== */

static char *ha_tool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1;

   if (!homeassistant_is_configured()) {
      return strdup("Home Assistant is not configured. Please set url in dawn.toml "
                    "[home_assistant] and token in secrets.toml [secrets.home_assistant].");
   }

   if (!homeassistant_is_connected()) {
      return strdup("Home Assistant is not connected. Check the URL and token in settings.");
   }

   /* Dispatch to action handlers */
   if (strcmp(action, "list") == 0)
      return handle_list();
   if (strcmp(action, "status") == 0)
      return handle_status(value);
   if (strcmp(action, "on") == 0)
      return handle_on(value);
   if (strcmp(action, "off") == 0)
      return handle_off(value);
   if (strcmp(action, "toggle") == 0)
      return handle_toggle(value);
   if (strcmp(action, "brightness") == 0)
      return handle_brightness(value);
   if (strcmp(action, "color") == 0)
      return handle_color(value);
   if (strcmp(action, "color_temp") == 0)
      return handle_color_temp(value);
   if (strcmp(action, "temperature") == 0)
      return handle_temperature(value);
   if (strcmp(action, "lock") == 0)
      return handle_lock(value);
   if (strcmp(action, "unlock") == 0)
      return handle_unlock(value);
   if (strcmp(action, "open") == 0)
      return handle_open(value);
   if (strcmp(action, "close") == 0)
      return handle_close(value);
   if (strcmp(action, "scene") == 0)
      return handle_scene(value);
   if (strcmp(action, "script") == 0)
      return handle_script(value);
   if (strcmp(action, "automation") == 0)
      return handle_automation(value);

   char buf[256];
   snprintf(buf, sizeof(buf), "Unknown Home Assistant action '%s'.", action);
   return strdup(buf);
}

/* ========== Public API ========== */

int homeassistant_tool_get_hue_correction(void) {
   return s_config.led_hue_correction;
}

int homeassistant_tool_register(void) {
   return tool_registry_register(&ha_metadata);
}
