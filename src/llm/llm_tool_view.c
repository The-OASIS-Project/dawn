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
 * A view of a tool result too big to send whole (llm_tool_view.h).
 */

#define _GNU_SOURCE /* memrchr */
#include "llm/llm_tool_view.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define VIEW_JSON_FLAGS (JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE)

/* A path longer than this is shown cut (it only names where an omission is). */
#define VIEW_PATH_MAX 512

/* What the text view keeps for its omission marker, beyond head and tail. */
#define VIEW_TEXT_MARKER_ROOM 96

/* Containers nested deeper than this are always shown as markers: it bounds
 * the render's recursion for a tree not made by json-c's parser (whose own
 * depth limit is lower). */
#define VIEW_DEPTH_HARD 64

/* An items' shape: "{" + 8 keys of up to LLM_TOOL_VIEW_KEY_MAX / 2 bytes with
 * their types + ", ...}" fits, so it is never cut (a cut would still end on a
 * character boundary). */
#define VIEW_SHAPE_MAX 384

/* A marker's text: its path plus a count, a range and an items' shape. */
#define VIEW_MARKER_MAX (VIEW_PATH_MAX + VIEW_SHAPE_MAX + 128)

/* What a render cut, one bit per shape dimension (the loosening step grows
 * only a dimension that cut something). */
enum {
   CUT_ITEMS = 1u << 0,
   CUT_STRING = 1u << 1,
   CUT_KEYS = 1u << 2,
   CUT_DEPTH = 1u << 3,
   CUT_KEYNAME = 1u << 4, /* a key shown cut (not a shape dimension) */
};

/* ---------------------------------------------------------------------------
 * A buffer capped at the budget: a write past it stops the render.
 * ------------------------------------------------------------------------- */

typedef struct {
   char *buf;
   size_t len;
   size_t cap;
   bool over;
   unsigned cuts; /* CUT_* made so far */
} wbuf_t;

static void w(wbuf_t *b, const char *s, size_t n) {
   if (b->over) {
      return;
   }
   if (b->len + n > b->cap) {
      b->over = true;
      return;
   }
   memcpy(b->buf + b->len, s, n);
   b->len += n;
}

static void ws(wbuf_t *b, const char *s) {
   w(b, s, strlen(s));
}

/* @p n bytes of text, a NUL byte shown as a space (a view is a C string). */
static void w_text(wbuf_t *b, const char *s, size_t n) {
   const size_t from = b->len;
   w(b, s, n);
   if (b->over) {
      return;
   }
   for (char *p = b->buf + from; (p = memchr(p, '\0', (size_t)(b->buf + b->len - p))) != NULL;) {
      *p = ' ';
   }
}

/* @p n bytes of @p s as the inside of a JSON string (no quotes). */
static void w_escaped(wbuf_t *b, const char *s, size_t n) {
   const char *run = s;
   for (const char *p = s; p < s + n && !b->over; p++) {
      const unsigned char c = (unsigned char)*p;
      if (c != '"' && c != '\\' && c >= 0x20) {
         continue;
      }
      w(b, run, (size_t)(p - run));
      char esc[8];
      switch (c) {
         case '"':
            ws(b, "\\\"");
            break;
         case '\\':
            ws(b, "\\\\");
            break;
         case '\n':
            ws(b, "\\n");
            break;
         case '\r':
            ws(b, "\\r");
            break;
         case '\t':
            ws(b, "\\t");
            break;
         default:
            snprintf(esc, sizeof(esc), "\\u%04x", c);
            ws(b, esc);
      }
      run = p + 1;
   }
   w(b, run, (size_t)(s + n - run));
}

/* @p text as a JSON string. */
static void w_quoted(wbuf_t *b, const char *text) {
   ws(b, "\"");
   w_escaped(b, text, strlen(text));
   ws(b, "\"");
}

/* The bytes @p text takes as the inside of a JSON string (w_escaped). */
static size_t escaped_len(const char *text) {
   size_t n = 0;
   for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
      n += *p == '"' || *p == '\\' || *p == '\n' || *p == '\r' || *p == '\t' ? 2
           : *p < 0x20                                                       ? 6
                                                                             : 1;
   }
   return n;
}

/* The length of @p s's first @p max bytes, cut back to a character boundary. */
static size_t utf8_cut(const char *s, size_t len, size_t max) {
   if (len <= max) {
      return len;
   }
   size_t n = max;
   while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) {
      n--;
   }
   return n;
}

/* Where @p s's last @p max bytes start, moved forward to a character boundary. */
static size_t utf8_tail_start(const char *s, size_t len, size_t max) {
   if (len <= max) {
      return 0;
   }
   size_t start = len - max;
   while (start < len && ((unsigned char)s[start] & 0xC0) == 0x80) {
      start++;
   }
   return start;
}

/* Characters (not bytes) in @p n bytes of UTF-8: the bytes less the
 * continuation bytes (10xxxxxx), counted eight at a time. */
static size_t utf8_chars(const char *s, size_t n) {
   size_t cont = 0;
   size_t i = 0;
   for (; i + 8 <= n; i += 8) {
      uint64_t x;
      memcpy(&x, s + i, sizeof(x));
      /* Per byte: bit 7 set and bit 6 (shifted up to bit 7) clear. */
      cont += (size_t)__builtin_popcountll(x & ~(x << 1) & 0x8080808080808080ULL);
   }
   for (; i < n; i++) {
      cont += ((unsigned char)s[i] & 0xC0) == 0x80;
   }
   return n - cont;
}

