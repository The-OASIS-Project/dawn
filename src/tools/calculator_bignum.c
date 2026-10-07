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
 * Arbitrary-precision integer expression evaluator (calculator "exact" mode).
 * Sign-magnitude big integers stored little-endian in base 10^9 limbs, plus a
 * small recursive-descent parser over + - * / ^ ! and parentheses.
 */

/**
 * @file calculator_bignum.c
 * @brief Arbitrary-precision integer expression evaluator for the calculator's
 *        digit-perfect "exact" mode.
 */

#include "tools/calculator_bignum.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Base 10^9: each limb holds a 9-digit chunk, products of two limbs fit u64. */
#define BN_BASE 1000000000u
#define BN_BASE_DIGITS 9

/* Largest factorial operand accepted (keeps fac() loops + output bounded; the
 * digit cap also guards results — 20000! would far exceed CALC_EXACT_MAX_DIGITS). */
#define BN_FAC_MAX_N 20000

/* Sign-magnitude big integer. n == 0 represents zero (sign +1). */
typedef struct {
   int sign;       /* +1 or -1 */
   size_t n;       /* significant limbs */
   size_t cap;     /* allocated limbs */
   uint32_t *limb; /* little-endian, each < BN_BASE */
} bignum_t;

/* ── allocation / lifecycle ─────────────────────────────────────────────── */

static void bn_init(bignum_t *a) {
   a->sign = 1;
   a->n = 0;
   a->cap = 0;
   a->limb = NULL;
}

static void bn_free(bignum_t *a) {
   free(a->limb);
   a->limb = NULL;
   a->n = 0;
   a->cap = 0;
   a->sign = 1;
}

/** Ensure capacity for @p need limbs. Returns false on allocation failure. */
static bool bn_reserve(bignum_t *a, size_t need) {
   if (need <= a->cap) {
      return true;
   }
   size_t cap = a->cap ? a->cap : 4;
   while (cap < need) {
      cap *= 2;
   }
   uint32_t *p = realloc(a->limb, cap * sizeof(*p));
   if (!p) {
      return false;
   }
   a->limb = p;
   a->cap = cap;
   return true;
}

/** Drop leading zero limbs; collapse zero to canonical (n=0, sign=+1). */
static void bn_normalize(bignum_t *a) {
   while (a->n > 0 && a->limb[a->n - 1] == 0) {
      a->n--;
   }
   if (a->n == 0) {
      a->sign = 1;
   }
}

/** Move ownership of @p src into @p dst (dst freed first; src emptied). */
static void bn_move(bignum_t *dst, bignum_t *src) {
   bn_free(dst);
   *dst = *src;
   bn_init(src);
}

static bool bn_is_zero(const bignum_t *a) {
   return a->n == 0;
}

static bool bn_set_u64(bignum_t *a, uint64_t v) {
   a->sign = 1;
   a->n = 0;
   while (v > 0) {
      if (!bn_reserve(a, a->n + 1)) {
         return false;
      }
      a->limb[a->n++] = (uint32_t)(v % BN_BASE);
      v /= BN_BASE;
   }
   return true;
}

/* ── conversion ─────────────────────────────────────────────────────────── */

/** Parse a pure decimal digit string (no sign) into magnitude. */
static bool bn_from_digits(bignum_t *a, const char *s, size_t len) {
   /* Skip leading zeros. */
   size_t start = 0;
   while (start < len && s[start] == '0') {
      start++;
   }
   size_t ndig = len - start;
   a->sign = 1;
   a->n = 0;
   if (ndig == 0) {
      return true; /* value is zero */
   }
   size_t nlimb = (ndig + BN_BASE_DIGITS - 1) / BN_BASE_DIGITS;
   if (!bn_reserve(a, nlimb)) {
      return false;
   }
   /* Walk from the least-significant end in 9-digit chunks. */
   size_t end = len; /* exclusive */
   for (size_t li = 0; li < nlimb; li++) {
      size_t chunk_start = (end >= start + BN_BASE_DIGITS) ? end - BN_BASE_DIGITS : start;
      uint32_t v = 0;
      for (size_t k = chunk_start; k < end; k++) {
         v = v * 10u + (uint32_t)(s[k] - '0');
      }
      a->limb[li] = v;
      end = chunk_start;
   }
   a->n = nlimb;
   bn_normalize(a);
   return true;
}

