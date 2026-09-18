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
 * Unit tests for the STAT telemetry service: ingest → accumulator fold →
 * history flush → SQL aggregation round-trip, plus staleness and topic gating.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "core/stat_db.h"
#include "core/stat_net_interpret.h"
#include "core/stat_service.h"
#include "tools/stat_render.h"
#include "unity.h"

#define TEST_DB "/tmp/dawn_stat_test.db"

static void unlink_db(void) {
   unlink(TEST_DB);
   unlink(TEST_DB "-wal");
   unlink(TEST_DB "-shm");
}

void setUp(void) {
   unlink_db();
   stat_service_cfg_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.enabled = true;
   snprintf(cfg.db_path, sizeof(cfg.db_path), "%s", TEST_DB);
   cfg.stale_after_sec = 30;
   cfg.history_retention_days = 30;
   TEST_ASSERT_EQUAL_INT(0, stat_service_init(&cfg));
}

void tearDown(void) {
   stat_service_shutdown();
   unlink_db();
}

static void feed_system(double cpu, double mem, double temp) {
   char msg[256];
   snprintf(msg, sizeof(msg),
            "{\"device\":\"stat\",\"msg_type\":\"telemetry\",\"type\":\"SystemMetrics\","
            "\"cpu_usage\":%.2f,\"memory_usage\":%.2f,\"system_temp\":%.2f}",
            cpu, mem, temp);
   int consumed = stat_service_handle_mqtt("stat/telemetry", msg, (int)strlen(msg));
   TEST_ASSERT_EQUAL_INT(1, consumed);
}

static void feed_battery(double level, double voltage, double power, double temp) {
   char msg[384];
   snprintf(msg, sizeof(msg),
            "{\"device\":\"stat\",\"msg_type\":\"telemetry\",\"type\":\"BatteryStatus\","
            "\"voltage\":%.2f,\"current\":1.20,\"power\":%.2f,\"battery_level\":%.2f,"
            "\"temperature\":%.2f,\"charging_state\":\"discharging\","
            "\"battery_status\":\"OK\",\"time_remaining_min\":200.0,"
            "\"critical_fault_count\":0,\"warning_fault_count\":0,\"info_fault_count\":0}",
            voltage, power, level, temp);
   int consumed = stat_service_handle_mqtt("stat/telemetry", msg, (int)strlen(msg));
   TEST_ASSERT_EQUAL_INT(1, consumed);
}

static void feed_fan(int rpm) {
   char msg[192];
   snprintf(msg, sizeof(msg),
            "{\"device\":\"stat\",\"msg_type\":\"telemetry\",\"type\":\"Fan\","
            "\"rpm\":%d,\"load\":30,\"pwm\":100}",
            rpm);
   int consumed = stat_service_handle_mqtt("stat/telemetry", msg, (int)strlen(msg));
   TEST_ASSERT_EQUAL_INT(1, consumed);
}

/* Foreign topics must not be consumed (return 0 → normal dispatch continues). */
void test_topic_gating(void) {
   int consumed = stat_service_handle_mqtt("echo/events", "{}", 2);
   TEST_ASSERT_EQUAL_INT(0, consumed);
}

/* Before any message: no data, stale. */
void test_snapshot_never_seen(void) {
   stat_snapshot_t s;
   stat_service_get_snapshot(&s);
   TEST_ASSERT_FALSE(s.ever_seen);
   TEST_ASSERT_TRUE(s.stale);
   TEST_ASSERT_FALSE(s.have_system);
}

/* Live cache reflects the latest values of each family. */
void test_live_cache_latest(void) {
   feed_system(10.0, 40.0, 45.0);
   feed_system(20.0, 42.0, 50.0);
   feed_battery(80.0, 14.6, 17.5, 30.0);
   feed_fan(2000);

   stat_snapshot_t s;
   stat_service_get_snapshot(&s);
   TEST_ASSERT_TRUE(s.ever_seen);
   TEST_ASSERT_TRUE(s.stat_online);
   TEST_ASSERT_FALSE(s.stale);
   TEST_ASSERT_TRUE(s.have_system);
   TEST_ASSERT_EQUAL_DOUBLE(20.0, s.cpu_usage); /* latest, not first */
   TEST_ASSERT_EQUAL_DOUBLE(50.0, s.system_temp);
   TEST_ASSERT_TRUE(s.have_battery);
   TEST_ASSERT_EQUAL_DOUBLE(80.0, s.batt_level);
   TEST_ASSERT_EQUAL_STRING("discharging", s.charging_state);
   TEST_ASSERT_TRUE(s.have_fan);
   TEST_ASSERT_EQUAL_INT(2000, s.fan_rpm);
}

