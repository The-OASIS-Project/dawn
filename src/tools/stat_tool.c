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
 * STAT system-telemetry tool ("system_status") - LLM descriptor + callback.
 * Live actions read the stat_service cache; history/trend query stat_db.
 * See include/tools/stat_tool.h.  Config template: homeassistant_tool.c.
 */

#include "tools/stat_tool.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "core/stat_db.h"
#include "core/stat_service.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/toml.h"
#include "tools/tool_registry.h"

/* Heap buffer for a `trend` series result.  Worst case ~49 points × (label +
 * three numeric arrays) ≈ 2.5 KB; 8 KB leaves ample headroom so the Chart.js-
 * ready arrays are never truncated (a truncation would hand malformed JSON to
 * the LLM). */
#define STAT_SERIES_TEXT_MAX 8192

#define STAT_SECS_PER_DAY (24 * 3600)

/* Which stored value an emitted series array carries. */
typedef enum {
   SERIES_FIELD_AVG,
   SERIES_FIELD_MIN,
   SERIES_FIELD_MAX
} series_field_t;

/* ========== Forward declarations ========== */

static char *stat_tool_callback(const char *action, char *value, int *should_respond);
static bool stat_tool_is_available(void);
static int stat_tool_init(void);
static void stat_tool_cleanup(void);
static void stat_parse_config(toml_table_t *table, void *config);
static void stat_write_config(void *fp, const void *config);

/* ========== Tool-owned config ========== */

typedef struct {
   bool enabled;
   char telemetry_topic[128];
   char status_topic[128];
   char db_path[256];
   int stale_after_sec;
   int history_retention_days;
} stat_tool_config_t;

static stat_tool_config_t s_config = {
   .enabled = true,
   .telemetry_topic = STAT_TELEMETRY_TOPIC_DEFAULT,
   .status_topic = STAT_STATUS_TOPIC_DEFAULT,
   .db_path = "/var/lib/dawn/stat.db",
   .stale_after_sec = 30,
   .history_retention_days = 30,
};

/* ========== Config parser / writer ========== */

static void copy_toml_str(toml_table_t *table, const char *key, char *dst, size_t dst_sz) {
   toml_datum_t v = toml_string_in(table, key);
   if (v.ok) {
      snprintf(dst, dst_sz, "%s", v.u.s);
      free(v.u.s);
   }
}