static size_t bn_num_digits(const bignum_t *a) {
   if (a->n == 0) {
      return 1;
   }
   uint32_t top = a->limb[a->n - 1];
   size_t top_digits = 0;
   while (top > 0) {
      top_digits++;
      top /= 10;
   }
   return (a->n - 1) * BN_BASE_DIGITS + top_digits;
}

/** Allocate a decimal string for @p a (caller frees), or NULL on failure. */
static char *bn_to_str(const bignum_t *a) {
   if (a->n == 0) {
      return strdup("0");
   }
   size_t digits = bn_num_digits(a);
   size_t cap = digits + 2; /* sign + NUL */
   char *out = malloc(cap);
   if (!out) {
      return NULL;
   }
   size_t pos = 0;
   if (a->sign < 0) {
      out[pos++] = '-';
   }
   /* Most-significant limb without padding, the rest zero-padded to 9. */
   pos += (size_t)snprintf(out + pos, cap - pos, "%u", a->limb[a->n - 1]);
   for (size_t i = a->n - 1; i > 0; i--) {
      pos += (size_t)snprintf(out + pos, cap - pos, "%09u", a->limb[i - 1]);
   }
   out[pos] = '\0';
   return out;
}

/** Convert to u64 if it fits. Returns false on overflow or negative. */
static bool bn_to_u64(const bignum_t *a, uint64_t *out) {
   if (a->sign < 0) {
      return false;
   }
   uint64_t v = 0;
   for (size_t i = a->n; i > 0; i--) {
      if (v > (UINT64_MAX - a->limb[i - 1]) / BN_BASE) {
         return false;
      }
      v = v * BN_BASE + a->limb[i - 1];
   }
   *out = v;
   return true;
}

/* ── magnitude arithmetic (ignore sign) ─────────────────────────────────── */

static int bn_cmp_abs(const bignum_t *a, const bignum_t *b) {
   if (a->n != b->n) {
      return a->n < b->n ? -1 : 1;
   }
   for (size_t i = a->n; i > 0; i--) {
      if (a->limb[i - 1] != b->limb[i - 1]) {
         return a->limb[i - 1] < b->limb[i - 1] ? -1 : 1;
      }
   }
   return 0;
}

/** r = |a| + |b| (sign set by caller). */
static bool bn_add_mag(bignum_t *r, const bignum_t *a, const bignum_t *b) {
   bignum_t t;
   bn_init(&t);
   size_t maxn = (a->n > b->n ? a->n : b->n) + 1;
   if (!bn_reserve(&t, maxn)) {
      bn_free(&t);
      return false;
   }
   uint64_t carry = 0;
   for (size_t i = 0; i < maxn; i++) {
      uint64_t s = carry;
      if (i < a->n) {
         s += a->limb[i];
      }
      if (i < b->n) {
         s += b->limb[i];
      }
      t.limb[i] = (uint32_t)(s % BN_BASE);
      carry = s / BN_BASE;
   }
   t.n = maxn;
   bn_normalize(&t);
   bn_move(r, &t);
   return true;
}

