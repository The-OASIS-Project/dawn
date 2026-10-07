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
 * Calculator tool implementation with math evaluation, unit conversion,
 * base conversion, and random number generation.
 */

#include "tools/calculator.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <sodium.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "logging.h"
#include "tinyexpr.h"
#include "tools/calculator_bignum.h"

#define RESULT_BUFFER_SIZE 256

/* Maximum integer value that can be exactly represented in IEEE-754 double (2^53) */
#define MAX_SAFE_INTEGER_DOUBLE 9007199254740992.0

/* Binary string buffer: 64 bits + "0b" prefix + optional sign + null terminator */
#define BINARY_BUFFER_SIZE (64 + 3 + 1)

/* Unit conversion factors (to base SI unit) */
typedef struct {
   const char *name;
   const char *category;
   double to_base; /* multiply by this to get base unit */
} unit_def_t;

static const unit_def_t units[] = {
   /* Length (base: meters) */
   { "m", "length", 1.0 },
   { "meter", "length", 1.0 },
   { "meters", "length", 1.0 },
   { "km", "length", 1000.0 },
   { "kilometer", "length", 1000.0 },
   { "kilometers", "length", 1000.0 },
   { "cm", "length", 0.01 },
   { "centimeter", "length", 0.01 },
   { "centimeters", "length", 0.01 },
   { "mm", "length", 0.001 },
   { "millimeter", "length", 0.001 },
   { "millimeters", "length", 0.001 },
   { "mi", "length", 1609.344 },
   { "mile", "length", 1609.344 },
   { "miles", "length", 1609.344 },
   { "yd", "length", 0.9144 },
   { "yard", "length", 0.9144 },
   { "yards", "length", 0.9144 },
   { "ft", "length", 0.3048 },
   { "foot", "length", 0.3048 },
   { "feet", "length", 0.3048 },
   { "in", "length", 0.0254 },
   { "inch", "length", 0.0254 },
   { "inches", "length", 0.0254 },

   /* Mass (base: grams) */
   { "g", "mass", 1.0 },
   { "gram", "mass", 1.0 },
   { "grams", "mass", 1.0 },
   { "kg", "mass", 1000.0 },
   { "kilogram", "mass", 1000.0 },
   { "kilograms", "mass", 1000.0 },
   { "mg", "mass", 0.001 },
   { "milligram", "mass", 0.001 },
   { "milligrams", "mass", 0.001 },
   { "lb", "mass", 453.592 },
   { "lbs", "mass", 453.592 },
   { "pound", "mass", 453.592 },
   { "pounds", "mass", 453.592 },
   { "oz", "mass", 28.3495 },
   { "ounce", "mass", 28.3495 },
   { "ounces", "mass", 28.3495 },

   /* Volume (base: liters) */
   { "l", "volume", 1.0 },
   { "liter", "volume", 1.0 },
   { "liters", "volume", 1.0 },
   { "ml", "volume", 0.001 },
   { "milliliter", "volume", 0.001 },
   { "milliliters", "volume", 0.001 },
   { "gal", "volume", 3.78541 },
   { "gallon", "volume", 3.78541 },
   { "gallons", "volume", 3.78541 },
   { "qt", "volume", 0.946353 },
   { "quart", "volume", 0.946353 },
   { "quarts", "volume", 0.946353 },
   { "pt", "volume", 0.473176 },
   { "pint", "volume", 0.473176 },
   { "pints", "volume", 0.473176 },
   { "cup", "volume", 0.236588 },
   { "cups", "volume", 0.236588 },
   { "floz", "volume", 0.0295735 },

   /* Temperature handled specially */
   { "c", "temperature", 0.0 },
   { "celsius", "temperature", 0.0 },
   { "f", "temperature", 0.0 },
   { "fahrenheit", "temperature", 0.0 },
   { "k", "temperature", 0.0 },
   { "kelvin", "temperature", 0.0 },

   /* Time (base: seconds) */
   { "s", "time", 1.0 },
   { "sec", "time", 1.0 },
   { "second", "time", 1.0 },
   { "seconds", "time", 1.0 },
   { "min", "time", 60.0 },
   { "minute", "time", 60.0 },
   { "minutes", "time", 60.0 },
   { "h", "time", 3600.0 },
   { "hr", "time", 3600.0 },
   { "hour", "time", 3600.0 },
   { "hours", "time", 3600.0 },
   { "day", "time", 86400.0 },
   { "days", "time", 86400.0 },
   { "week", "time", 604800.0 },
   { "weeks", "time", 604800.0 },

   { NULL, NULL, 0.0 }
};

