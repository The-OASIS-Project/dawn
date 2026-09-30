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
 * A view of a tool result too big to send whole (src/llm/llm_tool_view.c).
 */

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm_tool_view.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* ---- builders ---- */

typedef struct {
   char *s;
   size_t len;
   size_t cap;
} sb_t;

static void sb_put(sb_t *b, const char *s) {
   const size_t n = strlen(s);
   if (b->len + n + 1 > b->cap) {
      b->cap = (b->len + n + 1) * 2;
      b->s = realloc(b->s, b->cap);
      TEST_ASSERT_NOT_NULL(b->s);
   }
   memcpy(b->s + b->len, s, n + 1);
   b->len += n;
}

/* {"total":N,"results":[{"id":i,"name":"item i","file":"src/f<i>.c"}, ...]} */
static char *wide_array(int n) {
   sb_t b = { 0 };
   char item[128];
   snprintf(item, sizeof(item), "{\"total\":%d,\"results\":[", n);
   sb_put(&b, item);
   for (int i = 0; i < n; i++) {
      snprintf(item, sizeof(item), "%s{\"id\":%d,\"name\":\"item %d\",\"file\":\"src/f%d.c\"}",
               i ? "," : "", i, i, i);
      sb_put(&b, item);
   }
   sb_put(&b, "]}");
   return b.s;
}

/* A view: the result, which must fit its budget (and, for JSON, parse). */
static char *view(const char *text, size_t budget, llm_tool_view_info_t *info) {
   llm_tool_view_info_t local;
   llm_tool_view_info_t *i = info ? info : &local;
   char *v = llm_tool_view(text, strlen(text), budget, NULL, i);
   TEST_ASSERT_NOT_NULL(v);
   const size_t cap = budget < LLM_TOOL_VIEW_MIN_BUDGET ? LLM_TOOL_VIEW_MIN_BUDGET : budget;
   TEST_ASSERT_TRUE_MESSAGE(strlen(v) <= cap, "the view exceeds its budget");
   TEST_ASSERT_EQUAL_size_t(strlen(v), i->view_bytes);
   if (i->mode == LLM_TOOL_VIEW_JSON) {
      struct json_object *parsed = json_tokener_parse(v);
      TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "the JSON view isn't valid JSON");
      json_object_put(parsed);
   }
   return v;
}

/* Whether @p s is valid UTF-8. */
static bool valid_utf8(const char *s) {
   const unsigned char *p = (const unsigned char *)s;
   while (*p) {
      const size_t n = *p < 0x80         ? 1
                       : (*p >> 5) == 6  ? 2
                       : (*p >> 4) == 14 ? 3
                       : (*p >> 3) == 30 ? 4
                                         : 0;
      if (n == 0) {
         return false;
      }
      for (size_t i = 1; i < n; i++) {
         if ((p[i] & 0xC0) != 0x80) {
            return false;
         }
      }
      p += n;
   }
   return true;
}

/* ---- tests ---- */

/* A result that fits is returned whole, untouched. */
static void test_a_result_that_fits_is_whole(void) {
   llm_tool_view_info_t info;
   char *v = view("{\"a\": [1, 2, 3]}", 1000, &info);
   TEST_ASSERT_EQUAL_STRING("{\"a\": [1, 2, 3]}", v);
   TEST_ASSERT_FALSE(info.shortened);
   free(v);
}

/* A wide array keeps its first items and its last, with a marker naming the
 * elided range's path and the items' shape. */
static void test_a_wide_array(void) {
   char *text = wide_array(1000);
   llm_tool_view_info_t info;
   char *v = view(text, 2000, &info);
   TEST_ASSERT_TRUE(info.shortened);
   TEST_ASSERT_EQUAL_INT(LLM_TOOL_VIEW_JSON, info.mode);
   TEST_ASSERT_NOT_NULL(strstr(v, "\"total\":1000"));
   TEST_ASSERT_NOT_NULL(strstr(v, "\"id\":0"));
   TEST_ASSERT_NOT_NULL(strstr(v, "\"id\":999")); /* the last item */
   TEST_ASSERT_NOT_NULL(strstr(v, "more items at $.results["));
   TEST_ASSERT_NOT_NULL(strstr(v, "each {id:int, name:str, file:str}"));
   free(v);
   free(text);
}

