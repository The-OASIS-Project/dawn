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
 * What result_read does with a stored result (result_read_ops.h).
 */

#define _GNU_SOURCE /* memrchr */
#include "tools/result_read_ops.h"

#include <ctype.h>
#include <json-c/json.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/strbuf.h"
#include "llm/llm_tool_view.h"
#include "llm/llm_tool_view_path.h"
#include "tools/tool_registry.h"

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static char *failure(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static char *failure(const char *fmt, ...) {
   char text[1024];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(text, sizeof(text), fmt, ap);
   va_end(ap);
   char *out = malloc(strlen(TOOL_RESULT_ERROR_MARK) + strlen(text) + 1);
   if (out) {
      strcpy(out, TOOL_RESULT_ERROR_MARK);
      strcat(out, text);
   }
   return out;
}

static size_t utf8_back(const char *s, size_t i) {
   while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) {
      i--;
   }
   return i;
}

/* @p n bytes of @p s as a JSON string, quoted. */
static void json_quoted(strbuf_t *sb, const char *s, size_t n) {
   strbuf_append(sb, "\"");
   const char *run = s;
   for (const char *p = s; p < s + n; p++) {
      const unsigned char c = (unsigned char)*p;
      if (c >= 0x20 && c != '"' && c != '\\') {
         continue;
      }
      strbuf_append_n(sb, run, (size_t)(p - run));
      switch (c) {
         case '"':
            strbuf_append(sb, "\\\"");
            break;
         case '\\':
            strbuf_append(sb, "\\\\");
            break;
         case '\n':
            strbuf_append(sb, "\\n");
            break;
         case '\t':
            strbuf_append(sb, "\\t");
            break;
         default:
            strbuf_appendf(sb, "\\u%04x", c);
      }
      run = p + 1;
   }
   strbuf_append_n(sb, run, (size_t)(s + n - run));
   strbuf_append(sb, "\"");
}

static unsigned char fold(unsigned char c) {
   return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 32) : c;
}

/* A literal, ASCII case-insensitive search (Horspool over folded bytes). */
typedef struct {
   unsigned char pat[RESULT_READ_PATTERN_MAX];
   size_t len;
   size_t skip[256];
} finder_t;

static void finder_init(finder_t *f, const char *pattern) {
   f->len = strlen(pattern);
   for (size_t i = 0; i < f->len; i++) {
      f->pat[i] = fold((unsigned char)pattern[i]);
   }
   for (int c = 0; c < 256; c++) {
      f->skip[c] = f->len;
   }
   for (size_t i = 0; i + 1 < f->len; i++) {
      f->skip[f->pat[i]] = f->len - 1 - i;
   }
}

/* Where @p f's pattern first is in @p hay (@p n bytes), or @p n when it isn't. */
static size_t finder_find(const finder_t *f, const char *hay, size_t n) {
   if (f->len == 0 || n < f->len) {
      return n;
   }
   if (f->len == 1) {
      /* Horspool gains nothing on one byte: memchr for each case of it. */
      const unsigned char lo = f->pat[0];
      const unsigned char up = lo >= 'a' && lo <= 'z' ? (unsigned char)(lo - 32) : lo;
      const char *a = memchr(hay, lo, n);
      const char *b = up != lo ? memchr(hay, up, a ? (size_t)(a - hay) : n) : NULL;
      const char *first = b ? b : a;
      return first ? (size_t)(first - hay) : n;
   }
   const unsigned char *h = (const unsigned char *)hay;
   size_t i = 0;
   while (i + f->len <= n) {
      const unsigned char last = fold(h[i + f->len - 1]);
      if (last == f->pat[f->len - 1]) {
         size_t j = 0;
         while (j + 1 < f->len && fold(h[i + j]) == f->pat[j]) {
            j++;
         }
         if (j + 1 >= f->len) {
            return i;
         }
      }
      i += f->skip[last];
   }
   return n;
}

static char *finish(strbuf_t *sb) {
   if (strbuf_oom(sb)) {
      strbuf_free(sb);
      return NULL;
   }
   char *out = strbuf_steal(sb);
   return out ? out : strdup("");
}

/* ---------------------------------------------------------------------------
 * Paths (the grammar is llm_tool_view_path.h's: what a view writes, read back)
 * ------------------------------------------------------------------------- */

/* The object member @p key names: the key itself, or the key a view showed
 * cut as @p key (the key it is, in *@p real). */