static const unit_def_t *find_unit(const char *name) {
   for (int i = 0; units[i].name != NULL; i++) {
      if (strcasecmp(name, units[i].name) == 0) {
         return &units[i];
      }
   }
   return NULL;
}

static double convert_temperature(double value, const char *from, const char *to) {
   double celsius;

   /* Convert to Celsius first */
   if (strcasecmp(from, "f") == 0 || strcasecmp(from, "fahrenheit") == 0) {
      celsius = (value - 32.0) * 5.0 / 9.0;
   } else if (strcasecmp(from, "k") == 0 || strcasecmp(from, "kelvin") == 0) {
      celsius = value - 273.15;
   } else {
      celsius = value;
   }

   /* Convert from Celsius to target */
   if (strcasecmp(to, "f") == 0 || strcasecmp(to, "fahrenheit") == 0) {
      return celsius * 9.0 / 5.0 + 32.0;
   } else if (strcasecmp(to, "k") == 0 || strcasecmp(to, "kelvin") == 0) {
      return celsius + 273.15;
   } else {
      return celsius;
   }
}

/* Maximum parenthesis nesting depth handled by the factorial rewriter. */
#define FACTORIAL_REWRITE_MAX_DEPTH 64

/* Scratch buffer size for the factorial-rewrite output.  If a factorial-bearing
 * expression doesn't fit, rewrite_factorials returns false and the caller
 * evaluates the original (which then errors on the bare '!') — a safe, if
 * opaque, degradation.  Comfortably larger than any real expression. */
#define FACTORIAL_REWRITE_BUFFER_SIZE 512

/**
 * @brief Rewrite postfix factorial ('!') into tinyexpr's fac() prefix form.
 *
 * tinyexpr has no postfix operators and never uses '!', so claiming it for
 * factorial is safe.  The operand to the left of each '!' is the immediately-
 * preceding atom: a number, a parenthesized group, or a function call
 * (identifier + group).  Examples: "52!" -> "fac(52)", "(3+2)!" -> "fac((3+2))",
 * "sqrt(4)!" -> "fac(sqrt(4))", "5!!" -> "fac(fac(5))".
 *
 * Single left-to-right pass: each char is copied to @p out while tracking where
 * the last completed atom began; on '!' that atom is wrapped in fac(...).
 *
 * @return true if a rewrite was written to @p out; false if the expression is
 *         malformed (a '!' with no operand) or the result would not fit — in
 *         which case the caller falls back to the original expression.
 */
static bool rewrite_factorials(const char *in, char *out, size_t out_size) {
   size_t out_len = 0;
   long last_atom_start = -1; /* index in `out` where the last atom began, or -1 */
   bool in_number = false;
   size_t paren_stack[FACTORIAL_REWRITE_MAX_DEPTH];
   int paren_depth = 0;

#define FR_PUT(ch)                 \
   do {                            \
      if (out_len + 1 >= out_size) \
         return false;             \
      out[out_len++] = (ch);       \
   } while (0)

   for (const char *p = in; *p; p++) {
      char c = *p;

      if (c == '!') {
         if (last_atom_start < 0) {
            return false; /* '!' with no operand to its left */
         }
         /* Wrap out[last_atom_start, out_len) as fac(<operand>). */
         size_t operand_len = out_len - (size_t)last_atom_start;
         if (out_len + 5 >= out_size) { /* "fac(" + ")" */
            return false;
         }
         memmove(out + last_atom_start + 4, out + last_atom_start, operand_len);
         memcpy(out + last_atom_start, "fac(", 4);
         out_len += 4;
         out[out_len++] = ')';
         in_number = false;
         /* The new atom is the whole fac(...), so chained '!' wraps it again. */
         continue;
      }

      if (isdigit((unsigned char)c) || c == '.') {
         if (!in_number) {
            in_number = true;
            last_atom_start = (long)out_len;
         }
         FR_PUT(c);
      } else if (c == '(') {
         if (paren_depth >= FACTORIAL_REWRITE_MAX_DEPTH) {
            return false;
         }
         paren_stack[paren_depth++] = out_len;
         last_atom_start = -1;
         in_number = false;
         FR_PUT(c);
      } else if (c == ')') {
         if (paren_depth <= 0) {
            return false; /* unbalanced — let te_interp report it on the original */
         }
         size_t open_idx = paren_stack[--paren_depth];
         /* Absorb a function-name identifier immediately before the '(' so a
          * call like sqrt(4) is treated as one atom. */
         size_t atom = open_idx;
         while (atom > 0 && (isalnum((unsigned char)out[atom - 1]) || out[atom - 1] == '_')) {
            atom--;
         }
         last_atom_start = (long)atom;
         in_number = false;
         FR_PUT(c);
      } else if (isspace((unsigned char)c)) {
         in_number = false; /* keep last_atom_start so "5 !" still works */
         FR_PUT(c);
      } else {
         /* Operator or identifier char: not a factorial-able atom on its own. */
         in_number = false;
         last_atom_start = -1;
         FR_PUT(c);
      }
   }

#undef FR_PUT
   out[out_len] = '\0';
   return true;
}

