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
 * A conversation's tools are decided by registration, never by enable flags:
 * toggling a tool or a component coming online changes no definition (so
 * appends no tool_change row and declares no boundary), and a tool a session
 * may not use, frozen or added by a change, is never offered to it for
 * execution and is refused when called.  Links the real llm_tools.c against
 * the dup-check stub, filling the tool table directly.
 */

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "core/session_manager.h"
#include "llm/llm_tool_defs.h"
#include "llm/llm_tools.h"
#include "llm/llm_tools_internal.h"
#include "unity.h"

extern void *g_stub_command_context;

static session_t s_ctx;

static void add_tool(const char *name, bool enabled, bool local) {
   tool_definition_t *t = &llm_tools_table[llm_tools_count++];
   memset(t, 0, sizeof(*t));
   snprintf(t->name, sizeof(t->name), "%s", name);
   snprintf(t->description, sizeof(t->description), "The %s tool", name);
   t->enabled = enabled;
   t->enabled_local = local;
   t->enabled_remote = local;
}

void setUp(void) {
   llm_tools_count = 0;
   llm_tools_ready = true;
   add_tool("weather", true, true);
   add_tool("hud_control", false, true); /* the helmet is offline */
   add_tool("job", true, true);
   memset(&s_ctx, 0, sizeof(s_ctx));
   g_stub_command_context = NULL;
}

void tearDown(void) {
   g_stub_command_context = NULL;
   llm_tools_filter_release();
   llm_tools_count = 0;
   llm_tools_ready = false;
}

/* Every registered tool is defined, enabled or not, and a toggle (or a
 * component coming online) changes no definition. */
static void test_definitions_ignore_enable_flags(void) {
   char *before = llm_tools_definitions();
   TEST_ASSERT_NOT_NULL(before);
   TEST_ASSERT_NOT_NULL(strstr(before, "\"name\":\"hud_control\""));
   llm_tools_table[1].enabled = true;          /* the helmet comes online */
   llm_tools_table[0].enabled_local = false;   /* a tool turned off */
   atomic_fetch_add(&llm_tools_generation, 1); /* recomputed, not cached */
   char *after = llm_tools_definitions();
   TEST_ASSERT_EQUAL_STRING(before, after);
   free(before);
   free(after);
}

/* A history whose frozen set has weather, and a change that added job. */
static struct json_object *history_with_a_change(void) {
   struct json_object *h = json_tokener_parse(
       "[{\"role\":\"system\",\"content\":\"P\",\"_kind\":\"prefix\",\"_tools\":"
       "[{\"name\":\"weather\",\"description\":\"W\",\"parameters\":{\"type\":\"object\"}}]},"
       "{\"role\":\"user\",\"content\":\"Q\"}]");
   struct json_object *defs = json_tokener_parse(
       "[{\"name\":\"job\",\"description\":\"J\",\"parameters\":{\"type\":\"object\"}}]");
   json_object_array_add(h, llm_tool_change_new(defs, true));
   return h;
}

/* A job's own session: the job tool a change added is defined but never
 * offered for execution, and a call to it is refused. */
static void test_a_job_session_is_never_offered_a_changed_tool(void) {
   struct json_object *h = history_with_a_change();
   s_ctx.type = SESSION_TYPE_JOB;
   g_stub_command_context = &s_ctx;
   TEST_ASSERT_TRUE(llm_tools_request_offers(h, false, "weather"));
   TEST_ASSERT_FALSE(llm_tools_request_offers(h, false, "job"));
   tool_call_t call;
   memset(&call, 0, sizeof(call));
   snprintf(call.id, sizeof(call.id), "c1");
   snprintf(call.name, sizeof(call.name), "job");
   tool_result_t result;
   memset(&result, 0, sizeof(result));
   llm_tools_execute(&call, &result);
   TEST_ASSERT_FALSE(result.success);
   free(result.result_extended);
   json_object_put(h);
}

/* A research run: its request carries the allowlist, never the
 * conversation's tools or their changes. */
