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
 * SAGE metric catalog — the natural-language <-> concrete-metric bridge.  Each
 * entry names a watchable signal, how to read it from a telemetry snapshot, its
 * sensible default rule shape, and human label/unit.  This is the SINGLE source
 * of truth for the watchable vocabulary: the `attention` tool generates its
 * metric-param description from it at registration (no second hardcoded list),
 * and the WebUI dropdown + "what can you watch?" read it too.  Readers touch only
 * the sampled ctx struct (no service
 * calls), so a compiled-out STAT/suit service leaves its snapshot zeroed and the
 * reader simply reports "not present".
 */

#include <string.h>
#include <strings.h>

#include "core/attention/attention_internal.h"

/* --- readers: fill *value, return true if the value is present --- */

static bool read_battery_soc(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->stat_valid || !ctx->stat.have_battery) {
      return false;
   }
   *value = ctx->stat.batt_level;
   return true;
}

static bool read_system_temp(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->stat_valid || !ctx->stat.have_system) {
      return false;
   }
   *value = ctx->stat.system_temp;
   return true;
}

static bool read_cpu_usage(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->stat_valid || !ctx->stat.have_system) {
      return false;
   }
   *value = ctx->stat.cpu_usage;
   return true;
}

static bool read_memory_usage(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->stat_valid || !ctx->stat.have_system) {
      return false;
   }
   *value = ctx->stat.memory_usage;
   return true;
}

static bool read_batt_temp(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->stat_valid || !ctx->stat.have_battery) {
      return false;
   }
   *value = ctx->stat.batt_temp;
   return true;
}

static bool read_batt_voltage(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->stat_valid || !ctx->stat.have_battery) {
      return false;
   }
   *value = ctx->stat.batt_voltage;
   return true;
}

static bool read_batt_current(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->stat_valid || !ctx->stat.have_battery) {
      return false;
   }
   *value = ctx->stat.batt_current;
   return true;
}

static bool read_batt_power(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->stat_valid || !ctx->stat.have_battery) {
      return false;
   }
   *value = ctx->stat.batt_power;
   return true;
}

static bool read_time_remaining(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->stat_valid || !ctx->stat.have_battery) {
      return false;
   }
   *value = ctx->stat.time_remaining_min;
   return true;
}

static bool read_crit_faults(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->stat_valid || !ctx->stat.ever_seen) {
      return false;
   }
   *value = (double)ctx->stat.crit_faults;
   return true;
}

static bool read_warn_faults(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->stat_valid || !ctx->stat.ever_seen) {
      return false;
   }
   *value = (double)ctx->stat.warn_faults;
   return true;
}

static bool read_fan_rpm(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->stat_valid || !ctx->stat.have_fan) {
      return false;
   }
   *value = (double)ctx->stat.fan_rpm;
   return true;
}

static bool read_co2(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->suit_valid || !ctx->suit.have_enviro) {
      return false;
   }
   *value = ctx->suit.co2_ppm;
   return true;
}

static bool read_suit_temp(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->suit_valid || !ctx->suit.have_enviro) {
      return false;
   }
   *value = ctx->suit.temp;
   return true;
}

static bool read_suit_humidity(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->suit_valid || !ctx->suit.have_enviro) {
      return false;
   }
   *value = ctx->suit.humidity;
   return true;
}

static bool read_air_quality(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->suit_valid || !ctx->suit.have_enviro) {
      return false;
   }
   *value = ctx->suit.air_quality;
   return true;
}

static bool read_eco2(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->suit_valid || !ctx->suit.have_enviro) {
      return false;
   }
   *value = ctx->suit.eco2_ppm;
   return true;
}

static bool read_tvoc(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->suit_valid || !ctx->suit.have_enviro) {
      return false;
   }
   *value = ctx->suit.tvoc_ppb;
   return true;
}

static bool read_heat_index(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->suit_valid || !ctx->suit.have_enviro) {
      return false;
   }
   *value = ctx->suit.heat_index_c;
   return true;
}

static bool read_dew_point(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->suit_valid || !ctx->suit.have_enviro) {
      return false;
   }
   *value = ctx->suit.dew_point;
   return true;
}

/* Lowest voltage across armor pieces that report one (a watch on this fires when
 * ANY piece drops low).  Piece-naming in the summary is a P1 refinement. */
