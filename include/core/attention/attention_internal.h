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
 * SAGE proactive-attention subsystem — internal shared state, tuning constants,
 * and cross-file prototypes.  Not part of the public API.
 */

#ifndef ATTENTION_INTERNAL_H
#define ATTENTION_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "core/attention/attention.h"
#include "core/stat_service.h"
#include "core/suit_service.h"

/* --- tuning constants (no magic numbers) --- */
#define SAGE_HYSTERESIS_DEFAULT 5.0                /* band re-entry margin (metric units) */
#define SAGE_BACKOFF_INITIAL_MS (5 * 60 * 1000)    /* first suppression window */
#define SAGE_BACKOFF_MAX_MS (60 * 60 * 1000)       /* suppression cap */
#define SAGE_BACKOFF_RESET_MS (2 * 60 * 60 * 1000) /* quiet period resets backoff */
#define SAGE_TTL_DEFAULT_MIN 30                    /* per-event opportunity window */
#define SAGE_MAX_ALERTS_PER_HOUR 4                 /* default global alert budget */
#define SAGE_SLOPE_SAMPLES 32                      /* slope history ring depth */
#define SAGE_SLOPE_WINDOW_SEC_DEFAULT 120          /* default slope window */
#define SAGE_LIVENESS_SILENT_MIN 15                /* silent-source warn threshold */
/* SAGE_MAX_WATCHES_PER_USER is public (attention.h). */
#define SAGE_MAX_WATCHES_TOTAL 128 /* in-memory cache cap across all users (2x per-user) */
#define SAGE_QUEUE_CAP 32          /* pending-event ring capacity (P0 fires few/tick) */

/**
 * Per-source snapshot bundle taken once per tick and shared across all watches
 * on that source (avoids re-snapshotting per watch).  Embeds the telemetry
 * snapshot structs unconditionally — their headers are always present even when
 * the STAT/suit services are compiled out; the ingest fill guards only the
 * get_snapshot() CALLS, leaving these structs zeroed (=> readers see "not
 * present" => watches never fire).  Graceful degradation, no #ifdef in readers.
 */
typedef struct attention_sample_ctx {
   stat_snapshot_t stat;
   bool stat_valid;
   suit_snapshot_t suit;
   bool suit_valid;
   bool hud_ever;   /* HUD/MIRAGE was ever seen online */
   bool hud_online; /* currently online */
   int hud_age_sec; /* seconds since last hud/status */
   /* Derived network state (from ctx->stat via stat_net_interpret). net_present
    * is false when STAT is absent/stale/lacks network telemetry, so the network
    * readers report "not present" rather than a frozen reading. The durations are
    * dwell counters maintained idempotently in attention_ingest.c. */
   bool net_present;
   int net_uplink_down_sec; /* seconds the IPv4 primary path has been unhealthy */
   int net_cellular_sec;    /* seconds the IPv4 primary path has been cellular */
   int64_t now_ms;
} attention_sample_ctx_t;

/** A catalog metric definition + its snapshot reader. */
typedef struct {
   const char *key;    /* stable id, e.g. "suit.co2_ppm" */
   const char *source; /* "stat"|"suit"|"component" */
   const char *label;  /* human label, e.g. "CO2" */
   const char *unit;   /* "ppm", "%", "V", "s", "" */
   sage_rule_type_t default_rule_type;
   sage_direction_t default_direction;
   double default_threshold;
   double default_hysteresis;
   int default_absence_after_sec;
   sage_notify_t default_notify; /* loudness when created without an explicit level */
   /* Fill *value from the sampled ctx; return true if the value is present. */
   bool (*read)(const attention_sample_ctx_t *ctx, double *value);
} attention_catalog_entry_t;

/** Per-watch runtime state (hysteresis/backoff/slope), keyed alongside the cache. */
typedef struct {
   int64_t rule_id; /* which watch this state belongs to */
   bool armed;      /* threshold/absence: ready to fire again */
   int64_t last_fired_ms;
   int64_t backoff_ms;
   /* slope ring */
   double hist_val[SAGE_SLOPE_SAMPLES];
   int64_t hist_ts[SAGE_SLOPE_SAMPLES];
   int hist_head;
   int hist_count;
} sage_rule_state_t;

/* --- catalog (attention_catalog.c) --- */
const attention_catalog_entry_t *attention_catalog_lookup(const char *key);
const attention_catalog_entry_t *attention_catalog_at(int i);

/* --- ingest (attention_ingest.c) --- */
/* Fill @ctx from all available sources for this tick. @advance_dwell must be true
 * ONLY for the single heartbeat-tick caller: it commits the persistent network
 * dwell timers. Off-tick query paths (metric_current / readings_snapshot, which
 * run on other threads) pass false — they read the committed dwell without
 * mutating it, so a poll can't reset a dwell window the tick depends on. */
void attention_ingest_sample(attention_sample_ctx_t *ctx, int64_t now_ms, bool advance_dwell);

/* --- gate (attention_gate.c) --- pure, explicit-state, unit-testable --- */
/*
 * Evaluate a single watch against @value (present) for this tick.  Updates
 * @state (hysteresis/backoff/slope), and on fire fills @out_event and returns
 * true.  @now_ms is the logical clock.  For absence watches @value is the feed
 * age in seconds and @present must be true (age is always known).
 */
bool attention_gate_eval(const sage_watch_t *w,
                         sage_rule_state_t *state,
                         double value,
                         bool present,
                         int64_t now_ms,
                         const attention_catalog_entry_t *cat,
                         sage_event_t *out_event);

/* Admit a pre-formed event (P1 push path): apply backoff + TTL only. */
bool attention_gate_admit(sage_event_t *ev, sage_rule_state_t *state, int64_t now_ms);

/* --- policy (attention_policy.c) --- the P1/P2 seam --- */
/* Decide the delivery mode for an event; enforces the per-hour alert budget.
 * Over-budget ALERTs are demoted to AMBIENT (kept visible + logged, just not
 * spoken).  Recording the emitted-alert timestamp is the caller's job via
 * attention_policy_note_alert() once delivery actually happens. */
sage_delivery_mode_t attention_policy_decide(const sage_event_t *ev, int64_t now_ms);

/* Set the per-hour alert budget (attention_init passes the config value; tests
 * call directly to stay independent of g_config). */
void attention_policy_configure(int max_alerts_per_hour);

/* Record that an ALERT was actually emitted at @now_ms (feeds the budget). */
void attention_policy_note_alert(int64_t now_ms);

/* Clear the budget ring (test isolation). */
void attention_policy_reset_budget(void);

/* --- db wrapper (attention_db.c) --- */
int attention_db_log_event(const sage_event_t *ev,
                           sage_delivery_mode_t mode,
                           const char *surface,
                           int64_t delivered_at_ms);

#endif /* ATTENTION_INTERNAL_H */
