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
 * SAGE proactive-attention subsystem — public API.  P0: conversationally-managed
 * "watches" (per-user DB rows, created/muted by the `attention` LLM tool and a
 * WebUI panel) are evaluated on the main-loop heartbeat against the live STAT /
 * suit / component telemetry snapshots; a deterministic gate (threshold /
 * slope / absence + hysteresis + backoff + TTL) decides what fires, a trivial
 * policy maps the watch's notify level to a delivery mode, and delivery reuses
 * the scheduler alert path (spoken + banner).  Every processed event is written
 * to attention_log.  The judge (P2) and urgency tiers/budgets (P1) slot into the
 * existing seams without restructuring ingest, gate, or log.  See
 * docs/PROACTIVE_ATTENTION_DESIGN.md (Track A / P0).
 */

#ifndef ATTENTION_H
#define ATTENTION_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- field sizes (design §5; static allocation, fixed-size strings) --- */
#define SAGE_EVENT_KEY_LEN 64
#define SAGE_SOURCE_LEN 16
#define SAGE_TYPE_LEN 32
#define SAGE_SUMMARY_LEN 256
#define SAGE_ENTITY_LEN 64
#define SAGE_ENTITY_COUNT 3
/* P0 never populates raw_json (it's the P2-judge context seam); keep it small
 * for now — P2 can enlarge it or move it to a heap/side table when the judge
 * lands.  Carrying 1 KB by value through the event queue/batch is dead weight. */
#define SAGE_RAW_JSON_LEN 256
#define SAGE_GATE_RULE_LEN 32
#define SAGE_PRIVACY_LEN 8
#define SAGE_DELIVER_TO_LEN 64

#define SAGE_WATCH_NAME_LEN 64
#define SAGE_METRIC_LEN 48
#define SAGE_DIRECTION_LEN 8
#define SAGE_SOURCE_TAG_LEN 16

/* Owner user for seeded safety watches + un-attributed telemetry alerts in the
 * single-owner deployment.  Watches are per-user from day one; multi-user
 * routing folds in with the presence/speaker-ID work. */
#define ATTENTION_OWNER_USER_ID 1

/* Per-user watch cap (also the list-buffer bound for callers). */
#define SAGE_MAX_WATCHES_PER_USER 64

/** What a watch REQUESTS when it fires (persisted on the watch). */
typedef enum {
   SAGE_NOTIFY_DIGEST = 0,  /* log only in P0 (P1 folds into briefings) */
   SAGE_NOTIFY_AMBIENT = 1, /* WebUI/HUD banner, no voice */
   SAGE_NOTIFY_ALERT = 2    /* spoken + banner */
} sage_notify_t;

/** What policy DECIDES (distinct from notify so P1 can demote under budget). */
typedef enum {
   SAGE_MODE_DROP = 0,
   SAGE_MODE_DIGEST = 1,
   SAGE_MODE_AMBIENT = 2,
   SAGE_MODE_ALERT = 3
} sage_delivery_mode_t;

/** Gate rule family. */
typedef enum {
   SAGE_RULE_THRESHOLD = 0, /* value crosses below/above a band edge */
   SAGE_RULE_SLOPE = 1,     /* rate-of-change over a window */
   SAGE_RULE_ABSENCE = 2,   /* a feed went silent (age > threshold) */
   SAGE_RULE_MATCH = 3      /* discrete event pattern (P1 push sources) */
} sage_rule_type_t;

/** Threshold/slope direction. */
typedef enum {
   SAGE_DIR_BELOW = 0,
   SAGE_DIR_ABOVE = 1,
   SAGE_DIR_RISING = 2 /* slope only */
} sage_direction_t;

/**
 * One normalized attention event (design §5).  Produced by the gate when a
 * watch fires, carried through policy to delivery + attention_log.  The
 * raw_json / entities / privacy / deliver_to fields are present-but-unused
 * seams so P1/P2 add no fields.
 */
