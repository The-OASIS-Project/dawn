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
 * Unit tests for calculator tool (evaluate, convert, base_convert, random).
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "tools/calculator.h"
#include "unity.h"

void setUp(void) {
}

void tearDown(void) {
}

/* ── Evaluate: basic arithmetic ─────────────────────────────────────────── */

static void test_eval_addition(void) {
   calc_result_t r = calculator_evaluate("2 + 3");
   TEST_ASSERT_EQUAL_INT(1, r.success);
   TEST_ASSERT_DOUBLE_WITHIN(0.0001, 5.0, r.result);
}

static void test_eval_order_of_operations(void) {
   calc_result_t r = calculator_evaluate("2 + 3 * 4");
   TEST_ASSERT_EQUAL_INT(1, r.success);
   TEST_ASSERT_DOUBLE_WITHIN(0.0001, 14.0, r.result);
}

static void test_eval_parentheses(void) {
   calc_result_t r = calculator_evaluate("(2 + 3) * 4");
   TEST_ASSERT_EQUAL_INT(1, r.success);
   TEST_ASSERT_DOUBLE_WITHIN(0.0001, 20.0, r.result);
}

static void test_eval_power(void) {
   calc_result_t r = calculator_evaluate("2^10");
   TEST_ASSERT_EQUAL_INT(1, r.success);
   TEST_ASSERT_DOUBLE_WITHIN(0.0001, 1024.0, r.result);
}

/* ── Evaluate: functions ────────────────────────────────────────────────── */

static void test_eval_sqrt(void) {
   calc_result_t r = calculator_evaluate("sqrt(144)");
   TEST_ASSERT_EQUAL_INT(1, r.success);
   TEST_ASSERT_DOUBLE_WITHIN(0.0001, 12.0, r.result);
}

static void test_eval_nested_functions(void) {
   calc_result_t r = calculator_evaluate("abs(-5) + ceil(3.2)");
   TEST_ASSERT_EQUAL_INT(1, r.success);
   TEST_ASSERT_DOUBLE_WITHIN(0.0001, 9.0, r.result);
}

/* ── Evaluate: error cases ──────────────────────────────────────────────── */

static void test_eval_null(void) {
   calc_result_t r = calculator_evaluate(NULL);
   TEST_ASSERT_EQUAL_INT(0, r.success);
}

static void test_eval_empty(void) {
   calc_result_t r = calculator_evaluate("");
   TEST_ASSERT_EQUAL_INT(0, r.success);
}

static void test_eval_parse_error(void) {
   calc_result_t r = calculator_evaluate("2 +");
   TEST_ASSERT_EQUAL_INT(0, r.success);
}

static void test_eval_division_by_zero(void) {
   calc_result_t r = calculator_evaluate("1/0");
   TEST_ASSERT_EQUAL_INT(0, r.success);
}

static void test_eval_nan(void) {
   calc_result_t r = calculator_evaluate("sqrt(-1)");
   TEST_ASSERT_EQUAL_INT(0, r.success);
}

/* ── Evaluate: factorial (postfix '!') ──────────────────────────────────── */

static void test_eval_factorial_small(void) {
   calc_result_t r = calculator_evaluate("5!");
   TEST_ASSERT_EQUAL_INT(1, r.success);
   TEST_ASSERT_DOUBLE_WITHIN(0.0001, 120.0, r.result);
}

static void test_eval_factorial_large(void) {
   /* 52! ≈ 8.0658e67 — must NOT overflow to infinity (the upstream fac() bug). */
   calc_result_t r = calculator_evaluate("52!");
   TEST_ASSERT_EQUAL_INT(1, r.success);
   TEST_ASSERT_DOUBLE_WITHIN(1e63, 8.0658175170943879e67, r.result);
}

static void test_eval_factorial_of_group(void) {
   calc_result_t r = calculator_evaluate("(3 + 2)!");
   TEST_ASSERT_EQUAL_INT(1, r.success);
   TEST_ASSERT_DOUBLE_WITHIN(0.0001, 120.0, r.result);
}

static void test_eval_factorial_of_function(void) {
   calc_result_t r = calculator_evaluate("sqrt(16)!");
   TEST_ASSERT_EQUAL_INT(1, r.success);
   TEST_ASSERT_DOUBLE_WITHIN(0.0001, 24.0, r.result);
}

static void test_eval_factorial_in_expression(void) {
   calc_result_t r = calculator_evaluate("3! + 4!");
   TEST_ASSERT_EQUAL_INT(1, r.success);
   TEST_ASSERT_DOUBLE_WITHIN(0.0001, 30.0, r.result);
}

static void test_eval_factorial_chained(void) {
   /* (3!)! = 6! = 720 */
   calc_result_t r = calculator_evaluate("3!!");
   TEST_ASSERT_EQUAL_INT(1, r.success);
   TEST_ASSERT_DOUBLE_WITHIN(0.0001, 720.0, r.result);
}

