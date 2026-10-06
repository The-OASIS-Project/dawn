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
 * Unit tests for tools/tavily_rate_limit — leaky-bucket per-user counter
 * that bounds Tavily quota burn on runaway tool loops (SEC-M3).
 */

#include "config/dawn_config.h"
#include "tools/tavily_rate_limit.h"
#include "unity.h"

/* Stub g_config — limiter reads rate_limit_per_{minute,hour,day} from here;
 * zero-init means "use compile-time defaults" per the limiter's fallback. */
dawn_config_t g_config = { 0 };

/* Stub the session-resolver dependencies. tavily_rate_limit.c references
 * session_get_command_context/local from tavily_rate_limit_resolve_user_id(),
 * but this test doesn't exercise that helper — it tests the bucket logic
 * directly via tavily_rate_limit_check(user_id). The stubs satisfy the
 * linker without pulling in the full session manager.
 *
 * session_get_local() is a `static inline` in session_manager.h that calls
 * json_object_new_array() + llm_get_default_config() inside a one-time-init
 * block.  Including the header forces that inline into tavily_rate_limit.c's
 * translation unit, so the test must also stub those two transitively-
 * referenced symbols even though the inline's init path never runs in this
 * test (resolve_user_id() isn't called).  Same shape as the session stubs:
 * satisfy the linker, never executed. */
struct session;
struct session *session_get_command_context(void) {
   return NULL;
}
struct session *session_get_local(void) {
   return NULL;
}
int session_effective_user_id(struct session *session) {
   (void)session;
   return 0;
}
struct json_object;
struct json_object *json_object_new_array(void) {
   return NULL;
}
struct session_llm_config;
void llm_get_default_config(struct session_llm_config *cfg) {
   (void)cfg;
}

void setUp(void) {
   /* Test isolation: zero all buckets between cases. */
   tavily_rate_limit_reset();
}

void tearDown(void) {
}

/* ── Initial budget ─────────────────────────────────────────────────────── */

static void test_initial_budget_full(void) {
   int m = -1, h = -1;
   tavily_rate_limit_remaining(1, &m, &h);
   TEST_ASSERT_EQUAL_INT(TAVILY_RL_PER_MINUTE_DEFAULT, m);
   TEST_ASSERT_EQUAL_INT(TAVILY_RL_PER_HOUR_DEFAULT, h);
}

static void test_unknown_user_full_budget(void) {
   /* Querying a fresh user reports the full budget without creating a charged
    * bucket. */
   int m = -1, h = -1;
   tavily_rate_limit_remaining(42, &m, &h);
   TEST_ASSERT_EQUAL_INT(TAVILY_RL_PER_MINUTE_DEFAULT, m);
   TEST_ASSERT_EQUAL_INT(TAVILY_RL_PER_HOUR_DEFAULT, h);
}

/* ── Charging within budget ─────────────────────────────────────────────── */

static void test_within_minute_budget(void) {
   for (int i = 0; i < TAVILY_RL_PER_MINUTE_DEFAULT; i++) {
      TEST_ASSERT_TRUE(tavily_rate_limit_check(1));
   }
}

static void test_minute_cap_blocks_extra(void) {
   for (int i = 0; i < TAVILY_RL_PER_MINUTE_DEFAULT; i++) {
      TEST_ASSERT_TRUE(tavily_rate_limit_check(1));
   }
   /* 11th call within the minute should be blocked. */
   TEST_ASSERT_FALSE(tavily_rate_limit_check(1));
}

static void test_denied_call_does_not_charge(void) {
   for (int i = 0; i < TAVILY_RL_PER_MINUTE_DEFAULT; i++) {
      (void)tavily_rate_limit_check(1);
   }
   /* Multiple denied calls should keep counters pegged at cap, not above. */
   (void)tavily_rate_limit_check(1);
   (void)tavily_rate_limit_check(1);
   int m = -1, h = -1;
   tavily_rate_limit_remaining(1, &m, &h);
   TEST_ASSERT_EQUAL_INT(0, m);
   /* Hour budget should still match minute count (since they're in lockstep
    * on the way up). Above-cap rejections don't get counted. */
   TEST_ASSERT_EQUAL_INT(TAVILY_RL_PER_HOUR_DEFAULT - TAVILY_RL_PER_MINUTE_DEFAULT, h);
}

/* ── Per-user isolation ─────────────────────────────────────────────────── */

static void test_per_user_isolation(void) {
   for (int i = 0; i < TAVILY_RL_PER_MINUTE_DEFAULT; i++) {
      TEST_ASSERT_TRUE(tavily_rate_limit_check(1));
   }
   /* User 1 exhausted; user 2 should still have full budget. */
   TEST_ASSERT_TRUE(tavily_rate_limit_check(2));
   int m = -1, h = -1;
   tavily_rate_limit_remaining(2, &m, &h);
   TEST_ASSERT_EQUAL_INT(TAVILY_RL_PER_MINUTE_DEFAULT - 1, m);
}