/* ---------------------------------------------------------------------------
 * Keys: one longer than LLM_TOOL_VIEW_KEY_MAX is shown by its head, its length
 * and a hash of it, so two cut keys never show alike.
 * ------------------------------------------------------------------------- */

#define VIEW_KEY_SHOWN_MAX (LLM_TOOL_VIEW_KEY_MAX + 48)

/* @p key (@p kl bytes) as shown: NULL when shown whole, else its cut form in
 * @p out. */
static const char *key_cut(const char *key, size_t kl, char out[VIEW_KEY_SHOWN_MAX]) {
   if (kl <= LLM_TOOL_VIEW_KEY_MAX) {
      return NULL;
   }
   uint32_t h = 2166136261u; /* FNV-1a */
   for (size_t i = 0; i < kl; i++) {
      h = (h ^ (unsigned char)key[i]) * 16777619u;
   }
   const size_t head = utf8_cut(key, kl, LLM_TOOL_VIEW_KEY_MAX);
   memcpy(out, key, head);
   snprintf(out + head, VIEW_KEY_SHOWN_MAX - head, "...(%zu chars, #%08" PRIx32 ")",
            utf8_chars(key, kl), h);
   return out;
}

/* ---------------------------------------------------------------------------
 * JSON paths: "$.name" for a name that is an identifier, "$[\"...\"]" for any
 * other key (escaped, as shown), "[i]" for an item.  A segment is added whole
 * or not at all, so a cut path stays valid UTF-8.
 * ------------------------------------------------------------------------- */

typedef struct {
   char s[VIEW_PATH_MAX + 4];
   size_t len;
   bool cut;
} path_t;

static bool identifier(const char *k, size_t n) {
   if (n == 0 || !(isalpha((unsigned char)k[0]) || k[0] == '_')) {
      return false;
   }
   for (size_t i = 1; i < n; i++) {
      if (!(isalnum((unsigned char)k[i]) || k[i] == '_')) {
         return false;
      }
   }
   return true;
}

static void path_put(path_t *p, const char *s, size_t n) {
   if (p->cut) {
      return;
   }
   if (p->len + n > VIEW_PATH_MAX) {
      memcpy(p->s + p->len, "...", 3);
      p->len += 3;
      p->s[p->len] = '\0';
      p->cut = true;
      return;
   }
   memcpy(p->s + p->len, s, n);
   p->len += n;
   p->s[p->len] = '\0';
}

/* @p p with key @p key appended, as shown (@p shown: its cut form, or NULL);
 * the caller restores len and cut after. */
static void path_key(path_t *p, const char *key, size_t kl, const char *shown) {
   char seg[VIEW_KEY_SHOWN_MAX * 2 + 8];
   if (!shown && identifier(key, kl)) {
      seg[0] = '.';
      memcpy(seg + 1, key, kl);
      path_put(p, seg, kl + 1);
      return;
   }
   const char *k = shown ? shown : key;
   const size_t n = shown ? strlen(shown) : kl;
   size_t len = 0;
   seg[len++] = '[';
   seg[len++] = '"';
   for (size_t i = 0; i < n; i++) {
      const unsigned char c = (unsigned char)k[i];
      if (c == '"' || c == '\\') {
         seg[len++] = '\\';
         seg[len++] = (char)c;
      } else if (c < 0x20 || c == 0x7F) {
         seg[len++] = ' '; /* a path is one line */
      } else {
         seg[len++] = (char)c;
      }
   }
   seg[len++] = '"';
   seg[len++] = ']';
   path_put(p, seg, len);
}

static void path_index(path_t *p, size_t i) {
   char seg[32];
   const int n = snprintf(seg, sizeof(seg), "[%zu]", i);
   path_put(p, seg, (size_t)n);
}

/* ---------------------------------------------------------------------------
 * The JSON view
 * ------------------------------------------------------------------------- */

typedef struct {
   size_t items;  /* an array's first items shown (then its last) */
   size_t string; /* a string's bytes shown */
   size_t keys;   /* an object's keys shown */
   int depth;     /* containers shown this deep */
} shape_t;

/* A shape that cuts nothing (the "fits whole?" render of a tree). */
static const shape_t k_whole = { SIZE_MAX / 4, SIZE_MAX / 4, SIZE_MAX / 4, INT_MAX };

static const char *type_name(struct json_object *v) {
   switch (json_object_get_type(v)) {
      case json_type_null:
         return "null";
      case json_type_boolean:
         return "bool";
      case json_type_int:
         return "int";
      case json_type_double:
         return "num";
      case json_type_string:
         return "str";
      case json_type_array:
         return "arr";
      case json_type_object:
         return "obj";
   }
   return "?";
}

/* Adds to *@p sum the bytes @p v renders in whole, stopping once it passes
 * @p limit (so the walk costs at most about @p limit nodes): whether it did.
 * A number counts one byte, so it is a lower bound. */