/* A roomy budget loosens the shape: the view uses more than half of it. */
static void test_a_roomy_budget_is_used(void) {
   char *text = wide_array(3000);
   llm_tool_view_info_t info;
   char *v = view(text, 30000, &info);
   TEST_ASSERT_TRUE(info.shortened);
   TEST_ASSERT_TRUE(info.view_bytes > 15000);
   free(v);
   free(text);
}

/* An elision is made only when it saves more than its marker costs: a few
 * items or a string a little too long are shown whole. */
static void test_small_elisions_are_not_made(void) {
   char *arr = wide_array(400);
   char *text = malloc(strlen(arr) + 300);
   snprintf(
       text, strlen(arr) + 300,
       "{\"tags\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\"],\"name\":\"%0430d\",\"data\":%s}", 0,
       arr);
   char *v = view(text, 3000, NULL);
   TEST_ASSERT_NOT_NULL(strstr(v, "\"tags\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\"]"));
   TEST_ASSERT_NULL(strstr(v, "at $.name)")); /* 430 chars: under the cut threshold */
   free(v);
   free(text);
   free(arr);
}

/* A deep value is shown to a depth, then as a marker with its path. */
static void test_a_deep_value(void) {
   sb_t b = { 0 };
   for (int i = 0; i < 25; i++) {
      sb_put(&b, "{\"n\":");
   }
   sb_put(&b, "\"bottom\"");
   for (int i = 0; i < 25; i++) {
      sb_put(&b, "}");
   }
   /* Padding so it doesn't fit. */
   char *text = malloc(b.len + 4000);
   snprintf(text, b.len + 4000, "[%s,\"%0*d\"]", b.s, 3000, 0);
   char *v = view(text, 600, NULL);
   TEST_ASSERT_NOT_NULL(strstr(v, "keys at $[0].n.n"));
   free(v);
   free(text);
   free(b.s);
}

/* A wide object keeps its first keys, then says how many more there are. */
static void test_a_wide_object(void) {
   sb_t b = { 0 };
   sb_put(&b, "{\"map\":{");
   char kv[64];
   for (int i = 0; i < 5000; i++) {
      snprintf(kv, sizeof(kv), "%s\"k%d\":%d", i ? "," : "", i, i);
      sb_put(&b, kv);
   }
   sb_put(&b, "}}");
   char *v = view(b.s, 1500, NULL);
   TEST_ASSERT_NOT_NULL(strstr(v, "\"k0\":0"));
   TEST_ASSERT_NOT_NULL(strstr(v, "more keys at $.map"));
   free(v);
   free(b.s);
}

/* A long string keeps its head, and says how long it was and where. */
static void test_a_long_string(void) {
   char *body = malloc(50001);
   memset(body, 'x', 50000);
   body[50000] = '\0';
   char *text = malloc(50100);
   snprintf(text, 50100, "{\"file\":\"main.c\",\"body\":\"%s\"}", body);
   char *v = view(text, 1000, NULL);
   TEST_ASSERT_NOT_NULL(strstr(v, "\"file\":\"main.c\""));
   TEST_ASSERT_NOT_NULL(strstr(v, "(50000 chars at $.body)"));
   free(v);
   free(text);
   free(body);
}

/* Numbers are shown as they were sent: a view never changes an id or a value. */
static void test_numbers_are_as_sent(void) {
   char *arr = wide_array(300);
   char *text = malloc(strlen(arr) + 200);
   snprintf(text, strlen(arr) + 200,
            "{\"ratio\":0.1,\"precise\":1.10,\"id\":9007199254740993,\"neg\":-42,\"exp\":1e3,"
            "\"data\":%s}",
            arr);
   char *v = view(text, 1500, NULL);
   TEST_ASSERT_NOT_NULL(strstr(v, "\"ratio\":0.1,"));
   TEST_ASSERT_NOT_NULL(strstr(v, "\"precise\":1.10,"));
   TEST_ASSERT_NOT_NULL(strstr(v, "\"id\":9007199254740993,"));
   TEST_ASSERT_NOT_NULL(strstr(v, "\"neg\":-42,"));
   TEST_ASSERT_NOT_NULL(strstr(v, "\"exp\":1e3,"));
   free(v);
   free(text);
   free(arr);
}