static struct json_object *member(struct json_object *obj,
                                  const char *key,
                                  bool *found,
                                  const char **real) {
   struct json_object *v = NULL;
   if (json_object_object_get_ex(obj, key, &v)) {
      *found = true;
      if (real) {
         *real = key;
      }
      return v;
   }
   if (strlen(key) > LLM_TOOL_VIEW_KEY_MAX) {
      json_object_object_foreach(obj, k, val) {
         if (llm_tool_view_key_shown_as(k, key)) {
            *found = true;
            if (real) {
               *real = k;
            }
            return val;
         }
      }
   }
   *found = false;
   return NULL;
}

/* @p p with key @p key as a view writes it (cut past LLM_TOOL_VIEW_KEY_MAX). */
static void path_key_shown(llm_tool_view_path_t *p, const char *key) {
   char cut[LLM_TOOL_VIEW_KEY_SHOWN_MAX];
   llm_tool_view_path_key(p, key, strlen(key), llm_tool_view_key_shown(key, cut));
}

/* Some of @p obj's keys, for a "no such key" answer. */
static void some_keys(strbuf_t *sb, struct json_object *obj) {
   int n = 0;
   const int total = json_object_object_length(obj);
   json_object_object_foreach(obj, k, v) {
      (void)v;
      if (n == 20) {
         strbuf_appendf(sb, ", ... %d more", total - n);
         break;
      }
      strbuf_append(sb, n ? ", " : "");
      json_quoted(sb, k, utf8_back(k, strlen(k) > 64 ? 64 : strlen(k)));
      n++;
   }
}

/* Where @p path leads in @p root. */
typedef struct {
   struct json_object *value;  /* the value; for a slice, its array */
   llm_tool_view_path_t shown; /* its path; for a slice, the array's */
   bool slice;
   size_t from;
   size_t to;
} resolved_t;

/* Resolve @p path in @p root into @p r: NULL, or why not (heap). */
static char *resolve(struct json_object *root, const char *path, resolved_t *r) {
   memset(r, 0, sizeof(*r));
   llm_tool_view_path_expr_t e;
   const char *bad = llm_tool_view_path_parse(path, &e);
   if (bad) {
      return failure("Can't read the path: %s.", bad);
   }
   struct json_object *cur = root;
   llm_tool_view_path_put(&r->shown, "$", 1);
   for (int i = 0; i < e.n; i++) {
      const llm_tool_view_path_seg_t *seg = &e.seg[i];
      if (seg->kind == LLM_TOOL_VIEW_PATH_KEY) {
         if (!json_object_is_type(cur, json_type_object)) {
            return failure("%s is %s, not an object.", r->shown.s,
                           json_object_is_type(cur, json_type_array) ? "an array" : "a value");
         }
         bool found = false;
         const char *real = NULL;
         struct json_object *next = member(cur, seg->key, &found, &real);
         if (!found) {
            strbuf_t sb;
            strbuf_init(&sb, 256);
            strbuf_appendf(&sb, "%sNo key ", TOOL_RESULT_ERROR_MARK);
            json_quoted(&sb, seg->key, strlen(seg->key));
            strbuf_appendf(&sb, " at %s. Its keys: ", r->shown.s);
            some_keys(&sb, cur);
            strbuf_append(&sb, ".");
            return finish(&sb);
         }
         path_key_shown(&r->shown, real);
         cur = next;
         continue;
      }
      if (!json_object_is_type(cur, json_type_array)) {
         return failure("%s is %s, not an array.", r->shown.s,
                        json_object_is_type(cur, json_type_object) ? "an object" : "a value");
      }
      const long long n = (long long)json_object_array_length(cur);
      if (seg->kind == LLM_TOOL_VIEW_PATH_INDEX) {
         const long long idx = seg->a < 0 ? n + seg->a : seg->a;
         if (idx < 0 || idx >= n) {
            return failure("Index %lld is outside %s, which has %lld items.", seg->a, r->shown.s,
                           n);
         }
         llm_tool_view_path_index(&r->shown, (size_t)idx);
         cur = json_object_array_get_idx(cur, (size_t)idx);
         continue;
      }
      long long a = seg->has_a ? seg->a : 0;
      long long b = seg->has_b ? seg->b : n;
      a = a < 0 ? n + a : a;
      b = b < 0 ? n + b : b;
      a = a < 0 ? 0 : a > n ? n : a;
      b = b < 0 ? 0 : b > n ? n : b;
      if (b < a) {
         b = a;
      }
      r->slice = true;
      r->from = (size_t)a;
      r->to = (size_t)b;
   }
   r->value = cur;
   return NULL;
}