/** r = |a| - |b|, requires |a| >= |b| (sign set by caller). */
static bool bn_sub_mag(bignum_t *r, const bignum_t *a, const bignum_t *b) {
   bignum_t t;
   bn_init(&t);
   if (!bn_reserve(&t, a->n)) {
      bn_free(&t);
      return false;
   }
   int64_t borrow = 0;
   for (size_t i = 0; i < a->n; i++) {
      int64_t s = (int64_t)a->limb[i] - borrow - (i < b->n ? (int64_t)b->limb[i] : 0);
      if (s < 0) {
         s += BN_BASE;
         borrow = 1;
      } else {
         borrow = 0;
      }
      t.limb[i] = (uint32_t)s;
   }
   t.n = a->n;
   bn_normalize(&t);
   bn_move(r, &t);
   return true;
}

/* ── signed arithmetic ──────────────────────────────────────────────────── */

static bool bn_add(bignum_t *r, const bignum_t *a, const bignum_t *b) {
   bignum_t t;
   bn_init(&t);
   bool ok;
   if (a->sign == b->sign) {
      ok = bn_add_mag(&t, a, b);
      t.sign = a->sign;
   } else {
      int c = bn_cmp_abs(a, b);
      if (c == 0) {
         ok = true; /* a + (-a) = 0 */
      } else if (c > 0) {
         ok = bn_sub_mag(&t, a, b);
         t.sign = a->sign;
      } else {
         ok = bn_sub_mag(&t, b, a);
         t.sign = b->sign;
      }
   }
   if (!ok) {
      bn_free(&t);
      return false;
   }
   bn_normalize(&t);
   bn_move(r, &t);
   return true;
}

static bool bn_sub(bignum_t *r, const bignum_t *a, const bignum_t *b) {
   bignum_t nb;
   bn_init(&nb);
   if (!bn_reserve(&nb, b->n ? b->n : 1)) {
      bn_free(&nb);
      return false;
   }
   if (b->n > 0) { /* a zero has no limbs, and memcpy's source must not be NULL */
      memcpy(nb.limb, b->limb, b->n * sizeof(*b->limb));
   }
   nb.n = b->n;
   nb.sign = -b->sign;
   bool ok = bn_add(r, a, &nb);
   bn_free(&nb);
   return ok;
}

static bool bn_mul(bignum_t *r, const bignum_t *a, const bignum_t *b) {
   if (bn_is_zero(a) || bn_is_zero(b)) {
      bignum_t z;
      bn_init(&z);
      bn_move(r, &z);
      return true;
   }
   bignum_t t;
   bn_init(&t);
   size_t rn = a->n + b->n;
   if (!bn_reserve(&t, rn)) {
      bn_free(&t);
      return false;
   }
   memset(t.limb, 0, rn * sizeof(*t.limb));
   for (size_t i = 0; i < a->n; i++) {
      uint64_t carry = 0;
      uint64_t ai = a->limb[i];
      for (size_t j = 0; j < b->n; j++) {
         uint64_t cur = (uint64_t)t.limb[i + j] + ai * b->limb[j] + carry;
         t.limb[i + j] = (uint32_t)(cur % BN_BASE);
         carry = cur / BN_BASE;
      }
      t.limb[i + b->n] += (uint32_t)carry;
   }
   t.n = rn;
   t.sign = a->sign * b->sign;
   bn_normalize(&t);
   bn_move(r, &t);
   return true;
}

/** r = a * m, where 0 <= m < BN_BASE. */
static bool bn_mul_small(bignum_t *r, const bignum_t *a, uint32_t m) {
   if (m == 0 || bn_is_zero(a)) {
      bignum_t z;
      bn_init(&z);
      bn_move(r, &z);
      return true;
   }
   bignum_t t;
   bn_init(&t);
   if (!bn_reserve(&t, a->n + 1)) {
      bn_free(&t);
      return false;
   }
   uint64_t carry = 0;
   for (size_t i = 0; i < a->n; i++) {
      uint64_t cur = (uint64_t)a->limb[i] * m + carry;
      t.limb[i] = (uint32_t)(cur % BN_BASE);
      carry = cur / BN_BASE;
   }
   t.n = a->n;
   if (carry) {
      t.limb[t.n++] = (uint32_t)carry;
   }
   t.sign = a->sign;
   bn_normalize(&t);
   bn_move(r, &t);
   return true;
}