static bool read_armor_voltage_min(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->suit_valid || ctx->suit.armor_count <= 0) {
      return false;
   }
   bool any = false;
   double lo = 0.0;
   for (int i = 0; i < ctx->suit.armor_count && i < SUIT_MAX_ARMOR_PIECES; i++) {
      if (!ctx->suit.armor[i].have_voltage) {
         continue;
      }
      double v = ctx->suit.armor[i].voltage;
      if (!any || v < lo) {
         lo = v;
         any = true;
      }
   }
   if (!any) {
      return false;
   }
   *value = lo;
   return true;
}

/* Highest temperature across armor pieces that report one (fires when ANY piece
 * runs hot). */
static bool read_armor_temp_max(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->suit_valid || ctx->suit.armor_count <= 0) {
      return false;
   }
   bool any = false;
   double hi = 0.0;
   for (int i = 0; i < ctx->suit.armor_count && i < SUIT_MAX_ARMOR_PIECES; i++) {
      if (!ctx->suit.armor[i].have_temp) {
         continue;
      }
      double t = ctx->suit.armor[i].temp;
      if (!any || t > hi) {
         hi = t;
         any = true;
      }
   }
   if (!any) {
      return false;
   }
   *value = hi;
   return true;
}

/* Absence readers return the feed AGE in seconds; "present" means the source has
 * a known liveness (ever seen), so the absence gate can compare age vs threshold. */
static bool read_hud_age(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->hud_ever) {
      return false;
   }
   *value = (double)ctx->hud_age_sec;
   return true;
}

static bool read_enviro_age(const attention_sample_ctx_t *ctx, double *value) {
   if (!ctx->suit_valid || !ctx->suit.ever_seen) {
      return false;
   }
   *value = (double)ctx->suit.enviro_age_sec;
   return true;
}

/* --- the catalog --- */
/* Fields: key, source, label, unit, default_rule_type, default_direction,
 * default_threshold, default_hysteresis, default_absence_after_sec,
 * default_notify, reader.  Keep the key set in sync with the `attention` tool's
 * `metric` enum (src/tools/attention_tool.c). */
#define DEG_C \
   "\xC2\xB0" \
   "C"