/* ---------------------------------------------------------------------------
 * path, count, distinct
 * ------------------------------------------------------------------------- */

static size_t view_budget(size_t budget, size_t header) {
   return budget > header + LLM_TOOL_VIEW_MIN_BUDGET ? budget - header : LLM_TOOL_VIEW_MIN_BUDGET;
}

char *result_read_path(struct json_object *root, const char *path, size_t budget) {
   if (!root) {
      return NULL;
   }
   resolved_t r;
   char *bad = resolve(root, path, &r);
   if (bad) {
      return bad;
   }
   strbuf_t sb;
   strbuf_init_with_max(&sb, 256, budget + 1024);
   char *view = NULL;
   if (r.slice) {
      const size_t n = json_object_array_length(r.value);
      if (r.from == r.to) {
         strbuf_appendf(&sb, "%s[%zu:%zu]: no items (it has %zu).", r.shown.s, r.from, r.to, n);
         return finish(&sb);
      }
      strbuf_appendf(&sb, "%s[%zu:%zu] (items %zu-%zu of %zu):\n", r.shown.s, r.from, r.to, r.from,
                     r.to - 1, n);
      struct json_object *items = json_object_new_array_ext((int)(r.to - r.from));
      for (size_t i = r.from; items && i < r.to; i++) {
         json_object_array_add(items, json_object_get(json_object_array_get_idx(r.value, i)));
      }
      view = items ? llm_tool_view_slice(items, r.from, view_budget(budget, strbuf_len(&sb)),
                                         r.shown.s, NULL)
                   : NULL;
      json_object_put(items);
   } else {
      strbuf_appendf(&sb, "%s:\n", r.shown.s);
      view = llm_tool_view_tree(r.value, view_budget(budget, strbuf_len(&sb)), r.shown.s, NULL);
   }
   if (!view) {
      strbuf_free(&sb);
      return NULL;
   }
   strbuf_append(&sb, view);
   free(view);
   return finish(&sb);
}

char *result_read_count(struct json_object *root, const char *path) {
   if (!root) {
      return NULL;
   }
   resolved_t r;
   char *bad = resolve(root, path, &r);
   if (bad) {
      return bad;
   }
   strbuf_t sb;
   strbuf_init(&sb, 128);
   if (r.slice) {
      strbuf_appendf(&sb, "%s[%zu:%zu]: %zu items", r.shown.s, r.from, r.to, r.to - r.from);
      return finish(&sb);
   }
   switch (json_object_get_type(r.value)) {
      case json_type_array:
         strbuf_appendf(&sb, "%s: %zu items", r.shown.s, json_object_array_length(r.value));
         break;
      case json_type_object:
         strbuf_appendf(&sb, "%s: %d keys", r.shown.s, json_object_object_length(r.value));
         break;
      case json_type_string: {
         const char *s = json_object_get_string(r.value);
         const size_t n = (size_t)json_object_get_string_len(r.value);
         size_t chars = 0;
         for (size_t i = 0; i < n; i++) {
            chars += ((unsigned char)s[i] & 0xC0) != 0x80;
         }
         strbuf_appendf(&sb, "%s: a string of %zu characters", r.shown.s, chars);
         break;
      }
      default:
         strbuf_appendf(&sb, "%s: a single value", r.shown.s);
         break;
   }
   return finish(&sb);
}

typedef struct {
   const char *value; /* JSON text */
   int count;
} tally_t;

static int tally_order(const void *a, const void *b) {
   const tally_t *x = a;
   const tally_t *y = b;
   if (x->count != y->count) {
      return x->count > y->count ? -1 : 1;
   }
   return strcmp(x->value, y->value);
}