calc_result_t calculator_evaluate(const char *expression) {
   calc_result_t res = { 0 };
   int error_pos = 0;

   if (expression == NULL || expression[0] == '\0') {
      res.success = 0;
      snprintf(res.error, sizeof(res.error), "Empty expression");
      return res;
   }

   /* Bound input before tinyexpr's recursive-descent parser — guards every
    * caller, including the exact-mode fallback, against stack exhaustion from a
    * deeply nested expression. */
   if (strlen(expression) > CALC_MAX_EXPR_LEN) {
      res.success = 0;
      snprintf(res.error, sizeof(res.error), "Expression too long");
      return res;
   }

   /* Rewrite postfix factorial (e.g. "52!") into tinyexpr's fac() form.  Only
    * when a '!' is present and the rewrite fits; otherwise evaluate as-is. */
   char rewritten[FACTORIAL_REWRITE_BUFFER_SIZE];
   const char *expr = expression;
   if (strchr(expression, '!') != NULL &&
       rewrite_factorials(expression, rewritten, sizeof(rewritten))) {
      expr = rewritten;
   }

   res.result = te_interp(expr, &error_pos);

   if (error_pos != 0) {
      res.success = 0;
      snprintf(res.error, sizeof(res.error),
               "Parse error at position %d. Check for mismatched parentheses or unknown functions.",
               error_pos);
      OLOG_WARNING("Calculator: Failed to parse '%s' at position %d", expression, error_pos);
   } else if (isnan(res.result)) {
      res.success = 0;
      snprintf(res.error, sizeof(res.error), "Result is undefined (NaN)");
      OLOG_WARNING("Calculator: Expression '%s' resulted in NaN", expression);
   } else if (isinf(res.result)) {
      res.success = 0;
      snprintf(res.error, sizeof(res.error), "Result is infinite");
      OLOG_WARNING("Calculator: Expression '%s' resulted in infinity", expression);
   } else {
      res.success = 1;
      OLOG_INFO("Calculator: '%s' = %g", expression, res.result);
   }

   return res;
}

char *calculator_evaluate_exact_str(const char *expression) {
   if (expression == NULL || expression[0] == '\0') {
      return strdup("Error: Empty expression");
   }

   char *exact = NULL;
   char errbuf[128] = "";
   calc_exact_status_t status = calculator_evaluate_exact(expression, &exact, errbuf,
                                                          sizeof(errbuf));

   if (status == CALC_EXACT_OK) {
      OLOG_INFO("Calculator: exact '%s' = %.40s%s", expression, exact,
                strlen(exact) > 40 ? "..." : "");
      return exact; /* full digit string */
   }

   /* Not exactly representable (or too large): fall back to the float evaluator,
    * which handles irrationals, non-terminating division, etc. */
   calc_result_t fr = calculator_evaluate(expression);
   char *approx = calculator_format_result(&fr);
   if (approx == NULL) {
      return strdup("Failed to evaluate expression.");
   }

   /* Only when the user asked for exact digits but the integer result is too big
    * do we annotate that the value shown is an approximation. */
   if (status == CALC_EXACT_TOO_LARGE && fr.success) {
      size_t need = strlen(approx) + strlen(errbuf) + 32;
      char *msg = malloc(need);
      if (msg != NULL) {
         snprintf(msg, need, "%s (approximate — %s)", approx, errbuf);
         free(approx);
         return msg;
      }
      OLOG_WARNING("Calculator: annotation alloc failed, returning unannotated approximation");
   }
   return approx;
}

