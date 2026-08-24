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
 * Unit tests for the memory-citation <cited> tokenizer + two-set validation
 * (Option B).  Drives the pure memory_citation_resolve_cited() with focus-stash
 * and tool-set fixtures — no session, DB, or config.
 */

#include <stdint.h>
#include <string.h>

#include "memory/memory_citation_internal.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

static citation_stash_t g_stash;
static tool_cited_set_t g_tool;
static char g_all[256];
static char g_focus[256];
static int g_fc, g_tc, g_dr, g_dt;

static void set_stash(const char *const *ids, int n) {
   memset(&g_stash, 0, sizeof(g_stash));
   for (int i = 0; i < n && i < MAX_CITATION_STASH; i++) {
      strncpy(g_stash.entries[i].item_id, ids[i], sizeof(g_stash.entries[i].item_id) - 1);
   }
   g_stash.count = n;
}

static void set_tool(const int64_t *ids, int n) {
   memset(&g_tool, 0, sizeof(g_tool));
   for (int i = 0; i < n && i < MAX_TOOL_CITED_FACTS; i++) {
      g_tool.entries[i].fact_id = ids[i];
   }
   g_tool.count = n;
}

static void run(const char *text) {
   memory_citation_resolve_cited(text, &g_stash, &g_tool, g_all, sizeof(g_all), g_focus,
                                 sizeof(g_focus), &g_fc, &g_tc, &g_dr, &g_dt);
}

/* ── focus-only (Phase-1 back-compat) ─────────────────────────────────────── */

static void test_focus_ordinals(void) {
   const char *ids[] = { "fact:10", "fact:20", "fact:30" };
   set_stash(ids, 3);
   set_tool(NULL, 0);
   run("answer <cited>M1,M3</cited>");
   TEST_ASSERT_EQUAL_STRING("fact:10,fact:30", g_all);
   TEST_ASSERT_EQUAL_STRING("fact:10,fact:30", g_focus);
   TEST_ASSERT_EQUAL_INT(2, g_fc);
   TEST_ASSERT_EQUAL_INT(0, g_tc);
   TEST_ASSERT_EQUAL_INT(0, g_dr);
   TEST_ASSERT_EQUAL_INT(0, g_dt);
}

static void test_bare_numbers_are_ordinals(void) {
   const char *ids[] = { "fact:10", "fact:20", "fact:30" };
   set_stash(ids, 3);
   set_tool(NULL, 0);
   run("<cited>1,3</cited>");
   TEST_ASSERT_EQUAL_STRING("fact:10,fact:30", g_focus);
   TEST_ASSERT_EQUAL_INT(2, g_fc);
}

static void test_ordinal_out_of_range_dropped(void) {
   const char *ids[] = { "fact:10", "fact:20" };
   set_stash(ids, 2);
   set_tool(NULL, 0);
   run("<cited>M9</cited>");
   TEST_ASSERT_EQUAL_STRING("", g_all);
   TEST_ASSERT_EQUAL_INT(0, g_fc);
   TEST_ASSERT_EQUAL_INT(1, g_dr);
}

static void test_duplicate_ordinal_dropped(void) {
   const char *ids[] = { "fact:10" };
   set_stash(ids, 1);
   set_tool(NULL, 0);
   run("<cited>M1,M1</cited>");
   TEST_ASSERT_EQUAL_STRING("fact:10", g_focus);
   TEST_ASSERT_EQUAL_INT(1, g_fc);
   TEST_ASSERT_EQUAL_INT(1, g_dr); /* the second M1 */
}

static void test_orphan_opener_still_parses(void) {
   const char *ids[] = { "fact:10", "fact:20" };
   set_stash(ids, 2);
   set_tool(NULL, 0);
   run("text <cited>M2"); /* no closer */
   TEST_ASSERT_EQUAL_STRING("fact:20", g_focus);
   TEST_ASSERT_EQUAL_INT(1, g_fc);
}

/* ── tool-sourced (Option B) ──────────────────────────────────────────────── */

static void test_tool_id_only(void) {
   const int64_t t[] = { 5881, 5939 };
   set_stash(NULL, 0);
   set_tool(t, 2);
   run("<cited>ID:5881</cited>");
   TEST_ASSERT_EQUAL_STRING("fact:5881", g_all);
   TEST_ASSERT_EQUAL_STRING("", g_focus); /* NOT broadcast (no panel row) */
   TEST_ASSERT_EQUAL_INT(0, g_fc);
   TEST_ASSERT_EQUAL_INT(1, g_tc);
   TEST_ASSERT_EQUAL_INT(0, g_dt);
}

static void test_tool_id_lowercase(void) {
   const int64_t t[] = { 5881 };
   set_stash(NULL, 0);
   set_tool(t, 1);
   run("<cited>id:5881</cited>");
   TEST_ASSERT_EQUAL_STRING("fact:5881", g_all);
   TEST_ASSERT_EQUAL_INT(1, g_tc);
}