static void test_eval_factorial_no_operand(void) {
   /* '!' with nothing to its left falls back to the original and te_interp errors. */
   calc_result_t r = calculator_evaluate("!5");
   TEST_ASSERT_EQUAL_INT(0, r.success);
}

/* ── Exact mode: digit-perfect arbitrary precision ──────────────────────── */

static void test_exact_small(void) {
   char *s = calculator_evaluate_exact_str("2 + 3 * 4");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_EQUAL_STRING("14", s);
   free(s);
}

static void test_exact_factorial_52(void) {
   /* The whole point: every digit of 52!, no rounding. */
   char *s = calculator_evaluate_exact_str("52!");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_EQUAL_STRING("80658175170943878571660636856403766975289505440883277824000000000000",
                            s);
   free(s);
}

static void test_exact_power(void) {
   /* 2^100 exactly. */
   char *s = calculator_evaluate_exact_str("2^100");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_EQUAL_STRING("1267650600228229401496703205376", s);
   free(s);
}

static void test_exact_negative_result(void) {
   char *s = calculator_evaluate_exact_str("3 - 10");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_EQUAL_STRING("-7", s);
   free(s);
}

static void test_exact_even_division(void) {
   char *s = calculator_evaluate_exact_str("100 / 4");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_EQUAL_STRING("25", s);
   free(s);
}

static void test_exact_nested_factorial_power(void) {
   /* (3!)^3 = 6^3 = 216. */
   char *s = calculator_evaluate_exact_str("3!^3");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_EQUAL_STRING("216", s);
   free(s);
}

static void test_exact_inexact_division_falls_back(void) {
   /* 10/3 isn't an integer → falls back to the float evaluator. */
   char *s = calculator_evaluate_exact_str("10 / 3");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_NOT_NULL(strstr(s, "3.33"));
   free(s);
}

static void test_exact_irrational_falls_back(void) {
   /* sqrt(2) can't be exact → float path. */
   char *s = calculator_evaluate_exact_str("sqrt(2)");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_NOT_NULL(strstr(s, "1.41"));
   free(s);
}

static void test_exact_deep_nesting_no_crash(void) {
   /* Adversarial: thousands of nested parens must NOT exhaust the stack — the
    * parser depth guard aborts exact mode and the length cap stops the float
    * fallback. Assert it returns gracefully rather than crashing. */
   char expr[6000];
   size_t n = 2500;
   for (size_t i = 0; i < n; i++) {
      expr[i] = '(';
   }
   expr[n] = '1';
   size_t pos = n + 1;
   for (size_t i = 0; i < n; i++) {
      expr[pos++] = ')';
   }
   expr[pos] = '\0';
   char *s = calculator_evaluate_exact_str(expr);
   TEST_ASSERT_NOT_NULL(s);
   free(s);
}

static void test_eval_length_cap(void) {
   /* Over-long expressions are rejected before the recursive parser runs. */
   char expr[CALC_MAX_EXPR_LEN + 100];
   memset(expr, '1', CALC_MAX_EXPR_LEN + 50);
   expr[CALC_MAX_EXPR_LEN + 50] = '\0';
   calc_result_t r = calculator_evaluate(expr);
   TEST_ASSERT_EQUAL_INT(0, r.success);
}

static void test_exact_irrational_but_integer_value(void) {
   /* sqrt(16) = 4: not handled by the integer parser, but the float fallback
    * still returns a clean integer. */
   char *s = calculator_evaluate_exact_str("sqrt(16)");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_EQUAL_STRING("4", s);
   free(s);
}

/* ── Format result ──────────────────────────────────────────────────────── */

static void test_format_integer(void) {
   calc_result_t r = { .result = 42.0, .success = 1 };
   char *s = calculator_format_result(&r);
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_EQUAL_STRING("42", s);
   free(s);
}

static void test_format_float(void) {
   calc_result_t r = { .result = 3.14159, .success = 1 };
   char *s = calculator_format_result(&r);
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_NOT_NULL(strstr(s, "3.14159"));
   free(s);
}

static void test_format_error(void) {
   calc_result_t r = { .success = 0 };
   strncpy(r.error, "bad expression", sizeof(r.error));
   char *s = calculator_format_result(&r);
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_NOT_NULL(strstr(s, "Error:"));
   free(s);
}

/* ── Unit conversion ────────────────────────────────────────────────────── */

static void test_convert_length(void) {
   char *s = calculator_convert("5 miles to km");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_NOT_NULL(strstr(s, "8.04672"));
   free(s);
}

static void test_convert_temperature(void) {
   char *s = calculator_convert("32 f to c");
   TEST_ASSERT_NOT_NULL(s);
   /* 32°F = 0°C; output should be "0 c" or close to it */
   TEST_ASSERT_NOT_NULL(strstr(s, "0"));
   free(s);
}

static void test_convert_same_unit(void) {
   char *s = calculator_convert("1 meter to m");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_NOT_NULL(strstr(s, "1"));
   free(s);
}

static void test_convert_incompatible(void) {
   char *s = calculator_convert("5 miles to kg");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_NOT_NULL(strstr(s, "Cannot convert"));
   free(s);
}

