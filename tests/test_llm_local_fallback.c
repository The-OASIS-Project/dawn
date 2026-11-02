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
 * Unit tests for llm_local_fallback.c: the outage window and the note.
 */

#include <stdlib.h>
#include <string.h>

#include "llm/llm_local_fallback.h"
#include "unity.h"

void setUp(void) {
   for (int p = 0; p < CLOUD_PROVIDER_NONE; p++) {
      llm_local_fallback_mark_up((cloud_provider_t)p);
   }
}

void tearDown(void) {
}

static void test_no_outage_by_default(void) {
   TEST_ASSERT_FALSE(llm_local_fallback_cloud_down(CLOUD_PROVIDER_CLAUDE, 1000));
   TEST_ASSERT_FALSE(llm_local_fallback_in_outage(CLOUD_PROVIDER_CLAUDE));
}

static void test_window_grows_while_down_and_caps(void) {
   const cloud_provider_t p = CLOUD_PROVIDER_CLAUDE;
   int64_t now = 1000;
   int expected = LLM_CLOUD_RETRY_FIRST_SEC;
   for (int i = 0; i < 8; i++) {
      llm_local_fallback_mark_down(p, now);
      TEST_ASSERT_TRUE(llm_local_fallback_cloud_down(p, now + expected - 1));
      TEST_ASSERT_FALSE(llm_local_fallback_cloud_down(p, now + expected));
      now += expected; /* the window ran out; the cloud failed again */
      expected = expected * 2 > LLM_CLOUD_RETRY_MAX_SEC ? LLM_CLOUD_RETRY_MAX_SEC : expected * 2;
   }
   TEST_ASSERT_TRUE(llm_local_fallback_in_outage(p));
}

static void test_an_answer_ends_the_outage(void) {
   const cloud_provider_t p = CLOUD_PROVIDER_OPENAI;
   llm_local_fallback_mark_down(p, 1000);
   llm_local_fallback_mark_down(p, 1100);
   llm_local_fallback_mark_up(p);
   TEST_ASSERT_FALSE(llm_local_fallback_cloud_down(p, 1101));
   TEST_ASSERT_FALSE(llm_local_fallback_in_outage(p));
   llm_local_fallback_mark_down(p, 2000); /* a new outage starts over */
   TEST_ASSERT_FALSE(llm_local_fallback_cloud_down(p, 2000 + LLM_CLOUD_RETRY_FIRST_SEC));
}

static void test_retry_now_keeps_the_outage(void) {
   const cloud_provider_t p = CLOUD_PROVIDER_GEMINI;
   llm_local_fallback_mark_down(p, 1000);
   llm_local_fallback_retry_now(p);
   TEST_ASSERT_FALSE(llm_local_fallback_cloud_down(p, 1001));
   TEST_ASSERT_TRUE(llm_local_fallback_in_outage(p)); /* no new note when local answers next */
}

static void test_providers_are_separate_and_unknown_is_ignored(void) {
   llm_local_fallback_mark_down(CLOUD_PROVIDER_CLAUDE, 1000);
   TEST_ASSERT_FALSE(llm_local_fallback_cloud_down(CLOUD_PROVIDER_OPENROUTER, 1001));
   llm_local_fallback_mark_down(CLOUD_PROVIDER_NONE, 1000);
   llm_local_fallback_mark_down((cloud_provider_t)99, 1000);
   TEST_ASSERT_FALSE(llm_local_fallback_cloud_down(CLOUD_PROVIDER_NONE, 1001));
   TEST_ASSERT_FALSE(llm_local_fallback_in_outage((cloud_provider_t)99));
}

static void test_note_goes_in_front_of_an_answer_only(void) {
   char *noted = llm_local_fallback_with_note(strdup("It's 4 pm."));
   TEST_ASSERT_EQUAL_STRING(LLM_LOCAL_FALLBACK_NOTE "It's 4 pm.", noted);
   free(noted);
   char *empty = llm_local_fallback_with_note(strdup(""));
   TEST_ASSERT_EQUAL_STRING("", empty);
   free(empty);
   TEST_ASSERT_NULL(llm_local_fallback_with_note(NULL));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_no_outage_by_default);
   RUN_TEST(test_window_grows_while_down_and_caps);
   RUN_TEST(test_an_answer_ends_the_outage);
   RUN_TEST(test_retry_now_keeps_the_outage);
   RUN_TEST(test_providers_are_separate_and_unknown_is_ignored);
   RUN_TEST(test_note_goes_in_front_of_an_answer_only);
   return UNITY_END();
}
