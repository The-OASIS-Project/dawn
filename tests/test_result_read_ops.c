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
 * What result_read does with a stored result (src/tools/result_read_ops.c).
 */

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm_tool_view.h"
#include "tools/result_read_ops.h"
#include "tools/tool_registry.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

#define BUDGET 4000

/* {"total":N,"results":[{"id":i,"name":"item i","kind":"a|b|c"}, ...]} */
static struct json_object *rows(int n) {
   struct json_object *root = json_object_new_object();
   json_object_object_add(root, "total", json_object_new_int(n));
   struct json_object *arr = json_object_new_array();
   for (int i = 0; i < n; i++) {
      struct json_object *item = json_object_new_object();
      char name[32];
      snprintf(name, sizeof(name), "item %d", i);
      json_object_object_add(item, "id", json_object_new_int(i));
      json_object_object_add(item, "name", json_object_new_string(name));
      json_object_object_add(item, "kind",
                             json_object_new_string(i % 3 == 0   ? "a"
                                                    : i % 3 == 1 ? "b"
                                                                 : "c"));
      json_object_array_add(arr, item);
   }
   json_object_object_add(root, "results", arr);
   return root;
}

static bool is_failure(const char *s) {
   return s && strncmp(s, TOOL_RESULT_ERROR_MARK, strlen(TOOL_RESULT_ERROR_MARK)) == 0;
}

/* A path to one value: the value, named by its path. */
static void test_path_to_a_value(void) {
   struct json_object *root = rows(50);
   char *out = result_read_path(root, "$.results[7]", BUDGET);
   TEST_ASSERT_EQUAL_STRING("$.results[7]:\n{\"id\":7,\"name\":\"item 7\",\"kind\":\"b\"}", out);
   free(out);
   out = result_read_path(root, "$.results[-1].name", BUDGET); /* from the end */
   TEST_ASSERT_EQUAL_STRING("$.results[49].name:\n\"item 49\"", out);
   free(out);
   out = result_read_path(root, "$[\"total\"]", BUDGET);
   TEST_ASSERT_EQUAL_STRING("$.total:\n50", out);
   free(out);
   json_object_put(root);
}

/* A slice keeps its items' own indices, in its paths and its markers. */
static void test_a_slice_keeps_its_indices(void) {
   struct json_object *root = rows(5000);
   char *out = result_read_path(root, "$.results[100:120]", BUDGET);
   TEST_ASSERT_NOT_NULL(strstr(out, "(items 100-119 of 5000)"));
   TEST_ASSERT_NOT_NULL(strstr(out, "\"id\":100,"));
   free(out);
   out = result_read_path(root, "$.results[1000:4000]", 1500);
   TEST_ASSERT_NOT_NULL(strstr(out, "more items at $.results[10"));
   TEST_ASSERT_NOT_NULL(strstr(out, "\"id\":3999"));
   TEST_ASSERT_TRUE(strlen(out) <= 1500 + 128);
   free(out);
   out = result_read_path(root, "$.results[4990:]", BUDGET); /* an open end */
   TEST_ASSERT_NOT_NULL(strstr(out, "(items 4990-4999 of 5000)"));
   free(out);
   out = result_read_path(root, "$.results[6000:7000]", BUDGET);
   TEST_ASSERT_NOT_NULL(strstr(out, "no items"));
   free(out);
   json_object_put(root);
}

/* A bad path says what's wrong, and a missing key lists the keys there. */
static void test_bad_paths(void) {
   struct json_object *root = rows(3);
   static const char *const k_bad[] = { "$.results[9]", "$.nope",       "$.results.id",
                                        "$.total[0]",   "$.results[*]", "$.results[1:2].id",
                                        "$[\"unclosed", "results",      "$.a-b" };
   for (size_t i = 0; i < sizeof(k_bad) / sizeof(k_bad[0]); i++) {
      char *out = result_read_path(root, k_bad[i], BUDGET);
      TEST_ASSERT_TRUE_MESSAGE(is_failure(out), k_bad[i]);
      free(out);
   }
   char *out = result_read_path(root, "$.nope", BUDGET);
   TEST_ASSERT_NOT_NULL(strstr(out, "Its keys: \"total\", \"results\""));
   free(out);
   json_object_put(root);
}

/* A key a view showed cut is found by its cut form; quoted keys decode JSON
 * escapes. */
static void test_keys_as_views_write_them(void) {
   char key[101];
   memset(key, 'k', 100);
   key[100] = '\0';
   struct json_object *root = json_object_new_object();
   json_object_object_add(root, key, json_object_new_int(7));
   json_object_object_add(root, "a \"b\"", json_object_new_int(8));
   json_object_object_add(root, "\xC3\xA9t\xC3\xA9", json_object_new_int(9));
   char *view = llm_tool_view_tree(root, 256, NULL, NULL);
   /* The cut key as the view shows it: find it in the view. */
   const char *start = strstr(view, "\"kkkk");
   TEST_ASSERT_NOT_NULL(start);
   const char *end = strchr(start + 1, '"');
   char path[256];
   snprintf(path, sizeof(path), "$[%.*s\"]", (int)(end - start), start);
   char *out = result_read_path(root, path, BUDGET);
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, ":\n7"), out);
   free(out);
   free(view);
   out = result_read_path(root, "$[\"a \\\"b\\\"\"]", BUDGET);
   TEST_ASSERT_NOT_NULL(strstr(out, ":\n8"));
   free(out);
   out = result_read_path(root, "$[\"\\u00e9t\\u00E9\"]", BUDGET);
   TEST_ASSERT_NOT_NULL(strstr(out, ":\n9"));
   free(out);
   json_object_put(root);
}