/* Accumulator fold + SQL aggregation: exact min/max, weighted avg, counts. */
void test_history_aggregation(void) {
   feed_system(10.0, 40.0, 45.0);
   feed_system(20.0, 42.0, 50.0); /* peak temp */
   feed_system(30.0, 44.0, 40.0); /* min temp */
   feed_battery(80.0, 14.6, 17.5, 30.0);
   feed_battery(78.0, 14.5, 18.0, 31.0); /* min level */
   feed_fan(2000);

   TEST_ASSERT_EQUAL_INT(0 /*SUCCESS*/, stat_history_flush());

   stat_history_agg_t agg;
   TEST_ASSERT_EQUAL_INT(0, stat_db_history(0, (int64_t)time(NULL) + 1000, &agg));
   TEST_ASSERT_TRUE(agg.have_data);
   TEST_ASSERT_EQUAL_INT(1, agg.bucket_count);
   TEST_ASSERT_EQUAL_INT(3, agg.total_sys);
   TEST_ASSERT_EQUAL_INT(2, agg.total_batt);
   TEST_ASSERT_EQUAL_INT(1, agg.total_fan);

   /* Temperature: min 40, max 50, avg (45+50+40)/3 = 45 */
   TEST_ASSERT_EQUAL_DOUBLE(40.0, agg.temp_min);
   TEST_ASSERT_EQUAL_DOUBLE(50.0, agg.temp_max);
   TEST_ASSERT_DOUBLE_WITHIN(0.01, 45.0, agg.temp_avg);
   /* CPU: avg (10+20+30)/3 = 20, peak 30 */
   TEST_ASSERT_DOUBLE_WITHIN(0.01, 20.0, agg.cpu_avg);
   TEST_ASSERT_EQUAL_DOUBLE(30.0, agg.cpu_max);
   /* Battery: min 78, max 80, avg 79 */
   TEST_ASSERT_EQUAL_DOUBLE(78.0, agg.batt_min);
   TEST_ASSERT_EQUAL_DOUBLE(80.0, agg.batt_max);
   TEST_ASSERT_DOUBLE_WITHIN(0.01, 79.0, agg.batt_avg);
   /* Fan */
   TEST_ASSERT_EQUAL_DOUBLE(2000.0, agg.fan_max);
}

/* An empty bucket (no samples since last flush) must NOT write a row. */
void test_empty_bucket_skipped(void) {
   feed_system(10.0, 40.0, 45.0);
   TEST_ASSERT_EQUAL_INT(0, stat_history_flush()); /* writes 1 bucket */
   TEST_ASSERT_EQUAL_INT(0, stat_history_flush()); /* no samples → no row */

   stat_history_agg_t agg;
   TEST_ASSERT_EQUAL_INT(0, stat_db_history(0, (int64_t)time(NULL) + 1000, &agg));
   TEST_ASSERT_EQUAL_INT(1, agg.bucket_count); /* still just the one */
}

/* Two populated buckets aggregate together across the window. */
void test_multi_bucket(void) {
   feed_system(10.0, 40.0, 60.0);
   TEST_ASSERT_EQUAL_INT(0, stat_history_flush());
   feed_system(20.0, 40.0, 70.0); /* higher peak in second bucket */
   TEST_ASSERT_EQUAL_INT(0, stat_history_flush());

   stat_history_agg_t agg;
   TEST_ASSERT_EQUAL_INT(0, stat_db_history(0, (int64_t)time(NULL) + 1000, &agg));
   TEST_ASSERT_EQUAL_INT(2, agg.bucket_count);
   TEST_ASSERT_EQUAL_INT(2, agg.total_sys);
   TEST_ASSERT_EQUAL_DOUBLE(70.0, agg.temp_max);
   TEST_ASSERT_EQUAL_DOUBLE(60.0, agg.temp_min);
}

/* ===== stat_db_series() — the chartable trend reader ===== */

/* Insert a SystemMetrics-family bucket at an explicit time (weight = count). */
static void insert_sys_bucket(int64_t t,
                              double temp_avg,
                              double temp_min,
                              double temp_max,
                              double cpu,
                              int count) {
   stat_bucket_row_t r;
   memset(&r, 0, sizeof(r));
   r.bucket_start = t;
   r.have_sys = true;
   r.sys_count = count;
   r.temp_avg = temp_avg;
   r.temp_min = temp_min;
   r.temp_max = temp_max;
   r.cpu_avg = cpu;
   r.cpu_max = cpu;
   r.mem_avg = 40.0;
   r.mem_max = 40.0;
   TEST_ASSERT_EQUAL_INT(0, stat_db_insert_bucket(&r));
}

/* Insert a BatteryStatus-family bucket at an explicit time. */
static void insert_batt_bucket(int64_t t, double level, double power) {
   stat_bucket_row_t r;
   memset(&r, 0, sizeof(r));
   r.bucket_start = t;
   r.have_batt = true;
   r.batt_count = 1;
   r.batt_avg = level;
   r.batt_min = level;
   r.batt_max = level;
   r.batt_v_avg = 14.5;
   r.batt_p_avg = power;
   r.batt_temp_max = 30.0;
   TEST_ASSERT_EQUAL_INT(0, stat_db_insert_bucket(&r));
}

/* Short window (4 native buckets) → no grouping. */
void test_series_short_window(void) {
   for (int i = 0; i < 4; i++) {
      insert_sys_bucket(1000 + i * 900, 45.0, 45.0, 45.0, 10.0, 1);
   }
   stat_series_t s;
   TEST_ASSERT_EQUAL_INT(0, stat_db_series(1000, 1000 + 3 * 900, STAT_METRIC_TEMP, &s));
   TEST_ASSERT_EQUAL_INT(4, s.count);
   TEST_ASSERT_EQUAL_INT(900, s.group_secs);
   TEST_ASSERT_TRUE(s.has_min);
   TEST_ASSERT_TRUE(s.has_max);
   TEST_ASSERT_TRUE(s.points[0].have_avg);
   TEST_ASSERT_TRUE(s.points[0].have_min);
}

