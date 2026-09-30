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
 * An MCP tools/call result as a model reads it (mcp_result.h).
 */

#include "tools/mcp_result.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compact, and "/" as it is: an escaped slash costs a character and reads badly. */
#define MCP_RESULT_JSON_FLAGS (JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE)

typedef struct {
   char *buf;
   size_t len;
   size_t cap;
   bool failed;
} text_buf_t;

static void put(text_buf_t *t, const char *s, size_t n) {
   if (t->failed || n == 0) {
      return;
   }
   if (t->len + n + 1 > t->cap) {
      size_t cap = t->cap ? t->cap : 256;
      while (cap < t->len + n + 1) {
         cap *= 2;
      }
      char *grown = realloc(t->buf, cap);
      if (!grown) {
         t->failed = true;
         return;
      }
      t->buf = grown;
      t->cap = cap;
   }
   memcpy(t->buf + t->len, s, n);
   t->len += n;
   t->buf[t->len] = '\0';
}

static const char *str_of(struct json_object *obj, const char *key) {
   struct json_object *v = NULL;
   return json_object_object_get_ex(obj, key, &v) && json_object_is_type(v, json_type_string)
              ? json_object_get_string(v)
              : NULL;
}

/* The code point of the UTF-8 sequence at @p s (at most @p avail bytes) and
 * its length; U+FFFD and 1 for a byte that starts no valid sequence. */