static void test_count(void) {
   struct json_object *root = rows(1204);
   char *out = result_read_count(root, "$.results");
   TEST_ASSERT_EQUAL_STRING("$.results: 1204 items", out);
   free(out);
   out = result_read_count(root, "$.results[0]");
   TEST_ASSERT_EQUAL_STRING("$.results[0]: 3 keys", out);
   free(out);
   out = result_read_count(root, "$.results[3].name");
   TEST_ASSERT_EQUAL_STRING("$.results[3].name: a string of 6 characters", out);
   free(out);
   out = result_read_count(root, "$.results[10:20]");
   TEST_ASSERT_EQUAL_STRING("$.results[10:20]: 10 items", out);
   free(out);
   json_object_put(root);
}

/* distinct: a field's values with how many items have each, most first. */
static void test_distinct(void) {
   struct json_object *root = rows(10);
   /* Two items without the field. */
   json_object_object_del(json_object_array_get_idx(json_object_object_get(root, "results"), 0),
                          "kind");
   json_object_array_add(json_object_object_get(root, "results"), json_object_new_int(5));
   char *out = result_read_distinct(root, "$.results", "kind", BUDGET);
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "11 items, 3 distinct values of \"kind\" (2 without "
                                            "it)"),
                                out);
   /* a: 3,6,9  b: 1,4,7  c: 2,5,8 — ties sorted by value. */
   TEST_ASSERT_NOT_NULL(strstr(out, "      3  \"a\"\n      3  \"b\"\n      3  \"c\"\n"));
   free(out);
   out = result_read_distinct(root, "$.total", "kind", BUDGET);
   TEST_ASSERT_TRUE(is_failure(out));
   free(out);
   json_object_put(root);
}

/* grep on a tree: each match's path and the value around it. */
static void test_grep_tree(void) {
   struct json_object *root = rows(300);
   char *out = result_read_grep_tree(root, "ITEM 25", BUDGET);
   /* item 25, and 250-259. */
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "11 matches for \"ITEM 25\":\n"), out);
   TEST_ASSERT_NOT_NULL(strstr(out, "$.results[25].name: \"item 25\"\n"));
   TEST_ASSERT_NOT_NULL(strstr(out, "$.results[259].name: \"item 259\"\n"));
   free(out);
   out = result_read_grep_tree(root, "item", BUDGET);
   TEST_ASSERT_NOT_NULL(strstr(out, "300 matches"));
   TEST_ASSERT_NOT_NULL(strstr(out, "250 more; narrow the pattern"));
   free(out);
   out = result_read_grep_tree(root, "", BUDGET);
   TEST_ASSERT_TRUE(is_failure(out));
   free(out);
   json_object_put(root);
}

/* grep on text: numbered matching lines, with context not repeated. */
static void test_grep_text(void) {
   const char *text = "alpha\nbeta\nGamma ray\ndelta\nepsilon\nzeta gamma\neta\n";
   char *out = result_read_grep_text(text, strlen(text), "gamma", 1, BUDGET);
   TEST_ASSERT_EQUAL_STRING("2 matching lines for \"gamma\":\n"
                            "     2- beta\n"
                            "     3: Gamma ray\n"
                            "     4- delta\n"
                            "     5- epsilon\n"
                            "     6: zeta gamma\n"
                            "     7- eta\n",
                            out);
   free(out);
   /* One character, either case, first in its line. */
   out = result_read_grep_text(text, strlen(text), "Z", 0, BUDGET);
   TEST_ASSERT_EQUAL_STRING("1 matching line for \"Z\":\n     6: zeta gamma\n", out);
   free(out);
   out = result_read_grep_text(text, strlen(text), "iota", 0, BUDGET);
   TEST_ASSERT_EQUAL_STRING("0 matching lines for \"iota\".", out);
   free(out);
   /* A long line: a window around the match. */
   char *big = malloc(5001);
   memset(big, 'x', 5000);
   memcpy(big + 3000, "needle", 6);
   big[5000] = '\0';
   out = result_read_grep_text(big, 5000, "NEEDLE", 0, BUDGET);
   TEST_ASSERT_NOT_NULL(strstr(out, "needle"));
   TEST_ASSERT_NOT_NULL(strstr(out, "(a line of 5000 bytes)"));
   TEST_ASSERT_TRUE(strlen(out) < 400);
   free(out);
   free(big);
}