static void test_convert_unknown_unit(void) {
   char *s = calculator_convert("5 flurbs to km");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_NOT_NULL(strstr(s, "Error"));
   free(s);
}

/* ── Base conversion ────────────────────────────────────────────────────── */

static void test_base_dec_to_hex(void) {
   char *s = calculator_base_convert("255 to hex");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_EQUAL_STRING("0xFF", s);
   free(s);
}

static void test_base_hex_to_dec(void) {
   char *s = calculator_base_convert("0xFF to decimal");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_EQUAL_STRING("255", s);
   free(s);
}

static void test_base_dec_to_bin(void) {
   char *s = calculator_base_convert("10 to bin");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_NOT_NULL(strstr(s, "1010"));
   free(s);
}

static void test_base_bin_to_dec(void) {
   char *s = calculator_base_convert("0b1010 to decimal");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_EQUAL_STRING("10", s);
   free(s);
}

/* ── Random ─────────────────────────────────────────────────────────────── */

static void test_random_range(void) {
   char *s = calculator_random("1 to 100");
   TEST_ASSERT_NOT_NULL(s);
   long val = strtol(s, NULL, 10);
   TEST_ASSERT_TRUE(val >= 1 && val <= 100);
   free(s);
}

static void test_random_single(void) {
   char *s = calculator_random("50");
   TEST_ASSERT_NOT_NULL(s);
   long val = strtol(s, NULL, 10);
   TEST_ASSERT_TRUE(val >= 1 && val <= 50);
   free(s);
}

/* A range past 2^31 is drawn from in full, not cut to RAND_MAX */
static void test_random_large_range(void) {
   bool above = false;
   for (int i = 0; i < 64 && !above; i++) {
      char *s = calculator_random("1 to 10000000000");
      TEST_ASSERT_NOT_NULL(s);
      long long val = strtoll(s, NULL, 10);
      TEST_ASSERT_TRUE(val >= 1 && val <= 10000000000LL);
      above = val > 2147483647LL;
      free(s);
   }
   TEST_ASSERT_TRUE(above);
}

static void test_random_full_and_single_value(void) {
   char *s = calculator_random("-9223372036854775808 to 9223372036854775807");
   TEST_ASSERT_NOT_NULL(s);
   free(s);
   s = calculator_random("7 to 7");
   TEST_ASSERT_NOT_NULL(s);
   TEST_ASSERT_EQUAL_STRING("7", s);
   free(s);
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(void) {
   UNITY_BEGIN();

   /* evaluate */
   RUN_TEST(test_eval_addition);
   RUN_TEST(test_eval_order_of_operations);
   RUN_TEST(test_eval_parentheses);
   RUN_TEST(test_eval_power);
   RUN_TEST(test_eval_sqrt);
   RUN_TEST(test_eval_nested_functions);
   RUN_TEST(test_eval_null);
   RUN_TEST(test_eval_empty);
   RUN_TEST(test_eval_parse_error);
   RUN_TEST(test_eval_division_by_zero);
   RUN_TEST(test_eval_nan);

   /* factorial */
   RUN_TEST(test_eval_factorial_small);
   RUN_TEST(test_eval_factorial_large);
   RUN_TEST(test_eval_factorial_of_group);
   RUN_TEST(test_eval_factorial_of_function);
   RUN_TEST(test_eval_factorial_in_expression);
   RUN_TEST(test_eval_factorial_chained);
   RUN_TEST(test_eval_factorial_no_operand);

   /* exact (arbitrary precision) */
   RUN_TEST(test_exact_small);
   RUN_TEST(test_exact_factorial_52);
   RUN_TEST(test_exact_power);
   RUN_TEST(test_exact_negative_result);
   RUN_TEST(test_exact_even_division);
   RUN_TEST(test_exact_nested_factorial_power);
   RUN_TEST(test_exact_inexact_division_falls_back);
   RUN_TEST(test_exact_irrational_falls_back);
   RUN_TEST(test_exact_irrational_but_integer_value);
   RUN_TEST(test_exact_deep_nesting_no_crash);
   RUN_TEST(test_eval_length_cap);

   /* format */
   RUN_TEST(test_format_integer);
   RUN_TEST(test_format_float);
   RUN_TEST(test_format_error);

   /* unit conversion */
   RUN_TEST(test_convert_length);
   RUN_TEST(test_convert_temperature);
   RUN_TEST(test_convert_same_unit);
   RUN_TEST(test_convert_incompatible);
   RUN_TEST(test_convert_unknown_unit);

   /* base conversion */
   RUN_TEST(test_base_dec_to_hex);
   RUN_TEST(test_base_hex_to_dec);
   RUN_TEST(test_base_dec_to_bin);
   RUN_TEST(test_base_bin_to_dec);

   /* random */
   RUN_TEST(test_random_range);
   RUN_TEST(test_random_single);
   RUN_TEST(test_random_large_range);
   RUN_TEST(test_random_full_and_single_value);

   return UNITY_END();
}
