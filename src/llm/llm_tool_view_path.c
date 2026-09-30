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
 * JSON paths as views write them and readers take them back (llm_tool_view_path.h).
 */

#include "llm/llm_tool_view_path.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "llm/llm_tool_view.h"

/* ---------------------------------------------------------------------------
 * Writing
 * ------------------------------------------------------------------------- */

void llm_tool_view_path_start(llm_tool_view_path_t *p, const char *prefix) {
   p->len = 0;
   p->cut = false;
   p->s[0] = '\0';
   llm_tool_view_path_put(p, prefix ? prefix : "$", prefix ? strlen(prefix) : 1);
}

void llm_tool_view_path_put(llm_tool_view_path_t *p, const char *s, size_t n) {
   if (p->cut) {
      return;
   }
   if (p->len + n > LLM_TOOL_VIEW_PATH_MAX) {
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

bool llm_tool_view_path_identifier(const char *k, size_t n) {
   if (n == 0 || n > LLM_TOOL_VIEW_KEY_MAX || !(isalpha((unsigned char)k[0]) || k[0] == '_')) {
      return false;
   }
   for (size_t i = 1; i < n; i++) {
      if (!(isalnum((unsigned char)k[i]) || k[i] == '_')) {
         return false;
      }
   }
   return true;
}

void llm_tool_view_path_key(llm_tool_view_path_t *p,
                            const char *key,
                            size_t kl,
                            const char *shown) {
   if (!shown && llm_tool_view_path_identifier(key, kl)) {
      char seg[LLM_TOOL_VIEW_KEY_MAX + 2];
      seg[0] = '.';
      memcpy(seg + 1, key, kl);
      llm_tool_view_path_put(p, seg, kl + 1);
      return;
   }
   const char *k = shown ? shown : key;
   const size_t n = shown ? strlen(shown) : kl;
   if (n > LLM_TOOL_VIEW_PATH_MAX) {
      llm_tool_view_path_put(p, k, n); /* past the path's length: cut there */
      return;
   }
   /* A key's characters as a JSON string's: a path stays one line, and a
    * reader decodes it back to the key. */
   char seg[LLM_TOOL_VIEW_PATH_MAX * 6 + 8];
   size_t len = 0;
   seg[len++] = '[';
   seg[len++] = '"';
   for (size_t i = 0; i < n; i++) {
      const unsigned char c = (unsigned char)k[i];
      if (c == '"' || c == '\\') {
         seg[len++] = '\\';
         seg[len++] = (char)c;
      } else if (c == '\n' || c == '\t' || c == '\r') {
         seg[len++] = '\\';
         seg[len++] = c == '\n' ? 'n' : c == '\t' ? 't' : 'r';
      } else if (c < 0x20 || c == 0x7F) {
         len += (size_t)snprintf(seg + len, sizeof(seg) - len, "\\u%04x", c);
      } else {
         seg[len++] = (char)c;
      }
   }
   seg[len++] = '"';
   seg[len++] = ']';
   llm_tool_view_path_put(p, seg, len);
}

void llm_tool_view_path_index(llm_tool_view_path_t *p, size_t i) {
   char seg[32];
   const int n = snprintf(seg, sizeof(seg), "[%zu]", i);
   llm_tool_view_path_put(p, seg, (size_t)n);
}

/* ---------------------------------------------------------------------------
 * Reading
 * ------------------------------------------------------------------------- */

static const char *parse_int(const char *p, long long *out, bool *has) {
   *has = false;
   bool neg = false;
   if (*p == '-') {
      neg = true;
      p++;
   }
   long long v = 0;
   int digits = 0;
   while (isdigit((unsigned char)*p)) {
      if (++digits > 18) {
         return NULL;
      }
      v = v * 10 + (*p - '0');
      p++;
   }
   if (digits == 0) {
      return neg ? NULL : p;
   }
   *out = neg ? -v : v;
   *has = true;
   return p;
}

static int hex4(const char *p) {
   int v = 0;
   for (int i = 0; i < 4; i++) {
      const char c = p[i];
      v <<= 4;
      if (c >= '0' && c <= '9') {
         v |= c - '0';
      } else if (c >= 'a' && c <= 'f') {
         v |= c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
         v |= c - 'A' + 10;
      } else {
         return -1;
      }
   }
   return v;
}

static void put_utf8(char *out, size_t *len, unsigned cp) {
   if (cp < 0x80) {
      out[(*len)++] = (char)cp;
   } else if (cp < 0x800) {
      out[(*len)++] = (char)(0xC0 | (cp >> 6));
      out[(*len)++] = (char)(0x80 | (cp & 0x3F));
   } else if (cp < 0x10000) {
      out[(*len)++] = (char)(0xE0 | (cp >> 12));
      out[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[(*len)++] = (char)(0x80 | (cp & 0x3F));
   } else {
      out[(*len)++] = (char)(0xF0 | (cp >> 18));
      out[(*len)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
      out[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[(*len)++] = (char)(0x80 | (cp & 0x3F));
   }
}

/* A quoted key at @p p (after its '"'), decoded into @p e's key space: past
 * its closing quote, or NULL. */
static const char *parse_quoted(const char *p, llm_tool_view_path_expr_t *e, const char **key_out) {
   char *out = e->keys + e->keys_len;
   size_t len = 0;
   const size_t room = sizeof(e->keys) - e->keys_len - 1;
   while (*p && *p != '"') {
      if (len + 4 > room) {
         return NULL;
      }
      if (*p != '\\') {
         out[len++] = *p++;
         continue;
      }
      p++;
      switch (*p) {
         case '"':
         case '\\':
         case '/':
            out[len++] = *p++;
            break;
         case 'n':
            out[len++] = '\n';
            p++;
            break;
         case 't':
            out[len++] = '\t';
            p++;
            break;
         case 'r':
            out[len++] = '\r';
            p++;
            break;
         case 'b':
            out[len++] = '\b';
            p++;
            break;
         case 'f':
            out[len++] = '\f';
            p++;
            break;
         case 'u': {
            int cp = hex4(p + 1);
            if (cp <= 0) {
               return NULL; /* bad, or a NUL a key can't hold */
            }
            p += 5;
            if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
               const int lo = hex4(p + 2);
               if (lo >= 0xDC00 && lo <= 0xDFFF) {
                  cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                  p += 6;
               }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) {
               return NULL;
            }
            put_utf8(out, &len, (unsigned)cp);
            break;
         }
         default:
            return NULL;
      }
   }
   if (*p != '"') {
      return NULL;
   }
   out[len] = '\0';
   *key_out = out;
   e->keys_len += len + 1;
   return p + 1;
}

const char *llm_tool_view_path_parse(const char *path, llm_tool_view_path_expr_t *e) {
   memset(e, 0, sizeof(*e));
   if (!path) {
      path = "$";
   }
   if (strlen(path) > LLM_TOOL_VIEW_PATH_MAX) {
      return "the path is too long";
   }
   const char *p = path;
   while (isspace((unsigned char)*p)) {
      p++;
   }
   if (*p == '$') {
      p++;
   }
   while (*p && !isspace((unsigned char)*p)) {
      if (e->n == LLM_TOOL_VIEW_PATH_SEGMENTS) {
         return "the path has too many parts";
      }
      llm_tool_view_path_seg_t *seg = &e->seg[e->n];
      if (*p == '.') {
         p++;
         if (*p == '*') {
            return "wildcards aren't supported: use distinct for a field's values, or grep";
         }
         const char *start = p;
         while (isalnum((unsigned char)*p) || *p == '_') {
            p++;
         }
         const size_t n = (size_t)(p - start);
         if (n == 0 || e->keys_len + n + 1 > sizeof(e->keys)) {
            return "a name after '.' must be letters, digits or _ (write [\"key\"] for any "
                   "other key)";
         }
         memcpy(e->keys + e->keys_len, start, n);
         e->keys[e->keys_len + n] = '\0';
         seg->kind = LLM_TOOL_VIEW_PATH_KEY;
         seg->key = e->keys + e->keys_len;
         e->keys_len += n + 1;
      } else if (*p == '[') {
         p++;
         while (*p == ' ') {
            p++;
         }
         if (*p == '"') {
            p = parse_quoted(p + 1, e, &seg->key);
            if (!p) {
               return "a quoted key isn't a valid JSON string";
            }
            seg->kind = LLM_TOOL_VIEW_PATH_KEY;
            seg->quoted = true;
         } else if (*p == '*') {
            return "wildcards aren't supported: use distinct for a field's values, or grep";
         } else {
            p = parse_int(p, &seg->a, &seg->has_a);
            if (!p) {
               return "an index must be a whole number";
            }
            while (*p == ' ') {
               p++;
            }
            if (*p == ':') {
               p = parse_int(p + 1, &seg->b, &seg->has_b);
               if (!p) {
                  return "a slice's ends must be whole numbers";
               }
               seg->kind = LLM_TOOL_VIEW_PATH_SLICE;
            } else if (!seg->has_a) {
               return "[] needs an index, a slice (i:j) or a quoted key";
            } else {
               seg->kind = LLM_TOOL_VIEW_PATH_INDEX;
            }
         }
         while (*p == ' ') {
            p++;
         }
         if (*p != ']') {
            return "a '[' isn't closed with ']'";
         }
         p++;
      } else {
         return "a path is $ then .name, [\"key\"], [i] or [i:j] parts";
      }
      e->n++;
   }
   while (isspace((unsigned char)*p)) {
      p++;
   }
   if (*p) {
      return "a path is one expression: nothing may follow it";
   }
   for (int i = 0; i + 1 < e->n; i++) {
      if (e->seg[i].kind == LLM_TOOL_VIEW_PATH_SLICE) {
         return "a slice [i:j] can only be the last part of a path";
      }
   }
   return NULL;
}