/* H1: a non-aligned span ("today" mid-afternoon, 47,220s) must not blow the cap.
 * 53 native buckets → ceil(47220/43200)=2 → group_secs 1800 → 27 groups. */
void test_series_span_47220_cap(void) {
   int64_t base = 100000;
   for (int i = 0; i < 53; i++) {
      insert_sys_bucket(base + i * 900, 45.0, 45.0, 45.0, 10.0, 1);
   }
   stat_series_t s;
   TEST_ASSERT_EQUAL_INT(0, stat_db_series(base, base + 47220, STAT_METRIC_TEMP, &s));
   TEST_ASSERT_EQUAL_INT(1800, s.group_secs);
   TEST_ASSERT_TRUE(s.count <= STAT_SERIES_MAX_POINTS);
   TEST_ASSERT_EQUAL_INT(27, s.count);
}

/* H2: an aligned end (span 43,200) with a bucket AT end can yield a 49th group;
 * the +1 array slot + capped loop must hold it with no overrun. */
void test_series_aligned_end_no_overrun(void) {
   int64_t base = 200000;
   for (int i = 0; i <= 48; i++) { /* 49 buckets, last exactly at base+43200 */
      insert_sys_bucket(base + i * 900, 45.0, 45.0, 45.0, 10.0, 1);
   }
   stat_series_t s;
   TEST_ASSERT_EQUAL_INT(0, stat_db_series(base, base + 43200, STAT_METRIC_TEMP, &s));
   TEST_ASSERT_EQUAL_INT(900, s.group_secs);
   TEST_ASSERT_TRUE(s.count <= STAT_SERIES_MAX_POINTS + 1);
   TEST_ASSERT_EQUAL_INT(49, s.count);
}

/* 7-day span (672 native buckets) → ceil(604800/43200)=14 → group_secs 12600 → 48 groups. */
void test_series_7d_downsample(void) {
   int64_t base = 1000000;
   for (int i = 0; i < 672; i++) {
      insert_sys_bucket(base + i * 900, 50.0, 50.0, 50.0, 10.0, 1);
   }
   stat_series_t s;
   TEST_ASSERT_EQUAL_INT(0, stat_db_series(base, base + 604800, STAT_METRIC_TEMP, &s));
   TEST_ASSERT_EQUAL_INT(12600, s.group_secs);
   TEST_ASSERT_TRUE(s.count <= STAT_SERIES_MAX_POINTS);
}

/* Weighted average + exact min/max across a downsampled group. */
void test_series_weighted_group(void) {
   int64_t base = 300000;
   /* Two buckets share group 0 (group_secs will be 1800): weights 1 and 3. */
   insert_sys_bucket(base + 0, 40.0, 38.0, 42.0, 10.0, 1);
   insert_sys_bucket(base + 900, 50.0, 48.0, 52.0, 10.0, 3);
   /* A distant bucket stretches the span so group_secs becomes 1800. */
   insert_sys_bucket(base + 50000, 60.0, 60.0, 60.0, 10.0, 1);

   stat_series_t s;
   TEST_ASSERT_EQUAL_INT(0, stat_db_series(base, base + 50000, STAT_METRIC_TEMP, &s));
   TEST_ASSERT_EQUAL_INT(1800, s.group_secs);
   /* group 0: weighted avg (40*1 + 50*3)/4 = 47.5, min 38, max 52 */
   TEST_ASSERT_TRUE(s.points[0].have_avg);
   TEST_ASSERT_DOUBLE_WITHIN(0.01, 47.5, s.points[0].avg);
   TEST_ASSERT_EQUAL_DOUBLE(38.0, s.points[0].min);
   TEST_ASSERT_EQUAL_DOUBLE(52.0, s.points[0].max);
}

/* H3: a group with no samples for the metric's family is a GAP (have_avg false),
 * never a spurious 0.0. */
void test_series_null_gap_not_zero(void) {
   int64_t base = 400000;
   insert_sys_bucket(base, 45.0, 45.0, 45.0, 10.0, 1);       /* sys only */
   insert_batt_bucket(base, 80.0, 15.0);                     /* + battery in group 0 */
   insert_sys_bucket(base + 900, 46.0, 46.0, 46.0, 10.0, 1); /* group 1: sys only, NO battery */

   stat_series_t s;
   TEST_ASSERT_EQUAL_INT(0, stat_db_series(base, base + 900, STAT_METRIC_BATTERY, &s));
   TEST_ASSERT_EQUAL_INT(2, s.count);
   TEST_ASSERT_TRUE(s.points[0].have_avg); /* battery present */
   TEST_ASSERT_EQUAL_DOUBLE(80.0, s.points[0].avg);
   TEST_ASSERT_FALSE(s.points[1].have_avg); /* battery absent → gap, not 0 */
}

