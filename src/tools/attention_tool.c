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
 * `attention` tool — the conversational surface for SAGE watches.  Lets the user
 * tell DAWN what to keep an eye on and what to ignore, at runtime:
 *   "keep an eye on the CO2"                 -> watch (catalog default threshold)
 *   "tell me if CO2 goes above 1200"         -> watch (explicit threshold)
 *   "what are you watching?"                 -> list (with live values)
 *   "ignore that temperature for now"        -> ignore (disable)
 *   "stop watching the battery"              -> remove
 * Watches persist as per-user attention_rules rows; this tool is the primary
 * management surface (the WebUI Watches panel is the other).
 */

#include "tools/attention_tool.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config/dawn_config.h"
#include "core/attention/attention.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/tool_registry.h"

#define ATTN_RESULT_MAX 1536

/* ========== Forward declarations ========== */
static char *attention_tool_callback(const char *action, char *value, int *should_respond);
static bool attention_tool_is_available(void);

/* ========== Parameters ========== */
/* NON-const: the `metric` param's description is generated from the catalog at
 * registration (attention_tool_register) so the metric vocabulary has ONE source
 * of truth (attention_catalog.c) and no 16-entry enum cap.  See s_metric_desc. */
static treg_param_t attention_params[] = {
   {
       .name = "action",
       .description = "watch (start watching a metric), list (what am I watching, with live "
                      "values), ignore (disable a watch), set (change a watch's threshold/level), "
                      "or remove (delete a watch).",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "watch", "list", "ignore", "set", "remove" },
       .enum_count = 5,
   },
   {
       .name = "metric",
       /* .description is filled at registration from the catalog (see below).
        * Free-form string (not an enum) so the vocabulary isn't capped at 16 and
        * isn't a second list to maintain; the callback validates against the
        * catalog and lists valid keys on a miss. */
       .description = "Which signal to watch. Set at registration.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
   {
       .name = "threshold",
       .description = "Optional numeric trigger point (e.g. 1200 for CO2 ppm, 20 for battery %). "
                      "If omitted a sensible default for the metric is used. For a 'silent feed' "
                      "watch this is the number of seconds of silence.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "threshold",
   },
   {
       .name = "direction",
       .description = "Optional: 'above' or 'below' the threshold (defaults to the natural "
                      "direction for the metric — below for battery/voltage, above for CO2/temp).",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "direction",
       .enum_values = { "above", "below" },
       .enum_count = 2,
   },
   {
       .name = "level",
       .description =
           "How loud the alert is — CHOOSE FROM THE USER'S WORDING, not the metric. Use 'alert' "
           "(spoken aloud + banner) whenever they ask to be told/alerted/notified/warned ('alert "
           "me', 'tell me', 'let me know', 'warn me', 'notify me') — this is the common case. Use "
           "'ambient' (silent banner, no voice) only when they clearly want it unobtrusive ('just "
           "show it', 'quietly', 'don't interrupt', 'log it', 'keep it on the HUD'). Omit ONLY if "
           "they gave no cue about how loud — then the metric's natural default applies.",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "level",
       .enum_values = { "alert", "ambient" },
       .enum_count = 2,
   },
};

/* ========== Metadata ========== */
static const tool_metadata_t attention_metadata = {
   .name = "attention",
   .device_string = "attention",
   .topic = "dawn",
   .aliases = { "watch", "watches" },
   .alias_count = 2,

   .description =
       "Manage what DAWN proactively watches and alerts on. Use 'watch' when the user asks you to "
       "keep an eye on something ('watch the CO2', 'tell me if the battery gets low', 'let me know "
       "if the helmet link drops'); 'list' for 'what are you watching?'; 'ignore' to quiet a watch "
       "('ignore that temperature for now'); 'set' to change a threshold; 'remove' to delete one. "
       "Watches run against live suit/system telemetry and alert the user when they trip. This is "
       "for setting up PROACTIVE alerts — to read a current value right now, use suit_status or "
       "system_status instead.",
   .params = attention_params,
   .param_count = 5,

   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_NONE,
   .skip_followup = false,
   .default_local = true,
   .default_remote = true,

   .is_available = attention_tool_is_available,
   .callback = attention_tool_callback,
};

/* ========== Helpers ========== */

/* Notify-level parse/format + absence-window clamp are shared with the WebUI
 * Watches panel via the attention core (sage_notify_from_str / _to_str /
 * attention_clamp_seconds) so the two surfaces can't drift on the wire vocab. */

/* Append a "(disabled — enable proactive attention in settings)" note when the
 * master switch is off, so a freshly-created watch honestly reports it's dormant. */
static void append_disabled_note(char *buf, size_t sz) {
   if (!g_config.attention.enabled) {
      size_t off = strlen(buf);
      snprintf(buf + off, sz - off,
               " (Proactive attention is currently off, so this won't fire until it's enabled in "
               "settings.)");
   }
}