/* A key that isn't an identifier is named in quoted form in a path; a key
 * with quotes or a line break can't break the path or the JSON. */
static void test_odd_keys_in_paths(void) {
   char *arr = wide_array(200);
   char *text = malloc(strlen(arr) * 2 + 200);
   snprintf(text, strlen(arr) * 2 + 200, "{\"a.b\":%s,\"say \\\"hi\\\"\\nnow\":%s}", arr, arr);
   char *v = view(text, 1500, NULL);
   TEST_ASSERT_NOT_NULL(strstr(v, "$[\\\"a.b\\\"].results["));
   TEST_ASSERT_NULL(strstr(v, "hi\\\"\\nnow\\\"]")); /* no raw line break in the path */
   free(v);
   free(text);
   free(arr);
}

/* When even the barest skeleton won't fit, the text view is used, and it fits. */
static void test_the_skeleton_falls_back_to_text(void) {
   sb_t b = { 0 };
   sb_put(&b, "{");
   char kv[200];
   for (int i = 0; i < 40; i++) {
      snprintf(kv, sizeof(kv), "%s\"%060d\":\"%0100d\"", i ? "," : "", i, i);
      sb_put(&b, kv);
   }
   sb_put(&b, "}");
   llm_tool_view_info_t info;
   char *v = view(b.s, 300, &info);
   TEST_ASSERT_EQUAL_INT(LLM_TOOL_VIEW_TEXT, info.mode);
   TEST_ASSERT_NOT_NULL(strstr(v, "omitted"));
   free(v);
   free(b.s);
}

/* Text: numbered head and tail lines, with what was left out between them. */
static void test_text_head_and_tail(void) {
   sb_t b = { 0 };
   char line[64];
   for (int i = 1; i <= 5000; i++) {
      snprintf(line, sizeof(line), "log line %d\n", i);
      sb_put(&b, line);
   }
   llm_tool_view_info_t info;
   char *v = view(b.s, 2000, &info);
   TEST_ASSERT_EQUAL_INT(LLM_TOOL_VIEW_TEXT, info.mode);
   TEST_ASSERT_EQUAL_size_t(5000, info.lines);
   TEST_ASSERT_EQUAL_INT(0, strncmp(v, "     1\tlog line 1\n", 18));
   TEST_ASSERT_NOT_NULL(strstr(v, "  5000\tlog line 5000\n"));
   TEST_ASSERT_NOT_NULL(strstr(v, "lines ("));
   TEST_ASSERT_NOT_NULL(strstr(v, "omitted: lines "));
   free(v);
   free(b.s);
}

/* One huge line (minified text) is cut by characters: its head and tail. */
static void test_one_huge_line(void) {
   char *text = malloc(100001);
   for (int i = 0; i < 100000; i++) {
      text[i] = (char)('a' + i % 26);
   }
   text[100000] = '\0';
   char *v = view(text, 1000, NULL);
   TEST_ASSERT_EQUAL_INT(0, strncmp(v, "     1\tabcdef", 13));
   TEST_ASSERT_NOT_NULL(strstr(v, "chars omitted"));
   free(v);
   free(text);
}

/* A cut never splits a character. */
static void test_cuts_keep_utf8_whole(void) {
   /* "é" is two bytes; a string and a text of them, cut at odd budgets. */
   sb_t b = { 0 };
   sb_put(&b, "{\"s\":\"");
   for (int i = 0; i < 3000; i++) {
      sb_put(&b, "\xC3\xA9");
   }
   sb_put(&b, "\"}");
   for (size_t budget = 257; budget < 400; budget += 7) {
      char *v = view(b.s, budget, NULL);
      TEST_ASSERT_TRUE(valid_utf8(v));
      free(v);
   }
   char *line = b.s + 6; /* the text of it, as one line */
   for (size_t budget = 257; budget < 400; budget += 7) {
      char *v = view(line, budget, NULL);
      TEST_ASSERT_TRUE(valid_utf8(v));
      free(v);
   }
   free(b.s);
}

/* The same input and budget give the same bytes; every budget is kept. */
static void test_deterministic_and_bounded(void) {
   char *text = wide_array(2000);
   static const size_t k_budgets[] = { 1, 256, 700, 3000, 20000, 90000 };
   for (size_t i = 0; i < sizeof(k_budgets) / sizeof(k_budgets[0]); i++) {
      char *a = view(text, k_budgets[i], NULL);
      char *b = view(text, k_budgets[i], NULL);
      TEST_ASSERT_EQUAL_STRING(a, b);
      free(a);
      free(b);
   }
   free(text);
}

