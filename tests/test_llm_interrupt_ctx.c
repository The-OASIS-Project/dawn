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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Unit tests for llm_interrupt_ctx_triggered() (src/llm/llm_rate_limit.c) — the
 * cancel-context evaluator that decides whether a turn aborts.  This is the one
 * predicate the tool loop's step-11 gate and both wait helpers consult, so the
 * truth table below locks the foreground/background cancellation policy:
 *
 *   - foreground turn: {session_flag, honor_global=true}  → session OR global
 *   - background turn: {session_flag, honor_global=false} → session ONLY
 *   - legacy caller:   NULL ctx                            → global only
 *
 * The global interrupt flag is stubbed here (see llm_is_interrupt_requested
 * below) so each case controls it deterministically; the real symbol lives in
 * llm_interface.c, which this test deliberately does not link.
 */

#include <stdatomic.h>
#include <stdbool.h>

#include "llm/llm_rate_limit.h"
#include "unity.h"

/* Test-controlled stand-in for the process-global interrupt flag.  llm_rate_limit.c
 * calls llm_is_interrupt_requested(); this definition satisfies that symbol and
 * lets each test drive the "global wake-word / Ctrl+C" input. */
static int s_global_interrupt = 0;

int llm_is_interrupt_requested(void) {
   return s_global_interrupt;
}

void setUp(void) {
   s_global_interrupt = 0;
}

void tearDown(void) {
}

/* ---- NULL ctx: legacy global-only behavior ---- */

static void test_null_ctx_global_clear(void) {
   s_global_interrupt = 0;
   TEST_ASSERT_FALSE(llm_interrupt_ctx_triggered(NULL));
}

static void test_null_ctx_global_set(void) {
   s_global_interrupt = 1;
   TEST_ASSERT_TRUE(llm_interrupt_ctx_triggered(NULL));
}

/* ---- Foreground turn (honor_global = true): session OR global ---- */

static void test_foreground_no_session_flag_global_clear(void) {
   llm_interrupt_ctx_t ctx = { .session_flag = NULL, .honor_global = true };
   s_global_interrupt = 0;
   TEST_ASSERT_FALSE(llm_interrupt_ctx_triggered(&ctx));
}

static void test_foreground_no_session_flag_global_set(void) {
   llm_interrupt_ctx_t ctx = { .session_flag = NULL, .honor_global = true };
   s_global_interrupt = 1;
   TEST_ASSERT_TRUE(llm_interrupt_ctx_triggered(&ctx));
}

static void test_foreground_session_clear_global_set(void) {
   _Atomic bool flag = false;
   llm_interrupt_ctx_t ctx = { .session_flag = &flag, .honor_global = true };
   s_global_interrupt = 1;
   /* Foreground honors the global barge-in even with its own flag clear. */
   TEST_ASSERT_TRUE(llm_interrupt_ctx_triggered(&ctx));
}

static void test_foreground_session_set_global_clear(void) {
   _Atomic bool flag = true;
   llm_interrupt_ctx_t ctx = { .session_flag = &flag, .honor_global = true };
   s_global_interrupt = 0;
   TEST_ASSERT_TRUE(llm_interrupt_ctx_triggered(&ctx));
}

/* ---- Background turn (honor_global = false): session ONLY ---- */

static void test_background_ignores_global(void) {
   llm_interrupt_ctx_t ctx = { .session_flag = NULL, .honor_global = false };
   s_global_interrupt = 1;
   /* The whole point: a background/job turn does NOT die on a foreground
    * wake word / Ctrl+C. */
   TEST_ASSERT_FALSE(llm_interrupt_ctx_triggered(&ctx));
}

static void test_background_session_clear_global_set(void) {
   _Atomic bool flag = false;
   llm_interrupt_ctx_t ctx = { .session_flag = &flag, .honor_global = false };
   s_global_interrupt = 1;
   TEST_ASSERT_FALSE(llm_interrupt_ctx_triggered(&ctx));
}

static void test_background_session_set_global_clear(void) {
   _Atomic bool flag = true;
   llm_interrupt_ctx_t ctx = { .session_flag = &flag, .honor_global = false };
   s_global_interrupt = 0;
   /* Its own cancel flag DOES stop it. */
   TEST_ASSERT_TRUE(llm_interrupt_ctx_triggered(&ctx));
}

static void test_background_session_set_global_set(void) {
   _Atomic bool flag = true;
   llm_interrupt_ctx_t ctx = { .session_flag = &flag, .honor_global = false };
   s_global_interrupt = 1;
   TEST_ASSERT_TRUE(llm_interrupt_ctx_triggered(&ctx));
}

/* ---- Session flag takes precedence over honor_global ---- */

static void test_session_flag_short_circuits_before_global_check(void) {
   _Atomic bool flag = true;
   /* honor_global=false, global set — result must be true via the session
    * flag, never consulting the (ignored) global. */
   llm_interrupt_ctx_t ctx = { .session_flag = &flag, .honor_global = false };
   s_global_interrupt = 1;
   TEST_ASSERT_TRUE(llm_interrupt_ctx_triggered(&ctx));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_null_ctx_global_clear);
   RUN_TEST(test_null_ctx_global_set);
   RUN_TEST(test_foreground_no_session_flag_global_clear);
   RUN_TEST(test_foreground_no_session_flag_global_set);
   RUN_TEST(test_foreground_session_clear_global_set);
   RUN_TEST(test_foreground_session_set_global_clear);
   RUN_TEST(test_background_ignores_global);
   RUN_TEST(test_background_session_clear_global_set);
   RUN_TEST(test_background_session_set_global_clear);
   RUN_TEST(test_background_session_set_global_set);
   RUN_TEST(test_session_flag_short_circuits_before_global_check);
   return UNITY_END();
}