static char *err(const char *msg) {
   size_t n = strlen(TOOL_RESULT_ERROR_MARK) + strlen(msg) + 1;
   char *out = malloc(n);
   if (out) {
      snprintf(out, n, "%s%s", TOOL_RESULT_ERROR_MARK, msg);
   }
   return out;
}

/* Bounded formatted append: writes at *off and advances it, clamped to sz.  A
 * truncating OR failed (negative-return) write pins *off at sz so no later call
 * can underflow (sz - *off) or index past the buffer. */
static void append_fmt(char *buf, size_t sz, size_t *off, const char *fmt, ...) {
   if (*off >= sz) {
      return;
   }
   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(buf + *off, sz - *off, fmt, ap);
   va_end(ap);
   if (n < 0) {
      *off = sz;
      return;
   }
   *off += (size_t)n;
   if (*off > sz) {
      *off = sz;
   }
}

/* ========== Actions ========== */

static char *do_list(int user_id, char *out) {
   sage_watch_t watches[SAGE_MAX_WATCHES_PER_USER];
   int count = 0;
   if (attention_watch_list(user_id, watches, SAGE_MAX_WATCHES_PER_USER, &count) != SUCCESS) {
      return err("attention: couldn't read your watches.");
   }
   if (count == 0) {
      snprintf(out, ATTN_RESULT_MAX,
               "Not watching anything right now. Ask me to keep an eye on the CO2, the battery, "
               "the helmet link, and so on.");
      return out;
   }
   size_t off = 0;
   append_fmt(out, ATTN_RESULT_MAX, &off, "Watching %d thing%s: ", count, count == 1 ? "" : "s");
   for (int i = 0; i < count && off < ATTN_RESULT_MAX; i++) {
      const sage_watch_t *w = &watches[i];
      const char *label = attention_catalog_label(w->metric);
      const char *unit = attention_catalog_unit(w->metric);
      double cur = 0.0;
      bool have = attention_metric_current(w->metric, &cur);

      append_fmt(out, ATTN_RESULT_MAX, &off, "%s", label ? label : w->metric);
      if (have) {
         if (w->rule_type == SAGE_RULE_ABSENCE) {
            append_fmt(out, ATTN_RESULT_MAX, &off, " (silent %.0fs)", cur);
         } else {
            append_fmt(out, ATTN_RESULT_MAX, &off, " (now %g%s%s)", cur, unit[0] ? " " : "", unit);
         }
      }
      if (w->rule_type == SAGE_RULE_ABSENCE) {
         append_fmt(out, ATTN_RESULT_MAX, &off, " — alerts if silent over %ds",
                    w->absence_after_sec);
      } else {
         append_fmt(out, ATTN_RESULT_MAX, &off, " — %s %s %g%s", sage_notify_to_str(w->notify),
                    w->direction == SAGE_DIR_BELOW ? "below" : "above", w->threshold,
                    unit[0] ? unit : "");
      }
      if (!w->enabled) {
         append_fmt(out, ATTN_RESULT_MAX, &off, " [paused]");
      }
      append_fmt(out, ATTN_RESULT_MAX, &off, "%s", (i + 1 < count) ? "; " : ".");
   }
   return out;
}

/* Append the metric's LIVE value to a confirmation so DAWN states the real
 * reading instead of guessing (same snapshot the `list` action uses).  Says
 * nothing when the source isn't reporting — better silent than invented. */
static void append_current_value(char *out, size_t sz, const sage_watch_t *w) {
   double cur = 0.0;
   if (!attention_metric_current(w->metric, &cur)) {
      return;
   }
   size_t off = strlen(out);
   if (w->rule_type == SAGE_RULE_ABSENCE) {
      if (cur > (double)w->absence_after_sec) {
         append_fmt(out, sz, &off, " It's already been silent %.0fs (past the window).", cur);
      } else {
         append_fmt(out, sz, &off, " It's reporting normally right now.");
      }
      return;
   }
   const char *unit = attention_catalog_unit(w->metric);
   bool tripped = (w->direction == SAGE_DIR_BELOW) ? (cur < w->threshold) : (cur > w->threshold);
   append_fmt(out, sz, &off, " Currently %g%s%s%s.", cur, unit[0] ? " " : "", unit,
              tripped ? " — already past the threshold" : "");
}

/* Parse a user-supplied threshold string; false unless it's a finite number
 * (rejects garbage like "high" that atof would silently turn into 0.0). */
static bool parse_threshold(const char *s, double *out) {
   char *end = NULL;
   double v = strtod(s, &end);
   while (*end == ' ' || *end == '\t') {
      end++;
   }
   if (end == s || *end != '\0' || !isfinite(v)) {
      return false;
   }
   *out = v;
   return true;
}