static bool exceeds(struct json_object *v, size_t limit, size_t *sum) {
   switch (json_object_get_type(v)) {
      case json_type_null:
      case json_type_boolean:
         *sum += 4;
         break;
      case json_type_string:
         *sum += (size_t)json_object_get_string_len(v) + 2;
         break;
      case json_type_array: {
         *sum += 2;
         const size_t n = json_object_array_length(v);
         for (size_t i = 0; i < n && *sum <= limit; i++) {
            *sum += i > 0; /* , */
            exceeds(json_object_array_get_idx(v, i), limit, sum);
         }
         break;
      }
      case json_type_object: {
         *sum += 2;
         bool first = true;
         json_object_object_foreach(v, key, val) {
            if (*sum > limit) {
               break;
            }
            *sum += strlen(key) + (first ? 3 : 4); /* ,"key": */
            first = false;
            exceeds(val, limit, sum);
         }
         break;
      }
      default:
         *sum += 1;
         break;
   }
   return *sum > limit;
}

/* Whether items [@p from, @p to) of @p arr, with their commas, render in more
 * than @p limit bytes. */
static bool items_exceed(struct json_object *arr, size_t from, size_t to, size_t limit) {
   size_t sum = 0;
   for (size_t i = from; i < to && sum <= limit; i++) {
      sum += 1;
      exceeds(json_object_array_get_idx(arr, i), limit, &sum);
   }
   return sum > limit;
}

/* The shape of the items of @p arr in [@p from, @p to): sampled evenly, "{a:int,
 * b:str, ...}" when they are objects, else their types ("str", "int|null").
 * Written to @p out (@p cap bytes). */
static void items_shape(struct json_object *arr, size_t from, size_t to, char *out, size_t cap) {
   enum {
      SHAPE_KEYS = 8,
      SHAPE_TYPES = 7
   };
   const char *keys[SHAPE_KEYS];
   const char *key_types[SHAPE_KEYS];
   size_t n_keys = 0;
   bool more_keys = false;
   const char *types[SHAPE_TYPES];
   size_t n_types = 0;
   bool objects = true;
   const size_t count = to - from;
   const size_t samples = count < LLM_TOOL_VIEW_SHAPE_SAMPLES ? count : LLM_TOOL_VIEW_SHAPE_SAMPLES;
   for (size_t j = 0; j < samples; j++) {
      struct json_object *item = json_object_array_get_idx(arr, from + j * count / samples);
      const char *t = type_name(item);
      bool seen = false;
      for (size_t k = 0; k < n_types && !seen; k++) {
         seen = strcmp(types[k], t) == 0;
      }
      if (!seen && n_types < SHAPE_TYPES) {
         types[n_types++] = t;
      }
      if (!json_object_is_type(item, json_type_object)) {
         objects = false;
         continue;
      }
      if (n_keys == SHAPE_KEYS && more_keys) {
         continue; /* nothing more to learn about the keys */
      }
      json_object_object_foreach(item, key, val) {
         bool known = false;
         for (size_t k = 0; k < n_keys && !known; k++) {
            known = strcmp(keys[k], key) == 0;
         }
         if (known) {
            continue;
         }
         if (n_keys == SHAPE_KEYS) {
            more_keys = true;
            break;
         }
         keys[n_keys] = key;
         key_types[n_keys++] = type_name(val);
      }
   }
   size_t off = 0;
   out[0] = '\0';
#define SHAPE_PUT(...)                                                       \
   do {                                                                      \
      if (off < cap) {                                                       \
         const int m = snprintf(out + off, cap - off, __VA_ARGS__);          \
         off = m < 0 ? cap : (size_t)m >= cap - off ? cap : off + (size_t)m; \
      }                                                                      \
   } while (0)
   if (objects && n_types == 1) {
      SHAPE_PUT("{");
      for (size_t k = 0; k < n_keys; k++) {
         char shown[LLM_TOOL_VIEW_KEY_MAX + 4];
         const size_t kl = utf8_cut(keys[k], strlen(keys[k]), LLM_TOOL_VIEW_KEY_MAX / 2);
         memcpy(shown, keys[k], kl);
         shown[kl] = '\0';
         SHAPE_PUT("%s%s:%s", k ? ", " : "", shown, key_types[k]);
      }
      SHAPE_PUT("%s}", more_keys ? ", ..." : "");
   } else {
      for (size_t k = 0; k < n_types; k++) {
         SHAPE_PUT("%s%s", k ? "|" : "", types[k]);
      }
   }
#undef SHAPE_PUT
   if (off >= cap) {
      out[utf8_cut(out, cap - 1, cap - 1 - 1)] = '\0';
   }
}

/* A container at or past the depth limit, as a marker when that saves bytes
 * (always past VIEW_DEPTH_HARD): whether it was. */
static bool depth_marker(wbuf_t *b,
                         struct json_object *v,
                         size_t n,
                         const char *what,
                         const shape_t *s,
                         int depth,
                         const path_t *path) {
   if (n == 0 || (depth < s->depth && depth < VIEW_DEPTH_HARD)) {
      return false;
   }
   char text[VIEW_MARKER_MAX];
   snprintf(text, sizeof(text), what, n, path->s);
   size_t sum = 0;
   if (depth < VIEW_DEPTH_HARD && !exceeds(v, escaped_len(text) + 2, &sum)) {
      return false;
   }
   w_quoted(b, text);
   b->cuts |= CUT_DEPTH;
   return true;
}

static void render(wbuf_t *b, struct json_object *v, const shape_t *s, int depth, path_t *path);