static void test_anonymous_bucket(void) {
   /* Non-positive user ids share one anonymous bucket. */
   TEST_ASSERT_TRUE(tavily_rate_limit_check(0));
   TEST_ASSERT_TRUE(tavily_rate_limit_check(-1));
   TEST_ASSERT_TRUE(tavily_rate_limit_check(-42));

   int m = -1, h = -1;
   tavily_rate_limit_remaining(0, &m, &h);
   TEST_ASSERT_EQUAL_INT(TAVILY_RL_PER_MINUTE_DEFAULT - 3, m);
   /* Query under a different non-positive id reports the same bucket state. */
   tavily_rate_limit_remaining(-1, &m, &h);
   TEST_ASSERT_EQUAL_INT(TAVILY_RL_PER_MINUTE_DEFAULT - 3, m);
}

/* ── Reset ──────────────────────────────────────────────────────────────── */

static void test_reset_restores_budget(void) {
   for (int i = 0; i < TAVILY_RL_PER_MINUTE_DEFAULT; i++) {
      (void)tavily_rate_limit_check(1);
   }
   TEST_ASSERT_FALSE(tavily_rate_limit_check(1));
   tavily_rate_limit_reset();
   TEST_ASSERT_TRUE(tavily_rate_limit_check(1));
}

/* ── Many users (bucket-table sizing) ───────────────────────────────────── */

static void test_many_users_eventually_overflow(void) {
   /* The implementation sizes the bucket table to TAVILY_RL_MAX_USERS=8.
    * Overflow path: when full, the call is allowed (fail-open) and logged.
    * We can't observe the log, but we can confirm calls keep returning true. */
   for (int uid = 1; uid <= 20; uid++) {
      TEST_ASSERT_TRUE(tavily_rate_limit_check(uid));
   }
}

/* ── Global daily ceiling (SEC-L3) ──────────────────────────────────────── */

/* Helper: saturate the global daily counter without touching real time.
 * Uses bucket-overflow path — uids beyond TAVILY_RL_MAX_USERS fail-open and
 * still charge global, which sidesteps the per-minute cap we'd otherwise
 * have to wait through. Each call adds exactly 1 to s_global_day_count. */
static void saturate_global_daily(void) {
   const int start_uid = TAVILY_RL_MAX_USERS + 100;
   for (int i = 0; i < TAVILY_RL_PER_DAY_DEFAULT; i++) {
      (void)tavily_rate_limit_check(start_uid + i);
   }
}

static void test_global_daily_caps_distinct_user(void) {
   saturate_global_daily();
   /* Global ceiling now at PER_DAY. A fresh user with full per-user budget
    * should still be blocked by the global cap. */
   TEST_ASSERT_FALSE(tavily_rate_limit_check(99999));
}

static void test_global_daily_denied_does_not_charge_user_bucket(void) {
   saturate_global_daily();
   /* Multiple denied calls under the global ceiling should not advance the
    * per-user bucket — fresh user reports full budget. */
   (void)tavily_rate_limit_check(99999);
   (void)tavily_rate_limit_check(99999);
   int m = -1, h = -1;
   tavily_rate_limit_remaining(99999, &m, &h);
   TEST_ASSERT_EQUAL_INT(TAVILY_RL_PER_MINUTE_DEFAULT, m);
   TEST_ASSERT_EQUAL_INT(TAVILY_RL_PER_HOUR_DEFAULT, h);
}

static void test_reset_clears_global_daily(void) {
   saturate_global_daily();
   TEST_ASSERT_FALSE(tavily_rate_limit_check(99999));
   tavily_rate_limit_reset();
   TEST_ASSERT_TRUE(tavily_rate_limit_check(99999));
}

/* ── Entrypoint ─────────────────────────────────────────────────────────── */

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_initial_budget_full);
   RUN_TEST(test_unknown_user_full_budget);
   RUN_TEST(test_within_minute_budget);
   RUN_TEST(test_minute_cap_blocks_extra);
   RUN_TEST(test_denied_call_does_not_charge);
   RUN_TEST(test_per_user_isolation);
   RUN_TEST(test_anonymous_bucket);
   RUN_TEST(test_reset_restores_budget);
   RUN_TEST(test_many_users_eventually_overflow);
   RUN_TEST(test_global_daily_caps_distinct_user);
   RUN_TEST(test_global_daily_denied_does_not_charge_user_bucket);
   RUN_TEST(test_reset_clears_global_daily);
   return UNITY_END();
}