/* Power is an avg-only metric: no min/max columns. */
void test_series_power_avg_only(void) {
   insert_batt_bucket(500000, 80.0, 17.5);
   stat_series_t s;
   TEST_ASSERT_EQUAL_INT(0, stat_db_series(500000, 500000, STAT_METRIC_POWER, &s));
   TEST_ASSERT_EQUAL_INT(1, s.count);
   TEST_ASSERT_FALSE(s.has_min);
   TEST_ASSERT_FALSE(s.has_max);
   TEST_ASSERT_TRUE(s.points[0].have_avg);
   TEST_ASSERT_FALSE(s.points[0].have_min);
   TEST_ASSERT_DOUBLE_WITHIN(0.01, 17.5, s.points[0].avg);
}

/* CPU stores avg + max (no min). */
void test_series_cpu_avg_max(void) {
   insert_sys_bucket(600000, 45.0, 45.0, 45.0, 22.0, 1);
   stat_series_t s;
   TEST_ASSERT_EQUAL_INT(0, stat_db_series(600000, 600000, STAT_METRIC_CPU, &s));
   TEST_ASSERT_FALSE(s.has_min);
   TEST_ASSERT_TRUE(s.has_max);
   TEST_ASSERT_TRUE(s.points[0].have_avg);
   TEST_ASSERT_TRUE(s.points[0].have_max);
   TEST_ASSERT_FALSE(s.points[0].have_min);
}

/* Empty window → zero points (tool surfaces the error-mark path). */
void test_series_empty_window(void) {
   stat_series_t s;
   TEST_ASSERT_EQUAL_INT(0, stat_db_series(900000000, 900000900, STAT_METRIC_TEMP, &s));
   TEST_ASSERT_EQUAL_INT(0, s.count);
}

/* ===== Network telemetry ===== */

/* Synthetic payload exercising the parser independent of the STAT checkout:
 * link-local-only IPv6 (no global), speed_mbps -1, an unreachable gateway with
 * fail_streak and no rtt_ms, and interfaces_truncated. Always runs in CI. */
void test_network_parse_synthetic(void) {
   const char *msg =
       "{\"device\":\"stat\",\"msg_type\":\"telemetry\",\"type\":\"Network\","
       "\"probe_available\":true,\"interfaces_truncated\":true,"
       "\"interfaces\":[{\"name\":\"usb0\",\"kind\":\"cellular\",\"driver\":\"rndis_host\","
       "\"state\":\"unknown\",\"up\":true,\"carrier\":true,\"mtu\":1420,\"speed_mbps\":-1,"
       "\"ipv4\":[\"192.168.225.40\"],\"ipv6\":[\"fe80::1\"]}],"
       "\"default_routes\":[{\"iface\":\"usb0\",\"gateway\":\"192.168.225.1\",\"metric\":20100,"
       "\"family\":\"ipv4\"}],"
       "\"reachability\":[{\"gateway\":\"192.168.225.1\",\"iface\":\"usb0\",\"target_kind\":"
       "\"gateway\",\"reachable\":false,\"fail_streak\":3,\"bound\":true}]}";
   int consumed = stat_service_handle_mqtt("stat/telemetry", msg, (int)strlen(msg));
   TEST_ASSERT_EQUAL_INT(1, consumed);

   stat_snapshot_t s;
   stat_service_get_snapshot(&s);
   TEST_ASSERT_TRUE(s.have_network);
   TEST_ASSERT_TRUE(s.net_ifaces_truncated);
   TEST_ASSERT_EQUAL_INT(1, s.net_iface_count);
   TEST_ASSERT_EQUAL_STRING("cellular", s.net_ifaces[0].kind);
   TEST_ASSERT_EQUAL_INT(-1, s.net_ifaces[0].speed_mbps);
   TEST_ASSERT_TRUE(s.net_ifaces[0].up); /* up despite state "unknown" */
   TEST_ASSERT_TRUE(s.net_ifaces[0].has_ipv6);
   TEST_ASSERT_EQUAL_STRING("", s.net_ifaces[0].ipv6_global); /* fe80:: is not global */
   TEST_ASSERT_EQUAL_INT(1, s.net_reach_count);
   TEST_ASSERT_FALSE(s.net_reach[0].reachable);
   TEST_ASSERT_FALSE(s.net_reach[0].has_rtt); /* rtt_ms omitted when unreachable */
   TEST_ASSERT_EQUAL_INT(3, s.net_reach[0].fail_streak);
}

/* A non-object array element is skipped, but must NOT read as a capacity
 * truncation (the *_truncated flag reflects a real cap hit, not a bad element). */
void test_network_malformed_not_truncated(void) {
   const char *msg =
       "{\"device\":\"stat\",\"msg_type\":\"telemetry\",\"type\":\"Network\",\"probe_available\":"
       "true,\"interfaces\":[{\"name\":\"eth0\",\"kind\":\"ethernet\",\"up\":true,\"carrier\":true,"
       "\"ipv4\":[\"10.0.0.2\"]},null]}";
   int consumed = stat_service_handle_mqtt("stat/telemetry", msg, (int)strlen(msg));
   TEST_ASSERT_EQUAL_INT(1, consumed);
   stat_snapshot_t s;
   stat_service_get_snapshot(&s);
   TEST_ASSERT_EQUAL_INT(1, s.net_iface_count); /* the null element was skipped */
   TEST_ASSERT_FALSE(s.net_ifaces_truncated);   /* a skip is not a capacity truncation */
}