/** q = a / d (truncated), *rem = a % d; 0 < d < BN_BASE. Magnitude only. */
static bool bn_divmod_small(bignum_t *q, const bignum_t *a, uint32_t d, uint32_t *rem) {
   bignum_t t;
   bn_init(&t);
   if (!bn_reserve(&t, a->n ? a->n : 1)) {
      bn_free(&t);
      return false;
   }
   uint64_t r = 0;
   for (size_t i = a->n; i > 0; i--) {
      uint64_t cur = r * BN_BASE + a->limb[i - 1];
      t.limb[i - 1] = (uint32_t)(cur / d);
      r = cur % d;
   }
   t.n = a->n;
   bn_normalize(&t);
   *rem = (uint32_t)r;
   bn_move(q, &t);
   return true;
}

/* ── parser ─────────────────────────────────────────────────────────────── */

/* Recursion-depth ceiling for the parser.  Bounds nested parentheses and unary
 * chains so an adversarial expression (e.g. thousands of '(') can't exhaust the
 * thread stack — tool callbacks run on reduced 256-512 KB stacks. */
#define BN_MAX_PARSE_DEPTH 256

typedef struct {
   const char *p;
   calc_exact_status_t status; /* CALC_EXACT_OK until a problem is hit */
   int depth;                  /* current recursion depth (guarded by BN_MAX_PARSE_DEPTH) */
} exact_ctx_t;

static void parse_expr(exact_ctx_t *c, bignum_t *out);

static void skip_ws(exact_ctx_t *c) {
   while (*c->p == ' ' || *c->p == '\t') {
      c->p++;
   }
}

/* Set status only if still OK (preserve the first/most-specific failure). */
static void set_status(exact_ctx_t *c, calc_exact_status_t s) {
   if (c->status == CALC_EXACT_OK) {
      c->status = s;
   }
}

/** Reject a result whose digit count exceeds the cap. */
static void check_size(exact_ctx_t *c, const bignum_t *a) {
   if (bn_num_digits(a) > CALC_EXACT_MAX_DIGITS) {
      set_status(c, CALC_EXACT_TOO_LARGE);
   }
}

static void parse_primary(exact_ctx_t *c, bignum_t *out) {
   bn_init(out);
   skip_ws(c);
   if (*c->p == '(') {
      c->p++;
      if (++c->depth > BN_MAX_PARSE_DEPTH) {
         set_status(c, CALC_EXACT_ERROR); /* too deeply nested — abort, don't recurse */
         return;
      }
      parse_expr(c, out);
      c->depth--;
      skip_ws(c);
      if (*c->p == ')') {
         c->p++;
      } else {
         set_status(c, CALC_EXACT_ERROR);
      }
      return;
   }
   if (isdigit((unsigned char)*c->p)) {
      const char *start = c->p;
      while (isdigit((unsigned char)*c->p)) {
         c->p++;
      }
      if (!bn_from_digits(out, start, (size_t)(c->p - start))) {
         set_status(c, CALC_EXACT_ERROR);
      }
      return;
   }
   /* A letter (function name like sqrt), '.', or anything else means this is not
    * a pure-integer expression — signal INEXACT so the caller uses the float path. */
   if (isalpha((unsigned char)*c->p) || *c->p == '.') {
      set_status(c, CALC_EXACT_INEXACT);
   } else {
      set_status(c, CALC_EXACT_ERROR);
   }
}