typedef struct {
   char event_key[SAGE_EVENT_KEY_LEN];                /* stable dedup/backoff key */
   char source[SAGE_SOURCE_LEN];                      /* "stat"|"suit"|"component"|"sched" */
   char type[SAGE_TYPE_LEN];                          /* "threshold"|"slope"|"absence" */
   char summary[SAGE_SUMMARY_LEN];                    /* one-line, ready to speak/display */
   char entities[SAGE_ENTITY_COUNT][SAGE_ENTITY_LEN]; /* memory-graph names (P2) */
   double magnitude;                                  /* source-normalized 0.0-1.0 */
   int64_t observed_at;                               /* epoch ms */
   int64_t ttl_ms;                                    /* opportunity window; expired => DROP */
   char raw_json[SAGE_RAW_JSON_LEN];                  /* compact context slice (P2 judge) */
   /* gate output, carried to policy/log */
   char gate_rule[SAGE_GATE_RULE_LEN];
   sage_notify_t notify;
   char privacy[SAGE_PRIVACY_LEN];       /* P0 always "public" */
   char deliver_to[SAGE_DELIVER_TO_LEN]; /* empty in P0 */
   int64_t rule_id;                      /* originating watch id (0 if none) */
   int user_id;                          /* owner of the watch */
} sage_event_t;

/**
 * A watch — the runtime form of an attention_rules row.  Created/edited/muted
 * conversationally (the `attention` LLM tool) or via the WebUI Watches panel.
 * NAN threshold/hysteresis means "use the catalog default for this metric".
 */
typedef struct {
   int64_t id;
   int user_id;
   char name[SAGE_WATCH_NAME_LEN];
   char metric[SAGE_METRIC_LEN]; /* catalog key */
   sage_rule_type_t rule_type;
   sage_direction_t direction;
   double threshold;  /* NAN => catalog default */
   double hysteresis; /* NAN => catalog default */
   double slope_per_min;
   int slope_window_sec;
   int absence_after_sec;
   sage_notify_t notify;
   int ttl_min;
   bool enabled;
   int64_t muted_until;                  /* 0 = not muted (P1 timed-snooze) */
   char source_tag[SAGE_SOURCE_TAG_LEN]; /* voice|webui|seed */
} sage_watch_t;

/**
 * One watch's live reading — the volatile half of a watch, streamed at ~1 Hz to
 * a subscribed Watches panel so its gauges tick live (see
 * attention_readings_snapshot / the WebUI watch_readings push).  Carries only the
 * moving number keyed by watch id; the rule structure travels once via watch_list.
 */
typedef struct {
   int64_t id;       /* watch id (the client indexes readings by this) */
   bool has_current; /* false => the metric source hasn't reported */
   double value;     /* live value; meaningful iff has_current && isfinite(value) */
} sage_reading_t;

/**
 * Cumulative self-monitoring counters (never reset; a consumer computes deltas).
 * `evaluations` counts watch evaluations (one per enabled watch per tick), not
 * distinct ingested events.  The §9.9 per-source liveness check is a P0 deferral.
 */
typedef struct {
   uint64_t evaluations; /* watch evaluations performed */
   uint64_t gate_passed; /* watches that fired an event */
   uint64_t delivered;
   uint64_t expired;
   uint64_t dropped;
} attention_metrics_t;

/* =============================================================================
 * Lifecycle + heartbeat
 * ============================================================================= */

/** Initialize: load watches from the DB, seed safety watches on first run. */
int attention_init(void);

/** Free state (cache + queue). */
void attention_shutdown(void);

/** True when [attention] enabled (delivery + evaluation active). */
bool attention_is_enabled(void);

/** Advance one heartbeat step: poll → gate → policy → deliver → log. */
void attention_tick(time_t now);

/**
 * @brief Push a proactive-attention banner to a user's WebUI/HUD sessions.
 *
 * SAGE's own notification channel (distinct from the scheduler's) so alerts show
 * an "ATTENTION" badge and never inherit the scheduler's chime.  @level is
 * "alert" (accompanied by voice) or "ambient" (silent banner).  Weak default
 * stub when built without the WebUI; strong override in webui_broadcasts.c.
 */
void webui_broadcast_attention_alert(int user_id, const char *summary, const char *level);

/** Reload the in-memory watch cache from the DB (after any mutation). */
int attention_reload(void);

/** Copy out the current self-monitoring counters. */
void attention_get_metrics(attention_metrics_t *out);

