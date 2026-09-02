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
 * Unit tests for the SAGE gate + policy + catalog.  Pure logic — the gate takes
 * its state explicitly and uses a synthetic clock (now_ms), so no DB, MQTT, or
 * daemon is needed.
 */

#include <math.h>
#include <string.h>

#include "core/attention/attention_internal.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* Base synthetic clock (arbitrary epoch ms). */
static const int64_t T0 = 1000000000000LL;

static sage_watch_t mk_threshold(sage_direction_t dir,
                                 double thr,
                                 double hyst,
                                 sage_notify_t notify) {
   sage_watch_t w;
   memset(&w, 0, sizeof(w));
   w.id = 1;
   w.user_id = 1;
   w.rule_type = SAGE_RULE_THRESHOLD;
   w.direction = dir;
   w.threshold = thr;
   w.hysteresis = hyst;
   w.notify = notify;
   w.enabled = true;
   w.ttl_min = 30;
   strncpy(w.metric, "stat.battery.soc", sizeof(w.metric) - 1);
   return w;
}

static bool eval(const sage_watch_t *w, sage_rule_state_t *st, double value, int64_t now_ms) {
   sage_event_t ev;
   return attention_gate_eval(w, st, value, true, now_ms, NULL, &ev);
}

/* --- threshold + hysteresis + backoff --- */
static void test_threshold_hysteresis_and_backoff(void) {
   sage_watch_t w = mk_threshold(SAGE_DIR_BELOW, 15.0, 5.0, SAGE_NOTIFY_ALERT);
   sage_rule_state_t st;
   memset(&st, 0, sizeof(st));

   /* Above band: no fire, stays armed. */
   TEST_ASSERT_FALSE(eval(&w, &st, 20.0, T0));
   /* Crosses below: fires. */
   TEST_ASSERT_TRUE(eval(&w, &st, 10.0, T0 + 1000));
   TEST_ASSERT_EQUAL_INT64(SAGE_BACKOFF_INITIAL_MS, st.backoff_ms);
   /* Still below, but disarmed: no re-fire. */
   TEST_ASSERT_FALSE(eval(&w, &st, 10.0, T0 + 2000));
   /* Recovered above threshold but NOT past hysteresis margin (15..20): no re-arm. */
   TEST_ASSERT_FALSE(eval(&w, &st, 16.0, T0 + 3000));
   /* Back past hysteresis (>= 20): re-arms (still no fire — not met). */
   TEST_ASSERT_FALSE(eval(&w, &st, 21.0, T0 + 4000));
   /* Met + armed, but inside the backoff window: suppressed. */
   TEST_ASSERT_FALSE(eval(&w, &st, 10.0, T0 + 5000));
   /* Past the backoff window: fires again. */
   TEST_ASSERT_TRUE(eval(&w, &st, 10.0, T0 + SAGE_BACKOFF_INITIAL_MS + 6000));
}

/* --- exponential backoff doubling + reset --- */
static void test_backoff_doubling_and_reset(void) {
   sage_watch_t w = mk_threshold(SAGE_DIR_BELOW, 15.0, 5.0, SAGE_NOTIFY_ALERT);
   sage_rule_state_t st;
   memset(&st, 0, sizeof(st));

   int64_t t = T0;
   TEST_ASSERT_TRUE(eval(&w, &st, 10.0, t)); /* fire 1 */
   TEST_ASSERT_EQUAL_INT64(SAGE_BACKOFF_INITIAL_MS, st.backoff_ms);

   /* Re-arm then fire again within the reset window => doubles. */
   TEST_ASSERT_FALSE(eval(&w, &st, 21.0, t + 1000));
   t += SAGE_BACKOFF_INITIAL_MS + 2000;
   TEST_ASSERT_TRUE(eval(&w, &st, 10.0, t));
   TEST_ASSERT_EQUAL_INT64((int64_t)SAGE_BACKOFF_INITIAL_MS * 2, st.backoff_ms);

   TEST_ASSERT_FALSE(eval(&w, &st, 21.0, t + 1000));
   t += (int64_t)SAGE_BACKOFF_INITIAL_MS * 2 + 2000;
   TEST_ASSERT_TRUE(eval(&w, &st, 10.0, t));
   TEST_ASSERT_EQUAL_INT64((int64_t)SAGE_BACKOFF_INITIAL_MS * 4, st.backoff_ms);

   /* A long quiet gap (> reset) collapses the window back to the initial. */
   TEST_ASSERT_FALSE(eval(&w, &st, 21.0, t + 1000));
   t += SAGE_BACKOFF_RESET_MS + 5000;
   TEST_ASSERT_TRUE(eval(&w, &st, 10.0, t));
   TEST_ASSERT_EQUAL_INT64(SAGE_BACKOFF_INITIAL_MS, st.backoff_ms);
}