/* read: lines numbered as in the whole result; a range too big is a view. */
static void test_read_lines(void) {
   char *text = malloc(200000);
   size_t len = 0;
   for (int i = 1; i <= 10000; i++) {
      len += (size_t)sprintf(text + len, "line %d\n", i);
   }
   char *out = result_read_lines(text, len, 5000, 5002, BUDGET);
   TEST_ASSERT_EQUAL_STRING("Lines 5000-5002 of 10000:\n"
                            "  5000\tline 5000\n"
                            "  5001\tline 5001\n"
                            "  5002\tline 5002\n",
                            out);
   free(out);
   out = result_read_lines(text, len, 3000, 9000, 2000);
   TEST_ASSERT_NOT_NULL(strstr(out, "  3000\tline 3000\n"));
   TEST_ASSERT_NOT_NULL(strstr(out, "  9000\tline 9000\n"));
   TEST_ASSERT_NOT_NULL(strstr(out, "omitted: lines "));
   free(out);
   out = result_read_lines(text, len, 20000, 0, BUDGET);
   TEST_ASSERT_TRUE(is_failure(out));
   free(out);
   free(text);
}

/* One line reads whole: nothing is said to be left out when nothing is. */
static void test_one_line_reads_whole(void) {
   const char *text = "alpha\nbeta foo\ngamma";
   char *out = result_read_lines(text, strlen(text), 3, 3, BUDGET);
   TEST_ASSERT_EQUAL_STRING("Lines 3-3 of 3:\n     3\tgamma\n", out);
   free(out);
   out = result_read_lines("just one", 8, 0, 0, BUDGET);
   TEST_ASSERT_EQUAL_STRING("Lines 1-1 of 1:\n     1\tjust one\n", out);
   free(out);
}

/* grep's context stops at the last line; a pattern is one line; a path is
 * one expression; distinct on a slice names the slice. */
static void test_edges(void) {
   const char *text = "a\nb\nc\nd\ne\nmatch\n";
   char *out = result_read_grep_text(text, strlen(text), "match", 2, BUDGET);
   TEST_ASSERT_NULL_MESSAGE(strstr(out, "     7-"), out);
   free(out);
   out = result_read_grep_text(text, strlen(text), "a\nb", 0, BUDGET);
   TEST_ASSERT_TRUE(is_failure(out));
   free(out);
   struct json_object *root = rows(10);
   out = result_read_path(root, "$ .results", BUDGET);
   TEST_ASSERT_TRUE(is_failure(out));
   free(out);
   out = result_read_path(root, "$.results[1] .name", BUDGET);
   TEST_ASSERT_TRUE(is_failure(out));
   free(out);
   out = result_read_distinct(root, "$.results[0:5]", "kind", BUDGET);
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "$.results[0:5]: 5 items"), out);
   free(out);
   json_object_put(root);
}

/* A path to a null is null (json-c's NULL object), not a failure; grep
 * finds null too; a key with a tab resolves from the path a view wrote. */
static void test_nulls_and_control_keys(void) {
   struct json_object *root = json_tokener_parse("{\"a\":null,\"arr\":[null,1],\"k\\tx\":7}");
   char *out = result_read_path(root, "$.a", BUDGET);
   TEST_ASSERT_EQUAL_STRING("$.a:\nnull", out);
   free(out);
   out = result_read_path(root, "$.arr[0]", BUDGET);
   TEST_ASSERT_EQUAL_STRING("$.arr[0]:\nnull", out);
   free(out);
   out = result_read_grep_tree(root, "null", BUDGET);
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "2 matches"), out);
   free(out);
   out = result_read_path(root, "$[\"k\\tx\"]", BUDGET);
   TEST_ASSERT_EQUAL_STRING("$[\"k\\tx\"]:\n7", out);
   free(out);
   json_object_put(root);
}

/* A path grep prints for a long key (shown cut, as views show it) resolves. */
static void test_a_grep_path_resolves(void) {
   char key[101];
   memset(key, 'q', 100);
   key[100] = '\0';
   struct json_object *root = json_object_new_object();
   json_object_object_add(root, key, json_object_new_string("needle here"));
   char *out = result_read_grep_tree(root, "needle", BUDGET);
   const char *path = strstr(out, "$[");
   TEST_ASSERT_NOT_NULL_MESSAGE(path, out);
   const char *end = strstr(path, "\"]: ");
   TEST_ASSERT_NOT_NULL(end);
   char copied[512];
   snprintf(copied, sizeof(copied), "%.*s", (int)(end + 2 - path), path);
   char *value = result_read_path(root, copied, BUDGET);
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(value, "\"needle here\""), value);
   free(value);
   free(out);
   json_object_put(root);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_path_to_a_value);
   RUN_TEST(test_a_slice_keeps_its_indices);
   RUN_TEST(test_bad_paths);
   RUN_TEST(test_keys_as_views_write_them);
   RUN_TEST(test_count);
   RUN_TEST(test_distinct);
   RUN_TEST(test_grep_tree);
   RUN_TEST(test_grep_text);
   RUN_TEST(test_read_lines);
   RUN_TEST(test_one_line_reads_whole);
   RUN_TEST(test_edges);
   RUN_TEST(test_nulls_and_control_keys);
   RUN_TEST(test_a_grep_path_resolves);
   return UNITY_END();
}