/* A view of part of a result names paths from the whole result's root. */
static void test_a_subtree_names_paths_from_the_root(void) {
   char *text = wide_array(500);
   struct json_object *root = json_tokener_parse(text);
   struct json_object *results = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(root, "results", &results));
   llm_tool_view_info_t info;
   char *v = llm_tool_view_tree(results, 1000, "$.results", &info);
   TEST_ASSERT_NOT_NULL(v);
   TEST_ASSERT_TRUE(info.shortened);
   TEST_ASSERT_NOT_NULL(strstr(v, "more items at $.results["));
   free(v);
   /* Small enough: whole, as compact JSON. */
   struct json_object *first = json_object_array_get_idx(results, 0);
   v = llm_tool_view_tree(first, 1000, "$.results[0]", &info);
   TEST_ASSERT_EQUAL_STRING("{\"id\":0,\"name\":\"item 0\",\"file\":\"src/f0.c\"}", v);
   TEST_ASSERT_FALSE(info.shortened);
   free(v);
   json_object_put(root);
   free(text);
}

/* JSON followed by anything else isn't JSON: the text view is used. */
static void test_trailing_text_is_text(void) {
   char *arr = wide_array(300);
   char *text = malloc(strlen(arr) + 32);
   snprintf(text, strlen(arr) + 32, "%s and more", arr);
   llm_tool_view_info_t info;
   char *v = view(text, 1000, &info);
   TEST_ASSERT_EQUAL_INT(LLM_TOOL_VIEW_TEXT, info.mode);
   free(v);
   free(text);
   free(arr);
}

/* An integer json-c can't hold (past 64 bits) or a NaN would be shown wrong:
 * that text gets the text view, its numbers as sent. */
static void test_unholdable_numbers_are_text(void) {
   static const char *const k_heads[] = { "[99999999999999999999999,",
                                          "[-99999999999999999999,",
                                          "[NaN,",
                                          "[-Infinity,",
                                          "[1e400,",
                                          "[-1.5e999,",
                                          "[1.,",
                                          "['\"',99999999999999999999,'\"'," };
   for (size_t i = 0; i < sizeof(k_heads) / sizeof(k_heads[0]); i++) {
      char text[3200];
      snprintf(text, sizeof(text), "%s\"%03000d\"]", k_heads[i], 0);
      llm_tool_view_info_t info;
      char *v = view(text, 400, &info);
      TEST_ASSERT_EQUAL_INT_MESSAGE(LLM_TOOL_VIEW_TEXT, info.mode, k_heads[i]);
      TEST_ASSERT_NOT_NULL(strstr(v, k_heads[i]));
      free(v);
   }
   /* After a string ending in an escaped backslash, a number is a number. */
   char text[3200];
   snprintf(text, sizeof(text), "[\"a\\\\\",99999999999999999999999,\"%03000d\"]", 0);
   llm_tool_view_info_t text_info;
   char *tv = view(text, 400, &text_info);
   TEST_ASSERT_EQUAL_INT(LLM_TOOL_VIEW_TEXT, text_info.mode);
   free(tv);
   /* The largest that fit, and numbers and a "NaN" inside strings (an escaped
    * quote doesn't end one), stay JSON. */
   snprintf(text, sizeof(text),
            "[18446744073709551615,-9223372036854775808,\"NaN \\\"99999999999999999999999\","
            "\"%03000d\"]",
            0);
   llm_tool_view_info_t info;
   char *v = view(text, 400, &info);
   TEST_ASSERT_EQUAL_INT(LLM_TOOL_VIEW_JSON, info.mode);
   TEST_ASSERT_NOT_NULL(strstr(v, "[18446744073709551615,-9223372036854775808,"));
   free(v);
}

/* A tree's NaN or infinity is shown as null: the view stays JSON. */
static void test_a_tree_nan_is_null(void) {
   struct json_object *arr = json_object_new_array();
   json_object_array_add(arr, json_object_new_double(0.0 / 0.0));
   json_object_array_add(arr, json_object_new_double(1.0 / 0.0));
   char *v = llm_tool_view_tree(arr, 300, NULL, NULL);
   TEST_ASSERT_EQUAL_STRING("[null,null]", v);
   free(v);
   json_object_put(arr);
}