/* @p v as JSON text for counting: a string quoted, anything else its JSON. */
static char *value_text(struct json_object *v) {
   if (json_object_is_type(v, json_type_string)) {
      strbuf_t sb;
      strbuf_init_with_max(&sb, 64, (size_t)json_object_get_string_len(v) * 6 + 16);
      json_quoted(&sb, json_object_get_string(v), (size_t)json_object_get_string_len(v));
      return finish(&sb);
   }
   if (json_object_is_type(v, json_type_object) || json_object_is_type(v, json_type_array)) {
      /* Compact JSON written by the view, not json-c's printbuf (which would
       * stay on the tree); a value past this is counted by its view. */
      return llm_tool_view_tree(v, RESULT_READ_DISTINCT_VALUE_BYTES, "$", NULL);
   }
   char num[LLM_TOOL_VIEW_SCALAR_MAX];
   return strdup(llm_tool_view_scalar(v, num));
}

char *result_read_distinct(struct json_object *root,
                           const char *path,
                           const char *field,
                           size_t budget) {
   if (!root) {
      return NULL;
   }
   if (!field || !field[0]) {
      return failure("distinct needs a field: the key whose values to count.");
   }
   resolved_t r;
   char *bad = resolve(root, path, &r);
   if (bad) {
      return bad;
   }
   if (!json_object_is_type(r.value, json_type_array)) {
      return failure("%s is not an array: distinct counts a field of an array's items.", r.shown.s);
   }
   const size_t from = r.slice ? r.from : 0;
   const size_t to = r.slice ? r.to : json_object_array_length(r.value);
   struct json_object *counts = json_object_new_object();
   if (!counts) {
      return NULL;
   }
   size_t missing = 0;
   size_t items = 0;
   for (size_t i = from; i < to; i++) {
      struct json_object *item = json_object_array_get_idx(r.value, i);
      items++;
      bool found = false;
      struct json_object *v = json_object_is_type(item, json_type_object)
                                  ? member(item, field, &found, NULL)
                                  : NULL;
      if (!found) {
         missing++;
         continue;
      }
      char *text = value_text(v);
      if (!text) {
         json_object_put(counts);
         return NULL;
      }
      struct json_object *n = NULL;
      if (json_object_object_get_ex(counts, text, &n)) {
         json_object_set_int(n, json_object_get_int(n) + 1);
      } else {
         json_object_object_add(counts, text, json_object_new_int(1));
      }
      free(text);
   }
   const int distinct = json_object_object_length(counts);
   tally_t *tally = distinct ? calloc((size_t)distinct, sizeof(*tally)) : NULL;
   if (distinct && !tally) {
      json_object_put(counts);
      return NULL;
   }
   int k = 0;
   json_object_object_foreach(counts, key, val) {
      /* tally is NULL only when distinct is 0, and then the loop does not run */
      // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
      tally[k].value = key;
      tally[k++].count = json_object_get_int(val);
   }
   if (distinct > 1) { /* tally is NULL when nothing was counted */
      qsort(tally, (size_t)distinct, sizeof(*tally), tally_order);
   }

   strbuf_t sb;
   strbuf_init_with_max(&sb, 512, budget + 256);
   if (r.slice) {
      strbuf_appendf(&sb, "%s[%zu:%zu]: ", r.shown.s, r.from, r.to);
   } else {
      strbuf_appendf(&sb, "%s: ", r.shown.s);
   }
   strbuf_appendf(&sb, "%zu items, %d distinct values of ", items, distinct);
   json_quoted(&sb, field, strlen(field));
   if (missing) {
      strbuf_appendf(&sb, " (%zu without it)", missing);
   }
   strbuf_append(&sb, ":\n");
   int shown = 0;
   for (; shown < distinct && shown < RESULT_READ_DISTINCT_MAX; shown++) {
      const char *v = tally[shown].value;
      /* tally values are JSON object keys, never NULL */
      // NOLINTNEXTLINE(clang-analyzer-core.NonNullParamChecker)
      const size_t n = strlen(v);
      const size_t keep = n > RESULT_READ_VALUE_MAX ? utf8_back(v, RESULT_READ_VALUE_MAX) : n;
      if (strbuf_len(&sb) + keep + 32 > budget) {
         break;
      }
      strbuf_appendf(&sb, "%7d  %.*s%s\n", tally[shown].count, (int)keep, v,
                     keep < n ? "...(cut)" : "");
   }
   if (shown < distinct) {
      strbuf_appendf(&sb, "... %d more values\n", distinct - shown);
   }
   free(tally);
   json_object_put(counts);
   return finish(&sb);
}

/* ---------------------------------------------------------------------------
 * grep
 * ------------------------------------------------------------------------- */