static void parse_postfix(exact_ctx_t *c, bignum_t *out) {
   parse_primary(c, out);
   for (;;) {
      skip_ws(c);
      if (*c->p != '!') {
         break;
      }
      c->p++;
      if (c->status != CALC_EXACT_OK) {
         continue;
      }
      uint64_t nn;
      if (out->sign < 0 || !bn_to_u64(out, &nn)) {
         set_status(c, CALC_EXACT_INEXACT); /* negative/huge factorial → float path */
         continue;
      }
      if (nn > BN_FAC_MAX_N) {
         set_status(c, CALC_EXACT_TOO_LARGE);
         continue;
      }
      bignum_t acc;
      bn_init(&acc);
      if (!bn_set_u64(&acc, 1)) {
         set_status(c, CALC_EXACT_ERROR);
         bn_free(&acc);
         continue;
      }
      for (uint64_t i = 2; i <= nn && c->status == CALC_EXACT_OK; i++) {
         if (!bn_mul_small(&acc, &acc, (uint32_t)i)) {
            set_status(c, CALC_EXACT_ERROR);
            break;
         }
         check_size(c, &acc);
      }
      bn_move(out, &acc);
   }
}

static void parse_factor(exact_ctx_t *c, bignum_t *out); /* fwd */

/* power := postfix ('^' factor)?   — right-associative, integer exponent only */
static void parse_power(exact_ctx_t *c, bignum_t *out) {
   parse_postfix(c, out);
   skip_ws(c);
   if (*c->p != '^') {
      return;
   }
   c->p++;
   bignum_t exp;
   bn_init(&exp);
   parse_factor(c, &exp);
   if (c->status != CALC_EXACT_OK) {
      bn_free(&exp);
      return;
   }
   uint64_t e;
   if (exp.sign < 0 || !bn_to_u64(&exp, &e)) {
      set_status(c, CALC_EXACT_INEXACT); /* negative exponent → non-integer → float path */
      bn_free(&exp);
      return;
   }
   bn_free(&exp);
   /* Exponentiation by squaring; guard size between steps. */
   bignum_t result, base;
   bn_init(&result);
   bn_init(&base);
   bool ok = bn_set_u64(&result, 1);
   if (ok) {
      if (!bn_reserve(&base, out->n ? out->n : 1)) {
         ok = false;
      } else {
         if (out->n > 0) { /* a zero has no limbs, and memcpy's source must not be NULL */
            memcpy(base.limb, out->limb, out->n * sizeof(*out->limb));
         }
         base.n = out->n;
         base.sign = out->sign;
      }
   }
   while (ok && e > 0 && c->status == CALC_EXACT_OK) {
      if (e & 1u) {
         ok = bn_mul(&result, &result, &base);
         check_size(c, &result);
      }
      e >>= 1;
      if (e > 0 && ok && c->status == CALC_EXACT_OK) {
         ok = bn_mul(&base, &base, &base);
         check_size(c, &base);
      }
   }
   if (!ok) {
      set_status(c, CALC_EXACT_ERROR);
   }
   bn_free(&base);
   bn_move(out, &result);
}

/* factor := ('-' | '+') factor | power */
static void parse_factor(exact_ctx_t *c, bignum_t *out) {
   bn_init(out);
   skip_ws(c);
   if (*c->p == '-' || *c->p == '+') {
      char op = *c->p;
      c->p++;
      if (++c->depth > BN_MAX_PARSE_DEPTH) {
         set_status(c, CALC_EXACT_ERROR); /* unary chain too deep — abort */
         return;
      }
      parse_factor(c, out);
      c->depth--;
      if (op == '-' && c->status == CALC_EXACT_OK && !bn_is_zero(out)) {
         out->sign = -out->sign;
      }
      return;
   }
   parse_power(c, out);
}