/* Apply an optional user threshold onto @w; returns false (with the reason in
 * @out) if the string was non-numeric. */
static bool apply_threshold(sage_watch_t *w, const char *threshold, char **err_out) {
   if (!threshold || !threshold[0]) {
      return true;
   }
   double t;
   if (!parse_threshold(threshold, &t)) {
      *err_out = err("attention: I didn't catch a number for that threshold.");
      return false;
   }
   if (w->rule_type == SAGE_RULE_ABSENCE) {
      w->absence_after_sec = attention_clamp_seconds(t);
   } else {
      w->threshold = t;
   }
   return true;
}

static char *do_watch(int user_id,
                      const char *metric,
                      const char *threshold,
                      const char *direction,
                      const char *level,
                      char *out) {
   sage_watch_t w;
   if (attention_watch_template(metric, user_id, &w) != SUCCESS) {
      return err("attention: I don't have a sensor for that. Try CO2, battery, temperature, "
                 "humidity, armor voltage, or the helmet link.");
   }
   if (direction && direction[0]) {
      w.direction = (strcasecmp(direction, "below") == 0) ? SAGE_DIR_BELOW : SAGE_DIR_ABOVE;
   }
   w.notify = sage_notify_from_str(level, w.notify);
   char *terr = NULL;
   if (!apply_threshold(&w, threshold, &terr)) {
      return terr;
   }

   /* Update-or-create: one watch per metric keeps the conversational model
    * coherent (set/ignore/remove address it unambiguously by metric). */
   sage_watch_t existing;
   bool updating = (attention_watch_find_by_metric(user_id, metric, &existing) == SUCCESS);
   int rc = updating ? attention_watch_update(user_id, existing.id, &w)
                     : attention_watch_add(&w, NULL);
   if (rc != SUCCESS) {
      return err("attention: couldn't set that watch (you may be at the watch limit).");
   }
   if (updating && !existing.enabled) {
      attention_watch_set_enabled(user_id, existing.id, true); /* re-watching re-activates */
   }

   const char *unit = attention_catalog_unit(metric);
   if (w.rule_type == SAGE_RULE_ABSENCE) {
      snprintf(out, ATTN_RESULT_MAX, "Watching %s — I'll speak up if it goes silent for over %ds.",
               w.name, w.absence_after_sec);
   } else {
      snprintf(out, ATTN_RESULT_MAX, "Watching %s — %s if it goes %s %g%s.", w.name,
               w.notify == SAGE_NOTIFY_ALERT ? "I'll tell you" : "I'll flag it quietly",
               w.direction == SAGE_DIR_BELOW ? "below" : "above", w.threshold, unit[0] ? unit : "");
   }
   append_current_value(out, ATTN_RESULT_MAX, &w);
   append_disabled_note(out, ATTN_RESULT_MAX);
   return out;
}

static char *do_ignore(int user_id, const char *metric, char *out) {
   sage_watch_t w;
   int rc = attention_watch_find_by_metric(user_id, metric, &w);
   if (rc != SUCCESS) {
      return err("attention: I'm not watching that, so there's nothing to ignore.");
   }
   if (attention_watch_set_enabled(user_id, w.id, false) != SUCCESS) {
      return err("attention: couldn't pause that watch.");
   }
   snprintf(out, ATTN_RESULT_MAX, "Okay, I'll stop watching %s.", w.name);
   return out;
}

static char *do_remove(int user_id, const char *metric, char *out) {
   sage_watch_t w;
   int rc = attention_watch_find_by_metric(user_id, metric, &w);
   if (rc != SUCCESS) {
      return err("attention: I'm not watching that.");
   }
   if (attention_watch_remove(user_id, w.id) != SUCCESS) {
      return err("attention: couldn't remove that watch.");
   }
   snprintf(out, ATTN_RESULT_MAX, "Removed the watch on %s.", w.name);
   return out;
}

static char *do_set(int user_id,
                    const char *metric,
                    const char *threshold,
                    const char *direction,
                    const char *level,
                    char *out) {
   sage_watch_t w;
   int rc = attention_watch_find_by_metric(user_id, metric, &w);
   if (rc != SUCCESS) {
      /* Nothing to change yet — treat as a fresh watch. */
      return do_watch(user_id, metric, threshold, direction, level, out);
   }
   if (direction && direction[0]) {
      w.direction = (strcasecmp(direction, "below") == 0) ? SAGE_DIR_BELOW : SAGE_DIR_ABOVE;
   }
   w.notify = sage_notify_from_str(level, w.notify);
   char *terr = NULL;
   if (!apply_threshold(&w, threshold, &terr)) {
      return terr;
   }
   if (attention_watch_update(user_id, w.id, &w) != SUCCESS) {
      return err("attention: couldn't update that watch.");
   }
   const char *unit = attention_catalog_unit(metric);
   if (w.rule_type == SAGE_RULE_ABSENCE) {
      snprintf(out, ATTN_RESULT_MAX, "Updated %s — silent over %ds.", w.name, w.absence_after_sec);
   } else {
      snprintf(out, ATTN_RESULT_MAX, "Updated %s — %s %s %g%s.", w.name,
               sage_notify_to_str(w.notify), w.direction == SAGE_DIR_BELOW ? "below" : "above",
               w.threshold, unit[0] ? unit : "");
   }
   append_current_value(out, ATTN_RESULT_MAX, &w);
   return out;
}