static void test_tool_id_not_surfaced_dropped(void) {
   const int64_t t[] = { 5881 };
   set_stash(NULL, 0);
   set_tool(t, 1);
   run("<cited>ID:9999</cited>"); /* mis-copied / hallucinated */
   TEST_ASSERT_EQUAL_STRING("", g_all);
   TEST_ASSERT_EQUAL_INT(0, g_tc);
   TEST_ASSERT_EQUAL_INT(1, g_dt);
}

static void test_malformed_id_no_colon_ignored(void) {
   const int64_t t[] = { 6432 };
   set_stash(NULL, 0);
   set_tool(t, 1);
   run("<cited>ID6432</cited>"); /* no colon — must NOT validate as ordinal 6432 or fact 6432 */
   TEST_ASSERT_EQUAL_STRING("", g_all);
   TEST_ASSERT_EQUAL_INT(0, g_tc);
   TEST_ASSERT_EQUAL_INT(0, g_dt);
}

/* ── mixed focus + tool ───────────────────────────────────────────────────── */

static void test_mixed_focus_then_tool(void) {
   const char *ids[] = { "fact:10", "fact:20" };
   const int64_t t[] = { 5939 };
   set_stash(ids, 2);
   set_tool(t, 1);
   run("<cited>M1,ID:5939</cited>");
   TEST_ASSERT_EQUAL_STRING("fact:10,fact:5939", g_all);
   TEST_ASSERT_EQUAL_STRING("fact:10", g_focus);
   TEST_ASSERT_EQUAL_INT(1, g_fc);
   TEST_ASSERT_EQUAL_INT(1, g_tc);
}

static void test_mixed_tool_then_focus(void) {
   const char *ids[] = { "fact:10", "fact:20" };
   const int64_t t[] = { 5939 };
   set_stash(ids, 2);
   set_tool(t, 1);
   run("<cited>ID:5939,M1</cited>"); /* order independent */
   TEST_ASSERT_EQUAL_STRING("fact:5939,fact:10", g_all);
   TEST_ASSERT_EQUAL_STRING("fact:10", g_focus);
   TEST_ASSERT_EQUAL_INT(1, g_fc);
   TEST_ASSERT_EQUAL_INT(1, g_tc);
}

static void test_cross_provenance_dedup(void) {
   /* Same fact cited via BOTH a focus ordinal and its tool id — once in cited_all,
    * present in cited_focus (it has a panel row), NOT counted as a tool cite. */
   const char *ids[] = { "fact:5881" };
   const int64_t t[] = { 5881 };
   set_stash(ids, 1);
   set_tool(t, 1);
   run("<cited>M1,ID:5881</cited>");
   TEST_ASSERT_EQUAL_STRING("fact:5881", g_all);
   TEST_ASSERT_EQUAL_STRING("fact:5881", g_focus);
   TEST_ASSERT_EQUAL_INT(1, g_fc);
   TEST_ASSERT_EQUAL_INT(0, g_tc); /* the ID re-cite dedups, not an independent tool cite */
   TEST_ASSERT_EQUAL_INT(0, g_dt);
}

/* ── robustness ───────────────────────────────────────────────────────────── */

static void test_no_tag(void) {
   const char *ids[] = { "fact:10" };
   set_stash(ids, 1);
   set_tool(NULL, 0);
   run("a plain reply, no citation");
   TEST_ASSERT_EQUAL_STRING("", g_all);
   TEST_ASSERT_EQUAL_INT(0, g_fc);
   TEST_ASSERT_EQUAL_INT(0, g_dr);
}

static void test_null_text_safe(void) {
   const char *ids[] = { "fact:10" };
   set_stash(ids, 1);
   set_tool(NULL, 0);
   run(NULL); /* must not crash */
   TEST_ASSERT_EQUAL_STRING("", g_all);
   TEST_ASSERT_EQUAL_INT(0, g_fc);
}

static void test_id_space_before_colon(void) {
   const int64_t t[] = { 5881 };
   set_stash(NULL, 0);
   set_tool(t, 1);
   run("<cited>ID :5881</cited>"); /* space between ID and : — still a tool cite */
   TEST_ASSERT_EQUAL_STRING("fact:5881", g_all);
   TEST_ASSERT_EQUAL_INT(1, g_tc);
}

static void test_id_first_cross_provenance(void) {
   /* Same fact, tool id BEFORE the focus ordinal — must match the focus-first
    * result (fc=1, tc=0): tool count is order-independent. */
   const char *ids[] = { "fact:5881" };
   const int64_t t[] = { 5881 };
   set_stash(ids, 1);
   set_tool(t, 1);
   run("<cited>ID:5881,M1</cited>");
   TEST_ASSERT_EQUAL_STRING("fact:5881", g_all);
   TEST_ASSERT_EQUAL_STRING("fact:5881", g_focus);
   TEST_ASSERT_EQUAL_INT(1, g_fc);
   TEST_ASSERT_EQUAL_INT(0, g_tc);
   TEST_ASSERT_EQUAL_INT(0, g_dt);
}