static const attention_catalog_entry_t s_catalog[] = {
   { "stat.battery.soc", "stat", "battery level", "%", SAGE_RULE_THRESHOLD, SAGE_DIR_BELOW, 15.0,
     5.0, 0, SAGE_NOTIFY_ALERT, read_battery_soc },
   { "stat.system_temp", "stat", "system temperature", DEG_C, SAGE_RULE_THRESHOLD, SAGE_DIR_ABOVE,
     80.0, 5.0, 0, SAGE_NOTIFY_ALERT, read_system_temp },
   { "stat.cpu_usage", "stat", "CPU usage", "%", SAGE_RULE_THRESHOLD, SAGE_DIR_ABOVE, 90.0, 5.0, 0,
     SAGE_NOTIFY_AMBIENT, read_cpu_usage },
   { "stat.memory_usage", "stat", "memory usage", "%", SAGE_RULE_THRESHOLD, SAGE_DIR_ABOVE, 90.0,
     5.0, 0, SAGE_NOTIFY_AMBIENT, read_memory_usage },
   { "stat.batt_temp", "stat", "battery temperature", DEG_C, SAGE_RULE_THRESHOLD, SAGE_DIR_ABOVE,
     45.0, 5.0, 0, SAGE_NOTIFY_ALERT, read_batt_temp },
   { "stat.batt_voltage", "stat", "battery voltage", "V", SAGE_RULE_THRESHOLD, SAGE_DIR_BELOW, 10.0,
     0.2, 0, SAGE_NOTIFY_AMBIENT, read_batt_voltage },
   { "stat.batt_current", "stat", "battery current", "A", SAGE_RULE_THRESHOLD, SAGE_DIR_ABOVE, 15.0,
     1.0, 0, SAGE_NOTIFY_AMBIENT, read_batt_current },
   { "stat.batt_power", "stat", "power draw", "W", SAGE_RULE_THRESHOLD, SAGE_DIR_ABOVE, 150.0, 10.0,
     0, SAGE_NOTIFY_AMBIENT, read_batt_power },
   { "stat.time_remaining_min", "stat", "battery runtime", "min", SAGE_RULE_THRESHOLD,
     SAGE_DIR_BELOW, 15.0, 5.0, 0, SAGE_NOTIFY_ALERT, read_time_remaining },
   { "stat.crit_faults", "stat", "critical faults", "", SAGE_RULE_THRESHOLD, SAGE_DIR_ABOVE, 0.0,
     0.0, 0, SAGE_NOTIFY_ALERT, read_crit_faults },
   { "stat.warn_faults", "stat", "warning faults", "", SAGE_RULE_THRESHOLD, SAGE_DIR_ABOVE, 0.0,
     0.0, 0, SAGE_NOTIFY_AMBIENT, read_warn_faults },
   { "stat.fan_rpm", "stat", "fan speed", "rpm", SAGE_RULE_THRESHOLD, SAGE_DIR_ABOVE, 5000.0, 200.0,
     0, SAGE_NOTIFY_AMBIENT, read_fan_rpm },
   { "suit.co2_ppm", "suit", "CO2", "ppm", SAGE_RULE_THRESHOLD, SAGE_DIR_ABOVE, 1500.0, 200.0, 0,
     SAGE_NOTIFY_ALERT, read_co2 },
   { "suit.temp", "suit", "helmet temperature", DEG_C, SAGE_RULE_THRESHOLD, SAGE_DIR_ABOVE, 35.0,
     2.0, 0, SAGE_NOTIFY_AMBIENT, read_suit_temp },
   { "suit.humidity", "suit", "helmet humidity", "%", SAGE_RULE_THRESHOLD, SAGE_DIR_ABOVE, 85.0,
     5.0, 0, SAGE_NOTIFY_AMBIENT, read_suit_humidity },
   { "suit.air_quality", "suit", "air quality", "", SAGE_RULE_THRESHOLD, SAGE_DIR_BELOW, 60.0, 5.0,
     0, SAGE_NOTIFY_AMBIENT, read_air_quality },
   { "suit.eco2_ppm", "suit", "eCO2", "ppm", SAGE_RULE_THRESHOLD, SAGE_DIR_ABOVE, 1500.0, 200.0, 0,
     SAGE_NOTIFY_AMBIENT, read_eco2 },
   { "suit.tvoc_ppb", "suit", "VOCs", "ppb", SAGE_RULE_THRESHOLD, SAGE_DIR_ABOVE, 1000.0, 100.0, 0,
     SAGE_NOTIFY_AMBIENT, read_tvoc },
   { "suit.heat_index_c", "suit", "heat index", DEG_C, SAGE_RULE_THRESHOLD, SAGE_DIR_ABOVE, 39.0,
     2.0, 0, SAGE_NOTIFY_ALERT, read_heat_index },
   { "suit.dew_point", "suit", "dew point", DEG_C, SAGE_RULE_THRESHOLD, SAGE_DIR_ABOVE, 24.0, 2.0,
     0, SAGE_NOTIFY_AMBIENT, read_dew_point },
   { "suit.armor.voltage", "suit", "armor voltage", "V", SAGE_RULE_THRESHOLD, SAGE_DIR_BELOW, 3.3,
     0.2, 0, SAGE_NOTIFY_AMBIENT, read_armor_voltage_min },
   { "suit.armor.temp", "suit", "armor temperature", DEG_C, SAGE_RULE_THRESHOLD, SAGE_DIR_ABOVE,
     60.0, 3.0, 0, SAGE_NOTIFY_AMBIENT, read_armor_temp_max },
   { "component.hud", "component", "helmet HUD link", "s", SAGE_RULE_ABSENCE, SAGE_DIR_ABOVE, 0.0,
     0.0, 60, SAGE_NOTIFY_AMBIENT, read_hud_age },
   { "suit.enviro", "suit", "suit telemetry", "s", SAGE_RULE_ABSENCE, SAGE_DIR_ABOVE, 0.0, 0.0, 120,
     SAGE_NOTIFY_AMBIENT, read_enviro_age },
};

static const int s_catalog_count = (int)(sizeof(s_catalog) / sizeof(s_catalog[0]));

const attention_catalog_entry_t *attention_catalog_at(int i) {
   if (i < 0 || i >= s_catalog_count) {
      return NULL;
   }
   return &s_catalog[i];
}

