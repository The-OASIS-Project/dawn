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
 *   "warn me if the temp is rising fast"     -> watch (rate/slope: direction+rate)
 *   "call the high one 'we're going to die'" -> watch (custom name)
 *   "what are you watching?"                 -> list (with live values + ids)
 *   "ignore that temperature for now"        -> ignore (by name/id, or a whole
 *                                               signal/component group)
 *   "turn the CO2 alert back on"             -> resume
 *   "stop watching the battery"              -> remove
 * A metric can hold several named watches (min AND max, tiers).  Targeted actions
 * resolve id > name > metric/prefix, list-and-ask on an ambiguous set/remove.
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
#include "core/path_utils.h"      /* safe_strncpy */
#include "core/session_manager.h" /* session_get_command_context — no-session backstop */
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
       .description =
           "watch (start a NEW watch — a signal can have several, e.g. a min AND a max, or "
           "warn/critical tiers), list (what am I watching, with live values + ids), set (change "
           "an existing watch's trigger/level/name — target it by name or id), ignore (pause), "
           "resume (unpause), or remove (delete). ignore/resume/remove can target one watch (name "
           "or id) OR a whole signal/component group (metric).",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "watch", "list", "set", "ignore", "resume", "remove" },
       .enum_count = 6,
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
       .name = "name",
       .description = "The name of a SPECIFIC watch — the main way to target one when a signal "
                      "has several (e.g. \"we're going to die\"). For 'watch', an optional custom "
                      "name for the new watch (omit to auto-name it from the condition).",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "name",
   },
   {
       .name = "id",
       .description = "A watch's numeric id from 'list' — the exact way to target one watch when "
                      "a name is ambiguous.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "id",
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
       .name = "rate",
       .description =
           "For a rising/falling watch: the rate of change per MINUTE that triggers it "
           "(e.g. 5 for 5 per minute). A positive number; the sign comes from direction.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "rate",
   },
   {
       .name = "direction",
       .description = "How the watch triggers: 'above'/'below' a level (a threshold watch), or "
                      "'rising'/'falling' fast (a rate watch — requires 'rate'). Defaults to the "
                      "metric's natural direction.",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "direction",
       .enum_values = { "above", "below", "rising", "falling" },
       .enum_count = 4,
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
   {
       .name = "duration",
       .description =
           "Optional: how long to 'ignore' for (e.g. '1 hour'). NOT YET SUPPORTED — "
           "omit it; ignore lasts until you 'resume'. Reserved for a future timed snooze.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "duration",
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
   .param_count = 9,

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

/* Render a watch's trigger condition, e.g. "above 1200 ppm" / "rising 5 /min" /
 * "silent over 30s". */
static void describe_condition(const sage_watch_t *w, char *buf, size_t sz) {
   const char *unit = attention_catalog_unit(w->metric);
   if (w->rule_type == SAGE_RULE_ABSENCE) {
      snprintf(buf, sz, "silent over %ds", w->absence_after_sec);
   } else if (w->rule_type == SAGE_RULE_SLOPE) {
      snprintf(buf, sz, "%s %g%s/min", (w->direction == SAGE_DIR_FALLING) ? "falling" : "rising",
               w->slope_per_min, unit[0] ? unit : "");
   } else {
      snprintf(buf, sz, "%s %g%s", (w->direction == SAGE_DIR_BELOW) ? "below" : "above",
               w->threshold, unit[0] ? unit : "");
   }
}

/* Resolve target watches for a targeted action from the id / name / metric
 * selectors, in that priority:
 *   id -> that one watch (0 or 1); else
 *   name -> that one watch (0 or 1); a name miss falls through to metric; else
 *   metric -> all watches on that metric or component prefix (0..N).
 * Fills up to @max into @out, count via @out_count.  SUCCESS even for 0 matches;
 * FAILURE only on a DB error. */
static int resolve_targets(int user_id,
                           int64_t id,
                           const char *name,
                           const char *metric,
                           sage_watch_t *out,
                           int max,
                           int *out_count) {
   *out_count = 0;
   if (id > 0) {
      if (attention_watch_find_by_id(user_id, id, &out[0]) == SUCCESS) {
         *out_count = 1;
      }
      return SUCCESS;
   }
   if (name && name[0]) {
      if (attention_watch_find_by_name(user_id, name, &out[0]) == SUCCESS) {
         *out_count = 1;
         return SUCCESS;
      }
      if (!(metric && metric[0])) {
         return SUCCESS; /* name miss, no metric to fall back to */
      }
   }
   if (metric && metric[0]) {
      return attention_watch_list_by_metric_prefix(user_id, metric, out, max, out_count);
   }
   return SUCCESS;
}

/* Render a numbered "which one?" reply for an ambiguous single-target action. */
static char *ambiguity_reply(const sage_watch_t *ws, int n, const char *verb, char *out) {
   size_t off = 0;
   append_fmt(out, ATTN_RESULT_MAX, &off, "That matches %d watches — which one to %s? ", n, verb);
   for (int i = 0; i < n && off < ATTN_RESULT_MAX; i++) {
      append_fmt(out, ATTN_RESULT_MAX, &off, "%s'%s' [id=%lld]", i ? ", " : "", ws[i].name,
                 (long long)ws[i].id);
   }
   append_fmt(out, ATTN_RESULT_MAX, &off, ". Say the name or the id.");
   return out;
}

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
      const char *unit = attention_catalog_unit(w->metric);
      double cur = 0.0;
      bool have = attention_metric_current(w->metric, &cur);
      char cond[96];
      describe_condition(w, cond, sizeof(cond));

      append_fmt(out, ATTN_RESULT_MAX, &off, "'%s' [id=%lld]", w->name, (long long)w->id);
      if (have) {
         if (w->rule_type == SAGE_RULE_ABSENCE) {
            append_fmt(out, ATTN_RESULT_MAX, &off, " (silent %.0fs)", cur);
         } else {
            append_fmt(out, ATTN_RESULT_MAX, &off, " (now %g%s%s)", cur, unit[0] ? " " : "", unit);
         }
      }
      append_fmt(out, ATTN_RESULT_MAX, &off, " — %s", cond);
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
   if (w->rule_type == SAGE_RULE_SLOPE) {
      /* A rate watch's threshold field is a meaningless catalog default — just
       * state the current level, no "past the threshold". */
      append_fmt(out, sz, &off, " Currently %g%s%s.", cur, unit[0] ? " " : "", unit);
      return;
   }
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

/* Apply direction (+ infer slope kind) and the trigger value (rate for slope,
 * threshold otherwise) onto @w.  Returns NULL on success, or an error string. */
static char *apply_condition(sage_watch_t *w,
                             const char *direction,
                             const char *threshold,
                             const char *rate) {
   if (direction && direction[0]) {
      w->direction = sage_direction_from_str(direction, w->direction);
   }
   /* rising/falling => a rate (slope) watch (shared validator rejects absence). */
   if (w->direction == SAGE_DIR_RISING || w->direction == SAGE_DIR_FALLING) {
      const char *kerr = NULL;
      if (attention_resolve_rule_kind(w, "slope", &kerr) != SUCCESS) {
         return err(kerr ? kerr : "attention: that signal can't be watched by rate of change.");
      }
   }
   if (w->rule_type == SAGE_RULE_SLOPE) {
      if (rate && rate[0]) {
         double r;
         if (!parse_threshold(rate, &r)) {
            return err("attention: I didn't catch a number for that rate.");
         }
         w->slope_per_min = r;
      }
   } else {
      char *terr = NULL;
      if (!apply_threshold(w, threshold, &terr)) {
         return terr;
      }
   }
   return NULL;
}

static char *do_watch(int user_id,
                      const char *metric,
                      const char *name,
                      const char *threshold,
                      const char *rate,
                      const char *direction,
                      const char *level,
                      char *out) {
   sage_watch_t w;
   if (attention_watch_template(metric, user_id, &w) != SUCCESS) {
      return err("attention: I don't have a sensor for that. Try CO2, battery, temperature, "
                 "humidity, armor voltage, or the helmet link.");
   }
   char *cerr = apply_condition(&w, direction, threshold, rate);
   if (cerr) {
      return cerr;
   }
   w.notify = sage_notify_from_str(level, w.notify);

   /* Optional user-chosen name (sanitized here; core enforces uniqueness). */
   if (name && name[0]) {
      safe_strncpy(w.name, name, sizeof(w.name));
      attention_sanitize_name(w.name);
      w.named = (w.name[0] != '\0');
   }

   const char *verr = NULL;
   if (attention_validate_watch_trigger(&w, &verr) != SUCCESS) {
      return err(verr ? verr : "attention: that watch isn't valid.");
   }

   /* Additive with identical-condition dedup: a repeated identical request updates
    * the matching watch (and re-enables it) rather than creating a duplicate. */
   sage_watch_t same;
   bool dedup = (attention_watch_find_identical(user_id, &w, &same) == SUCCESS);
   int rc;
   if (dedup) {
      if (w.name[0] == '\0') {
         safe_strncpy(w.name, same.name, sizeof(w.name));
         w.named = same.named;
      }
      rc = attention_watch_update(user_id, same.id, &w);
      if (rc == SUCCESS && !same.enabled) {
         attention_watch_set_enabled(user_id, same.id, true);
      }
   } else {
      rc = attention_watch_add(&w, NULL);
   }
   if (rc == ATTENTION_NAME_TAKEN) {
      snprintf(out, ATTN_RESULT_MAX,
               "You already have a watch called '%s' — pick a different name.", w.name);
      return out;
   }
   if (rc != SUCCESS) {
      return err("attention: couldn't set that watch (you may be at the watch limit).");
   }

   /* Re-fetch the stored form so the confirmation says the final (auto or given)
    * name. */
   sage_watch_t stored;
   if (attention_watch_find_identical(user_id, &w, &stored) != SUCCESS) {
      stored = w;
   }
   char cond[96];
   describe_condition(&stored, cond, sizeof(cond));
   snprintf(out, ATTN_RESULT_MAX, "Watching '%s' — %s if %s.", stored.name,
            stored.notify == SAGE_NOTIFY_ALERT ? "I'll tell you" : "I'll flag it quietly", cond);
   append_current_value(out, ATTN_RESULT_MAX, &stored);
   append_disabled_note(out, ATTN_RESULT_MAX);
   return out;
}

/* ignore (enable=false) / resume (enable=true): act on ALL matching watches — a
 * name/id targets one, a metric/prefix targets the whole group. */
static char *do_toggle(int user_id,
                       bool enable,
                       int64_t id,
                       const char *name,
                       const char *metric,
                       const char *duration,
                       char *out) {
   if (!enable && duration && duration[0]) {
      /* P1 seam: timed snooze isn't wired yet — be honest rather than silently
       * ignoring the duration. */
      return err("attention: timed snooze isn't supported yet — I can ignore it until you ask me "
                 "to resume. Leave off the duration.");
   }
   sage_watch_t ws[SAGE_MAX_WATCHES_PER_USER];
   int n = 0;
   if (resolve_targets(user_id, id, name, metric, ws, SAGE_MAX_WATCHES_PER_USER, &n) != SUCCESS) {
      return err("attention: couldn't read your watches.");
   }
   if (n == 0) {
      return err(enable ? "attention: I couldn't find that watch to resume."
                        : "attention: I couldn't find that watch to ignore.");
   }
   int done = 0;
   for (int i = 0; i < n; i++) {
      if (attention_watch_set_enabled(user_id, ws[i].id, enable) == SUCCESS) {
         done++;
      }
   }
   if (done == 0) {
      return err(enable ? "attention: couldn't resume that watch."
                        : "attention: couldn't pause that watch.");
   }
   if (done == 1) {
      snprintf(out, ATTN_RESULT_MAX,
               enable ? "Resumed '%s'." : "Ignoring '%s' until you say resume.", ws[0].name);
   } else {
      snprintf(out, ATTN_RESULT_MAX, enable ? "Resumed %d watches." : "Ignoring %d watches.", done);
   }
   return out;
}

static char *do_remove(int user_id, int64_t id, const char *name, const char *metric, char *out) {
   sage_watch_t ws[SAGE_MAX_WATCHES_PER_USER];
   int n = 0;
   if (resolve_targets(user_id, id, name, metric, ws, SAGE_MAX_WATCHES_PER_USER, &n) != SUCCESS) {
      return err("attention: couldn't read your watches.");
   }
   if (n == 0) {
      return err("attention: I couldn't find that watch.");
   }
   if (n > 1) {
      /* Destructive: never bulk-delete on an ambiguous match — list and ask. */
      return ambiguity_reply(ws, n, "remove", out);
   }
   if (attention_watch_remove(user_id, ws[0].id) != SUCCESS) {
      return err("attention: couldn't remove that watch.");
   }
   snprintf(out, ATTN_RESULT_MAX, "Removed '%s'.", ws[0].name);
   return out;
}

static char *do_set(int user_id,
                    int64_t id,
                    const char *name,
                    const char *metric,
                    const char *threshold,
                    const char *rate,
                    const char *direction,
                    const char *level,
                    char *out) {
   sage_watch_t ws[SAGE_MAX_WATCHES_PER_USER];
   int n = 0;
   if (resolve_targets(user_id, id, name, metric, ws, SAGE_MAX_WATCHES_PER_USER, &n) != SUCCESS) {
      return err("attention: couldn't read your watches.");
   }
   if (n == 0) {
      return err("attention: I couldn't find that watch to change. Use 'watch' to create one.");
   }
   if (n > 1) {
      return ambiguity_reply(ws, n, "change", out);
   }

   sage_watch_t w = ws[0];
   char *cerr = apply_condition(&w, direction, threshold, rate);
   if (cerr) {
      return cerr;
   }
   if (level && level[0]) {
      w.notify = sage_notify_from_str(level, w.notify);
   }
   const char *verr = NULL;
   if (attention_validate_watch_trigger(&w, &verr) != SUCCESS) {
      return err(verr ? verr : "attention: that change isn't valid.");
   }
   if (attention_watch_update(user_id, w.id, &w) != SUCCESS) {
      return err("attention: couldn't update that watch.");
   }
   /* Re-fetch so an auto-named watch reports its regenerated name. */
   sage_watch_t stored;
   if (attention_watch_find_by_id(user_id, w.id, &stored) != SUCCESS) {
      stored = w;
   }
   char cond[96];
   describe_condition(&stored, cond, sizeof(cond));
   snprintf(out, ATTN_RESULT_MAX, "Updated '%s' — %s.", stored.name, cond);
   append_current_value(out, ATTN_RESULT_MAX, &stored);
   return out;
}

/* ========== Callback ========== */

static char *attention_tool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1;
   if (!action || !action[0]) {
      return err("attention: missing action.");
   }

   /* Fail closed for MUTATIONS from a caller with no session context — an
    * unauthenticated MQTT publish would otherwise mutate as user 1
    * (tool_get_current_user_id() defaults to 1 with no session).  Reads (list) are
    * user-scoped + harmless, so they stay open.  Mirrors job_tool.c. */
   if (strcasecmp(action, "list") != 0 && session_get_command_context() == NULL) {
      OLOG_WARNING("attention: refused '%s' from a caller with no session context", action);
      return err("attention: I can only change what I'm watching from a user session.");
   }

   int user_id = tool_get_current_user_id();

   char metric[SAGE_METRIC_LEN] = { 0 };
   char name[SAGE_WATCH_NAME_LEN] = { 0 };
   char id_str[24] = { 0 };
   char threshold[32] = { 0 };
   char rate[32] = { 0 };
   char direction[16] = { 0 };
   char level[16] = { 0 };
   char duration[32] = { 0 };
   if (value) {
      tool_param_extract_base(value, metric, sizeof(metric));
      tool_param_extract_custom(value, "name", name, sizeof(name));
      tool_param_extract_custom(value, "id", id_str, sizeof(id_str));
      tool_param_extract_custom(value, "threshold", threshold, sizeof(threshold));
      tool_param_extract_custom(value, "rate", rate, sizeof(rate));
      tool_param_extract_custom(value, "direction", direction, sizeof(direction));
      tool_param_extract_custom(value, "level", level, sizeof(level));
      tool_param_extract_custom(value, "duration", duration, sizeof(duration));
   }
   int64_t id = (id_str[0]) ? (int64_t)strtoll(id_str, NULL, 10) : 0;

   char *out = malloc(ATTN_RESULT_MAX);
   if (!out) {
      return err("attention: out of memory.");
   }
   out[0] = '\0';

   char *r = out;
   if (strcasecmp(action, "list") == 0) {
      r = do_list(user_id, out);
   } else if (strcasecmp(action, "watch") == 0) {
      /* Creating a NEW watch needs a real catalog metric. */
      if (!metric[0]) {
         free(out);
         return err("attention: which signal? (e.g. CO2, battery, helmet link)");
      }
      if (!attention_catalog_has(metric)) {
         /* Self-correcting: list what IS watchable so the model can retry. */
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
      r = do_watch(user_id, metric, name, threshold, rate, direction, level, out);
   } else if (strcasecmp(action, "set") == 0 || strcasecmp(action, "ignore") == 0 ||
              strcasecmp(action, "resume") == 0 || strcasecmp(action, "remove") == 0) {
      /* Targeted actions need a selector (name / id / signal); the metric here may
       * be a component prefix, so it is NOT catalog-validated — a no-match is
       * reported gracefully by the action. */
      if (id <= 0 && !name[0] && !metric[0]) {
         free(out);
         return err("attention: which watch? Give its name, its id, or the signal.");
      }
      if (strcasecmp(action, "set") == 0) {
         r = do_set(user_id, id, name, metric, threshold, rate, direction, level, out);
      } else if (strcasecmp(action, "ignore") == 0) {
         r = do_toggle(user_id, false, id, name, metric, duration, out);
      } else if (strcasecmp(action, "resume") == 0) {
         r = do_toggle(user_id, true, id, name, metric, NULL, out);
      } else {
         r = do_remove(user_id, id, name, metric, out);
      }
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
