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
 * SAGE gate — pure, explicit-state rule evaluation.  Threshold (with hysteresis
 * re-arm), slope (least-recent vs most-recent over a window ring), and absence
 * (feed silent > N sec), plus exponential-backoff novelty suppression and TTL
 * stamping.  No globals: every function takes its state by pointer, so the whole
 * gate is unit-testable with a synthetic clock.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core/attention/attention_internal.h"

/* Bounded copy — kept local so the gate stays dependency-free for unit tests. */
static void gate_strcpy(char *dst, const char *src, size_t n) {
   if (n == 0) {
      return;
   }
   strncpy(dst, src, n - 1);
   dst[n - 1] = '\0';
}

/* True if the watch's raw condition is met right now (pre-hysteresis/backoff). */
static bool condition_met(const sage_watch_t *w, double value) {
   switch (w->rule_type) {
      case SAGE_RULE_THRESHOLD:
         return (w->direction == SAGE_DIR_BELOW) ? (value < w->threshold) : (value > w->threshold);
      case SAGE_RULE_ABSENCE:
         /* value is the feed age in seconds. */
         return value > (double)w->absence_after_sec;
      case SAGE_RULE_SLOPE:
      case SAGE_RULE_MATCH:
      default:
         return false; /* slope handled separately; match is a P1 push path */
   }
}

/* True once the signal has recovered enough to re-arm the watch. */
static bool recovered(const sage_watch_t *w, double value) {
   switch (w->rule_type) {
      case SAGE_RULE_THRESHOLD:
         return (w->direction == SAGE_DIR_BELOW) ? (value >= w->threshold + w->hysteresis)
                                                 : (value <= w->threshold - w->hysteresis);
      case SAGE_RULE_ABSENCE:
         /* Feed resumed (age back under the window) => re-arm. */
         return value <= (double)w->absence_after_sec;
      default:
         return true;
   }
}

/* Push a sample into the slope ring and compute slope (units/min) over the
 * configured window using the oldest in-window sample vs the newest. */
static bool slope_over_window(const sage_watch_t *w,
                              sage_rule_state_t *st,
                              double value,
                              int64_t now_ms,
                              double *slope_out) {
   int idx = st->hist_head;
   st->hist_val[idx] = value;
   st->hist_ts[idx] = now_ms;
   st->hist_head = (st->hist_head + 1) % SAGE_SLOPE_SAMPLES;
   if (st->hist_count < SAGE_SLOPE_SAMPLES) {
      st->hist_count++;
   }

   int win = w->slope_window_sec > 0 ? w->slope_window_sec : SAGE_SLOPE_WINDOW_SEC_DEFAULT;
   int64_t window_ms = (int64_t)win * 1000;

   /* Find the oldest sample still within the window. */
   double oldest_val = value;
   int64_t oldest_ts = now_ms;
   for (int k = 0; k < st->hist_count; k++) {
      int i = (st->hist_head - 1 - k + SAGE_SLOPE_SAMPLES * 2) % SAGE_SLOPE_SAMPLES;
      if (now_ms - st->hist_ts[i] > window_ms) {
         break;
      }
      oldest_val = st->hist_val[i];
      oldest_ts = st->hist_ts[i];
   }
   int64_t dt_ms = now_ms - oldest_ts;
   if (dt_ms <= 0) {
      return false; /* need at least two samples spanning time */
   }
   *slope_out = (value - oldest_val) / ((double)dt_ms / 60000.0); /* per minute */
   return true;
}

static const char *rule_type_str(sage_rule_type_t t) {
   switch (t) {
      case SAGE_RULE_THRESHOLD:
         return "threshold";
      case SAGE_RULE_SLOPE:
         return "slope";
      case SAGE_RULE_ABSENCE:
         return "absence";
      default:
         return "match";
   }
}

/* Compose the human-readable, ready-to-speak event summary. */
static void build_summary(const sage_watch_t *w,
                          const attention_catalog_entry_t *cat,
                          double value,
                          char *out,
                          size_t out_sz) {
   const char *label = (cat && cat->label) ? cat->label : w->metric;
   const char *unit = (cat && cat->unit) ? cat->unit : "";

   /* The reading clause (unnamed watches speak exactly this; named watches append
    * it after the name so the alert stays actionable — D2: name + value). */
   char reading[SAGE_SUMMARY_LEN];
   if (w->rule_type == SAGE_RULE_ABSENCE) {
      snprintf(reading, sizeof(reading), "%s has been silent for %.0f seconds", label, value);
   } else if (w->rule_type == SAGE_RULE_SLOPE) {
      snprintf(reading, sizeof(reading), "%s is changing rapidly (now %g %s)", label, value, unit);
   } else if (unit[0]) {
      snprintf(reading, sizeof(reading), "%s is %g %s", label, value, unit);
   } else {
      snprintf(reading, sizeof(reading), "%s is %g", label, value);
   }

   if (!w->named || !w->name[0]) {
      snprintf(out, out_sz, "%s", reading); /* unnamed: today's format, unchanged */
      return;
   }
   /* Named watch: speak the user's name so the alert is specific.  Threshold
    * watches get the "over/under the '<name>' threshold" flavour; other kinds use a
    * neutral "'<name>':" prefix. */
   if (w->rule_type == SAGE_RULE_THRESHOLD) {
      snprintf(out, out_sz, "you're %s the '%s' threshold — %s",
               (w->direction == SAGE_DIR_BELOW) ? "under" : "over", w->name, reading);
   } else {
      snprintf(out, out_sz, "'%s' — %s", w->name, reading);
   }
}