/* The "more keys" marker never goes under a key the object has. */
static void test_the_more_keys_marker_is_its_own_key(void) {
   sb_t b = { 0 };
   sb_put(&b, "{\"...\":\"real\"");
   char kv[64];
   for (int i = 0; i < 60; i++) {
      snprintf(kv, sizeof(kv), ",\"k%02d\":%d", i, i);
      sb_put(&b, kv);
   }
   sb_put(&b, ",\"big\":\"");
   for (int i = 0; i < 2000; i++) {
      sb_put(&b, "x");
   }
   sb_put(&b, "\"}");
   char *v = view(b.s, 400, NULL);
   TEST_ASSERT_NOT_NULL(strstr(v, "\"...\":\"real\""));
   TEST_ASSERT_NOT_NULL(strstr(v, "\"...#2\":\""));
   free(v);
   free(b.s);
}

/* Two long keys alike in their first bytes and length still show apart. */
static void test_cut_keys_show_apart(void) {
   char a[101], c[101];
   memset(a, 'k', 100);
   a[100] = '\0';
   memcpy(c, a, sizeof(c));
   a[99] = 'a';
   c[99] = 'c';
   char *text = malloc(4000);
   snprintf(text, 4000, "{\"%s\":1,\"%s\":2,\"pad\":\"%03000d\"}", a, c, 0);
   char *v = view(text, 600, NULL);
   struct json_object *parsed = json_tokener_parse(v);
   TEST_ASSERT_EQUAL_INT(3, json_object_object_length(parsed)); /* no key shown twice */
   TEST_ASSERT_NOT_NULL(strstr(v, "(100 chars, #"));
   json_object_put(parsed);
   free(v);
   free(text);
}

/* A deep path of non-ASCII keys, cut at its length limit, stays valid UTF-8. */
static void test_a_long_path_stays_utf8(void) {
   static const char k_key[] = "\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87-\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87-"
                               "\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87-\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87";
   sb_t b = { 0 };
   for (int i = 0; i < 12; i++) {
      sb_put(&b, "{\"");
      sb_put(&b, k_key);
      sb_put(&b, "\":");
   }
   sb_put(&b, "\"");
   for (int i = 0; i < 3000; i++) {
      sb_put(&b, "x");
   }
   sb_put(&b, "\"");
   for (int i = 0; i < 12; i++) {
      sb_put(&b, "}");
   }
   for (size_t budget = 1200; budget <= 2400; budget += 150) {
      char *v = llm_tool_view(b.s, b.len, budget, "$$", NULL);
      TEST_ASSERT_TRUE(valid_utf8(v));
      free(v);
   }
   free(b.s);
}

/* An elision never makes the view bigger: a few small items outweighed by the
 * marker that would stand for them are shown. */
static void test_an_elision_hides_more_than_its_marker(void) {
   char text[3200];
   snprintf(text, sizeof(text), "[1,2,3,4,5,6,7,8,9,10,\"%03000d\"]", 0);
   char *v = view(text, 400, NULL);
   TEST_ASSERT_EQUAL_INT(0, strncmp(v, "[1,2,3,4,5,6,7,8,9,10,\"", 23));
   TEST_ASSERT_NULL(strstr(v, "more items"));
   free(v);
   /* Nested: each small array counts its own brackets and commas only. */
   snprintf(text, sizeof(text),
            "{\"b\":\"%0600d\",\"a\":[[0],[0],[0],[0],[0],[0],[0],[0],[0],"
            "[0],[0],[0],[0],[0],[0]]}",
            0);
   v = view(text, 300, NULL);
   TEST_ASSERT_NOT_NULL(strstr(v, "[[0],[0],[0],[0],[0],[0],[0],[0],[0],[0],[0],[0],[0],[0],[0]]"));
   free(v);
}