char *calculator_convert(const char *value_str) {
   char *buf = malloc(RESULT_BUFFER_SIZE);
   if (buf == NULL) {
      return NULL;
   }

   double value;
   char from_unit[32] = { 0 };
   char to_unit[32] = { 0 };

   /* Parse "VALUE FROM to TO" format, e.g., "5 miles to km" */
   int parsed = sscanf(value_str, "%lf %31s to %31s", &value, from_unit, to_unit);
   if (parsed != 3) {
      /* Try "VALUE FROM in TO" format */
      parsed = sscanf(value_str, "%lf %31s in %31s", &value, from_unit, to_unit);
   }

   if (parsed != 3) {
      snprintf(buf, RESULT_BUFFER_SIZE, "Error: Use format '5 miles to km' or '100 f to c'");
      return buf;
   }

   const unit_def_t *from = find_unit(from_unit);
   const unit_def_t *to = find_unit(to_unit);

   if (from == NULL) {
      snprintf(buf, RESULT_BUFFER_SIZE, "Error: Unknown unit '%s'", from_unit);
      return buf;
   }

   if (to == NULL) {
      snprintf(buf, RESULT_BUFFER_SIZE, "Error: Unknown unit '%s'", to_unit);
      return buf;
   }

   if (strcmp(from->category, to->category) != 0) {
      snprintf(buf, RESULT_BUFFER_SIZE, "Error: Cannot convert %s (%s) to %s (%s)", from_unit,
               from->category, to_unit, to->category);
      return buf;
   }

   double result;
   if (strcmp(from->category, "temperature") == 0) {
      result = convert_temperature(value, from_unit, to_unit);
   } else {
      /* Convert: value * from_factor / to_factor */
      result = value * from->to_base / to->to_base;
   }

   OLOG_INFO("Calculator: Convert %.4g %s to %s = %.6g", value, from_unit, to_unit, result);
   snprintf(buf, RESULT_BUFFER_SIZE, "%.6g %s", result, to_unit);
   return buf;
}

char *calculator_base_convert(const char *value_str) {
   char *buf = malloc(RESULT_BUFFER_SIZE);
   if (buf == NULL) {
      return NULL;
   }

   char input[64] = { 0 };
   char to_base[16] = { 0 };

   /* Parse "VALUE to BASE" format */
   int parsed = sscanf(value_str, "%63s to %15s", input, to_base);
   if (parsed != 2) {
      snprintf(buf, RESULT_BUFFER_SIZE, "Error: Use format '255 to hex' or '0xFF to decimal'");
      return buf;
   }

   /* Detect input base and parse value */
   long long value = 0;
   int from_base = 10;
   char *endptr = NULL;
   const char *parse_start = input;

   errno = 0;
   if (strncasecmp(input, "0x", 2) == 0) {
      parse_start = input + 2;
      value = strtoll(parse_start, &endptr, 16);
      from_base = 16;
   } else if (strncasecmp(input, "0b", 2) == 0) {
      parse_start = input + 2;
      value = strtoll(parse_start, &endptr, 2);
      from_base = 2;
   } else if (input[0] == '0' && strlen(input) > 1 && isdigit(input[1])) {
      value = strtoll(input, &endptr, 8);
      from_base = 8;
   } else {
      value = strtoll(input, &endptr, 10);
      from_base = 10;
   }

   /* Validate parsing succeeded */
   if (errno == ERANGE) {
      snprintf(buf, RESULT_BUFFER_SIZE, "Error: Number '%s' is out of range", input);
      return buf;
   }
   if (endptr == parse_start || (endptr != NULL && *endptr != '\0')) {
      snprintf(buf, RESULT_BUFFER_SIZE, "Error: Invalid number format '%s'", input);
      return buf;
   }

   /* Convert to target base */
   if (strcasecmp(to_base, "hex") == 0 || strcasecmp(to_base, "hexadecimal") == 0) {
      snprintf(buf, RESULT_BUFFER_SIZE, "0x%llX", value);
   } else if (strcasecmp(to_base, "dec") == 0 || strcasecmp(to_base, "decimal") == 0) {
      snprintf(buf, RESULT_BUFFER_SIZE, "%lld", value);
   } else if (strcasecmp(to_base, "oct") == 0 || strcasecmp(to_base, "octal") == 0) {
      snprintf(buf, RESULT_BUFFER_SIZE, "0%llo", value);
   } else if (strcasecmp(to_base, "bin") == 0 || strcasecmp(to_base, "binary") == 0) {
      /* Manual binary conversion */
      if (value == 0) {
         snprintf(buf, RESULT_BUFFER_SIZE, "0b0");
      } else {
         char binary[BINARY_BUFFER_SIZE] = { 0 };
         int idx = BINARY_BUFFER_SIZE - 2; /* Start before null terminator */
         long long v = value < 0 ? -value : value;
         binary[BINARY_BUFFER_SIZE - 1] = '\0';
         while (v > 0 && idx >= 0) {
            binary[idx--] = (v & 1) ? '1' : '0';
            v >>= 1;
         }
         snprintf(buf, RESULT_BUFFER_SIZE, "%s0b%s", value < 0 ? "-" : "", &binary[idx + 1]);
      }
   } else {
      snprintf(buf, RESULT_BUFFER_SIZE, "Error: Unknown base '%s'. Use hex, dec, oct, or bin",
               to_base);
      return buf;
   }

   OLOG_INFO("Calculator: Base convert %s (base %d) to %s = %s", input, from_base, to_base, buf);
   return buf;
}