/* --- backoff never exceeds the cap --- */
static void test_backoff_caps(void) {
   sage_watch_t w = mk_threshold(SAGE_DIR_BELOW, 15.0, 5.0, SAGE_NOTIFY_ALERT);
   sage_rule_state_t st;
   memset(&st, 0, sizeof(st));
   int64_t t = T0;
   for (int i = 0; i < 12; i++) {
      TEST_ASSERT_TRUE(eval(&w, &st, 10.0, t));
      TEST_ASSERT_TRUE(st.backoff_ms <= SAGE_BACKOFF_MAX_MS);
      TEST_ASSERT_FALSE(eval(&w, &st, 21.0, t + 1000)); /* re-arm */
      t += st.backoff_ms + 2000;
   }
   TEST_ASSERT_EQUAL_INT64(SAGE_BACKOFF_MAX_MS, st.backoff_ms);
}

/* --- TTL admission (P1 push path) --- */
static void test_ttl_expiry(void) {
   sage_event_t ev;
   memset(&ev, 0, sizeof(ev));
   ev.observed_at = T0;
   ev.ttl_ms = 30 * 60 * 1000;

   sage_rule_state_t st1;
   memset(&st1, 0, sizeof(st1));
   TEST_ASSERT_TRUE(attention_gate_admit(&ev, &st1, T0 + 1000)); /* fresh */

   sage_rule_state_t st2;
   memset(&st2, 0, sizeof(st2));
   TEST_ASSERT_FALSE(attention_gate_admit(&ev, &st2, T0 + ev.ttl_ms + 1000)); /* expired */
}

/* --- slope over a window --- */
static void test_slope_window(void) {
   sage_watch_t w;
   memset(&w, 0, sizeof(w));
   w.id = 7;
   w.user_id = 1;
   w.rule_type = SAGE_RULE_SLOPE;
   w.direction = SAGE_DIR_RISING;
   w.slope_per_min = 400.0;
   w.slope_window_sec = 120;
   w.notify = SAGE_NOTIFY_AMBIENT;
   w.enabled = true;
   strncpy(w.metric, "suit.co2_ppm", sizeof(w.metric) - 1);

   sage_rule_state_t st;
   memset(&st, 0, sizeof(st));
   /* First sample: no span yet. */
   TEST_ASSERT_FALSE(eval(&w, &st, 400.0, T0));
   /* +60s, +500 => slope 500/min > 400 => fire. */
   TEST_ASSERT_TRUE(eval(&w, &st, 900.0, T0 + 60000));

   /* A gentle rise must NOT fire. */
   sage_rule_state_t st2;
   memset(&st2, 0, sizeof(st2));
   TEST_ASSERT_FALSE(eval(&w, &st2, 400.0, T0));
   TEST_ASSERT_FALSE(eval(&w, &st2, 450.0, T0 + 60000)); /* 50/min */

   /* A RISING watch must NOT fire on a sharp DROP (that's a falling watch's job). */
   sage_rule_state_t st3;
   memset(&st3, 0, sizeof(st3));
   TEST_ASSERT_FALSE(eval(&w, &st3, 900.0, T0));
   TEST_ASSERT_FALSE(eval(&w, &st3, 400.0, T0 + 60000)); /* -500/min */
}