/* The window of @p n bytes of @p s around the match at @p at (@p mlen long). */
static void window(strbuf_t *sb, const char *s, size_t n, size_t at, size_t mlen) {
   size_t start = at > RESULT_READ_VALUE_MAX / 3 ? at - RESULT_READ_VALUE_MAX / 3 : 0;
   start = utf8_back(s, start);
   size_t end = start + RESULT_READ_VALUE_MAX;
   if (end < at + mlen) {
      end = at + mlen;
   }
   if (end >= n) {
      end = n;
   } else {
      end = utf8_back(s, end);
   }
   if (start > 0) {
      strbuf_append(sb, "...");
   }
   strbuf_append_n(sb, s + start, end - start);
   if (end < n) {
      strbuf_append(sb, "...");
   }
}

typedef struct {
   const finder_t *finder;
   strbuf_t *out;
   size_t budget;
   size_t matches;
   size_t shown;
   llm_tool_view_path_t path;
} grep_walk_t;

static void grep_emit(grep_walk_t *g, const char *what_fmt, struct json_object *v, size_t at) {
   g->matches++;
   if (g->shown == RESULT_READ_MATCHES_MAX || strbuf_len(g->out) > g->budget) {
      return;
   }
   strbuf_t line;
   strbuf_init(&line, 256);
   strbuf_appendf(&line, "%s%s: ", g->path.s, what_fmt);
   if (json_object_is_type(v, json_type_string)) {
      strbuf_t win;
      strbuf_init(&win, 256);
      window(&win, json_object_get_string(v), (size_t)json_object_get_string_len(v), at,
             g->finder->len);
      json_quoted(&line, strbuf_str(&win), strbuf_len(&win));
      strbuf_free(&win);
   } else if (json_object_is_type(v, json_type_array)) {
      strbuf_appendf(&line, "[%zu items]", json_object_array_length(v));
   } else if (json_object_is_type(v, json_type_object)) {
      strbuf_appendf(&line, "{%d keys}", json_object_object_length(v));
   } else {
      char num[LLM_TOOL_VIEW_SCALAR_MAX];
      strbuf_append(&line, llm_tool_view_scalar(v, num));
   }
   strbuf_append(&line, "\n");
   if (!strbuf_oom(&line) && strbuf_len(g->out) + strbuf_len(&line) <= g->budget) {
      strbuf_append_n(g->out, strbuf_str(&line), strbuf_len(&line));
      g->shown++;
   }
   strbuf_free(&line);
}

static void grep_walk(grep_walk_t *g, struct json_object *v, int depth) {
   if (depth > 64) {
      return;
   }
   const size_t saved = g->path.len;
   const bool saved_cut = g->path.cut;
   switch (json_object_get_type(v)) {
      case json_type_object: {
         json_object_object_foreach(v, key, val) {
            path_key_shown(&g->path, key);
            const size_t kl = strlen(key);
            if (finder_find(g->finder, key, kl) < kl) {
               grep_emit(g, " (key)", val, 0);
            }
            grep_walk(g, val, depth + 1);
            g->path.len = saved;
            g->path.cut = saved_cut;
            g->path.s[saved] = '\0';
         }
         break;
      }
      case json_type_array: {
         const size_t n = json_object_array_length(v);
         for (size_t i = 0; i < n; i++) {
            llm_tool_view_path_index(&g->path, i);
            grep_walk(g, json_object_array_get_idx(v, i), depth + 1);
            g->path.len = saved;
            g->path.cut = saved_cut;
            g->path.s[saved] = '\0';
         }
         break;
      }
      case json_type_string: {
         const size_t n = (size_t)json_object_get_string_len(v);
         const size_t at = finder_find(g->finder, json_object_get_string(v), n);
         if (at < n) {
            grep_emit(g, "", v, at);
         }
         break;
      }
      default: {
         char num[LLM_TOOL_VIEW_SCALAR_MAX];
         const char *text = llm_tool_view_scalar(v, num);
         const size_t n = strlen(text);
         if (n && finder_find(g->finder, text, n) < n) {
            grep_emit(g, "", v, 0);
         }
         break;
      }
   }
}

/* A grep answer's header and footer: counts and the pattern quoted (a
 * control character escapes to six bytes). */
#define GREP_HEADER_ROOM (RESULT_READ_PATTERN_MAX * 6 + 256)

