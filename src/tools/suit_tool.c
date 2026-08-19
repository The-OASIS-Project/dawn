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
 * Suit-telemetry tool ("suit_status") — LLM descriptor + callback.  Reads the
 * live suit_service cache (AURA helmet + SPARK armor, republished by MIRAGE).
 * Live-only; no history.  See include/tools/suit_tool.h.
 */

#include "tools/suit_tool.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/suit_service.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/toml.h"
#include "tools/tool_registry.h"

#define SUIT_RESULT_MAX 1024

/* ========== Forward declarations ========== */

static char *suit_tool_callback(const char *action, char *value, int *should_respond);
static bool suit_tool_is_available(void);
static int suit_tool_init(void);
static void suit_tool_cleanup(void);
static void suit_parse_config(toml_table_t *table, void *config);
static void suit_write_config(void *fp, const void *config);

/* ========== Tool-owned config ========== */

typedef struct {
   bool enabled;
   char helmet_topic[128];
   char armor_topic[128];
   int stale_after_sec;
} suit_tool_config_t;

static suit_tool_config_t s_config = {
   .enabled = true,
   .helmet_topic = SUIT_HELMET_TOPIC_DEFAULT,
   .armor_topic = SUIT_ARMOR_TOPIC_DEFAULT,
   .stale_after_sec = SUIT_STALE_AFTER_SEC_DEFAULT,
};

/* ========== Config parser / writer ========== */

static void copy_toml_str(toml_table_t *table, const char *key, char *dst, size_t dst_sz) {
   toml_datum_t v = toml_string_in(table, key);
   if (v.ok) {
      snprintf(dst, dst_sz, "%s", v.u.s);
      free(v.u.s);
   }
}

static void suit_parse_config(toml_table_t *table, void *config) {
   suit_tool_config_t *cfg = (suit_tool_config_t *)config;
   if (!table) {
      return;
   }
   toml_datum_t enabled = toml_bool_in(table, "enabled");
   if (enabled.ok) {
      cfg->enabled = enabled.u.b;
   }
   copy_toml_str(table, "helmet_topic", cfg->helmet_topic, sizeof(cfg->helmet_topic));
   copy_toml_str(table, "armor_topic", cfg->armor_topic, sizeof(cfg->armor_topic));
   toml_datum_t stale = toml_int_in(table, "stale_after_sec");
   if (stale.ok && stale.u.i > 0) {
      cfg->stale_after_sec = (int)stale.u.i;
   }
}

/* Only emit a TOML string when it has no quotes/newlines (unescaped-safe). */
static void write_safe_str(FILE *f, const char *key, const char *val) {
   for (const char *p = val; *p; p++) {
      if (*p == '"' || *p == '\\' || *p == '\n' || *p == '\r') {
         return;
      }
   }
   fprintf(f, "%s = \"%s\"\n", key, val);
}

static void suit_write_config(void *fp, const void *config) {
   const suit_tool_config_t *cfg = (const suit_tool_config_t *)config;
   FILE *f = (FILE *)fp;
   fprintf(f, "enabled = %s\n", cfg->enabled ? "true" : "false");
   write_safe_str(f, "helmet_topic", cfg->helmet_topic);
   write_safe_str(f, "armor_topic", cfg->armor_topic);
   fprintf(f, "stale_after_sec = %d\n", cfg->stale_after_sec);
}

/* ========== Parameters ========== */

static const treg_param_t suit_params[] = {
   {
       .name = "action",
       .description =
           "What to report: 'all' (full suit status), 'environment' (helmet air quality, CO2, "
           "temperature, humidity, heat index — use this for 'what's my CO2/air'), 'orientation' "
           "(helmet heading/pitch/roll), 'position' (GPS fix, coordinates, altitude, speed), or "
           "'armor' (SPARK armor pieces: which are online, temperature, voltage).",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "all", "environment", "orientation", "position", "armor" },
       .enum_count = 5,
   },
};

/* ========== Metadata ========== */