#ifdef STAT_NET_FIXTURE
static char *read_file(const char *path, int *len_out) {
   FILE *f = fopen(path, "rb");
   if (!f) {
      return NULL;
   }
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (n <= 0) {
      fclose(f);
      return NULL;
   }
   char *buf = malloc((size_t)n + 1);
   if (!buf) {
      fclose(f);
      return NULL;
   }
   size_t rd = fread(buf, 1, (size_t)n, f);
   fclose(f);
   buf[rd] = '\0';
   if (len_out) {
      *len_out = (int)rd;
   }
   return buf;
}
#endif

/* Cross-repo contract pin: feed STAT's canonical network_v1.json and assert the
 * exact parsed values. A field-name/shape change on the STAT side breaks this. */
void test_network_contract_fixture(void) {
#ifndef STAT_NET_FIXTURE
   TEST_IGNORE_MESSAGE("STAT_NET_FIXTURE not defined");
#else
   int len = 0;
   char *payload = read_file(STAT_NET_FIXTURE, &len);
   if (!payload) {
      TEST_IGNORE_MESSAGE(
          "STAT fixture not present (sibling checkout absent) — cross-repo pin skipped");
      return;
   }
   int consumed = stat_service_handle_mqtt("stat/telemetry", payload, len);
   free(payload);
   TEST_ASSERT_EQUAL_INT(1, consumed);

   stat_snapshot_t s;
   stat_service_get_snapshot(&s);
   TEST_ASSERT_TRUE(s.have_network);
   TEST_ASSERT_TRUE(s.net_probe_available);
   TEST_ASSERT_FALSE(s.net_ifaces_truncated);
   TEST_ASSERT_FALSE(s.net_routes_truncated);
   TEST_ASSERT_EQUAL_INT(2, s.net_iface_count);
   TEST_ASSERT_EQUAL_INT(4, s.net_route_count);
   TEST_ASSERT_EQUAL_INT(2, s.net_reach_count);

   const stat_net_iface_t *eth = NULL, *cell = NULL;
   for (int i = 0; i < s.net_iface_count; i++) {
      if (strcmp(s.net_ifaces[i].kind, "ethernet") == 0) {
         eth = &s.net_ifaces[i];
      } else if (strcmp(s.net_ifaces[i].kind, "cellular") == 0) {
         cell = &s.net_ifaces[i];
      }
   }
   TEST_ASSERT_NOT_NULL(eth);
   TEST_ASSERT_NOT_NULL(cell);
   TEST_ASSERT_EQUAL_STRING("enP8p1s0", eth->name);
   TEST_ASSERT_TRUE(eth->up);
   TEST_ASSERT_TRUE(eth->carrier);
   TEST_ASSERT_EQUAL_STRING("192.168.1.159", eth->ipv4);
   TEST_ASSERT_TRUE(eth->has_ipv6);
   TEST_ASSERT_EQUAL_STRING("usb0", cell->name);
   TEST_ASSERT_EQUAL_STRING("unknown", cell->state);
   TEST_ASSERT_TRUE(cell->up); /* up true despite state "unknown" */
   TEST_ASSERT_EQUAL_INT(-1, cell->speed_mbps);
   TEST_ASSERT_EQUAL_STRING("192.168.225.40", cell->ipv4);
   TEST_ASSERT_EQUAL_STRING("2607:fb90:7c1c:ccc7:6627:9487:834b:af2b", cell->ipv6_global);

   /* min-metric route is the ethernet path (metric 100 < 20100). */
   int best = -1;
   for (int i = 0; i < s.net_route_count; i++) {
      if (best < 0 || s.net_routes[i].metric < s.net_routes[best].metric) {
         best = i;
      }
   }
   TEST_ASSERT_TRUE(best >= 0);
   TEST_ASSERT_EQUAL_STRING("enP8p1s0", s.net_routes[best].iface);
   TEST_ASSERT_EQUAL_INT(100, s.net_routes[best].metric);

   const stat_net_reach_t *ru = NULL;
   for (int i = 0; i < s.net_reach_count; i++) {
      if (strcmp(s.net_reach[i].iface, "usb0") == 0) {
         ru = &s.net_reach[i];
      }
   }
   TEST_ASSERT_NOT_NULL(ru);
   TEST_ASSERT_TRUE(ru->reachable);
   TEST_ASSERT_TRUE(ru->has_rtt);
   TEST_ASSERT_EQUAL_INT(0, ru->fail_streak);
   TEST_ASSERT_TRUE(ru->bound);
#endif
}

/* ===== Renderer interpretation rules (stat_render_network, hand-built snapshots
 * so the author-specified rules can't silently regress) ===== */

static void mk_iface(stat_net_iface_t *f,
                     const char *name,
                     const char *kind,
                     bool up,
                     bool carrier,
                     const char *ipv4,
                     const char *ipv6_global) {
   memset(f, 0, sizeof(*f));
   snprintf(f->name, sizeof(f->name), "%s", name);
   snprintf(f->kind, sizeof(f->kind), "%s", kind);
   snprintf(f->state, sizeof(f->state), "unknown"); /* deliberately not "up" */
   f->up = up;
   f->carrier = carrier;
   if (ipv4) {
      snprintf(f->ipv4, sizeof(f->ipv4), "%s", ipv4);
   }
   if (ipv6_global) {
      snprintf(f->ipv6_global, sizeof(f->ipv6_global), "%s", ipv6_global);
   }
}

