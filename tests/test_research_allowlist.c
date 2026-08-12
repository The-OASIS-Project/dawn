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
 * Regression test for the deep-research read-only tool allowlist
 * (core/research_allowlist.h) — DEEP_RESEARCH_DESIGN.md §11 HIGH-1.
 *
 * The allowlist IS the security boundary of a research fetch loop: it is the sole
 * decision in BOTH the native-tool gate (is_tool_enabled_for_session, llm_tools.c)
 * AND the command_execute() defense-in-depth close (command_executor.c).  This
 * pins the exact set so a rename or a new side-effecting tool cannot silently
 * widen it — "side-effecting device refused; research tools still work" reduced to
 * the predicate both call sites consult.
 */

#include "core/research_allowlist.h"
#include "unity.h"

void setUp(void) {
}

void tearDown(void) {
}

/* The four — and ONLY these four — tools a research loop may run. */
static void test_allowlisted_tools_admitted(void) {
   TEST_ASSERT_TRUE(research_tool_is_allowlisted("search"));
   TEST_ASSERT_TRUE(research_tool_is_allowlisted("url_fetch"));
   TEST_ASSERT_TRUE(research_tool_is_allowlisted("research_plan"));
   TEST_ASSERT_TRUE(research_tool_is_allowlisted("research_record"));
}

/* Every side-effecting / outward-facing tool must be refused — these are the
 * concrete verbs the read-only boundary exists to keep out of an untrusted-web
 * fetch loop (HA locks, email/phone egress, shutdown, doc mutation, job fan-out). */
static void test_side_effecting_tools_refused(void) {
   TEST_ASSERT_FALSE(research_tool_is_allowlisted("home_assistant"));
   TEST_ASSERT_FALSE(research_tool_is_allowlisted("email"));
   TEST_ASSERT_FALSE(research_tool_is_allowlisted("phone"));
   TEST_ASSERT_FALSE(research_tool_is_allowlisted("shutdown"));
   TEST_ASSERT_FALSE(research_tool_is_allowlisted("document_manage"));
   TEST_ASSERT_FALSE(research_tool_is_allowlisted("job"));
   TEST_ASSERT_FALSE(research_tool_is_allowlisted("memory"));
   TEST_ASSERT_FALSE(research_tool_is_allowlisted("calendar"));
   TEST_ASSERT_FALSE(research_tool_is_allowlisted("music"));
   TEST_ASSERT_FALSE(research_tool_is_allowlisted("weather"));
}

/* No near-miss slips through (prefix, empty, NULL). */
static void test_no_partial_or_null_match(void) {
   TEST_ASSERT_FALSE(research_tool_is_allowlisted("search_web"));
   TEST_ASSERT_FALSE(research_tool_is_allowlisted("url"));
   TEST_ASSERT_FALSE(research_tool_is_allowlisted("research"));
   TEST_ASSERT_FALSE(research_tool_is_allowlisted(""));
   TEST_ASSERT_FALSE(research_tool_is_allowlisted(NULL));
}

/* research_only is a strict SUBSET: the two ledger writers, never the two reads —
 * these are the tools that must be hidden from every NON-research session. */
static void test_research_only_subset(void) {
   TEST_ASSERT_TRUE(research_tool_is_research_only("research_plan"));
   TEST_ASSERT_TRUE(research_tool_is_research_only("research_record"));
   /* Reads are allowlisted-in-research but NOT research-only (visible elsewhere). */
   TEST_ASSERT_FALSE(research_tool_is_research_only("search"));
   TEST_ASSERT_FALSE(research_tool_is_research_only("url_fetch"));
   TEST_ASSERT_FALSE(research_tool_is_research_only("email"));
   TEST_ASSERT_FALSE(research_tool_is_research_only(NULL));

   /* Invariant: research_only ⊆ allowlisted. */
   TEST_ASSERT_TRUE(research_tool_is_allowlisted("research_plan"));
   TEST_ASSERT_TRUE(research_tool_is_allowlisted("research_record"));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_allowlisted_tools_admitted);
   RUN_TEST(test_side_effecting_tools_refused);
   RUN_TEST(test_no_partial_or_null_match);
   RUN_TEST(test_research_only_subset);
   return UNITY_END();
}