/* --- slope, falling direction (rate dropping past -slope_per_min) --- */
static void test_slope_falling(void) {
   sage_watch_t w;
   memset(&w, 0, sizeof(w));
   w.id = 8;
   w.user_id = 1;
   w.rule_type = SAGE_RULE_SLOPE;
   w.direction = SAGE_DIR_FALLING;
   w.slope_per_min = 400.0;
   w.slope_window_sec = 120;
   w.notify = SAGE_NOTIFY_AMBIENT;
   w.enabled = true;
   strncpy(w.metric, "suit.co2_ppm", sizeof(w.metric) - 1);

   /* A sharp drop fires: +60s, -500 => slope -500/min <= -400 => fire. */
   sage_rule_state_t st;
   memset(&st, 0, sizeof(st));
   TEST_ASSERT_FALSE(eval(&w, &st, 900.0, T0));
   TEST_ASSERT_TRUE(eval(&w, &st, 400.0, T0 + 60000));

   /* A gentle drop must NOT fire. */
   sage_rule_state_t st2;
   memset(&st2, 0, sizeof(st2));
   TEST_ASSERT_FALSE(eval(&w, &st2, 900.0, T0));
   TEST_ASSERT_FALSE(eval(&w, &st2, 850.0, T0 + 60000)); /* -50/min */

   /* A sharp RISE must NOT fire a falling watch. */
   sage_rule_state_t st3;
   memset(&st3, 0, sizeof(st3));
   TEST_ASSERT_FALSE(eval(&w, &st3, 400.0, T0));
   TEST_ASSERT_FALSE(eval(&w, &st3, 900.0, T0 + 60000)); /* +500/min */
}

/* --- absence --- */
static void test_absence(void) {
   sage_watch_t w;
   memset(&w, 0, sizeof(w));
   w.id = 9;
   w.user_id = 1;
   w.rule_type = SAGE_RULE_ABSENCE;
   w.direction = SAGE_DIR_ABOVE;
   w.absence_after_sec = 60;
   w.notify = SAGE_NOTIFY_AMBIENT;
   w.enabled = true;
   strncpy(w.metric, "component.hud", sizeof(w.metric) - 1);

   sage_rule_state_t st;
   memset(&st, 0, sizeof(st));
   /* Age within window: no fire. */
   TEST_ASSERT_FALSE(eval(&w, &st, 30.0, T0));
   /* Silent past window: fire. */
   TEST_ASSERT_TRUE(eval(&w, &st, 75.0, T0 + 1000));
   /* Still silent, disarmed: no re-fire. */
   TEST_ASSERT_FALSE(eval(&w, &st, 80.0, T0 + 2000));
   /* Feed resumed: re-arm. */
   TEST_ASSERT_FALSE(eval(&w, &st, 10.0, T0 + 3000));
   /* Silent again after backoff: fires. */
   TEST_ASSERT_TRUE(eval(&w, &st, 75.0, T0 + SAGE_BACKOFF_INITIAL_MS + 4000));
}

/* --- zero threshold + zero hysteresis (fault counters) must re-arm --- */
static void test_gate_zero_threshold_rearms(void) {
   sage_watch_t w;
   memset(&w, 0, sizeof(w));
   w.id = 11;
   w.user_id = 1;
   w.rule_type = SAGE_RULE_THRESHOLD;
   w.direction = SAGE_DIR_ABOVE;
   w.threshold = 0.0;  /* fire when count > 0 */
   w.hysteresis = 0.0; /* re-arm at 0 — a nonzero margin would demand a negative
                        * count and the watch would fire once then never again */
   w.notify = SAGE_NOTIFY_ALERT;
   w.enabled = true;
   strncpy(w.metric, "stat.crit_faults", sizeof(w.metric) - 1);

   sage_rule_state_t st;
   memset(&st, 0, sizeof(st));
   TEST_ASSERT_FALSE(eval(&w, &st, 0.0, T0));        /* no faults */
   TEST_ASSERT_TRUE(eval(&w, &st, 1.0, T0 + 1000));  /* a fault: fires */
   TEST_ASSERT_FALSE(eval(&w, &st, 1.0, T0 + 2000)); /* still faulting: disarmed */
   TEST_ASSERT_FALSE(eval(&w, &st, 0.0, T0 + 3000)); /* cleared: re-arms */
   /* A second fault episode (past backoff) must fire again — the bug was silence. */
   TEST_ASSERT_TRUE(eval(&w, &st, 1.0, T0 + SAGE_BACKOFF_INITIAL_MS + 4000));
}