static void render_string(wbuf_t *b, struct json_object *v, const shape_t *s, path_t *path) {
   const char *str = json_object_get_string(v);
   const size_t len = (size_t)json_object_get_string_len(v);
   ws(b, "\"");
   if (len > s->string + LLM_TOOL_VIEW_CUT_MIN) {
      const size_t head = utf8_cut(str, len, s->string);
      char text[VIEW_MARKER_MAX];
      snprintf(text, sizeof(text), "...(%zu chars at %s)", utf8_chars(str, len), path->s);
      if (len - head > escaped_len(text)) {
         w_escaped(b, str, head);
         w_escaped(b, text, strlen(text));
         ws(b, "\"");
         b->cuts |= CUT_STRING;
         return;
      }
   }
   w_escaped(b, str, len);
   ws(b, "\"");
}

static void render_array(wbuf_t *b,
                         struct json_object *v,
                         const shape_t *s,
                         int depth,
                         path_t *path) {
   const size_t n = json_object_array_length(v);
   if (depth_marker(b, v, n, "...[%zu items at %s]", s, depth, path)) {
      return;
   }
   const size_t saved = path->len;
   const bool saved_cut = path->cut;
   /* The items between the first shown and the last, elided when they are
    * enough of them and outweigh the marker that stands for them. */
   size_t shown = n;
   char text[VIEW_MARKER_MAX];
   if (n >= s->items + 1 + LLM_TOOL_VIEW_ELIDE_MIN) {
      char shape[VIEW_SHAPE_MAX];
      items_shape(v, s->items, n - 1, shape, sizeof(shape));
      snprintf(text, sizeof(text), "... %zu more items at %s[%zu:%zu], each %s", n - 1 - s->items,
               path->s, s->items, n - 1, shape);
      if (items_exceed(v, s->items, n - 1, escaped_len(text) + 3)) {
         shown = s->items;
      }
   }
   ws(b, "[");
   for (size_t i = 0; i < shown && !b->over; i++) {
      if (i) {
         ws(b, ",");
      }
      path_index(path, i);
      render(b, json_object_array_get_idx(v, i), s, depth + 1, path);
      path->len = saved;
      path->cut = saved_cut;
      path->s[saved] = '\0';
   }
   if (shown < n && !b->over) {
      ws(b, ",");
      w_quoted(b, text);
      b->cuts |= CUT_ITEMS;
      ws(b, ",");
      path_index(path, n - 1);
      render(b, json_object_array_get_idx(v, n - 1), s, depth + 1, path);
      path->len = saved;
      path->cut = saved_cut;
      path->s[saved] = '\0';
   }
   ws(b, "]");
}

/* The key the "more keys" marker goes under: "...", or "...#2", "...#3" and so
 * on when the object has a key of that name. */
static void more_key(struct json_object *v, char out[32]) {
   snprintf(out, 32, "...");
   for (unsigned i = 2; json_object_object_get_ex(v, out, NULL); i++) {
      snprintf(out, 32, "...#%u", i);
   }
}

/* Whether @p v's keys after its first @p from, with their values, render in
 * more than @p limit bytes. */
static bool keys_exceed(struct json_object *v, size_t from, size_t limit) {
   size_t i = 0;
   size_t sum = 0;
   json_object_object_foreach(v, key, val) {
      if (sum > limit) {
         break;
      }
      if (i++ < from) {
         continue;
      }
      sum += strlen(key) + 4; /* ,"key": */
      exceeds(val, limit, &sum);
   }
   return sum > limit;
}

static void render_object(wbuf_t *b,
                          struct json_object *v,
                          const shape_t *s,
                          int depth,
                          path_t *path) {
   const size_t n = (size_t)json_object_object_length(v);
   if (depth_marker(b, v, n, "...{%zu keys at %s}", s, depth, path)) {
      return;
   }
   const size_t saved = path->len;
   const bool saved_cut = path->cut;
   /* Keys past the first shown are elided when they are enough of them and
    * outweigh the marker that stands for them. */
   size_t keys = n;
   char mkey[32];
   char text[VIEW_MARKER_MAX];
   if (n >= s->keys + LLM_TOOL_VIEW_ELIDE_MIN) {
      more_key(v, mkey);
      snprintf(text, sizeof(text), "%zu more keys at %s", n - s->keys, path->s);
      if (keys_exceed(v, s->keys, strlen(mkey) + escaped_len(text) + 6)) {
         keys = s->keys;
      }
   }
   size_t i = 0;
   ws(b, "{");
   json_object_object_foreach(v, key, val) {
      if (b->over || i == keys) {
         break;
      }
      if (i) {
         ws(b, ",");
      }
      const size_t kl = strlen(key);
      char cut[VIEW_KEY_SHOWN_MAX];
      const char *shown = key_cut(key, kl, cut);
      ws(b, "\"");
      w_escaped(b, shown ? shown : key, shown ? strlen(shown) : kl);
      if (shown) {
         b->cuts |= CUT_KEYNAME;
      }
      ws(b, "\":");
      path_key(path, key, kl, shown);
      render(b, val, s, depth + 1, path);
      path->len = saved;
      path->cut = saved_cut;
      path->s[saved] = '\0';
      i++;
   }
   if (keys < n && !b->over) {
      ws(b, ",");
      w_quoted(b, mkey);
      ws(b, ":");
      w_quoted(b, text);
      b->cuts |= CUT_KEYS;
   }
   ws(b, "}");
}

/* A scalar: a number as its tree holds it (a double parsed by json-c keeps
 * its text), a NaN or infinity as null (not JSON). */
