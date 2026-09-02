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
 * Unit tests for the recall result formatter (src/tools/recall_format.c).
 * Drives recall_format_result() against synthetic focus_compose results — no
 * daemon, no DB, no embedding engine.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/focus/focus_source.h"
#include "tools/recall_format.h"
#include "unity.h"

/* Stubs: recall_format.c records surfaced facts + gates its citation footer via
 * the memory_citation module (Option B).  This is a pure-formatter unit test, so
 * stub both — recording is a no-op, citation reads as disabled (no footer hint,
 * keeping the existing assertions stable). */
void memory_citation_record_tool_fact_current(int64_t fact_id) {
   (void)fact_id;
}
bool memory_citation_enabled(void) {
   return false;
}

void setUp(void) {
}
void tearDown(void) {
}

/* Build a candidate that owns nothing the formatter frees (the formatter only
 * reads).  We strdup so there are no -Wwrite-strings issues and free after. */
static focus_candidate_t mk(const char *src, const char *item_id, const char *text) {
   focus_candidate_t c;
   memset(&c, 0, sizeof(c));
   c.source_id = src; /* const string literal — fine, framework owns in prod */
   c.item_id = strdup(item_id);
   c.text = strdup(text);
   return c;
}

static void free_candidates(focus_candidate_t *arr, int n) {
   for (int i = 0; i < n; i++) {
      free(arr[i].item_id);
      free(arr[i].text);
   }
}

static void test_zero_results_is_explicit(void) {
   focus_compose_result_t res;
   memset(&res, 0, sizeof(res));
   char *out = recall_format_result("project zephyr", &res, NULL, 0);
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_NOT_NULL(strstr(out, "nothing on file"));
   TEST_ASSERT_NOT_NULL(strstr(out, "project zephyr"));
   free(out);
}

static void test_grouping_and_pointers(void) {
   focus_candidate_t cands[4];
   cands[0] = mk("memory_fact", "fact:7858", "Open Sauce booth accepted on 2026-02-13.");
   cands[1] = mk("memory_summary", "summary:42", "Discussed slipping the beta to June.");
   cands[2] = mk("document_chunk", "document_chunk:903", "[spec_v3.pdf] Section 4 lists criteria.");
   cands[3] = mk("calendar_event", "calendar_occ:11", "Design review");

   focus_compose_result_t res;
   memset(&res, 0, sizeof(res));
   res.candidates = cands;
   res.candidate_count = 4;

   char *out = recall_format_result("open sauce status", &res, NULL, 0);
   TEST_ASSERT_NOT_NULL(out);

   /* Section headers present. */
   TEST_ASSERT_NOT_NULL(strstr(out, "MEMORY — facts & relationships"));
   TEST_ASSERT_NOT_NULL(strstr(out, "MEMORY — past conversation summaries"));
   TEST_ASSERT_NOT_NULL(strstr(out, "NOTES & DOCUMENTS"));
   TEST_ASSERT_NOT_NULL(strstr(out, "CALENDAR"));

   /* Read-pointers: fact gets an [ID:x] marker, document gets document_read. */
   TEST_ASSERT_NOT_NULL(strstr(out, "[ID:7858]"));
   TEST_ASSERT_NOT_NULL(strstr(out, "document_read \"spec_v3.pdf\""));
   TEST_ASSERT_NOT_NULL(strstr(out, "calendar"));
   /* Summary text is shown but gets NO fetch pointer — `memory get` can't
    * resolve a summary id, so `[ID:42]` must NOT appear. */
   TEST_ASSERT_NOT_NULL(strstr(out, "slipping the beta"));
   TEST_ASSERT_NULL(strstr(out, "[ID:42]"));

   free(out);
   free_candidates(cands, 4);
}

static void test_empty_families_listed(void) {
   focus_candidate_t cands[1];
   cands[0] = mk("memory_fact", "fact:1", "Only a memory fact here.");

   focus_compose_result_t res;
   memset(&res, 0, sizeof(res));
   res.candidates = cands;
   res.candidate_count = 1;

   char *out = recall_format_result("q", &res, NULL, 0);
   TEST_ASSERT_NOT_NULL(out);
   /* Families with no hits are named so absence is legible to the LLM. */
   TEST_ASSERT_NOT_NULL(strstr(out, "Nothing found in:"));
   TEST_ASSERT_NOT_NULL(strstr(out, "NOTES & DOCUMENTS"));
   TEST_ASSERT_NOT_NULL(strstr(out, "CALENDAR"));
   free(out);
   free_candidates(cands, 1);
}

static void test_null_injected_emits_overlap_note(void) {
   focus_candidate_t cands[1];
   cands[0] = mk("memory_fact", "fact:1", "A fact.");
   focus_compose_result_t res;
   memset(&res, 0, sizeof(res));
   res.candidates = cands;
   res.candidate_count = 1;

   char *out = recall_format_result("q", &res, NULL, 0);
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_NOT_NULL(strstr(out, "in this turn's injected context"));
   free(out);
   free_candidates(cands, 1);
}

static void test_injected_id_is_marked(void) {
   focus_candidate_t cands[2];
   cands[0] = mk("memory_fact", "fact:100", "Already-injected fact.");
   cands[1] = mk("memory_fact", "fact:200", "Fresh fact.");
   focus_compose_result_t res;
   memset(&res, 0, sizeof(res));
   res.candidates = cands;
   res.candidate_count = 2;

   const char *injected[] = { "fact:100" };
   char *out = recall_format_result("q", &res, injected, 1);
   TEST_ASSERT_NOT_NULL(out);
   /* The injected one is marked; the count footer reflects it. */
   TEST_ASSERT_NOT_NULL(strstr(out, "already in current context"));
   /* The NULL-fallback overlap note must NOT appear when a set was supplied. */
   TEST_ASSERT_NULL(strstr(out, "in this turn's injected context"));
   free(out);
   free_candidates(cands, 2);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_zero_results_is_explicit);
   RUN_TEST(test_grouping_and_pointers);
   RUN_TEST(test_empty_families_listed);
   RUN_TEST(test_null_injected_emits_overlap_note);
   RUN_TEST(test_injected_id_is_marked);
   return UNITY_END();
}