/* Primary path = the MIN-metric route, even when it isn't first in the array. */
void test_render_primary_min_metric(void) {
   stat_snapshot_t s;
   memset(&s, 0, sizeof(s));
   s.have_network = true;
   s.net_probe_available = true;
   s.net_iface_count = 2;
   mk_iface(&s.net_ifaces[0], "enP8p1s0", "ethernet", true, true, "192.168.1.9", NULL);
   mk_iface(&s.net_ifaces[1], "usb0", "cellular", true, true, "192.168.225.9", NULL);
   /* Higher-metric (penalized) route FIRST so "first" != "min". */
   s.net_route_count = 2;
   snprintf(s.net_routes[0].iface, sizeof(s.net_routes[0].iface), "usb0");
   s.net_routes[0].metric = 20100;
   snprintf(s.net_routes[1].iface, sizeof(s.net_routes[1].iface), "enP8p1s0");
   s.net_routes[1].metric = 100;

   char buf[512] = "";
   stat_render_network(&s, buf, sizeof(buf));
   TEST_ASSERT_NOT_NULL(strstr(buf, "primary path enP8p1s0")); /* min metric, not first */
   TEST_ASSERT_NULL(strstr(buf, "primary path usb0"));
}

/* Link state is up&&carrier, NOT the operstate string (which is "unknown"). */
void test_render_up_carrier_not_state(void) {
   stat_snapshot_t s;
   memset(&s, 0, sizeof(s));
   s.have_network = true;
   s.net_probe_available = true;
   s.net_iface_count = 2;
   mk_iface(&s.net_ifaces[0], "usb0", "cellular", true, true, "10.0.0.9", NULL); /* up */
   mk_iface(&s.net_ifaces[1], "eth1", "ethernet", false, false, NULL, NULL);     /* down */

   char buf[512] = "";
   stat_render_network(&s, buf, sizeof(buf));
   TEST_ASSERT_NOT_NULL(strstr(buf, "usb0 (cellular): up"));
   TEST_ASSERT_NOT_NULL(strstr(buf, "eth1 (ethernet): down"));
}

/* fail_streak >= 2 = unreachable; a single reachable:false = transient; else reachable. */
void test_render_fail_streak_down(void) {
   stat_snapshot_t s;
   memset(&s, 0, sizeof(s));
   s.have_network = true;
   s.net_probe_available = true;
   s.net_iface_count = 1;
   mk_iface(&s.net_ifaces[0], "eth0", "ethernet", true, true, "10.0.0.9", NULL);
   s.net_reach_count = 1;
   snprintf(s.net_reach[0].iface, sizeof(s.net_reach[0].iface), "eth0");

   /* fail_streak 3 → down */
   s.net_reach[0].reachable = false;
   s.net_reach[0].fail_streak = 3;
   char buf[512] = "";
   stat_render_network(&s, buf, sizeof(buf));
   TEST_ASSERT_NOT_NULL(strstr(buf, "gateway unreachable (3 consecutive failures)"));

   /* single miss (fail_streak 1) → transient, NOT down */
   s.net_reach[0].fail_streak = 1;
   buf[0] = '\0';
   stat_render_network(&s, buf, sizeof(buf));
   TEST_ASSERT_NOT_NULL(strstr(buf, "missed once (transient)"));
   TEST_ASSERT_NULL(strstr(buf, "unreachable"));

   /* reachable with rtt */
   s.net_reach[0].reachable = true;
   s.net_reach[0].fail_streak = 0;
   s.net_reach[0].has_rtt = true;
   s.net_reach[0].rtt_ms = 1.5;
   buf[0] = '\0';
   stat_render_network(&s, buf, sizeof(buf));
   TEST_ASSERT_NOT_NULL(strstr(buf, "gateway reachable (1.5 ms)"));
}

/* Cellular honesty: caveat present and leading; bearer claim keyed on global IPv6. */
void test_render_cellular_honesty(void) {
   stat_snapshot_t s;
   memset(&s, 0, sizeof(s));
   s.have_network = true;
   s.net_probe_available = true;
   s.net_iface_count = 1;

   /* With a global IPv6 → "likely active", but caveat still present + leads. */
   mk_iface(&s.net_ifaces[0], "usb0", "cellular", true, true, "192.168.225.9",
            "2607:fb90:1:2:3:4:5:6");
   char buf[512] = "";
   stat_render_network(&s, buf, sizeof(buf));
   TEST_ASSERT_NOT_NULL(strstr(buf, "not the cellular bearer"));
   TEST_ASSERT_NOT_NULL(strstr(buf, "bearer is likely active"));
   /* the caveat leads the hedge (survives truncation) */
   char *caveat = strstr(buf, "USB link to the modem");
   char *hedge = strstr(buf, "likely active");
   TEST_ASSERT_NOT_NULL(caveat);
   TEST_ASSERT_TRUE(caveat < hedge);

   /* No global IPv6 → "bearer may be down". */
   mk_iface(&s.net_ifaces[0], "usb0", "cellular", true, true, "192.168.225.9", NULL);
   buf[0] = '\0';
   stat_render_network(&s, buf, sizeof(buf));
   TEST_ASSERT_NOT_NULL(strstr(buf, "bearer may be down"));
}