static void render_scalar(wbuf_t *b, struct json_object *v) {
   char num[32];
   switch (json_object_get_type(v)) {
      case json_type_null:
         ws(b, "null");
         return;
      case json_type_boolean:
         ws(b, json_object_get_boolean(v) ? "true" : "false");
         return;
      case json_type_int: {
         const int64_t i = json_object_get_int64(v);
         if (i == INT64_MAX) { /* or an unsigned value above it */
            snprintf(num, sizeof(num), "%" PRIu64, json_object_get_uint64(v));
         } else {
            snprintf(num, sizeof(num), "%" PRId64, i);
         }
         ws(b, num);
         return;
      }
      case json_type_double:
         if (!isfinite(json_object_get_double(v))) {
            ws(b, "null");
            return;
         }
         ws(b, json_object_to_json_string_ext(v, VIEW_JSON_FLAGS));
         return;
      default:
         ws(b, "null");
         return;
   }
}

static void render(wbuf_t *b, struct json_object *v, const shape_t *s, int depth, path_t *path) {
   if (b->over) {
      return;
   }
   switch (json_object_get_type(v)) {
      case json_type_string:
         render_string(b, v, s, path);
         return;
      case json_type_array:
         render_array(b, v, s, depth, path);
         return;
      case json_type_object:
         render_object(b, v, s, depth, path);
         return;
      default:
         render_scalar(b, v);
         return;
   }
}

/* The next rung of the tightening ladder, or false at the barest shape. */
static bool tighten(shape_t *s) {
   if (s->string > LLM_TOOL_VIEW_STRING_MIN) {
      s->string = s->string / 2 > LLM_TOOL_VIEW_STRING_MIN ? s->string / 2
                                                           : LLM_TOOL_VIEW_STRING_MIN;
   } else if (s->items > LLM_TOOL_VIEW_ITEMS_MIN) {
      s->items /= 2;
   } else if (s->keys > LLM_TOOL_VIEW_KEYS_MIN) {
      s->keys = s->keys / 2 > LLM_TOOL_VIEW_KEYS_MIN ? s->keys / 2 : LLM_TOOL_VIEW_KEYS_MIN;
   } else if (s->depth > LLM_TOOL_VIEW_DEPTH_MIN) {
      s->depth = s->depth / 2 > LLM_TOOL_VIEW_DEPTH_MIN ? s->depth / 2 : LLM_TOOL_VIEW_DEPTH_MIN;
   } else {
      return false;
   }
   return true;
}

static size_t doubled(size_t v, size_t max) {
   return v >= max / 2 ? max : v * 2;
}

/* @p s with dimension @p cut (a CUT_* bit) grown a step, or false when it is
 * at its most. */
static bool loosen(shape_t *s, unsigned cut) {
   switch (cut) {
      case CUT_ITEMS:
         if (s->items >= LLM_TOOL_VIEW_ITEMS_MAX) {
            return false;
         }
         s->items = doubled(s->items, LLM_TOOL_VIEW_ITEMS_MAX);
         return true;
      case CUT_STRING:
         if (s->string >= LLM_TOOL_VIEW_STRING_MAX) {
            return false;
         }
         s->string = doubled(s->string, LLM_TOOL_VIEW_STRING_MAX);
         return true;
      case CUT_KEYS:
         if (s->keys >= LLM_TOOL_VIEW_KEYS_MAX) {
            return false;
         }
         s->keys = doubled(s->keys, LLM_TOOL_VIEW_KEYS_MAX);
         return true;
      default:
         if (s->depth >= LLM_TOOL_VIEW_DEPTH_MAX) {
            return false;
         }
         s->depth = (int)doubled((size_t)s->depth, LLM_TOOL_VIEW_DEPTH_MAX);
         return true;
   }
}

/* Render @p root with @p s into @p b (reset first): whether it fit. */
static bool render_into(wbuf_t *b, struct json_object *root, const shape_t *s, const char *prefix) {
   b->len = 0;
   b->over = false;
   b->cuts = 0;
   path_t path = { .len = 0 };
   path_put(&path, prefix, strlen(prefix));
   render(b, root, s, 0, &path);
   if (!b->over) {
      b->buf[b->len] = '\0';
   }
   return !b->over;
}

/* The JSON view of @p root in @p budget bytes (its length in *@p len_out), or
 * NULL (out of memory, or no shape fits: *@p fits false). */
static char *json_view(struct json_object *root,
                       size_t budget,
                       const char *prefix,
                       bool *fits,
                       size_t *len_out) {
   *fits = false;
   wbuf_t b = { .buf = malloc(budget + 1), .cap = budget };
   if (!b.buf) {
      return NULL;
   }
   shape_t s = { LLM_TOOL_VIEW_ITEMS, LLM_TOOL_VIEW_STRING, LLM_TOOL_VIEW_KEYS,
                 LLM_TOOL_VIEW_DEPTH };
   /* Tighten until it fits (or no shape does). */
   bool fit = false;
   do {
      fit = render_into(&b, root, &s, prefix);
   } while (!fit && tighten(&s));
   if (!fit) {
      free(b.buf);
      return NULL;
   }
   /* A roomy budget: while more than half of it is unused, grow one
    * dimension that cut something at a time, keeping each step that still
    * fits; a dimension whose step overflows (or that cuts nothing) is done. */
   char *best = malloc(b.len + 1);
   if (!best) {
      free(b.buf);
      return NULL;
   }
   memcpy(best, b.buf, b.len + 1);
   size_t best_len = b.len;
   unsigned best_cuts = b.cuts;
   unsigned open = CUT_ITEMS | CUT_STRING | CUT_KEYS | CUT_DEPTH;
   bool grew = true;
   while (grew && best_len < budget / 2) {
      grew = false;
      for (unsigned cut = CUT_ITEMS; cut <= CUT_DEPTH && best_len < budget / 2; cut <<= 1) {
         shape_t t = s;
         if (!(open & cut) || !(best_cuts & cut)) {
            continue; /* a later step may make it cut something */
         }
         if (!loosen(&t, cut) || !render_into(&b, root, &t, prefix)) {
            open &= ~cut;
            continue;
         }
         char *grown = realloc(best, b.len + 1);
         if (!grown) {
            open = 0;
            break;
         }
         best = grown;
         memcpy(best, b.buf, b.len + 1);
         best_len = b.len;
         best_cuts = b.cuts;
         s = t;
         grew = true;
      }
   }
   free(b.buf);
   *fits = true;
   *len_out = best_len;
   return best;
}