static void stat_parse_config(toml_table_t *table, void *config) {
   stat_tool_config_t *cfg = (stat_tool_config_t *)config;
   if (!table) {
      return;
   }
   toml_datum_t enabled = toml_bool_in(table, "enabled");
   if (enabled.ok) {
      cfg->enabled = enabled.u.b;
   }
   copy_toml_str(table, "telemetry_topic", cfg->telemetry_topic, sizeof(cfg->telemetry_topic));
   copy_toml_str(table, "status_topic", cfg->status_topic, sizeof(cfg->status_topic));
   copy_toml_str(table, "db_path", cfg->db_path, sizeof(cfg->db_path));
   toml_datum_t stale = toml_int_in(table, "stale_after_sec");
   if (stale.ok && stale.u.i > 0) {
      cfg->stale_after_sec = (int)stale.u.i;
   }
   toml_datum_t ret = toml_int_in(table, "history_retention_days");
   if (ret.ok && ret.u.i >= 0) {
      cfg->history_retention_days = (int)ret.u.i;
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

static void stat_write_config(void *fp, const void *config) {
   const stat_tool_config_t *cfg = (const stat_tool_config_t *)config;
   FILE *f = (FILE *)fp;
   fprintf(f, "enabled = %s\n", cfg->enabled ? "true" : "false");
   write_safe_str(f, "telemetry_topic", cfg->telemetry_topic);
   write_safe_str(f, "status_topic", cfg->status_topic);
   write_safe_str(f, "db_path", cfg->db_path);
   fprintf(f, "stale_after_sec = %d\n", cfg->stale_after_sec);
   fprintf(f, "history_retention_days = %d\n", cfg->history_retention_days);
}

/* ========== Parameters ========== */

static const treg_param_t stat_params[] = {
   {
       .name = "action",
       .description =
           "What to report: 'all' (full live status), 'temps', 'battery' "
           "(charge/power/health/time remaining), 'performance' (CPU and memory load), "
           "'history' (min/max/average and peak of a metric over a period, as prose), or "
           "'trend' (a per-interval time series of one metric — labels plus avg, and min/max "
           "for temperature and battery — suitable for charting).",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "all", "temps", "battery", "performance", "history", "trend" },
       .enum_count = 6,
   },
   {
       .name = "period",
       .description = "Time window for 'history'/'trend', e.g. 'last hour', 'overnight', 'today', "
                      "'24h', '7d'. 'history' defaults to the last hour; 'trend' defaults to the "
                      "last 24 hours.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
   {
       .name = "metric",
       .description = "Which metric to focus on. 'history' reports "
                      "temperature/battery/cpu/memory/fan (defaults to temperature + battery). "
                      "'trend' charts one metric — temperature, battery (charge %), power (watts "
                      "drawn), voltage, cpu, memory, or fan (default temperature).",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "metric",
   },
};

/* ========== Metadata ========== */

static const tool_metadata_t stat_metadata = {
   .name = "system_status",
   .device_string = "system_status",
   .topic = "dawn",
   .aliases = { "telemetry", "diagnostics" },
   .alias_count = 2,

   .description =
       "Report this host's live system telemetry and its recorded history: temperatures, battery "
       "(charge %, voltage, current, power draw, charging state, estimated time remaining, and "
       "health/faults), CPU and memory load, and cooling fan — from the on-board STAT sensor "
       "service. Use 'all' for a full status report, 'temps'/'battery'/'performance' for one "
       "area, or 'history' with a period for the min/max/average and peak of a metric over that "
       "window (e.g. overnight temperature peaks). When the user wants to SEE a graph "
       "(temperatures, power draw, load, etc.), use 'trend' to get the time series and then "
       "render it as a line chart with render_visual. Scope is THIS host (the Jetson "
       "compute unit); 'temperature' "
       "is the SoC junction temperature — CPU and GPU share one die, there is no separate GPU "
       "sensor. If STAT is offline or the data is stale, that is reported instead of guessed "
       "values.",
   .params = stat_params,
   .param_count = 3,

   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_SCHEDULABLE | TOOL_CAP_INFORMATIONAL,
   .skip_followup = false,
   .default_local = true,
   .default_remote = true,

   .config = &s_config,
   .config_size = sizeof(s_config),
   .config_parser = stat_parse_config,
   .config_writer = stat_write_config,
   .config_section = "stat",

   .is_available = stat_tool_is_available,
   .init = stat_tool_init,
   .cleanup = stat_tool_cleanup,
   .callback = stat_tool_callback,
};

/* ========== Formatting helpers ========== */

static void fmt_duration_min(double minutes, char *out, size_t out_sz) {
   /* Upper-bound the cast: a huge (finite) time_remaining_min from a misbehaving
    * publisher would make (int)(minutes+0.5) an out-of-range double->int (UB).
    * 10,000,000 min (~19 years) is well past any real battery runtime. */
   if (minutes <= 0 || minutes > 10000000.0) {
      snprintf(out, out_sz, "unknown");
      return;
   }
   int total = (int)(minutes + 0.5);
   int h = total / 60, m = total % 60;
   if (h > 0) {
      snprintf(out, out_sz, "%dh %dm", h, m);
   } else {
      snprintf(out, out_sz, "%dm", m);
   }
}

static void fmt_hhmm(int64_t ts, char *out, size_t out_sz) {
   time_t t = (time_t)ts;
   struct tm tm_buf;
   localtime_r(&t, &tm_buf);
   strftime(out, out_sz, "%H:%M", &tm_buf);
}

/* Append a battery description to buf (bounded). */
static void append_battery(const stat_snapshot_t *s, char *buf, size_t sz) {
   char remain[32];
   fmt_duration_min(s->time_remaining_min, remain, sizeof(remain));
   char health[160];
   const char *st = s->battery_status[0] ? s->battery_status : "OK";
   if (s->crit_faults > 0 || s->warn_faults > 0) {
      snprintf(health, sizeof(health), "health %s (%d critical, %d warning)%s%s", st,
               s->crit_faults, s->warn_faults, s->status_reason[0] ? ": " : "", s->status_reason);
   } else {
      snprintf(health, sizeof(health), "health %s", st);
   }
   size_t off = strlen(buf);
   snprintf(buf + off, sz - off, "Battery %.0f%% (%.1f V, %.2f A, %.1f W)%s%s, ~%s remaining, %s. ",
            s->batt_level, s->batt_voltage, s->batt_current, s->batt_power,
            s->charging_state[0] ? ", " : "", s->charging_state, remain, health);
}

static void append_temp(const stat_snapshot_t *s, char *buf, size_t sz) {
   size_t off = strlen(buf);
   snprintf(buf + off, sz - off, "Temperature: SoC %.1f°C. ", s->system_temp);
}

static void append_perf(const stat_snapshot_t *s, char *buf, size_t sz) {
   size_t off = strlen(buf);
   snprintf(buf + off, sz - off, "Load: CPU %.0f%%, memory %.0f%%. ", s->cpu_usage,
            s->memory_usage);
}

static void append_fan(const stat_snapshot_t *s, char *buf, size_t sz) {
   size_t off = strlen(buf);
   snprintf(buf + off, sz - off, "Fan %d rpm (%d%% load). ", s->fan_rpm, s->fan_load);
}

/* Build a live-status string.  Returns SUCCESS, or FAILURE with an error message
 * (offline/stale/no-data) already written into buf. */
static int format_live(const char *action, char *buf, size_t sz) {
   stat_snapshot_t s;
   stat_service_get_snapshot(&s);

   if (!s.ever_seen) {
      snprintf(buf, sz, "No telemetry received yet - STAT may not be running.");
      return FAILURE;
   }
   if (!s.stat_online) {
      snprintf(buf, sz,
               "STAT reports offline; no live system telemetry available "
               "(last data %ds ago).",
               s.age_sec);
      return FAILURE;
   }
   if (s.stale) {
      snprintf(buf, sz,
               "No fresh telemetry - STAT last reported %ds ago (stale after %ds); "
               "it may have stopped.",
               s.age_sec, s_config.stale_after_sec);
      return FAILURE;
   }

   snprintf(buf, sz, "STAT (last seen %ds ago) - ", s.age_sec);

   if (strcasecmp(action, "all") == 0) {
      if (s.have_battery) {
         append_battery(&s, buf, sz);
      }
      if (s.have_system) {
         append_temp(&s, buf, sz);
         append_perf(&s, buf, sz);
      }
      if (s.have_fan) {
         append_fan(&s, buf, sz);
      }
   } else if (strcasecmp(action, "temps") == 0) {
      if (s.have_system) {
         append_temp(&s, buf, sz);
      }
      if (s.have_fan) {
         append_fan(&s, buf, sz);
      }
   } else if (strcasecmp(action, "battery") == 0) {
      if (s.have_battery) {
         append_battery(&s, buf, sz);
      } else {
         snprintf(buf + strlen(buf), sz - strlen(buf), "no battery data.");
      }
   } else if (strcasecmp(action, "performance") == 0) {
      if (s.have_system) {
         append_perf(&s, buf, sz);
      }
      if (s.have_fan) {
         append_fan(&s, buf, sz);
      }
   }
   return SUCCESS;
}

/* ========== Period parsing (duration windows for history/trend) ========== */

/* Parse a natural window like "last hour", "overnight", "today", "24h", "7d",
 * "30 min", "3 days" into [start, end].  Defaults to the last hour. */
static void parse_period(const char *period,
                         time_t now,
                         int64_t *start,
                         int64_t *end,
                         char *label,
                         size_t label_sz) {
   *end = (int64_t)now;

   char p[64] = "";
   if (period) {
      for (size_t i = 0; i < sizeof(p) - 1 && period[i]; i++) {
         p[i] = (char)tolower((unsigned char)period[i]);
      }
   }

   if (strstr(p, "overnight") || strstr(p, "last night")) {
      struct tm tm_buf;
      time_t t = now;
      localtime_r(&t, &tm_buf);
      /* End at 08:00 today; start at 20:00 the previous day. */
      struct tm end_tm = tm_buf;
      end_tm.tm_hour = 8;
      end_tm.tm_min = 0;
      end_tm.tm_sec = 0;
      time_t night_end = mktime(&end_tm);
      if (night_end > now) {
         night_end = now;
      }
      *end = (int64_t)night_end;
      *start = (int64_t)(night_end - 12 * 3600);
      snprintf(label, label_sz, "overnight");
      return;
   }
   if (strstr(p, "today")) {
      struct tm tm_buf;
      time_t t = now;
      localtime_r(&t, &tm_buf);
      tm_buf.tm_hour = 0;
      tm_buf.tm_min = 0;
      tm_buf.tm_sec = 0;
      *start = (int64_t)mktime(&tm_buf);
      snprintf(label, label_sz, "today");
      return;
   }

   /* Leading integer + unit (e.g. "24h", "7 d", "30 min"). */
   int n = 0;
   const char *q = p;
   while (*q == ' ')
      q++;
   bool has_n = false;
   while (*q >= '0' && *q <= '9') {
      if (n < 100000) { /* clamp: avoid signed overflow on absurd input; caps ~273 yrs of days */
         n = n * 10 + (*q - '0');
      }
      q++;
      has_n = true;
   }
   while (*q == ' ')
      q++;

   if (strstr(p, "week") || (*q == 'd' && has_n && n == 7)) {
      *start = *end - 7 * 86400;
      snprintf(label, label_sz, "the last 7 days");
      return;
   }
   if (has_n && (*q == 'd' || strncmp(q, "day", 3) == 0)) {
      if (n < 1)
         n = 1;
      *start = *end - (int64_t)n * 86400;
      snprintf(label, label_sz, "the last %d day%s", n, n == 1 ? "" : "s");
      return;
   }
   if (has_n && (*q == 'h' || strncmp(q, "hour", 4) == 0)) {
      if (n < 1)
         n = 1;
      *start = *end - (int64_t)n * 3600;
      snprintf(label, label_sz, "the last %d hour%s", n, n == 1 ? "" : "s");
      return;
   }
   if (has_n && (*q == 'm' || strncmp(q, "min", 3) == 0)) {
      if (n < 1)
         n = 1;
      *start = *end - (int64_t)n * 60;
      snprintf(label, label_sz, "the last %d minute%s", n, n == 1 ? "" : "s");
      return;
   }
   if (strstr(p, "day") || strstr(p, "24")) {
      *start = *end - 86400;
      snprintf(label, label_sz, "the last 24 hours");
      return;
   }

   /* Default: last hour. */
   *start = *end - 3600;
   snprintf(label, label_sz, "the last hour");
}

/* ========== History formatting ========== */

static void format_history(const char *metric,
                           const char *label,
                           const stat_history_agg_t *agg,
                           char *buf,
                           size_t sz) {
   if (!agg->have_data) {
      snprintf(buf, sz, "No telemetry history recorded for %s.", label);
      return;
   }

   bool want_temp = !metric || !metric[0] || strcasecmp(metric, "temperature") == 0 ||
                    strcasecmp(metric, "temp") == 0;
   bool want_batt = !metric || !metric[0] || strcasecmp(metric, "battery") == 0;
   bool want_cpu = metric && strcasecmp(metric, "cpu") == 0;
   bool want_mem = metric && strcasecmp(metric, "memory") == 0;
   bool want_fan = metric && strcasecmp(metric, "fan") == 0;

   snprintf(buf, sz, "System history over %s (%d samples): ", label, agg->bucket_count);

   if (want_temp && agg->total_sys > 0) {
      char peak[16] = "";
      if (agg->temp_peak_bucket > 0) {
         fmt_hhmm(agg->temp_peak_bucket, peak, sizeof(peak));
      }
      size_t off = strlen(buf);
      snprintf(buf + off, sz - off, "SoC temperature min %.1f°C, avg %.1f°C, peak %.1f°C%s%s. ",
               agg->temp_min, agg->temp_avg, agg->temp_max, peak[0] ? " ~" : "", peak);
   }
   if (want_batt && agg->total_batt > 0) {
      char low[16] = "";
      if (agg->batt_min_bucket > 0) {
         fmt_hhmm(agg->batt_min_bucket, low, sizeof(low));
      }
      size_t off = strlen(buf);
      snprintf(buf + off, sz - off, "Battery min %.0f%%, avg %.0f%%, max %.0f%%%s%s. ",
               agg->batt_min, agg->batt_avg, agg->batt_max, low[0] ? " (low ~" : "",
               low[0] ? low : "");
      if (low[0]) {
         off = strlen(buf);
         snprintf(buf + off, sz - off, ")");
      }
   }
   if (want_cpu && agg->total_sys > 0) {
      size_t off = strlen(buf);
      snprintf(buf + off, sz - off, "CPU load avg %.0f%%, peak %.0f%%. ", agg->cpu_avg,
               agg->cpu_max);
   }
   if (want_mem && agg->total_sys > 0) {
      size_t off = strlen(buf);
      snprintf(buf + off, sz - off, "Memory avg %.0f%%, peak %.0f%%. ", agg->mem_avg, agg->mem_max);
   }
   if (want_fan && agg->total_fan > 0) {
      size_t off = strlen(buf);
      snprintf(buf + off, sz - off, "Fan avg %.0f rpm, peak %.0f rpm. ", agg->fan_avg,
               agg->fan_max);
   }
}

/* ========== Trend series (chartable) ========== */

/* Map a metric string to a chartable metric enum.  Empty/unknown → temperature
 * (a chart wants one metric; temperature is the natural default).  Kept in the
 * tool layer so only the enum — never user text — crosses into stat_db. */
static stat_metric_t parse_metric(const char *metric) {
   if (!metric || !metric[0]) {
      return STAT_METRIC_TEMP;
   }
   if (strcasecmp(metric, "temperature") == 0 || strcasecmp(metric, "temp") == 0) {
      return STAT_METRIC_TEMP;
   }
   if (strcasecmp(metric, "battery") == 0 || strcasecmp(metric, "charge") == 0) {
      return STAT_METRIC_BATTERY;
   }
   if (strcasecmp(metric, "power") == 0) {
      return STAT_METRIC_POWER;
   }
   if (strcasecmp(metric, "voltage") == 0 || strcasecmp(metric, "volts") == 0) {
      return STAT_METRIC_VOLTAGE;
   }
   if (strcasecmp(metric, "cpu") == 0) {
      return STAT_METRIC_CPU;
   }
   if (strcasecmp(metric, "memory") == 0 || strcasecmp(metric, "mem") == 0) {
      return STAT_METRIC_MEMORY;
   }
   if (strcasecmp(metric, "fan") == 0) {
      return STAT_METRIC_FAN;
   }
   return STAT_METRIC_TEMP;
}

typedef struct {
   const char *label;
   const char *unit;
   int decimals;
} stat_metric_fmt_t;

static stat_metric_fmt_t metric_fmt(stat_metric_t m) {
   switch (m) {
      case STAT_METRIC_BATTERY:
         return (stat_metric_fmt_t){ "Battery charge", "%", 0 };
      case STAT_METRIC_POWER:
         return (stat_metric_fmt_t){ "Power draw", "W", 1 };
      case STAT_METRIC_VOLTAGE:
         return (stat_metric_fmt_t){ "Battery voltage", "V", 2 };
      case STAT_METRIC_CPU:
         return (stat_metric_fmt_t){ "CPU load", "%", 0 };
      case STAT_METRIC_MEMORY:
         return (stat_metric_fmt_t){ "Memory usage", "%", 0 };
      case STAT_METRIC_FAN:
         return (stat_metric_fmt_t){ "Fan speed", "rpm", 0 };
      case STAT_METRIC_TEMP:
         return (stat_metric_fmt_t){ "SoC temperature", "°C", 1 };
   }
   /* No default: -Wswitch flags a metric added without a format entry. */
   return (stat_metric_fmt_t){ "SoC temperature", "°C", 1 };
}

/* Point-time label: HH:MM within a day, "MM-DD HH:MM" for multi-day windows so
 * points stay distinct. */
static void fmt_label(int64_t ts, bool multiday, char *out, size_t out_sz) {
   time_t t = (time_t)ts;
   struct tm tm_buf;
   localtime_r(&t, &tm_buf);
   strftime(out, out_sz, multiday ? "%m-%d %H:%M" : "%H:%M", &tm_buf);
}

/* Emit one `"key":[…]` numeric array; missing points become `null` (a gap the
 * chart spans, never a false 0).  Returns bytes written (clamped to sz). */
static size_t append_series_array(char *buf,
                                  size_t sz,
                                  const char *key,
                                  const stat_series_t *ser,
                                  series_field_t field,
                                  int decimals) {
   size_t off = (size_t)snprintf(buf, sz, "\"%s\":[", key);
   for (int i = 0; i < ser->count && off < sz; i++) {
      const stat_series_point_t *p = &ser->points[i];
      bool have = (field == SERIES_FIELD_AVG)   ? p->have_avg
                  : (field == SERIES_FIELD_MIN) ? p->have_min
                                                : p->have_max;
      double v = (field == SERIES_FIELD_AVG)   ? p->avg
                 : (field == SERIES_FIELD_MIN) ? p->min
                                               : p->max;
      const char *sep = i ? "," : "";
      if (have) {
         off += (size_t)snprintf(buf + off, sz - off, "%s%.*f", sep, decimals, v);
      } else {
         off += (size_t)snprintf(buf + off, sz - off, "%snull", sep);
      }
   }
   if (off < sz) {
      off += (size_t)snprintf(buf + off, sz - off, "]");
   }
   return off;
}

/* Format a per-interval series as copy-exact Chart.js-ready JSON fragments: a
 * voice-friendly prose header + affordance line, then labels/avg[/min/max]
 * arrays the LLM can drop almost verbatim into render_visual. */
static void format_series(stat_metric_t metric,
                          const char *label,
                          const stat_series_t *ser,
                          char *buf,
                          size_t sz) {
   stat_metric_fmt_t mf = metric_fmt(metric);
   bool band = ser->has_min && ser->has_max;
   int64_t span = ser->covered_end - ser->covered_start;
   bool multiday = span > STAT_SECS_PER_DAY;

   size_t off = (size_t)snprintf(
       buf, sz,
       "%s over %s — %d points, %s. To chart, call "
       "render_visual_load_guidelines(modules=\"chart\") then render_visual (type html, LINE "
       "chart, plot avg%s, spanGaps:true, category x-axis of the labels, no time adapter).\n",
       mf.label, label, ser->count, mf.unit,
       band ? ", shade min–max as a band (second/third dataset, fill:'-1')" : "");

   if (off < sz) {
      off += (size_t)snprintf(buf + off, sz - off, "\"labels\":[");
      for (int i = 0; i < ser->count && off < sz; i++) {
         char lab[24];
         fmt_label(ser->points[i].t, multiday, lab, sizeof(lab));
         off += (size_t)snprintf(buf + off, sz - off, "%s\"%s\"", i ? "," : "", lab);
      }
      if (off < sz) {
         off += (size_t)snprintf(buf + off, sz - off, "],\n");
      }
   }

   if (off < sz) {
      off += append_series_array(buf + off, sz - off, "avg", ser, SERIES_FIELD_AVG, mf.decimals);
   }
   if (ser->has_min && off < sz) {
      off += (size_t)snprintf(buf + off, sz - off, ",\n");
      if (off < sz) {
         off += append_series_array(buf + off, sz - off, "min", ser, SERIES_FIELD_MIN, mf.decimals);
      }
   }
   if (ser->has_max && off < sz) {
      off += (size_t)snprintf(buf + off, sz - off, ",\n");
      if (off < sz) {
         off += append_series_array(buf + off, sz - off, "max", ser, SERIES_FIELD_MAX, mf.decimals);
      }
   }
}

/* ========== Callback ========== */

static char *stat_tool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1;
   if (!action) {
      return strdup(TOOL_RESULT_ERROR_MARK "system_status: missing action.");
   }

   char *out = malloc(1024);
   if (!out) {
      return strdup(TOOL_RESULT_ERROR_MARK "system_status: out of memory.");
   }
   out[0] = '\0';

   if (strcasecmp(action, "history") == 0) {
      if (!stat_db_is_ready()) {
         free(out);
         return strdup(TOOL_RESULT_ERROR_MARK
                       "No telemetry history available (history store off).");
      }
      char period[64] = "", metric[32] = "";
      if (value) {
         tool_param_extract_base(value, period, sizeof(period));
         tool_param_extract_custom(value, "metric", metric, sizeof(metric));
      }
      int64_t start = 0, end = 0;
      char label[48] = "";
      parse_period(period, time(NULL), &start, &end, label, sizeof(label));

      stat_history_agg_t agg;
      if (stat_db_history(start, end, &agg) != SUCCESS) {
         free(out);
         return strdup(TOOL_RESULT_ERROR_MARK "Failed to query telemetry history.");
      }
      format_history(metric, label, &agg, out, 1024);
      return out;
   }

   if (strcasecmp(action, "trend") == 0) {
      free(out); /* the series needs its own, larger buffer */
      out = NULL;
      if (!stat_db_is_ready()) {
         return strdup(TOOL_RESULT_ERROR_MARK
                       "No telemetry history available (history store off).");
      }
      char period[64] = "", metric[32] = "";
      if (value) {
         tool_param_extract_base(value, period, sizeof(period));
         tool_param_extract_custom(value, "metric", metric, sizeof(metric));
      }
      int64_t start = 0, end = 0;
      char label[48] = "";
      /* A chart wants a rich window: default trend to 24h, NOT parse_period's
       * last-hour default (~4 points at 15-min buckets). */
      parse_period(period[0] ? period : "24h", time(NULL), &start, &end, label, sizeof(label));

      stat_metric_t m = parse_metric(metric);
      stat_series_t ser;
      if (stat_db_series(start, end, m, &ser) != SUCCESS) {
         return strdup(TOOL_RESULT_ERROR_MARK "Failed to query telemetry series.");
      }
      if (ser.count == 0) {
         char msg[160];
         snprintf(msg, sizeof(msg), TOOL_RESULT_ERROR_MARK "No %s history recorded for %s.",
                  metric_fmt(m).label, label);
         return strdup(msg);
      }
      char *sbuf = malloc(STAT_SERIES_TEXT_MAX);
      if (!sbuf) {
         return strdup(TOOL_RESULT_ERROR_MARK "system_status: out of memory.");
      }
      sbuf[0] = '\0';
      format_series(m, label, &ser, sbuf, STAT_SERIES_TEXT_MAX);
      return sbuf;
   }

   /* Live actions. */
   if (format_live(action, out, 1024) != SUCCESS) {
      /* Prefix the error mark IN PLACE so the failure is surfaced (not spoken as
       * data) — avoids a second allocation whose OOM would drop the mark. */
      size_t len = strlen(out);
      if (len + 2 <= 1024) {
         memmove(out + 1, out, len + 1); /* shift including the NUL terminator */
         out[0] = TOOL_RESULT_ERROR_MARK[0];
      }
   }
   return out;
}

/* ========== Lifecycle ========== */

static bool stat_tool_is_available(void) {
   return s_config.enabled;
}

static int stat_tool_init(void) {
   stat_service_cfg_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.enabled = s_config.enabled;
   snprintf(cfg.telemetry_topic, sizeof(cfg.telemetry_topic), "%s", s_config.telemetry_topic);
   snprintf(cfg.status_topic, sizeof(cfg.status_topic), "%s", s_config.status_topic);
   snprintf(cfg.db_path, sizeof(cfg.db_path), "%s", s_config.db_path);
   cfg.stale_after_sec = s_config.stale_after_sec;
   cfg.history_retention_days = s_config.history_retention_days;
   return stat_service_init(&cfg) == SUCCESS ? SUCCESS : FAILURE;
}

static void stat_tool_cleanup(void) {
   stat_service_shutdown();
}

int stat_tool_register(void) {
   return tool_registry_register(&stat_metadata);
}