static const char *pattern_problem(const char *pattern) {
   if (!pattern || !pattern[0]) {
      return "grep needs a pattern (the text to find).";
   }
   if (strlen(pattern) > RESULT_READ_PATTERN_MAX) {
      return "the pattern is too long (200 characters at most).";
   }
   if (strpbrk(pattern, "\r\n")) {
      return "a pattern is one line.";
   }
   return NULL;
}

char *result_read_grep_tree(struct json_object *root, const char *pattern, size_t budget) {
   const char *bad = pattern_problem(pattern);
   if (bad) {
      return failure("%s", bad);
   }
   if (!root) {
      return NULL;
   }
   finder_t finder;
   finder_init(&finder, pattern);
   strbuf_t lines;
   strbuf_init_with_max(&lines, 1024, budget + 1024);
   grep_walk_t g = { .finder = &finder, .out = &lines, .budget = budget > 256 ? budget - 256 : 0 };
   llm_tool_view_path_put(&g.path, "$", 1);
   grep_walk(&g, root, 0);

   strbuf_t sb;
   strbuf_init_with_max(&sb, strbuf_len(&lines) + 256, budget + GREP_HEADER_ROOM);
   strbuf_appendf(&sb, "%zu match%s for ", g.matches, g.matches == 1 ? "" : "es");
   json_quoted(&sb, pattern, strlen(pattern));
   strbuf_append(&sb, g.matches ? ":\n" : ".");
   strbuf_append_n(&sb, strbuf_str(&lines), strbuf_len(&lines));
   if (g.shown < g.matches) {
      strbuf_appendf(&sb, "... %zu more; narrow the pattern, or read a path\n",
                     g.matches - g.shown);
   }
   strbuf_free(&lines);
   return finish(&sb);
}

/* The line of @p text around @p pos: [*start, *end). */
static void line_around(const char *text, size_t len, size_t pos, size_t *start, size_t *end) {
   const char *nl = pos > 0 ? memrchr(text, '\n', pos) : NULL;
   *start = nl ? (size_t)(nl - text) + 1 : 0;
   const char *nl2 = memchr(text + pos, '\n', len - pos);
   *end = nl2 ? (size_t)(nl2 - text) : len;
}

static void text_line(strbuf_t *sb,
                      const char *text,
                      size_t start,
                      size_t end,
                      size_t no,
                      char sep,
                      size_t at,
                      size_t mlen) {
   strbuf_appendf(sb, "%6zu%c ", no, sep);
   const size_t n = end - start;
   if (n <= RESULT_READ_LINE_WHOLE) {
      strbuf_append_n(sb, text + start, n);
   } else if (sep == ':') {
      window(sb, text + start, n, at, mlen);
      strbuf_appendf(sb, " (a line of %zu bytes)", n);
   } else {
      strbuf_append_n(sb, text + start, utf8_back(text + start, RESULT_READ_VALUE_MAX));
      strbuf_append(sb, "...");
   }
   strbuf_append(sb, "\n");
}