const attention_catalog_entry_t *attention_catalog_lookup(const char *key) {
   if (!key) {
      return NULL;
   }
   for (int i = 0; i < s_catalog_count; i++) {
      if (strcmp(s_catalog[i].key, key) == 0) {
         return &s_catalog[i];
      }
   }
   return NULL;
}

/* --- public catalog accessors (attention.h) --- */

int attention_catalog_count(void) {
   return s_catalog_count;
}

const char *attention_catalog_key(int i) {
   const attention_catalog_entry_t *e = attention_catalog_at(i);
   return e ? e->key : NULL;
}

const char *attention_catalog_label(const char *key) {
   const attention_catalog_entry_t *e = attention_catalog_lookup(key);
   return e ? e->label : NULL;
}

const char *attention_catalog_unit(const char *key) {
   const attention_catalog_entry_t *e = attention_catalog_lookup(key);
   return e ? e->unit : "";
}

bool attention_catalog_has(const char *key) {
   return attention_catalog_lookup(key) != NULL;
}

const char *attention_catalog_rule_type(const char *key) {
   const attention_catalog_entry_t *e = attention_catalog_lookup(key);
   return e ? sage_rule_type_to_str(e->default_rule_type) : NULL;
}

const char *attention_catalog_default_direction(const char *key) {
   const attention_catalog_entry_t *e = attention_catalog_lookup(key);
   return e ? sage_direction_to_str(e->default_direction) : NULL;
}

double attention_catalog_default_threshold(const char *key) {
   const attention_catalog_entry_t *e = attention_catalog_lookup(key);
   return e ? e->default_threshold : 0.0;
}

/* =============================================================================
 * Enum <-> wire-string serialization (canonical; shared by the `attention` tool
 * and the WebUI Watches panel — see the contract note in attention.h).
 * ============================================================================= */

const char *sage_notify_to_str(sage_notify_t n) {
   switch (n) {
      case SAGE_NOTIFY_ALERT:
         return "alert";
      case SAGE_NOTIFY_AMBIENT:
         return "ambient";
      case SAGE_NOTIFY_DIGEST:
      default:
         return "digest";
   }
}

sage_notify_t sage_notify_from_str(const char *s, sage_notify_t fallback) {
   if (!s || !s[0]) {
      return fallback;
   }
   if (strcasecmp(s, "alert") == 0) {
      return SAGE_NOTIFY_ALERT;
   }
   if (strcasecmp(s, "ambient") == 0) {
      return SAGE_NOTIFY_AMBIENT;
   }
   if (strcasecmp(s, "digest") == 0) {
      return SAGE_NOTIFY_DIGEST;
   }
   return fallback;
}

const char *sage_direction_to_str(sage_direction_t d) {
   switch (d) {
      case SAGE_DIR_BELOW:
         return "below";
      case SAGE_DIR_RISING:
         return "rising";
      case SAGE_DIR_FALLING:
         return "falling";
      case SAGE_DIR_ABOVE:
      default:
         return "above";
   }
}

sage_direction_t sage_direction_from_str(const char *s, sage_direction_t fallback) {
   if (!s || !s[0]) {
      return fallback;
   }
   if (strcasecmp(s, "below") == 0) {
      return SAGE_DIR_BELOW;
   }
   if (strcasecmp(s, "above") == 0) {
      return SAGE_DIR_ABOVE;
   }
   if (strcasecmp(s, "rising") == 0) {
      return SAGE_DIR_RISING;
   }
   if (strcasecmp(s, "falling") == 0) {
      return SAGE_DIR_FALLING;
   }
   return fallback;
}

const char *sage_rule_type_to_str(sage_rule_type_t r) {
   switch (r) {
      case SAGE_RULE_SLOPE:
         return "slope";
      case SAGE_RULE_ABSENCE:
         return "absence";
      case SAGE_RULE_MATCH:
         return "match";
      case SAGE_RULE_THRESHOLD:
      default:
         return "threshold";
   }
}

int attention_clamp_seconds(double v) {
   if (v < 0.0) {
      return 0;
   }
   if (v > 604800.0) {
      return 604800; /* a week — far above any real feed-silence window */
   }
   return (int)v;
}