static size_t utf8_next(const unsigned char *s, size_t avail, unsigned *cp) {
   const unsigned char c = s[0];
   size_t n = c < 0x80 ? 1 : c > 0xF4 ? 0 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 ? 2 : 0;
   if (n == 0 || n > avail) {
      *cp = 0xFFFD;
      return 1;
   }
   unsigned v = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
   for (size_t i = 1; i < n; i++) {
      if ((s[i] & 0xC0) != 0x80) {
         *cp = 0xFFFD;
         return 1;
      }
      v = (v << 6) | (s[i] & 0x3F);
   }
   static const unsigned k_min[] = { 0, 0, 0x80, 0x800, 0x10000 };
   if (v < k_min[n] || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {
      *cp = 0xFFFD;
      return n;
   }
   *cp = v;
   return n;
}

static void put_cp(text_buf_t *t, unsigned cp) {
   char out[4];
   size_t n;
   if (cp < 0x80) {
      out[0] = (char)cp;
      n = 1;
   } else if (cp < 0x800) {
      out[0] = (char)(0xC0 | (cp >> 6));
      out[1] = (char)(0x80 | (cp & 0x3F));
      n = 2;
   } else if (cp < 0x10000) {
      out[0] = (char)(0xE0 | (cp >> 12));
      out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[2] = (char)(0x80 | (cp & 0x3F));
      n = 3;
   } else {
      out[0] = (char)(0xF0 | (cp >> 18));
      out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
      out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[3] = (char)(0x80 | (cp & 0x3F));
      n = 4;
   }
   put(t, out, n);
}

/* A character nobody sees (zero-width, a direction override): it could hide
 * what a reference says, so it is left out. */
static bool invisible(unsigned cp) {
   return (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
          (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0x2066 && cp <= 0x2069) || cp == 0xFEFF ||
          cp == 0x00AD || cp == 0x180E;
}

/* A character that breaks or controls a line: C0 and C1 controls, DEL, and
 * the Unicode line and paragraph separators. */
static bool line_control(unsigned cp) {
   return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0x2028 || cp == 0x2029;
}

/* @p s as one inert line, at most @p max bytes of it (cut on a character
 * boundary): what breaks a line becomes a space, what can't be seen is left
 * out, brackets that could close the reference early become parentheses, and
 * bytes that aren't UTF-8 become U+FFFD.  A reference is shown, never
 * followed, so it must not open lines or frames of its own. */
static void put_inert(text_buf_t *t, const char *s, size_t max) {
   if (!s) {
      return;
   }
   const size_t len = strlen(s);
   const size_t n = len > max ? max : len;
   const unsigned char *u = (const unsigned char *)s;
   for (size_t i = 0; i < n;) {
      unsigned cp;
      const size_t k = utf8_next(u + i, len - i, &cp);
      if (i + k > n && k > 1) {
         break; /* a character the cut would split */
      }
      i += k;
      if (invisible(cp)) {
         continue;
      }
      if (line_control(cp)) {
         cp = ' ';
      } else if (cp == '[' || cp == '<') {
         cp = '(';
      } else if (cp == ']' || cp == '>') {
         cp = ')';
      }
      put_cp(t, cp);
   }
   if (n < len) {
      put(t, "...", 3);
   }
}

/* @p len bytes of @p text as a text part's payload: C0 controls other than
 * tab, line feed and carriage return, NUL included, are left out (a
 * terminal's escapes, a byte DAWN reads as its own marker like the tool-error
 * mark, or a NUL that would hide the rest). */
static void put_text(text_buf_t *t, const char *text, size_t len) {
   const char *run = text;
   for (const char *p = text; p < text + len; p++) {
      const unsigned char c = (unsigned char)*p;
      if ((c < 0x20 && c != '\t' && c != '\n' && c != '\r') || c == 0x7F) {
         put(t, run, (size_t)(p - run));
         run = p + 1;
      }
   }
   put(t, run, (size_t)(text + len - run));
}

/* A string field's bytes and length (all of them: a NUL inside is kept for
 * put_text to drop, not taken as the end). */
static const char *str_len_of(struct json_object *obj, const char *key, size_t *len) {
   struct json_object *v = NULL;
   if (!json_object_object_get_ex(obj, key, &v) || !json_object_is_type(v, json_type_string)) {
      return NULL;
   }
   *len = (size_t)json_object_get_string_len(v);
   return json_object_get_string(v);
}

/* A part the model can't read as text, as one line: what it is and where. */
static void put_reference(text_buf_t *t,
                          const char *what,
                          const char *name,
                          const char *uri,
                          const char *mime,
                          size_t bytes) {
   char head[64];
   snprintf(head, sizeof(head), "[%s", what);
   put(t, head, strlen(head));
   if (name) {
      put(t, ": ", 2);
      put_inert(t, name, MCP_RESULT_REF_MAX / 3);
   }
   if (uri) {
      put(t, " <", 2);
      put_inert(t, uri, MCP_RESULT_REF_MAX);
      put(t, ">", 1);
   }
   if (mime) {
      put(t, " ", 1);
      put_inert(t, mime, 64);
   }
   if (bytes) {
      char size[40];
      snprintf(size, sizeof(size), " (%zu bytes)", bytes);
      put(t, size, strlen(size));
   }
   put(t, "]", 1);
}

/* The bytes base64 text @p b64 decodes to (its length, not its content). */
static size_t base64_bytes(const char *b64) {
   if (!b64) {
      return 0;
   }
   size_t n = strlen(b64);
   while (n > 0 && b64[n - 1] == '=') {
      n--;
   }
   return n * 3 / 4; /* each 4 characters (padding aside) carry 3 bytes */
}

/* @p obj's JSON (compact, "/" as it is). */
static void put_json(text_buf_t *t, struct json_object *obj) {
   const char *json = json_object_to_json_string_ext(obj, MCP_RESULT_JSON_FLAGS);
   if (!json) {
      t->failed = true;
      return;
   }
   put(t, json, strlen(json));
}

static void put_part(text_buf_t *t, struct json_object *part) {
   const char *type = str_of(part, "type");
   if (!type) {
      put_json(t, part); /* not a part this knows: all of it, as sent */
      return;
   }
   if (strcmp(type, "text") == 0) {
      size_t len = 0;
      const char *text = str_len_of(part, "text", &len);
      if (text) {
         put_text(t, text, len);
      }
   } else if (strcmp(type, "resource_link") == 0) {
      put_reference(t, "resource link", str_of(part, "name"), str_of(part, "uri"),
                    str_of(part, "mimeType"), 0);
   } else if (strcmp(type, "resource") == 0) {
      struct json_object *res = NULL;
      json_object_object_get_ex(part, "resource", &res);
      size_t len = 0;
      const char *text = str_len_of(res, "text", &len);
      if (text) {
         put_text(t, text, len);
      } else {
         put_reference(t, "resource", NULL, str_of(res, "uri"), str_of(res, "mimeType"),
                       base64_bytes(str_of(res, "blob")));
      }
   } else if (strcmp(type, "image") == 0 || strcmp(type, "audio") == 0) {
      put_reference(t, type, NULL, NULL, str_of(part, "mimeType"),
                    base64_bytes(str_of(part, "data")));
   } else {
      put_json(t, part); /* a kind this doesn't know: all of it, as sent */
   }
}

/* A text buffer's result: the text, or NULL when out of memory. */
static char *finish(text_buf_t *t) {
   if (t->failed) {
      free(t->buf);
      return NULL;
   }
   return t->buf ? t->buf : strdup("");
}

char *mcp_result_text(struct json_object *result, bool *is_error) {
   if (is_error) {
      *is_error = false;
   }
   /* Only a boolean says the tool failed ("false" as a string doesn't). */
   struct json_object *flag = NULL;
   if (is_error && json_object_object_get_ex(result, "isError", &flag) &&
       json_object_is_type(flag, json_type_boolean)) {
      *is_error = json_object_get_boolean(flag);
   }

   text_buf_t t = { 0 };
   struct json_object *structured = NULL;
   struct json_object *content = NULL;
   if (json_object_object_get_ex(result, "structuredContent", &structured) &&
       !json_object_is_type(structured, json_type_null)) {
      put_json(&t, structured);
      return finish(&t);
   }
   if (!json_object_object_get_ex(result, "content", &content) ||
       !json_object_is_type(content, json_type_array) || json_object_array_length(content) == 0) {
      if (result) {
         put_json(&t, result); /* a shape this doesn't know: all of it, as sent */
      }
      return finish(&t);
   }
   const size_t n = json_object_array_length(content);
   for (size_t i = 0; i < n; i++) {
      /* A separator only between parts that say something. */
      const size_t before = t.len;
      if (before > 0) {
         put(&t, "\n", 1);
      }
      const size_t after_sep = t.len;
      put_part(&t, json_object_array_get_idx(content, i));
      if (t.len == after_sep && !t.failed) {
         t.len = before;
         if (t.buf) {
            t.buf[t.len] = '\0';
         }
      }
   }
   return finish(&t);
}