/* ---------------------------------------------------------------------------
 * The text view
 * ------------------------------------------------------------------------- */

static size_t count_lines(const char *text, size_t len) {
   size_t lines = 0;
   for (const char *p = text; (p = memchr(p, '\n', (size_t)(text + len - p))) != NULL; p++) {
      lines++;
   }
   return lines + (len > 0 && text[len - 1] != '\n');
}

static void w_line_no(wbuf_t *b, size_t no) {
   char num[32];
   const int n = snprintf(num, sizeof(num), "%6zu\t", no);
   w(b, num, (size_t)n);
}

/* One line (@p text is the whole text; the line is [start, end)) too long for
 * @p room bytes: numbered, its head and tail with what's between counted. */
static void w_cut_line(wbuf_t *b,
                       const char *text,
                       size_t start,
                       size_t end,
                       size_t no,
                       size_t room) {
   const char *line = text + start;
   const size_t len = end - start;
   const size_t marker = 48;
   const size_t keep = room > marker + 16 ? room - marker - 8 : 16;
   const size_t head = utf8_cut(line, len, keep * 2 / 3);
   const size_t tail_from = utf8_tail_start(line, len, keep - keep * 2 / 3);
   w_line_no(b, no);
   w_text(b, line, head);
   char mark[64];
   snprintf(mark, sizeof(mark), " ... %zu chars omitted ... ",
            utf8_chars(line + head, tail_from > head ? tail_from - head : 0));
   ws(b, mark);
   if (tail_from > head) {
      w_text(b, line + tail_from, len - tail_from);
   }
   ws(b, "\n");
}

/* The text view: numbered head and tail lines in @p budget bytes (it always
 * fits a budget of LLM_TOOL_VIEW_MIN_BUDGET or more); its length in
 * *@p len_out. */
static char *text_view(const char *text,
                       size_t len,
                       size_t budget,
                       size_t *lines_out,
                       size_t *len_out) {
   const size_t lines = count_lines(text, len);
   *lines_out = lines;
   wbuf_t b = { .buf = malloc(budget + 1), .cap = budget };
   if (!b.buf) {
      return NULL;
   }
   for (size_t room = budget; room >= LLM_TOOL_VIEW_MIN_BUDGET / 2; room = room * 9 / 10) {
      b.len = 0;
      b.over = false;
      const size_t usable = room > VIEW_TEXT_MARKER_ROOM ? room - VIEW_TEXT_MARKER_ROOM : 0;
      const size_t head_room = usable * 2 / 3;

      if (lines <= 1) {
         size_t end = len;
         if (end > 0 && text[end - 1] == '\n') {
            end--;
         }
         w_cut_line(&b, text, 0, end, 1, usable);
      } else {
         /* Head: whole lines while they fit (a first line too long is cut). */
         size_t pos = 0;
         size_t head_lines = 0;
         size_t used = 0;
         while (pos < len) {
            const char *nl = memchr(text + pos, '\n', len - pos);
            const size_t end = nl ? (size_t)(nl - text) : len;
            const size_t cost = 7 + (end - pos) + 1;
            if (used + cost > head_room) {
               if (head_lines == 0) {
                  w_cut_line(&b, text, pos, end, 1, head_room);
                  pos = nl ? end + 1 : len;
                  head_lines = 1;
               }
               break;
            }
            w_line_no(&b, head_lines + 1);
            w_text(&b, text + pos, end - pos);
            ws(&b, "\n");
            used += cost;
            head_lines++;
            pos = nl ? end + 1 : len;
         }
         const size_t head_end = pos;
         /* The tail gets whatever the head left of the usable room. */
         const size_t tail_room = usable > b.len ? usable - b.len : 0;

         /* Tail: whole lines from the end while they fit (a last line too long
          * is cut), never back into the head. */
         size_t tail_start = len;
         size_t tail_lines = 0;
         size_t tail_used = 0;
         bool tail_cut = false;
         size_t scan_end = len > 0 && text[len - 1] == '\n' ? len - 1 : len;
         while (scan_end > head_end) {
            const char *nl = memrchr(text + head_end, '\n', scan_end - head_end);
            const size_t start = nl ? (size_t)(nl - text) + 1 : head_end;
            const size_t cost = 7 + (scan_end - start) + 1;
            if (tail_used + cost > tail_room) {
               if (tail_lines == 0) {
                  tail_cut = true;
                  tail_start = start;
               }
               break;
            }
            tail_used += cost;
            tail_lines++;
            tail_start = start;
            if (start == 0) {
               break;
            }
            scan_end = start - 1;
         }

         const size_t first_tail_no = lines - tail_lines + (tail_cut ? 0 : 1);
         const size_t omitted_lines = first_tail_no > head_lines + 1
                                          ? first_tail_no - head_lines - 1
                                          : 0;
         const size_t omitted_chars = tail_start > head_end
                                          ? utf8_chars(text + head_end, tail_start - head_end)
                                          : 0;
         char mark[128];
         if (omitted_lines > 0) {
            snprintf(mark, sizeof(mark), "... %zu lines (%zu chars) omitted: lines %zu-%zu ...\n",
                     omitted_lines, omitted_chars, head_lines + 1, first_tail_no - 1);
            ws(&b, mark);
         }
         if (tail_cut) {
            size_t end = len > 0 && text[len - 1] == '\n' ? len - 1 : len;
            w_cut_line(&b, text, tail_start, end, lines, tail_room);
         } else {
            size_t no = first_tail_no;
            size_t p = tail_start;
            while (p < len) {
               const char *nl = memchr(text + p, '\n', len - p);
               const size_t end = nl ? (size_t)(nl - text) : len;
               w_line_no(&b, no++);
               w_text(&b, text + p, end - p);
               ws(&b, "\n");
               p = nl ? end + 1 : len;
            }
         }
      }
      if (!b.over) {
         b.buf[b.len] = '\0';
         *len_out = b.len;
         return b.buf;
      }
   }
   /* Unreachable for budgets of LLM_TOOL_VIEW_MIN_BUDGET or more. */
   b.buf[0] = '\0';
   *len_out = 0;
   return b.buf;
}