/* A route whose iface has no interface record must not crash the primary line. */
void test_render_route_without_iface(void) {
   stat_snapshot_t s;
   memset(&s, 0, sizeof(s));
   s.have_network = true;
   s.net_probe_available = true;
   s.net_route_count = 1;
   snprintf(s.net_routes[0].iface, sizeof(s.net_routes[0].iface), "ghost0");
   snprintf(s.net_routes[0].gateway, sizeof(s.net_routes[0].gateway), "10.0.0.1");
   s.net_routes[0].metric = 100;
   /* net_iface_count = 0 — no matching interface record */

   char buf[512] = "";
   stat_render_network(&s, buf, sizeof(buf));
   TEST_ASSERT_NOT_NULL(strstr(buf, "primary path ghost0")); /* no kind, no crash */
}

void test_render_no_network(void) {
   stat_snapshot_t s;
   memset(&s, 0, sizeof(s));
   s.have_network = false;
   char buf[512] = "";
   stat_render_network(&s, buf, sizeof(buf));
   TEST_ASSERT_NOT_NULL(strstr(buf, "No network telemetry"));
}

/* ---- stat_net_interpret: the shared honesty rules SAGE readers rely on ---- */

static void mk_route(stat_net_route_t *r, const char *iface, int metric, const char *family) {
   memset(r, 0, sizeof(*r));
   snprintf(r->iface, sizeof(r->iface), "%s", iface);
   r->metric = metric;
   snprintf(r->family, sizeof(r->family), "%s", family);
}

/* Primary = min metric, filtered to the requested family; a lower-metric route in
 * the OTHER family must not win when a family is specified. */
void test_interpret_primary_min_metric_family(void) {
   stat_snapshot_t s;
   memset(&s, 0, sizeof(s));
   s.net_route_count = 3;
   mk_route(&s.net_routes[0], "usb0", 20100, "ipv4");   /* penalized wired-fallback slot */
   mk_route(&s.net_routes[1], "enP8p1s0", 100, "ipv4"); /* the real min */
   mk_route(&s.net_routes[2], "enP8p1s0", 50, "ipv6");  /* lower, but wrong family */

   const stat_net_route_t *pr = stat_net_primary_route(&s, "ipv4");
   TEST_ASSERT_NOT_NULL(pr);
   TEST_ASSERT_EQUAL_INT(100, pr->metric);
   TEST_ASSERT_EQUAL_STRING("enP8p1s0", pr->iface);
   /* family NULL = any: the v6 route at metric 50 now wins. */
   pr = stat_net_primary_route(&s, NULL);
   TEST_ASSERT_EQUAL_INT(50, pr->metric);
}

/* Healthy needs a route + up/carrier iface + gateway not down (fail_streak < 2). */
void test_interpret_primary_healthy_and_failstreak(void) {
   stat_snapshot_t s;
   memset(&s, 0, sizeof(s));
   s.net_iface_count = 1;
   mk_iface(&s.net_ifaces[0], "eth0", "ethernet", true, true, "10.0.0.9", NULL);
   s.net_route_count = 1;
   mk_route(&s.net_routes[0], "eth0", 100, "ipv4");
   s.net_reach_count = 1;
   snprintf(s.net_reach[0].iface, sizeof(s.net_reach[0].iface), "eth0");

   s.net_reach[0].fail_streak = 1; /* a single miss is transient */
   TEST_ASSERT_TRUE(stat_net_primary_healthy(&s, "ipv4"));
   s.net_reach[0].fail_streak = 2; /* >= 2 = down */
   TEST_ASSERT_FALSE(stat_net_primary_healthy(&s, "ipv4"));
}

/* No default route, and a link-down primary iface, both read as unhealthy. */
void test_interpret_no_route_and_link_down(void) {
   stat_snapshot_t s;
   memset(&s, 0, sizeof(s));
   TEST_ASSERT_FALSE(stat_net_primary_healthy(&s, "ipv4")); /* no route at all */

   s.net_iface_count = 1;
   mk_iface(&s.net_ifaces[0], "eth0", "ethernet", false, false, NULL, NULL); /* link down */
   s.net_route_count = 1;
   mk_route(&s.net_routes[0], "eth0", 100, "ipv4");
   TEST_ASSERT_FALSE(stat_net_primary_healthy(&s, "ipv4"));
   /* No reach row = link-state-only: a healthy link with no probe is still healthy. */
   mk_iface(&s.net_ifaces[0], "eth0", "ethernet", true, true, "10.0.0.9", NULL);
   TEST_ASSERT_TRUE(stat_net_primary_healthy(&s, "ipv4"));
}