static void test_a_research_session_gets_the_allowlist_only(void) {
   struct json_object *h = history_with_a_change();
   atomic_store(&s_ctx.research_run_id, 7);
   s_ctx.type = SESSION_TYPE_JOB;
   g_stub_command_context = &s_ctx;
   const char *source = NULL;
   struct json_object *tools = llm_tools_request_tools(h, false, true, true, &source);
   TEST_ASSERT_EQUAL_STRING("the research allowlist", source);
   TEST_ASSERT_NULL(strstr(tools ? json_object_to_json_string(tools) : "", "\"job\""));
   json_object_put(tools);
   TEST_ASSERT_FALSE(llm_tools_request_offers(h, false, "job"));
   json_object_put(h);
}

/* Elsewhere a change's tool is offered like any frozen one. */
static void test_a_changed_tool_is_offered_where_it_may_run(void) {
   struct json_object *h = history_with_a_change();
   s_ctx.type = SESSION_TYPE_WEBUI;
   g_stub_command_context = &s_ctx;
   TEST_ASSERT_TRUE(llm_tools_request_offers(h, true, "job"));
   TEST_ASSERT_FALSE(llm_tools_request_offers(h, true, "hud_control")); /* not defined */
   json_object_put(h);
}

/* The request's tools render the conversation's definitions as they were
 * (parameters by reference), and its cache breakpoint lands on the outer tool
 * object: the history's definitions are left as they were. */
static void test_rendering_shares_parameters_and_leaves_the_history(void) {
   struct json_object *h = history_with_a_change();
   s_ctx.type = SESSION_TYPE_WEBUI;
   g_stub_command_context = &s_ctx;
   const char *hist_before = json_object_to_json_string_ext(h, JSON_C_TO_STRING_PLAIN);
   char *before = strdup(hist_before);
   struct json_object *tools = llm_tools_request_tools(h, false, true, false, NULL);
   TEST_ASSERT_NOT_NULL(tools);
   TEST_ASSERT_EQUAL_STRING("[{\"name\":\"weather\",\"description\":\"W\",\"input_schema\":{"
                            "\"type\":\"object\"}},{\"name\":\"job\",\"description\":\"J\","
                            "\"input_schema\":{\"type\":\"object\"}}]",
                            json_object_to_json_string_ext(tools, JSON_C_TO_STRING_PLAIN));
   struct json_object *last = json_object_array_get_idx(tools, 1);
   json_object_object_add(last, "cache_control", json_tokener_parse("{\"type\":\"ephemeral\"}"));
   (void)json_object_to_json_string_ext(tools, JSON_C_TO_STRING_PLAIN);
   json_object_put(tools);
   TEST_ASSERT_EQUAL_STRING(before, json_object_to_json_string_ext(h, JSON_C_TO_STRING_PLAIN));
   free(before);
   json_object_put(h);
}

/* A stored change's definitions are validated once, when loaded: one that
 * fails is left out of its text there, and a request reads it as it is. */
static void test_a_loaded_change_is_validated_once(void) {
   struct json_object *defs = json_tokener_parse(
       "[{\"name\":\"ok\",\"description\":\"O\",\"parameters\":{\"type\":\"object\"}},"
       "{\"name\":\"bad name!\",\"description\":\"B\",\"parameters\":{}}]");
   struct json_object *msg = llm_tool_change_new(defs, false);
   TEST_ASSERT_TRUE(llm_tool_change_normalize(msg));
   struct json_object *kept = llm_tool_change_defs(msg);
   TEST_ASSERT_EQUAL_INT(1, (int)json_object_array_length(kept));
   json_object_put(kept);
   TEST_ASSERT_FALSE(llm_tool_change_stored_inline(msg));
   json_object_put(msg);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_definitions_ignore_enable_flags);
   RUN_TEST(test_a_job_session_is_never_offered_a_changed_tool);
   RUN_TEST(test_a_research_session_gets_the_allowlist_only);
   RUN_TEST(test_a_changed_tool_is_offered_where_it_may_run);
   RUN_TEST(test_rendering_shares_parameters_and_leaves_the_history);
   RUN_TEST(test_a_loaded_change_is_validated_once);
   return UNITY_END();
}