/* ---------------------------------------------------------------------------
 * Entry points
 * ------------------------------------------------------------------------- */

static bool looks_json(const char *text, size_t len) {
   size_t i = 0;
   while (i < len && isspace((unsigned char)text[i])) {
      i++;
   }
   return i < len && (text[i] == '{' || text[i] == '[');
}

static size_t digits_at(const char *t, size_t len, size_t j) {
   size_t k = j;
   while (k < len && isdigit((unsigned char)t[k])) {
      k++;
   }
   return k - j;
}

/* The number at @p t[@p i] (to *@p end): whether json-c holds it as sent.  An
 * integer must fit 64 bits (json-c clamps one past them); any other number
 * must be JSON's grammar ("1." is not) and within a double's range (json-c
 * keeps a double's text, but one that overflows is infinity).  "-" with no
 * digit sets *@p end to @p i. */
static bool number_faithful(const char *t, size_t len, size_t i, size_t *end) {
   const size_t d0 = i + (t[i] == '-');
   size_t j = d0 + digits_at(t, len, d0);
   *end = i;
   if (j == d0) {
      return true;
   }
   const size_t int_digits = j - d0;
   bool integer = true;
   if (j < len && t[j] == '.') {
      integer = false;
      const size_t n = digits_at(t, len, j + 1);
      if (n == 0) {
         return false;
      }
      j += 1 + n;
   }
   if (j < len && (t[j] == 'e' || t[j] == 'E')) {
      integer = false;
      j++;
      if (j < len && (t[j] == '+' || t[j] == '-')) {
         j++;
      }
      const size_t n = digits_at(t, len, j);
      if (n == 0) {
         return false;
      }
      j += n;
   }
   *end = j;
   if (integer && int_digits < 19) {
      return true;
   }
   if (integer && int_digits > 20) {
      return false;
   }
   char small[64];
   char *num = j - i < sizeof(small) ? small : malloc(j - i + 1);
   if (!num) {
      return false;
   }
   memcpy(num, t + i, j - i);
   num[j - i] = '\0';
   errno = 0;
   if (!integer) {
      const double d = strtod(num, NULL);
      if (errno == ERANGE && isinf(d)) {
         errno = ERANGE;
      } else {
         errno = 0; /* an underflow: json-c keeps the text */
      }
   } else if (t[i] == '-') {
      (void)strtoll(num, NULL, 10);
   } else {
      (void)strtoull(num, NULL, 10);
   }
   const bool ok = errno != ERANGE;
   if (num != small) {
      free(num);
   }
   return ok;
}

/* Whether json-c's tree of @p text holds it faithfully: every integer within
 * 64 bits (json-c clamps one past them without an error) and no NaN or
 * Infinity (not JSON).  A number or word inside a string doesn't count;
 * where the text isn't strict JSON this may say no, never a wrong yes. */
static bool faithful(const char *t, size_t len) {
   for (size_t i = 0; i < len; i++) {
      const char c = t[i];
      if (c == '"') {
         /* To the quote that ends the string: one after an even run of
          * backslashes. */
         const size_t open = i;
         for (size_t from = i + 1;;) {
            const char *q = memchr(t + from, '"', len - from);
            if (!q) {
               return false; /* unterminated: not reached for parsed text */
            }
            size_t k = (size_t)(q - t);
            size_t slashes = 0;
            while (k - slashes > open + 1 && t[k - 1 - slashes] == '\\') {
               slashes++;
            }
            if (slashes % 2 == 0) {
               i = k;
               break;
            }
            from = k + 1;
         }
         continue;
      }
      if (c == '-' || isdigit((unsigned char)c)) {
         size_t end = i;
         if (!number_faithful(t, len, i, &end)) {
            return false;
         }
         if (end > i) {
            i = end - 1;
         }
         continue; /* "-" then a word ("-Infinity"): the word is looked at next */
      }
      if (isalpha((unsigned char)c)) {
         size_t j = i;
         while (j < len && isalpha((unsigned char)t[j])) {
            j++;
         }
         const size_t n = j - i;
         if (!((n == 4 && (!strncasecmp(t + i, "true", 4) || !strncasecmp(t + i, "null", 4))) ||
               (n == 5 && !strncasecmp(t + i, "false", 5)))) {
            return false; /* NaN, Infinity */
         }
         i = j - 1;
      }
   }
   return true;
}