/* A cellular primary route reads as on-cellular; a wired one does not. */
void test_interpret_cellular_primary(void) {
   stat_snapshot_t s;
   memset(&s, 0, sizeof(s));
   s.net_iface_count = 2;
   mk_iface(&s.net_ifaces[0], "enP8p1s0", "ethernet", true, true, "192.168.1.9", NULL);
   mk_iface(&s.net_ifaces[1], "usb0", "cellular", true, true, "192.168.225.9", NULL);
   s.net_route_count = 1;
   mk_route(&s.net_routes[0], "enP8p1s0", 100, "ipv4");
   TEST_ASSERT_FALSE(stat_net_primary_is_cellular(&s, "ipv4"));
   mk_route(&s.net_routes[0], "usb0", 100, "ipv4");
   TEST_ASSERT_TRUE(stat_net_primary_is_cellular(&s, "ipv4"));
}

/* O1: when wired and cellular TIE at the penalized level, the effective uplink is
 * cellular even though the wired route is listed first (first-wins picks wired).
 * Below the penalty level a tie is NOT treated as a failover. */
void test_interpret_cellular_penalized_tie(void) {
   stat_snapshot_t s;
   memset(&s, 0, sizeof(s));
   s.net_iface_count = 2;
   mk_iface(&s.net_ifaces[0], "enP8p1s0", "ethernet", true, true, "192.168.1.9", NULL);
   mk_iface(&s.net_ifaces[1], "usb0", "cellular", true, true, "192.168.225.9", NULL);
   s.net_route_count = 2;
   mk_route(&s.net_routes[0], "enP8p1s0", 20100, "ipv4"); /* penalized wired, listed first */
   mk_route(&s.net_routes[1], "usb0", 20100, "ipv4");     /* cellular, same metric */
   TEST_ASSERT_TRUE(stat_net_primary_is_cellular(&s, "ipv4"));

   s.net_routes[0].metric = 100; /* below penalty: a plain tie, wired wins, not failover */
   s.net_routes[1].metric = 100;
   TEST_ASSERT_FALSE(stat_net_primary_is_cellular(&s, "ipv4"));
}

/* Dwell bookkeeping: idempotent per now_ms, counts while in-state, clears on exit. */
void test_interpret_since_update_dwell(void) {
   int64_t since = 0;
   /* Not in state: stays 0. */
   TEST_ASSERT_EQUAL_INT(0, stat_net_since_update(&since, false, 1000));
   TEST_ASSERT_EQUAL_INT64(0, since);
   /* Onset stamps; same tick reads 0 elapsed; idempotent on repeat. */
   TEST_ASSERT_EQUAL_INT(0, stat_net_since_update(&since, true, 1000));
   TEST_ASSERT_EQUAL_INT64(1000, since);
   TEST_ASSERT_EQUAL_INT(0, stat_net_since_update(&since, true, 1000)); /* off-tick re-sample */
   TEST_ASSERT_EQUAL_INT64(1000, since);                                /* stamp unmoved */
   /* Held: elapsed grows, stamp fixed. */
   TEST_ASSERT_EQUAL_INT(16, stat_net_since_update(&since, true, 17000));
   TEST_ASSERT_EQUAL_INT64(1000, since);
   /* Freeze semantics: NOT calling it preserves the stamp; a later call resumes. */
   TEST_ASSERT_EQUAL_INT(20, stat_net_since_update(&since, true, 21000));
   /* Exit clears; re-entry re-stamps fresh. */
   TEST_ASSERT_EQUAL_INT(0, stat_net_since_update(&since, false, 22000));
   TEST_ASSERT_EQUAL_INT64(0, since);
   TEST_ASSERT_EQUAL_INT(0, stat_net_since_update(&since, true, 30000));
   TEST_ASSERT_EQUAL_INT64(30000, since);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_topic_gating);
   RUN_TEST(test_render_primary_min_metric);
   RUN_TEST(test_render_up_carrier_not_state);
   RUN_TEST(test_render_fail_streak_down);
   RUN_TEST(test_render_cellular_honesty);
   RUN_TEST(test_render_route_without_iface);
   RUN_TEST(test_render_no_network);
   RUN_TEST(test_interpret_primary_min_metric_family);
   RUN_TEST(test_interpret_primary_healthy_and_failstreak);
   RUN_TEST(test_interpret_no_route_and_link_down);
   RUN_TEST(test_interpret_cellular_primary);
   RUN_TEST(test_interpret_cellular_penalized_tie);
   RUN_TEST(test_interpret_since_update_dwell);
   RUN_TEST(test_network_parse_synthetic);
   RUN_TEST(test_network_malformed_not_truncated);
   RUN_TEST(test_network_contract_fixture);
   RUN_TEST(test_snapshot_never_seen);
   RUN_TEST(test_live_cache_latest);
   RUN_TEST(test_history_aggregation);
   RUN_TEST(test_empty_bucket_skipped);
   RUN_TEST(test_multi_bucket);
   RUN_TEST(test_series_short_window);
   RUN_TEST(test_series_span_47220_cap);
   RUN_TEST(test_series_aligned_end_no_overrun);
   RUN_TEST(test_series_7d_downsample);
   RUN_TEST(test_series_weighted_group);
   RUN_TEST(test_series_null_gap_not_zero);
   RUN_TEST(test_series_power_avg_only);
   RUN_TEST(test_series_cpu_avg_max);
   RUN_TEST(test_series_empty_window);
   return UNITY_END();
}