static const tool_metadata_t suit_metadata = {
   .name = "suit_status",
   .device_string = "suit_status",
   .topic = "dawn",
   .aliases = { "helmet_status", "armor_status" },
   .alias_count = 2,

   .description =
       "Report the wearer's live suit telemetry: helmet environment from the AURA sensors (air "
       "quality, CO2 (eCO2/CO2 ppm), VOCs, temperature, humidity, heat index, dew point), helmet "
       "orientation (heading, pitch, roll), GPS position (fix, coordinates, altitude, speed), and "
       "SPARK armor pieces (which are online, their temperature and voltage). Use 'environment' "
       "for questions about breathable air, CO2, or how hot it is inside the helmet; 'position' "
       "for where the wearer is; 'armor' for suit-piece status; 'all' for a full readout. This is "
       "the physical suit worn by the user (helmet + armor), NOT this host's own system telemetry "
       "(use system_status for CPU/battery/fan of the compute unit). If a feed is offline or "
       "stale, that is reported instead of guessed values.",
   .params = suit_params,
   .param_count = 1,

   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_SCHEDULABLE | TOOL_CAP_INFORMATIONAL,
   .skip_followup = false,
   .default_local = true,
   .default_remote = true,

   .config = &s_config,
   .config_size = sizeof(s_config),
   .config_parser = suit_parse_config,
   .config_writer = suit_write_config,
   .config_section = "suit",

   .is_available = suit_tool_is_available,
   .init = suit_tool_init,
   .cleanup = suit_tool_cleanup,
   .callback = suit_tool_callback,
};

/* ========== Formatting helpers ========== */

/* Bounded formatted append: writes at *off and advances it, clamped to sz.  A
 * truncating OR failed (negative-return) write leaves *off == sz so no later
 * call can underflow sz-*off or index past the buffer — the discipline the
 * accumulating armor loop needs (a full 16-piece roster can fill the buffer). */
static void append_fmt(char *buf, size_t sz, size_t *off, const char *fmt, ...) {
   if (*off >= sz) {
      return;
   }
   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(buf + *off, sz - *off, fmt, ap);
   va_end(ap);
   if (n < 0) {
      *off = sz; /* encoding error — stop all further writes */
      return;
   }
   *off += (size_t)n;
   if (*off > sz) {
      *off = sz;
   }
}

/* A short "(stale, Nds old)" caveat for a subsystem, or "" when fresh. */
static void freshness(bool stale, int age_sec, char *out, size_t out_sz) {
   if (stale) {
      snprintf(out, out_sz, " (stale, %ds old)", age_sec);
   } else {
      out[0] = '\0';
   }
}

static void append_environment(const suit_snapshot_t *s, char *buf, size_t sz) {
   size_t off = strlen(buf);
   if (!s->have_enviro) {
      snprintf(buf + off, sz - off, "No helmet environment data. ");
      return;
   }
   char fresh[32];
   freshness(s->enviro_stale, s->enviro_age_sec, fresh, sizeof(fresh));
   snprintf(buf + off, sz - off,
            "Helmet air%s: CO2 %.0f ppm%s%s (eCO2 %.0f ppm, VOC %.0f ppb), air quality %.0f%s%s, "
            "%.1f°C, %.0f%% humidity, heat index %.1f°C. ",
            fresh, s->co2_ppm, s->co2_quality_desc[0] ? " " : "", s->co2_quality_desc, s->eco2_ppm,
            s->tvoc_ppb, s->air_quality, s->air_quality_desc[0] ? " " : "", s->air_quality_desc,
            s->temp, s->humidity, s->heat_index_c);
}

static void append_orientation(const suit_snapshot_t *s, char *buf, size_t sz) {
   size_t off = strlen(buf);
   if (!s->have_motion) {
      snprintf(buf + off, sz - off, "No helmet orientation data. ");
      return;
   }
   char fresh[32];
   freshness(s->motion_stale, s->motion_age_sec, fresh, sizeof(fresh));
   snprintf(buf + off, sz - off, "Orientation%s: heading %.0f°, pitch %.0f°, roll %.0f°. ", fresh,
            s->heading, s->pitch, s->roll);
}

static void append_position(const suit_snapshot_t *s, char *buf, size_t sz) {
   size_t off = strlen(buf);
   if (!s->have_gps) {
      snprintf(buf + off, sz - off, "No GPS data. ");
      return;
   }
   char fresh[32];
   freshness(s->gps_stale, s->gps_age_sec, fresh, sizeof(fresh));
   if (!s->fix) {
      snprintf(buf + off, sz - off, "GPS%s: no fix (%d satellites). ", fresh, s->satellites);
      return;
   }
   snprintf(buf + off, sz - off, "GPS%s: %.5f, %.5f, altitude %.0f m, speed %.1f, %d satellites. ",
            fresh, s->latitude, s->longitude, s->altitude, s->speed, s->satellites);
}