/* @p text parsed as one JSON value and nothing after it, held faithfully, or
 * NULL. */
static struct json_object *parse_whole(const char *text, size_t len) {
   if (len > LLM_TOOL_VIEW_JSON_MAX_BYTES || !looks_json(text, len)) {
      return NULL;
   }
   struct json_tokener *tok = json_tokener_new();
   if (!tok) {
      return NULL;
   }
   /* Strict: no single quotes, comments or other extensions faithful()
    * wouldn't read the same way. */
   json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
   struct json_object *root = json_tokener_parse_ex(tok, text, (int)len);
   size_t off = (size_t)json_tokener_get_parse_end(tok);
   json_tokener_free(tok);
   while (root && off < len && isspace((unsigned char)text[off])) {
      off++;
   }
   if (root && (off != len || !faithful(text, len))) {
      json_object_put(root);
      return NULL;
   }
   return root;
}

static void info_set(llm_tool_view_info_t *info,
                     llm_tool_view_mode_t mode,
                     bool shortened,
                     size_t bytes,
                     size_t lines,
                     size_t view_bytes) {
   if (info) {
      info->mode = mode;
      info->shortened = shortened;
      info->bytes = bytes;
      info->lines = lines;
      info->view_bytes = view_bytes;
   }
}

/* The text view of @p text, filling @p info. */
static char *text_view_info(const char *text,
                            size_t len,
                            size_t budget,
                            size_t bytes,
                            llm_tool_view_info_t *info) {
   size_t lines = 0;
   size_t view_len = 0;
   char *view = text_view(text, len, budget, &lines, &view_len);
   if (view) {
      info_set(info, LLM_TOOL_VIEW_TEXT, true, bytes, lines, view_len);
   }
   return view;
}

char *llm_tool_view(const char *text,
                    size_t len,
                    size_t budget,
                    const char *path_prefix,
                    llm_tool_view_info_t *info) {
   if (!text) {
      return NULL;
   }
   if (budget < LLM_TOOL_VIEW_MIN_BUDGET) {
      budget = LLM_TOOL_VIEW_MIN_BUDGET;
   }
   const char *prefix = path_prefix && *path_prefix ? path_prefix : "$";
   if (len <= budget) {
      char *whole = malloc(len + 1);
      if (!whole) {
         return NULL;
      }
      memcpy(whole, text, len);
      whole[len] = '\0';
      for (char *p = whole; (p = memchr(p, '\0', (size_t)(whole + len - p))) != NULL;) {
         *p = ' ';
      }
      info_set(info, looks_json(text, len) ? LLM_TOOL_VIEW_JSON : LLM_TOOL_VIEW_TEXT, false, len,
               count_lines(text, len), len);
      return whole;
   }
   struct json_object *root = parse_whole(text, len);
   if (root) {
      bool fits = false;
      size_t view_len = 0;
      char *view = json_view(root, budget, prefix, &fits, &view_len);
      json_object_put(root);
      if (view) {
         info_set(info, LLM_TOOL_VIEW_JSON, true, len, count_lines(text, len), view_len);
         return view;
      }
      if (fits) {
         return NULL; /* out of memory */
      }
   }
   return text_view_info(text, len, budget, len, info);
}

char *llm_tool_view_tree(struct json_object *root,
                         size_t budget,
                         const char *path_prefix,
                         llm_tool_view_info_t *info) {
   if (budget < LLM_TOOL_VIEW_MIN_BUDGET) {
      budget = LLM_TOOL_VIEW_MIN_BUDGET;
   }
   const char *prefix = path_prefix && *path_prefix ? path_prefix : "$";
   /* Whole, when it renders uncut in the budget: rendered straight into a
    * buffer capped there, so a big tree is never serialized to find out. */
   wbuf_t b = { .buf = malloc(budget + 1), .cap = budget };
   if (!b.buf) {
      return NULL;
   }
   if (render_into(&b, root, &k_whole, prefix) && b.cuts == 0) {
      info_set(info, LLM_TOOL_VIEW_JSON, false, b.len, 0, b.len);
      return b.buf;
   }
   free(b.buf);
   bool fits = false;
   size_t view_len = 0;
   char *view = json_view(root, budget, prefix, &fits, &view_len);
   if (view) {
      info_set(info, LLM_TOOL_VIEW_JSON, true, 0, 0, view_len);
      return view;
   }
   if (fits) {
      return NULL; /* out of memory */
   }
   /* Even the skeleton won't fit: the text view of its JSON. */
   const char *json = json_object_to_json_string_ext(root, VIEW_JSON_FLAGS);
   return json ? text_view_info(json, strlen(json), budget, 0, info) : NULL;
}