/* --- policy: notify -> mode + per-hour budget --- */
static void test_policy_notify_and_budget(void) {
   attention_policy_configure(2);
   attention_policy_reset_budget();

   sage_event_t alert;
   memset(&alert, 0, sizeof(alert));
   alert.notify = SAGE_NOTIFY_ALERT;
   sage_event_t ambient;
   memset(&ambient, 0, sizeof(ambient));
   ambient.notify = SAGE_NOTIFY_AMBIENT;
   sage_event_t digest;
   memset(&digest, 0, sizeof(digest));
   digest.notify = SAGE_NOTIFY_DIGEST;

   /* First two spoken alerts pass; caller records each emission. */
   TEST_ASSERT_EQUAL_INT(SAGE_MODE_ALERT, attention_policy_decide(&alert, T0));
   attention_policy_note_alert(T0);
   TEST_ASSERT_EQUAL_INT(SAGE_MODE_ALERT, attention_policy_decide(&alert, T0));
   attention_policy_note_alert(T0);
   /* Third within the hour: demoted to AMBIENT (kept visible, not spoken). */
   TEST_ASSERT_EQUAL_INT(SAGE_MODE_AMBIENT, attention_policy_decide(&alert, T0));

   /* Ambient + digest are unaffected by the budget. */
   TEST_ASSERT_EQUAL_INT(SAGE_MODE_AMBIENT, attention_policy_decide(&ambient, T0));
   TEST_ASSERT_EQUAL_INT(SAGE_MODE_DIGEST, attention_policy_decide(&digest, T0));

   /* Past the hour, the budget frees up. */
   int64_t later = T0 + 3600LL * 1000 + 1000;
   TEST_ASSERT_EQUAL_INT(SAGE_MODE_ALERT, attention_policy_decide(&alert, later));
}

/* --- catalog readers --- */
static void test_catalog_reader(void) {
   const attention_catalog_entry_t *e = attention_catalog_lookup("stat.battery.soc");
   TEST_ASSERT_NOT_NULL(e);

   attention_sample_ctx_t ctx;
   memset(&ctx, 0, sizeof(ctx));
   ctx.stat_valid = true;
   ctx.stat.have_battery = true;
   ctx.stat.batt_level = 12.0;
   double v = 0.0;
   TEST_ASSERT_TRUE(e->read(&ctx, &v));
   TEST_ASSERT_EQUAL_DOUBLE(12.0, v);

   /* Source absent => not present. */
   attention_sample_ctx_t empty;
   memset(&empty, 0, sizeof(empty));
   TEST_ASSERT_FALSE(e->read(&empty, &v));

   /* Unknown key. */
   TEST_ASSERT_NULL(attention_catalog_lookup("does.not.exist"));
   /* Public accessors. */
   TEST_ASSERT_TRUE(attention_catalog_has("suit.co2_ppm"));
   TEST_ASSERT_EQUAL_STRING("CO2", attention_catalog_label("suit.co2_ppm"));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_threshold_hysteresis_and_backoff);
   RUN_TEST(test_backoff_doubling_and_reset);
   RUN_TEST(test_backoff_caps);
   RUN_TEST(test_ttl_expiry);
   RUN_TEST(test_slope_window);
   RUN_TEST(test_slope_falling);
   RUN_TEST(test_absence);
   RUN_TEST(test_gate_zero_threshold_rearms);
   RUN_TEST(test_policy_notify_and_budget);
   RUN_TEST(test_catalog_reader);
   return UNITY_END();
}