static void append_armor(const suit_snapshot_t *s, char *buf, size_t sz) {
   size_t off = strlen(buf);
   if (s->armor_count == 0) {
      append_fmt(buf, sz, &off, "No armor pieces reporting. ");
      return;
   }
   append_fmt(buf, sz, &off, "Armor (%d piece%s): ", s->armor_count,
              s->armor_count == 1 ? "" : "s");
   for (int i = 0; i < s->armor_count; i++) {
      const suit_armor_piece_t *p = &s->armor[i];
      append_fmt(buf, sz, &off, "%s %s", p->name, p->stale ? "offline" : "online");
      if (!p->stale && (p->have_temp || p->have_voltage)) {
         append_fmt(buf, sz, &off, " (");
         if (p->have_temp) {
            append_fmt(buf, sz, &off, "%.1f°C", p->temp);
         }
         if (p->have_voltage) {
            append_fmt(buf, sz, &off, "%s%.1f V", p->have_temp ? ", " : "", p->voltage);
         }
         append_fmt(buf, sz, &off, ")");
      }
      append_fmt(buf, sz, &off, "%s", i + 1 < s->armor_count ? ", " : ". ");
   }
}

/* Build the live-status string.  Returns SUCCESS, or FAILURE with an error
 * message (nothing received yet) already written into buf. */
static int format_live(const char *action, char *buf, size_t sz) {
   suit_snapshot_t s;
   suit_service_get_snapshot(&s);

   if (!s.ever_seen) {
      snprintf(buf, sz,
               "No suit telemetry received yet - the helmet/armor feed may not be running "
               "(is MIRAGE republishing helmet/telemetry?).");
      return FAILURE;
   }

   buf[0] = '\0';
   if (strcasecmp(action, "all") == 0) {
      append_environment(&s, buf, sz);
      append_orientation(&s, buf, sz);
      append_position(&s, buf, sz);
      append_armor(&s, buf, sz);
   } else if (strcasecmp(action, "environment") == 0) {
      append_environment(&s, buf, sz);
   } else if (strcasecmp(action, "orientation") == 0) {
      append_orientation(&s, buf, sz);
   } else if (strcasecmp(action, "position") == 0) {
      append_position(&s, buf, sz);
   } else if (strcasecmp(action, "armor") == 0) {
      append_armor(&s, buf, sz);
   } else {
      snprintf(buf, sz, "Unknown suit_status action '%s'.", action);
      return FAILURE;
   }
   return SUCCESS;
}

/* ========== Callback ========== */

static char *suit_tool_callback(const char *action, char *value, int *should_respond) {
   (void)value;
   *should_respond = 1;
   if (!action) {
      return strdup(TOOL_RESULT_ERROR_MARK "suit_status: missing action.");
   }

   char *out = malloc(SUIT_RESULT_MAX);
   if (!out) {
      return strdup(TOOL_RESULT_ERROR_MARK "suit_status: out of memory.");
   }
   out[0] = '\0';

   if (format_live(action, out, SUIT_RESULT_MAX) != SUCCESS) {
      /* Prefix the error mark IN PLACE so the failure is surfaced (not spoken as
       * data) — avoids a second allocation whose OOM would drop the mark. */
      size_t len = strlen(out);
      if (len + 2 <= SUIT_RESULT_MAX) {
         memmove(out + 1, out, len + 1); /* shift including the NUL terminator */
         out[0] = TOOL_RESULT_ERROR_MARK[0];
      }
   }
   return out;
}

/* ========== Lifecycle ========== */

static bool suit_tool_is_available(void) {
   return s_config.enabled;
}

static int suit_tool_init(void) {
   suit_service_cfg_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.enabled = s_config.enabled;
   snprintf(cfg.helmet_topic, sizeof(cfg.helmet_topic), "%s", s_config.helmet_topic);
   snprintf(cfg.armor_topic, sizeof(cfg.armor_topic), "%s", s_config.armor_topic);
   cfg.stale_after_sec = s_config.stale_after_sec;
   return suit_service_init(&cfg) == SUCCESS ? SUCCESS : FAILURE;
}

static void suit_tool_cleanup(void) {
   suit_service_shutdown();
}

int suit_tool_register(void) {
   return tool_registry_register(&suit_metadata);
}