/* ========== Callback ========== */

static char *attention_tool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1;
   if (!action || !action[0]) {
      return err("attention: missing action.");
   }

   int user_id = tool_get_current_user_id();

   char metric[SAGE_METRIC_LEN] = { 0 };
   char threshold[32] = { 0 };
   char direction[16] = { 0 };
   char level[16] = { 0 };
   if (value) {
      tool_param_extract_base(value, metric, sizeof(metric));
      tool_param_extract_custom(value, "threshold", threshold, sizeof(threshold));
      tool_param_extract_custom(value, "direction", direction, sizeof(direction));
      tool_param_extract_custom(value, "level", level, sizeof(level));
   }

   char *out = malloc(ATTN_RESULT_MAX);
   if (!out) {
      return err("attention: out of memory.");
   }
   out[0] = '\0';

   if (strcasecmp(action, "list") == 0) {
      char *r = do_list(user_id, out);
      if (r != out) {
         free(out);
      }
      return r;
   }

   /* All other actions need a metric. */
   if (!metric[0]) {
      free(out);
      return err("attention: which signal? (e.g. CO2, battery, helmet link)");
   }
   if (!attention_catalog_has(metric)) {
      /* Self-correcting: list what IS watchable (from the catalog) so the model
       * can retry with a valid key. */
      size_t off = 0;
      append_fmt(out, ATTN_RESULT_MAX, &off,
                 "%sattention: no sensor called '%s'. I can watch: ", TOOL_RESULT_ERROR_MARK,
                 metric);
      int n = attention_catalog_count();
      for (int i = 0; i < n; i++) {
         const char *key = attention_catalog_key(i);
         append_fmt(out, ATTN_RESULT_MAX, &off, "%s%s", i ? ", " : "",
                    attention_catalog_label(key));
      }
      append_fmt(out, ATTN_RESULT_MAX, &off, ".");
      return out;
   }

   char *r = out;
   if (strcasecmp(action, "watch") == 0) {
      r = do_watch(user_id, metric, threshold, direction, level, out);
   } else if (strcasecmp(action, "ignore") == 0) {
      r = do_ignore(user_id, metric, out);
   } else if (strcasecmp(action, "remove") == 0) {
      r = do_remove(user_id, metric, out);
   } else if (strcasecmp(action, "set") == 0) {
      r = do_set(user_id, metric, threshold, direction, level, out);
   } else {
      free(out);
      return err("attention: unknown action.");
   }
   if (r != out) {
      free(out);
   }
   return r;
}

/* ========== Lifecycle ========== */

/* Always available so the conversational setup works even before the master
 * switch is on; a watch created while disabled persists and activates when
 * enabled (the callback says so). */
static bool attention_tool_is_available(void) {
   return true;
}

/* Description for the `metric` param, generated once from the catalog so the
 * watchable vocabulary has a single source of truth (attention_catalog.c).
 * Held static: the registry retains the pointer for the process lifetime. */
static char s_metric_desc[2048];

static void build_metric_description(void) {
   size_t off = 0;
   append_fmt(s_metric_desc, sizeof(s_metric_desc), &off,
              "Which signal to watch (required for watch/ignore/set/remove; omit for list). "
              "Map the user's words to the closest key — stat.* is this compute unit, suit.* is "
              "the helmet/armor, component.* is a link. Available: ");
   int n = attention_catalog_count();
   for (int i = 0; i < n; i++) {
      const char *key = attention_catalog_key(i);
      if (!key) {
         continue;
      }
      const char *label = attention_catalog_label(key);
      append_fmt(s_metric_desc, sizeof(s_metric_desc), &off, "%s%s (%s)", i ? ", " : "", key,
                 label ? label : "");
   }
   append_fmt(s_metric_desc, sizeof(s_metric_desc), &off, ".");
}

int attention_tool_register(void) {
   /* Generate the metric-param description from the catalog and install it before
    * registration (the registry reads params[].description at schema-gen time). */
   build_metric_description();
   for (size_t i = 0; i < sizeof(attention_params) / sizeof(attention_params[0]); i++) {
      if (strcmp(attention_params[i].name, "metric") == 0) {
         attention_params[i].description = s_metric_desc;
         break;
      }
   }
   return tool_registry_register(&attention_metadata);
}