/* Update the backoff window after a fire. */
static void advance_backoff(sage_rule_state_t *st, int64_t now_ms) {
   if (st->last_fired_ms != 0 && (now_ms - st->last_fired_ms) <= SAGE_BACKOFF_RESET_MS &&
       st->backoff_ms > 0) {
      int64_t next = st->backoff_ms * 2;
      st->backoff_ms = (next > SAGE_BACKOFF_MAX_MS) ? SAGE_BACKOFF_MAX_MS : next;
   } else {
      st->backoff_ms = SAGE_BACKOFF_INITIAL_MS;
   }
   st->last_fired_ms = now_ms;
}

/* True if we are still inside the suppression window from the last fire. */
static bool in_backoff(const sage_rule_state_t *st, int64_t now_ms) {
   return st->last_fired_ms != 0 && (now_ms - st->last_fired_ms) < st->backoff_ms;
}

static void fill_event(const sage_watch_t *w,
                       const attention_catalog_entry_t *cat,
                       double value,
                       int64_t now_ms,
                       sage_event_t *ev) {
   memset(ev, 0, sizeof(*ev));
   snprintf(ev->event_key, sizeof(ev->event_key), "%s#%lld", w->metric, (long long)w->id);
   gate_strcpy(ev->source, (cat && cat->source) ? cat->source : "sage", sizeof(ev->source));
   gate_strcpy(ev->type, rule_type_str(w->rule_type), sizeof(ev->type));
   build_summary(w, cat, value, ev->summary, sizeof(ev->summary));
   gate_strcpy(ev->gate_rule, w->name[0] ? w->name : w->metric, sizeof(ev->gate_rule));
   ev->notify = w->notify;
   gate_strcpy(ev->privacy, "public", sizeof(ev->privacy));
   ev->observed_at = now_ms;
   int ttl_min = w->ttl_min > 0 ? w->ttl_min : SAGE_TTL_DEFAULT_MIN;
   ev->ttl_ms = (int64_t)ttl_min * 60 * 1000;
   ev->rule_id = w->id;
   ev->user_id = w->user_id;
   ev->magnitude = 1.0; /* P1 will normalize per-source; P0 fires binary */
}

bool attention_gate_eval(const sage_watch_t *w,
                         sage_rule_state_t *st,
                         double value,
                         bool present,
                         int64_t now_ms,
                         const attention_catalog_entry_t *cat,
                         sage_event_t *out_event) {
   if (!w || !st || !out_event) {
      return false;
   }

   /* Initialize state on first sight. */
   if (st->rule_id != w->id) {
      memset(st, 0, sizeof(*st));
      st->rule_id = w->id;
      st->armed = true;
   }

   if (!present) {
      /* Source hasn't reported — cannot evaluate.  Do not touch armed state. */
      return false;
   }

   bool met;
   if (w->rule_type == SAGE_RULE_SLOPE) {
      double slope = 0.0;
      if (!slope_over_window(w, st, value, now_ms, &slope)) {
         return false;
      }
      met = (w->direction == SAGE_DIR_FALLING) ? (slope <= -w->slope_per_min)
                                               : (slope >= w->slope_per_min);
   } else {
      met = condition_met(w, value);
   }

   if (!met) {
      if (recovered(w, value)) {
         st->armed = true;
      }
      return false;
   }

   if (!st->armed || in_backoff(st, now_ms)) {
      return false;
   }

   /* Fire. */
   st->armed = false;
   advance_backoff(st, now_ms);
   fill_event(w, cat, value, now_ms, out_event);
   return true;
}

bool attention_gate_admit(sage_event_t *ev, sage_rule_state_t *st, int64_t now_ms) {
   /* P1 push path: pre-formed event, apply TTL + backoff only. */
   if (!ev || !st) {
      return false;
   }
   if (ev->ttl_ms > 0 && (ev->observed_at + ev->ttl_ms) < now_ms) {
      return false; /* expired */
   }
   if (in_backoff(st, now_ms)) {
      return false;
   }
   advance_backoff(st, now_ms);
   return true;
}