/* =============================================================================
 * Metric catalog (the NL <-> metric bridge; used by the tool + WebUI)
 * ============================================================================= */

/** Number of catalog metrics. */
int attention_catalog_count(void);

/** Catalog key at index i (stable enum for the tool schema), or NULL. */
const char *attention_catalog_key(int i);

/** Human label for a catalog key (e.g. "CO2"), or NULL if unknown. */
const char *attention_catalog_label(const char *key);

/** Unit string for a catalog key (e.g. "ppm"), or "" if none/unknown. */
const char *attention_catalog_unit(const char *key);

/** True if @key is a known catalog metric. */
bool attention_catalog_has(const char *key);

/**
 * Current live value for a catalog metric (snapshots its source, runs the
 * reader).  For absence metrics @value is the feed age in seconds.
 * @return true if the value is present, false if the source hasn't reported.
 */
bool attention_metric_current(const char *key, double *value);

/**
 * Snapshot the live reading of every watch owned by @user_id.  Samples all metric
 * sources ONCE, then reads each of the user's watches from the in-memory cache —
 * so N watches cost one ingest, unlike N separate attention_metric_current calls.
 * Fills up to @max entries into @out and returns the count via @out_count.  A user
 * with no watches yields count 0 (still SUCCESS).  This is the per-tick source for
 * the WebUI watch_readings gauge stream.
 *
 * Unlike attention_tick (which skips disabled/muted watches from evaluation), this
 * reports ALL of the user's watches regardless of enabled/muted state — the panel
 * gauge shows the live value even for a paused watch, so the reading set matches
 * the watch_list set the client already renders.
 * @return SUCCESS (even for 0 watches); FAILURE only on a bad argument.
 */
int attention_readings_snapshot(int user_id, sage_reading_t *out, int max, int *out_count);

/* =============================================================================
 * Enum <-> wire-string serialization
 *
 * The `attention` LLM tool and the WebUI Watches panel both read and write the
 * same attention_rules rows, so the string forms of these enums are a wire
 * contract — a watch created by voice must render identically in the panel.
 * These are the canonical serializers (kept beside the enums they mirror) so the
 * two surfaces can't drift.  *_from_str return @fallback for NULL/empty/unknown.
 * ============================================================================= */

const char *sage_notify_to_str(sage_notify_t n);
sage_notify_t sage_notify_from_str(const char *s, sage_notify_t fallback);
const char *sage_direction_to_str(sage_direction_t d);
sage_direction_t sage_direction_from_str(const char *s, sage_direction_t fallback);
const char *sage_rule_type_to_str(sage_rule_type_t r);

/** Clamp a seconds value (e.g. an absence window) to a sane [0, 604800] range. */
int attention_clamp_seconds(double v);

/* =============================================================================
 * Watch management (tool + WebUI CRUD; each mutation reloads the cache)
 * ============================================================================= */

/**
 * Populate @out with the catalog defaults for @metric (rule type, direction,
 * threshold, hysteresis, absence window, notify level) for @user_id — the
 * starting point the tool/WebUI override before attention_watch_add.  Returns
 * FAILURE if @metric is not a known catalog key.
 */
int attention_watch_template(const char *metric, int user_id, sage_watch_t *out);

/** Create/enable a watch. @out_id optional. NAN threshold => catalog default. */
int attention_watch_add(const sage_watch_t *watch, int64_t *out_id);

/** Update threshold/notify/direction on an existing watch (by id, user-scoped). */
int attention_watch_update(int user_id, int64_t id, const sage_watch_t *watch);

/** Enable/disable a watch by id (user-scoped). */
int attention_watch_set_enabled(int user_id, int64_t id, bool enabled);

/** Delete a watch by id (user-scoped). */
int attention_watch_remove(int user_id, int64_t id);

/** Copy up to @max watches for @user_id into @out; returns count via @out_count. */
int attention_watch_list(int user_id, sage_watch_t *out, int max, int *out_count);

/** Find the first watch on @metric for @user_id (for "ignore that temp"). */
int attention_watch_find_by_metric(int user_id, const char *metric, sage_watch_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ATTENTION_H */
