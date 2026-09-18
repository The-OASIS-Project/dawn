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
 * STAT telemetry service — subscribes (passively, via mosquitto_comms) to the
 * external STAT daemon's MQTT telemetry, caches the latest values in memory
 * (mutex-guarded, TTL-stale), and folds every sample into rollup accumulators
 * that the maintenance thread flushes to stat_db.  Modeled on phone_service.c
 * (external MQTT daemon -> cache -> tool) + component_status.c (TTL staleness).
 */

#ifndef STAT_SERVICE_H
#define STAT_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STAT_TELEMETRY_TOPIC_DEFAULT "stat/telemetry"
#define STAT_STATUS_TOPIC_DEFAULT "stat/status"

/** Resolved configuration handed to the service by the tool at init. */
typedef struct {
   bool enabled;
   char telemetry_topic[128];
   char status_topic[128];
   char db_path[256];
   int stale_after_sec;        /* live reads older than this report "stale" */
   int history_retention_days; /* prune buckets older than this */
} stat_service_cfg_t;

/* Network telemetry (the STAT "Network" envelope). Bounded so the snapshot stays
 * a fixed copy-out; extra rows are dropped and the *_truncated flags note it. */
#define STAT_NET_MAX_IFACES 8
#define STAT_NET_MAX_ROUTES 8
#define STAT_NET_MAX_REACH 8

typedef struct {
   char name[24];   /* interface name (e.g. enP8p1s0, usb0) */
   char kind[16];   /* ethernet / cellular / wifi / loopback / ... */
   char driver[24]; /* kernel driver (r8168, rndis_host, ...) */
   char state[16];  /* operstate; may be "unknown" even when up */
   bool up;         /* administratively/operationally up */
   bool carrier;    /* physical/link carrier present */
   int mtu;
   int speed_mbps;       /* -1 = unknown (cellular) */
   char ipv4[24];        /* first IPv4, "" if none */
   char ipv6_global[46]; /* first global-scope IPv6 (2000::/3), "" if none */
   bool has_ipv6;        /* any IPv6 (incl. link-local) present */
   bool addr_truncated;  /* STAT dropped some addresses for this iface */
} stat_net_iface_t;

typedef struct {
   char iface[24];
   char gateway[46];
   int metric;     /* lower = preferred; NM adds +20000 on a penalized path */
   char family[8]; /* "ipv4" / "ipv6" */
} stat_net_route_t;

typedef struct {
   char gateway[46];
   char iface[24];
   char target_kind[16]; /* "gateway" (only kind today) */
   bool reachable;       /* UNAUTHENTICATED ICMP result — never gate security on it */
   bool has_rtt;         /* rtt_ms is omitted on the wire when unreachable */
   double rtt_ms;
   int fail_streak; /* consecutive failures; treat >= 2 as "down", not 1 */
   bool bound;      /* probe was bound to the specific iface */
} stat_net_reach_t;

/** A consistent copy-out of the live cache (see stat_service_get_snapshot). */
typedef struct {
   bool stat_online; /* last stat/status said "online" */
   bool ever_seen;   /* any telemetry received since boot */
   bool stale;       /* (now - last_seen) > stale_after_sec, or never seen */
   int age_sec;      /* seconds since last telemetry (0 if never) */
   time_t last_seen; /* wall-clock receipt of last telemetry */

   bool have_system;
   double cpu_usage, memory_usage, system_temp;

   bool have_fan;
   int fan_rpm, fan_load, fan_pwm;

   bool have_battery;
   double batt_voltage, batt_current, batt_power, batt_level, batt_temp, time_remaining_min;
   char charging_state[24];
   char battery_status[24]; /* OK / WARNING / CRITICAL */
   char status_reason[96];
   int crit_faults, warn_faults, info_faults;

   bool have_network;
   bool net_probe_available;  /* false = ICMP probe disabled / socket unavailable */
   bool net_ifaces_truncated; /* STAT dropped some interfaces */
   bool net_routes_truncated; /* STAT dropped some default routes */
   bool net_reach_truncated;  /* our cap dropped some reachability rows */
   int net_iface_count;       /* entries populated in net_ifaces[] */
   int net_route_count;
   int net_reach_count;
   stat_net_iface_t net_ifaces[STAT_NET_MAX_IFACES];
   stat_net_route_t net_routes[STAT_NET_MAX_ROUTES];
   stat_net_reach_t net_reach[STAT_NET_MAX_REACH];
} stat_snapshot_t;

/**
 * @brief Initialize the service (from the tool `.init`, before MQTT connects).
 *
 * Copies @p cfg, inits the cache, and opens stat_db if enabled.  Does NOT touch
 * MQTT (subscriptions are centralized in mosquitto_comms.c on_connect).  Returns
 * SUCCESS even if the DB fails to open (live cache still works; history off) so
 * tool registration is never aborted.
 */
int stat_service_init(const stat_service_cfg_t *cfg);

/** @brief Flush the final partial bucket, close stat_db, free state. */
void stat_service_shutdown(void);

/** @brief True when enabled and initialized (gate subscribe/dispatch on this). */
bool stat_service_is_active(void);

/** @brief Configured telemetry topic (for the on_connect subscribe). */
const char *stat_service_telemetry_topic(void);

/** @brief Configured status topic (retained/LWT; for the on_connect subscribe). */
const char *stat_service_status_topic(void);

/**
 * @brief Consume an MQTT message if @p topic is one of ours.
 *
 * Called from on_message ABOVE the generic INFO log so STAT's multi-Hz firehose
 * does not flood the log.  Parses length-bounded (payload may not be
 * NUL-terminated) and updates the cache + accumulators under the mutex.
 *
 * @return 1 if the message was ours (consumed), 0 otherwise.
 */
int stat_service_handle_mqtt(const char *topic, const char *payload, int payload_len);

/** @brief Copy the live cache out (mutex-guarded); computes stale/age vs now. */
void stat_service_get_snapshot(stat_snapshot_t *out);

/**
 * @brief Flush accumulators to one history bucket + prune + checkpoint.
 *
 * Called from the auth_maintenance loop.  Copies+resets accumulators under the
 * cache mutex, then writes/prunes via stat_db (no lock nesting).  No-op when the
 * bucket had zero samples.  Failure-isolated: always returns, never propagates.
 */
int stat_history_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* STAT_SERVICE_H */