char *calculator_random(const char *value_str) {
   char *buf = malloc(RESULT_BUFFER_SIZE);
   if (buf == NULL) {
      return NULL;
   }

   long long min_val = 0, max_val = 0;

   /* Parse "MIN to MAX" or just "MAX" */
   int parsed = sscanf(value_str, "%lld to %lld", &min_val, &max_val);
   if (parsed == 2) {
      /* Got range */
   } else {
      parsed = sscanf(value_str, "%lld", &max_val);
      if (parsed == 1) {
         min_val = 1; /* Default: 1 to MAX */
      } else {
         snprintf(buf, RESULT_BUFFER_SIZE, "Error: Use format '1 to 100' or just '100'");
         return buf;
      }
   }

   if (min_val > max_val) {
      long long tmp = min_val;
      min_val = max_val;
      max_val = tmp;
   }

   /* Uniform over the whole range: span is max - min, exact in unsigned arithmetic */
   const uint64_t span = (uint64_t)max_val - (uint64_t)min_val;
   uint64_t offset;
   if (span < UINT32_MAX) {
      offset = randombytes_uniform((uint32_t)(span + 1));
   } else {
      /* Reject the top partial block so every value is equally likely */
      const uint64_t limit = (span == UINT64_MAX) ? UINT64_MAX
                                                  : UINT64_MAX - (UINT64_MAX % (span + 1));
      do {
         randombytes_buf(&offset, sizeof(offset));
      } while (span != UINT64_MAX && offset >= limit);
      if (span != UINT64_MAX)
         offset %= span + 1;
   }
   const uint64_t drawn = (uint64_t)min_val + offset; /* lands back inside [min, max] */
   long long result = (long long)drawn;

   OLOG_INFO("Calculator: Random %lld to %lld = %lld", min_val, max_val, result);
   snprintf(buf, RESULT_BUFFER_SIZE, "%lld", result);
   return buf;
}

char *calculator_format_result(calc_result_t *result) {
   char *buf = malloc(RESULT_BUFFER_SIZE);
   if (buf == NULL) {
      OLOG_ERROR("Calculator: Failed to allocate result buffer");
      return NULL;
   }

   if (result->success) {
      double val = result->result;
      double int_part;
      double frac_part = modf(val, &int_part);

      if (frac_part == 0.0 && val >= -MAX_SAFE_INTEGER_DOUBLE && val <= MAX_SAFE_INTEGER_DOUBLE) {
         snprintf(buf, RESULT_BUFFER_SIZE, "%.0f", val);
      } else {
         snprintf(buf, RESULT_BUFFER_SIZE, "%.10g", val);
      }
   } else {
      snprintf(buf, RESULT_BUFFER_SIZE, "Error: %s", result->error);
   }

   return buf;
}