/* term := factor (('*' | '/') factor)* */
static void parse_term(exact_ctx_t *c, bignum_t *out) {
   parse_factor(c, out);
   for (;;) {
      skip_ws(c);
      char op = *c->p;
      if (op != '*' && op != '/') {
         break;
      }
      c->p++;
      bignum_t rhs;
      bn_init(&rhs);
      parse_factor(c, &rhs);
      if (c->status != CALC_EXACT_OK) {
         bn_free(&rhs);
         return;
      }
      if (op == '*') {
         if (!bn_mul(out, out, &rhs)) {
            set_status(c, CALC_EXACT_ERROR);
         }
         check_size(c, out);
      } else { /* '/' — exact integer division only */
         if (bn_is_zero(&rhs)) {
            set_status(c, CALC_EXACT_ERROR); /* division by zero */
         } else if (rhs.n > 1) {
            /* Multi-limb divisor: v1 hands these to the float path. */
            set_status(c, CALC_EXACT_INEXACT);
         } else {
            uint32_t d = rhs.limb[0];
            uint32_t rem = 0;
            bignum_t q;
            bn_init(&q);
            if (!bn_divmod_small(&q, out, d, &rem)) {
               set_status(c, CALC_EXACT_ERROR);
               bn_free(&q);
            } else if (rem != 0) {
               set_status(c, CALC_EXACT_INEXACT); /* not evenly divisible → float path */
               bn_free(&q);
            } else {
               q.sign = bn_is_zero(&q) ? 1 : out->sign * rhs.sign;
               bn_move(out, &q);
            }
         }
      }
      bn_free(&rhs);
      if (c->status != CALC_EXACT_OK) {
         return;
      }
   }
}

/* expr := term (('+' | '-') term)* */
static void parse_expr(exact_ctx_t *c, bignum_t *out) {
   parse_term(c, out);
   for (;;) {
      skip_ws(c);
      char op = *c->p;
      if (op != '+' && op != '-') {
         break;
      }
      c->p++;
      bignum_t rhs;
      bn_init(&rhs);
      parse_term(c, &rhs);
      if (c->status != CALC_EXACT_OK) {
         bn_free(&rhs);
         return;
      }
      bool ok = (op == '+') ? bn_add(out, out, &rhs) : bn_sub(out, out, &rhs);
      if (!ok) {
         set_status(c, CALC_EXACT_ERROR);
      }
      bn_free(&rhs);
      if (c->status != CALC_EXACT_OK) {
         return;
      }
   }
}

/* ── public entry point ─────────────────────────────────────────────────── */

calc_exact_status_t calculator_evaluate_exact(const char *expr,
                                              char **out_decimal,
                                              char *err,
                                              size_t err_size) {
   if (out_decimal) {
      *out_decimal = NULL;
   }
   if (!expr) {
      if (err && err_size) {
         snprintf(err, err_size, "Empty expression");
      }
      return CALC_EXACT_ERROR;
   }

   exact_ctx_t c = { .p = expr, .status = CALC_EXACT_OK };
   bignum_t result;
   bn_init(&result);
   parse_expr(&c, &result);

   /* Trailing junk after a well-formed expression is a hard error. */
   if (c.status == CALC_EXACT_OK) {
      skip_ws(&c);
      if (*c.p != '\0') {
         /* A letter/dot here means a function/decimal slipped in → float path. */
         c.status = (isalpha((unsigned char)*c.p) || *c.p == '.') ? CALC_EXACT_INEXACT
                                                                  : CALC_EXACT_ERROR;
      }
   }

   calc_exact_status_t status = c.status;
   if (status == CALC_EXACT_OK) {
      char *s = bn_to_str(&result);
      if (!s) {
         status = CALC_EXACT_ERROR;
      } else if (out_decimal) {
         *out_decimal = s;
      } else {
         free(s);
      }
   }
   bn_free(&result);

   if (status != CALC_EXACT_OK && err && err_size) {
      switch (status) {
         case CALC_EXACT_TOO_LARGE:
            snprintf(err, err_size, "Result exceeds %d digits — too large for exact mode",
                     CALC_EXACT_MAX_DIGITS);
            break;
         case CALC_EXACT_INEXACT:
            snprintf(err, err_size, "Not an exact integer");
            break;
         default:
            snprintf(err, err_size, "Malformed expression");
            break;
      }
   }
   return status;
}