/* A NUL byte in text is shown as a space: the whole view is one C string. */
static void test_a_nul_in_text(void) {
   char text[2001];
   memset(text, 'a', 2000);
   text[2000] = '\0';
   text[10] = '\0';
   llm_tool_view_info_t info;
   char *v = llm_tool_view(text, 2000, 400, NULL, &info);
   TEST_ASSERT_EQUAL_size_t(strlen(v), info.view_bytes);
   TEST_ASSERT_NOT_NULL(strstr(v, "chars omitted"));
   free(v);
   v = llm_tool_view(text, 300, 400, NULL, &info); /* whole */
   TEST_ASSERT_EQUAL_size_t(300, strlen(v));
   free(v);
}

/* A tree with a key too long to show whole isn't returned "whole". */
static void test_a_tree_with_a_cut_key_is_shortened(void) {
   char key[101];
   memset(key, 'k', 100);
   key[100] = '\0';
   struct json_object *obj = json_object_new_object();
   json_object_object_add(obj, key, json_object_new_int(1));
   llm_tool_view_info_t info;
   char *v = llm_tool_view_tree(obj, 4096, NULL, &info);
   TEST_ASSERT_TRUE(info.shortened);
   TEST_ASSERT_NOT_NULL(strstr(v, "(100 chars, #"));
   free(v);
   json_object_put(obj);
}

/* An items' shape of long non-ASCII keys stays valid UTF-8. */
static void test_an_items_shape_stays_utf8(void) {
   sb_t b = { 0 };
   sb_put(&b, "[");
   for (int i = 0; i < 200; i++) {
      sb_put(&b, i ? ",{" : "{");
      for (int k = 0; k < 7; k++) {
         char kv[128];
         snprintf(kv, sizeof(kv), "%s\"xxx%c", k ? "," : "", 'a' + k);
         sb_put(&b, kv);
         for (int e = 0; e < 20; e++) {
            sb_put(&b, "\xF0\x9F\x98\x80");
         }
         sb_put(&b, "\":1");
      }
      sb_put(&b, "}");
   }
   sb_put(&b, "]");
   char *v = view(b.s, 3000, NULL);
   TEST_ASSERT_TRUE(valid_utf8(v));
   TEST_ASSERT_NOT_NULL(strstr(v, "more items"));
   free(v);
   free(b.s);
}

/* A deep value is loosened past the starting depth when the budget allows. */
static void test_a_roomy_budget_shows_deeper(void) {
   sb_t b = { 0 };
   for (int i = 0; i < 24; i++) {
      sb_put(&b, "{\"n\":");
   }
   sb_put(&b, "[");
   for (int i = 0; i < 400; i++) {
      sb_put(&b, i ? ",\"leaf value\"" : "\"leaf value\"");
   }
   sb_put(&b, "]");
   for (int i = 0; i < 24; i++) {
      sb_put(&b, "}");
   }
   char *v = view(b.s, 3000, NULL);
   TEST_ASSERT_NOT_NULL(strstr(v, "\"leaf value\""));
   free(v);
   free(b.s);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_a_result_that_fits_is_whole);
   RUN_TEST(test_a_wide_array);
   RUN_TEST(test_a_roomy_budget_is_used);
   RUN_TEST(test_small_elisions_are_not_made);
   RUN_TEST(test_a_deep_value);
   RUN_TEST(test_a_wide_object);
   RUN_TEST(test_a_long_string);
   RUN_TEST(test_numbers_are_as_sent);
   RUN_TEST(test_odd_keys_in_paths);
   RUN_TEST(test_the_skeleton_falls_back_to_text);
   RUN_TEST(test_text_head_and_tail);
   RUN_TEST(test_one_huge_line);
   RUN_TEST(test_cuts_keep_utf8_whole);
   RUN_TEST(test_deterministic_and_bounded);
   RUN_TEST(test_a_subtree_names_paths_from_the_root);
   RUN_TEST(test_trailing_text_is_text);
   RUN_TEST(test_unholdable_numbers_are_text);
   RUN_TEST(test_a_tree_nan_is_null);
   RUN_TEST(test_the_more_keys_marker_is_its_own_key);
   RUN_TEST(test_cut_keys_show_apart);
   RUN_TEST(test_a_long_path_stays_utf8);
   RUN_TEST(test_an_elision_hides_more_than_its_marker);
   RUN_TEST(test_a_nul_in_text);
   RUN_TEST(test_a_tree_with_a_cut_key_is_shortened);
   RUN_TEST(test_an_items_shape_stays_utf8);
   RUN_TEST(test_a_roomy_budget_shows_deeper);
   return UNITY_END();
}