static void test_malformed_id_adjacent_to_ordinal(void) {
   /* A hallucinated tool id next to a valid focus ordinal: the ID drops, the
    * ordinal still cites — the malformed token must not corrupt the ordinal. */
   const char *ids[] = { "fact:10" };
   const int64_t t[] = { 5881 };
   set_stash(ids, 1);
   set_tool(t, 1);
   run("<cited>ID:9999,M1</cited>");
   TEST_ASSERT_EQUAL_STRING("fact:10", g_all);
   TEST_ASSERT_EQUAL_STRING("fact:10", g_focus);
   TEST_ASSERT_EQUAL_INT(1, g_fc);
   TEST_ASSERT_EQUAL_INT(0, g_tc);
   TEST_ASSERT_EQUAL_INT(1, g_dt);
}

static void test_tool_orphan_opener(void) {
   const int64_t t[] = { 5881 };
   set_stash(NULL, 0);
   set_tool(t, 1);
   run("answer <cited>ID:5881"); /* no closer — orphan-tolerant */
   TEST_ASSERT_EQUAL_STRING("fact:5881", g_all);
   TEST_ASSERT_EQUAL_INT(1, g_tc);
}

static void test_overlong_id_tail_drops_and_preserves_next(void) {
   /* A >18-digit id caps + consumes its tail; it drops, and the adjacent M1
    * must still cite (the over-long tail can't corrupt the next token). */
   const char *ids[] = { "fact:10" };
   const int64_t t[] = { 5881 };
   set_stash(ids, 1);
   set_tool(t, 1);
   run("<cited>ID:1234567890123456789,M1</cited>");
   TEST_ASSERT_EQUAL_STRING("fact:10", g_all);
   TEST_ASSERT_EQUAL_STRING("fact:10", g_focus);
   TEST_ASSERT_EQUAL_INT(1, g_fc);
   TEST_ASSERT_EQUAL_INT(1, g_dt); /* the over-long id is not in the set */
}

static void test_overlong_ordinal_drops(void) {
   const char *ids[] = { "fact:10", "fact:20", "fact:30" };
   set_stash(ids, 3);
   set_tool(NULL, 0);
   run("<cited>M12345678</cited>"); /* >7 digits, way out of range */
   TEST_ASSERT_EQUAL_STRING("", g_all);
   TEST_ASSERT_EQUAL_INT(0, g_fc);
   TEST_ASSERT_EQUAL_INT(1, g_dr);
}

static void test_focus_summary_and_tool_fact(void) {
   /* Focus item is a SUMMARY (different id-space than the tool fact) — both are
    * counted: derived cited_tool_count = distinct(2) - focus(1) = 1. */
   const char *ids[] = { "summary:2490" };
   const int64_t t[] = { 5881 };
   set_stash(ids, 1);
   set_tool(t, 1);
   run("<cited>M1,ID:5881</cited>");
   TEST_ASSERT_EQUAL_STRING("summary:2490,fact:5881", g_all);
   TEST_ASSERT_EQUAL_STRING("summary:2490", g_focus);
   TEST_ASSERT_EQUAL_INT(1, g_fc);
   TEST_ASSERT_EQUAL_INT(1, g_tc);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_focus_ordinals);
   RUN_TEST(test_bare_numbers_are_ordinals);
   RUN_TEST(test_ordinal_out_of_range_dropped);
   RUN_TEST(test_duplicate_ordinal_dropped);
   RUN_TEST(test_orphan_opener_still_parses);
   RUN_TEST(test_tool_id_only);
   RUN_TEST(test_tool_id_lowercase);
   RUN_TEST(test_tool_id_not_surfaced_dropped);
   RUN_TEST(test_malformed_id_no_colon_ignored);
   RUN_TEST(test_mixed_focus_then_tool);
   RUN_TEST(test_mixed_tool_then_focus);
   RUN_TEST(test_cross_provenance_dedup);
   RUN_TEST(test_no_tag);
   RUN_TEST(test_null_text_safe);
   RUN_TEST(test_id_space_before_colon);
   RUN_TEST(test_id_first_cross_provenance);
   RUN_TEST(test_malformed_id_adjacent_to_ordinal);
   RUN_TEST(test_tool_orphan_opener);
   RUN_TEST(test_overlong_id_tail_drops_and_preserves_next);
   RUN_TEST(test_overlong_ordinal_drops);
   RUN_TEST(test_focus_summary_and_tool_fact);
   return UNITY_END();
}