char *result_read_grep_text(const char *text,
                            size_t len,
                            const char *pattern,
                            int context,
                            size_t budget) {
   const char *bad = pattern_problem(pattern);
   if (bad) {
      return failure("%s", bad);
   }
   if (!text) {
      return NULL;
   }
   context = context < 0                         ? 0
             : context > RESULT_READ_CONTEXT_MAX ? RESULT_READ_CONTEXT_MAX
                                                 : context;
   finder_t finder;
   finder_init(&finder, pattern);

   strbuf_t lines;
   strbuf_init_with_max(&lines, 1024, budget + 1024);
   const size_t room = budget > 256 ? budget - 256 : 0;
   size_t matches = 0;
   size_t shown = 0;
   size_t line_no = 1;    /* of the line at pos */
   size_t pos = 0;        /* a line start */
   size_t printed_to = 0; /* the last line printed (0: none) */
   while (pos < len) {
      const size_t at = finder_find(&finder, text + pos, len - pos);
      if (at >= len - pos) {
         break;
      }
      /* Advance line numbers to the match's line. */
      const size_t abs = pos + at;
      size_t start = 0;
      size_t end = 0;
      line_around(text, len, abs, &start, &end);
      for (const char *p = text + pos; (p = memchr(p, '\n', (size_t)(text + start - p))) != NULL;
           p++) {
         line_no++;
      }
      matches++;
      if (shown < RESULT_READ_MATCHES_MAX && strbuf_len(&lines) < room) {
         strbuf_t block;
         strbuf_init(&block, 512);
         /* Context before, not repeating lines already printed. */
         size_t before_start[RESULT_READ_CONTEXT_MAX];
         size_t before_end[RESULT_READ_CONTEXT_MAX];
         int nb = 0;
         size_t s = start;
         while (nb < context && s > 0 && line_no - (size_t)nb - 1 > printed_to) {
            size_t ps = 0;
            size_t pe = 0;
            line_around(text, len, s - 1, &ps, &pe);
            before_start[nb] = ps;
            before_end[nb] = pe;
            nb++;
            s = ps;
         }
         if (printed_to > 0 && line_no - (size_t)nb > printed_to + 1) {
            strbuf_append(&block, "--\n");
         }
         for (int i = nb - 1; i >= 0; i--) {
            text_line(&block, text, before_start[i], before_end[i], line_no - (size_t)i - 1, '-', 0,
                      0);
         }
         text_line(&block, text, start, end, line_no, ':', abs - start, finder.len);
         size_t last = line_no;
         size_t e = end;
         for (int i = 0; i < context && e + 1 < len; i++) {
            size_t ns = 0;
            size_t ne = 0;
            line_around(text, len, e + 1, &ns, &ne);
            if (finder_find(&finder, text + ns, ne - ns) < ne - ns) {
               break; /* the next match prints it */
            }
            text_line(&block, text, ns, ne, ++last, '-', 0, 0);
            e = ne;
         }
         if (!strbuf_oom(&block) && strbuf_len(&lines) + strbuf_len(&block) <= room) {
            strbuf_append_n(&lines, strbuf_str(&block), strbuf_len(&block));
            shown++;
            printed_to = last;
         }
         strbuf_free(&block);
      }
      /* On to the next line: one match per line. */
      if (end >= len) {
         break;
      }
      pos = end + 1;
      line_no++;
   }

   strbuf_t sb;
   strbuf_init_with_max(&sb, strbuf_len(&lines) + 256, budget + GREP_HEADER_ROOM);
   strbuf_appendf(&sb, "%zu matching line%s for ", matches, matches == 1 ? "" : "s");
   json_quoted(&sb, pattern, strlen(pattern));
   strbuf_append(&sb, matches ? ":\n" : ".");
   strbuf_append_n(&sb, strbuf_str(&lines), strbuf_len(&lines));
   if (shown < matches) {
      strbuf_appendf(&sb, "... %zu more; narrow the pattern, or read a range of lines\n",
                     matches - shown);
   }
   strbuf_free(&lines);
   return finish(&sb);
}

/* ---------------------------------------------------------------------------
 * read
 * ------------------------------------------------------------------------- */

char *result_read_lines(const char *text, size_t len, long from, long to, size_t budget) {
   if (!text) {
      return NULL;
   }
   size_t total = 0;
   for (const char *p = text; (p = memchr(p, '\n', (size_t)(text + len - p))) != NULL; p++) {
      total++;
   }
   total += len > 0 && text[len - 1] != '\n';
   if (total == 0) {
      return strdup("(empty)");
   }
   size_t a = from > 0 ? (size_t)from : 1;
   size_t b = to > 0 ? (size_t)to : total;
   if (from > 0 && to > 0 && b < a) { /* both given, reversed */
      const size_t t = a;
      a = b;
      b = t;
   }
   if (a > total) {
      return failure("Line %zu is past the end: the result has %zu lines.", a, total);
   }
   if (b > total) {
      b = total;
   }
   /* Byte offsets of lines a and b + 1. */
   size_t line = 1;
   size_t start = 0;
   while (line < a) {
      const char *nl = memchr(text + start, '\n', len - start);
      start = (size_t)(nl - text) + 1;
      line++;
   }
   size_t end = start;
   for (; line <= b; line++) {
      const char *nl = memchr(text + end, '\n', len - end);
      end = nl ? (size_t)(nl - text) + 1 : len;
   }
   char header[96];
   const int h = snprintf(header, sizeof(header), "Lines %zu-%zu of %zu:\n", a, b, total);
   char *view = llm_tool_view_lines(text + start, end - start, view_budget(budget, (size_t)h), a,
                                    NULL);
   if (!view) {
      return NULL;
   }
   char *out = malloc((size_t)h + strlen(view) + 1);
   if (out) {
      memcpy(out, header, (size_t)h);
      strcpy(out + h, view);
   }
   free(view);
   return out;
}
