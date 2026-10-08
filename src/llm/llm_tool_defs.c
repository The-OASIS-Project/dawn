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
 * A conversation's tools, by value (llm_tool_defs.h).
 */

#include "llm/llm_tool_defs.h"

#include <json-c/json.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm_history_kind.h"
#include "logging.h"

/* The header mcp_schema_wrap_description puts on an MCP tool's description. */
#define MCP_DESC_HEADER "[BEGIN UNTRUSTED MCP TOOL DESCRIPTION from server '"

#define JSON_FLAGS (JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE)

static const char *str_of(struct json_object *obj, const char *key) {
   struct json_object *v = NULL;
   return json_object_is_type(obj, json_type_object) && json_object_object_get_ex(obj, key, &v) &&
                  json_object_is_type(v, json_type_string)
              ? json_object_get_string(v)
              : NULL;
}

/* Whether @p s[0, len) is well-formed UTF-8 (no overlongs, surrogates or code
 * points past U+10FFFF). */
static bool utf8_valid(const char *s, size_t len) {
   const unsigned char *p = (const unsigned char *)s;
   size_t i = 0;
   while (i < len) {
      const unsigned char c = p[i];
      size_t n = 0;
      uint32_t cp = 0;
      if (c < 0x80) {
         i++;
         continue;
      } else if (c >= 0xC2 && c <= 0xDF) {
         n = 1;
         cp = c & 0x1F;
      } else if (c >= 0xE0 && c <= 0xEF) {
         n = 2;
         cp = c & 0x0F;
      } else if (c >= 0xF0 && c <= 0xF4) {
         n = 3;
         cp = c & 0x07;
      } else {
         return false;
      }
      for (size_t k = 1; k <= n; k++) {
         if (i + k >= len || (p[i + k] & 0xC0) != 0x80) {
            return false;
         }
         cp = (cp << 6) | (p[i + k] & 0x3F);
      }
      if ((n == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) ||
          (n == 3 && (cp < 0x10000 || cp > 0x10FFFF))) {
         return false;
      }
      i += n + 1;
   }
   return true;
}

static bool name_ok(const char *name) {
   const size_t len = name ? strlen(name) : 0;
   if (len == 0 || len > LLM_TOOL_DEF_NAME_MAX) {
      return false;
   }
   for (size_t i = 0; i < len; i++) {
      const char c = name[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-')) {
         return false;
      }
   }
   return true;
}

bool llm_tool_def_valid(struct json_object *def) {
   if (!json_object_is_type(def, json_type_object) || !name_ok(str_of(def, "name"))) {
      return false;
   }
   const char *desc = str_of(def, "description");
   struct json_object *params = NULL;
   if (!desc || !json_object_object_get_ex(def, "parameters", &params) ||
       !json_object_is_type(params, json_type_object)) {
      return false;
   }
   const size_t dlen = strlen(desc);
   if (dlen > LLM_TOOL_DEF_DESC_MAX || !utf8_valid(desc, dlen)) {
      return false;
   }
   const char *p = json_object_to_json_string_ext(params, JSON_FLAGS);
   const size_t plen = p ? strlen(p) : 0;
   return p && plen <= LLM_TOOL_DEF_PARAMS_MAX && utf8_valid(p, plen);
}

const char *llm_tool_def_name(struct json_object *def) {
   if (json_object_is_type(def, json_type_string)) {
      return json_object_get_string(def);
   }
   return str_of(def, "name");
}

static int cmp_key(const void *a, const void *b) {
   return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* A copy of @p v with every object's keys in sorted order; NULL when out of
 * memory anywhere in it (a partial copy would hash as another definition). */
static struct json_object *canonical(struct json_object *v) {
   if (json_object_is_type(v, json_type_object)) {
      const int n = json_object_object_length(v);
      const char **keys = n > 0 ? calloc((size_t)n, sizeof(*keys)) : NULL;
      if (n > 0 && !keys) {
         return NULL;
      }
      int k = 0;
      json_object_object_foreach(v, key, val) {
         (void)val;
         /* keys is NULL only for an empty object, and then the loop does not run */
         // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
         keys[k++] = key;
      }
      if (k > 1) { /* keys is NULL for an empty object, and qsort's array must not be */
         qsort(keys, (size_t)k, sizeof(*keys), cmp_key);
      }
      struct json_object *out = json_object_new_object();
      for (int i = 0; out && i < k; i++) {
         struct json_object *child = NULL;
         json_object_object_get_ex(v, keys[i], &child);
         struct json_object *c = canonical(child);
         if (child && !c) {
            json_object_put(out);
            out = NULL;
            break;
         }
         json_object_object_add(out, keys[i], c);
      }
      free(keys);
      return out;
   }
   if (json_object_is_type(v, json_type_array)) {
      const size_t n = json_object_array_length(v);
      struct json_object *out = json_object_new_array();
      for (size_t i = 0; out && i < n; i++) {
         struct json_object *child = json_object_array_get_idx(v, i);
         struct json_object *c = canonical(child);
         if (child && !c) {
            json_object_put(out);
            return NULL;
         }
         json_object_array_add(out, c);
      }
      return out;
   }
   return json_object_get(v);
}

bool llm_tool_def_hash(struct json_object *def, char out[DAWN_SHA256_HEX_LEN]) {
   out[0] = '\0';
   struct json_object *c = def ? canonical(def) : NULL;
   const char *bytes = c ? json_object_to_json_string_ext(c, JSON_FLAGS) : NULL;
   if (!bytes) {
      json_object_put(c);
      return false;
   }
   dawn_sha256_hex(bytes, strlen(bytes), out);
   json_object_put(c);
   return true;
}

struct json_object *llm_tool_def_render(struct json_object *def, bool claude) {
   struct json_object *params = NULL;
   const char *name = str_of(def, "name");
   const char *desc = str_of(def, "description");
   if (!name || !desc || !json_object_object_get_ex(def, "parameters", &params)) {
      return NULL;
   }
   struct json_object *tool = json_object_new_object();
   struct json_object *function = (tool && !claude) ? json_object_new_object() : NULL;
   if (!tool || (!claude && !function)) {
      json_object_put(tool);
      return NULL;
   }
   /* The parameters by reference: nothing writes into them (a breakpoint goes
    * on the outer object), and serializing the request caches only its root. */
   if (claude) {
      json_object_object_add(tool, "name", json_object_new_string(name));
      json_object_object_add(tool, "description", json_object_new_string(desc));
      json_object_object_add(tool, "input_schema", json_object_get(params));
      return tool;
   }
   json_object_object_add(tool, "type", json_object_new_string("function"));
   json_object_object_add(function, "name", json_object_new_string(name));
   json_object_object_add(function, "description", json_object_new_string(desc));
   json_object_object_add(function, "parameters", json_object_get(params));
   json_object_object_add(tool, "function", function);
   return tool;
}

void llm_tool_def_server(struct json_object *def, char *out, size_t size) {
   if (!out || size == 0) {
      return;
   }
   out[0] = '\0';
   const char *desc = str_of(def, "description");
   const size_t hl = strlen(MCP_DESC_HEADER);
   if (!desc || strncmp(desc, MCP_DESC_HEADER, hl) != 0) {
      return;
   }
   const char *alias = desc + hl;
   const char *end = strchr(alias, '\'');
   const size_t len = end ? (size_t)(end - alias) : 0;
   snprintf(out, size, "%.*s", (int)(len < size ? len : size - 1), alias);
}

struct json_object *llm_tool_change_new(struct json_object *defs, bool rendered_inline) {
   struct json_object *body = json_object_new_object();
   struct json_object *msg = body ? json_object_new_object() : NULL;
   if (!msg || !json_object_is_type(defs, json_type_array)) {
      json_object_put(body);
      json_object_put(msg);
      json_object_put(defs);
      return NULL;
   }
   json_object_object_add(body, "rendered",
                          json_object_new_string(rendered_inline ? "inline" : "folded"));
   json_object_object_add(body, "tools", defs);
   json_object_object_add(msg, "role", json_object_new_string("system"));
   json_object_object_add(msg, "content",
                          json_object_new_string(json_object_to_json_string_ext(body, JSON_FLAGS)));
   json_object_put(body);
   llm_history_set_kind(msg, MESSAGE_KIND_TOOL_CHANGE);
   return msg;
}

/* The parsed body of a tool_change message (caller puts), or NULL. */
static struct json_object *change_body(struct json_object *msg) {
   if (llm_history_kind_of(msg) != MESSAGE_KIND_TOOL_CHANGE) {
      return NULL;
   }
   const char *text = str_of(msg, "content");
   struct json_object *body = text ? json_tokener_parse(text) : NULL;
   if (!json_object_is_type(body, json_type_object)) {
      json_object_put(body);
      return NULL;
   }
   return body;
}

/* The definitions @p body holds (borrowed), or NULL. */
static struct json_object *body_tools(struct json_object *body) {
   struct json_object *tools = NULL;
   return json_object_is_type(body, json_type_object) &&
                  json_object_object_get_ex(body, "tools", &tools) &&
                  json_object_is_type(tools, json_type_array)
              ? tools
              : NULL;
}

struct json_object *llm_tool_change_defs(struct json_object *msg) {
   /* Validated when it was made (llm_tool_change_new) or loaded
    * (llm_tool_change_normalize): read as it is. */
   struct json_object *body = change_body(msg);
   struct json_object *tools = body_tools(body);
   struct json_object *out = (tools && json_object_array_length(tools) > 0) ? json_object_get(tools)
                                                                            : NULL;
   json_object_put(body);
   return out;
}

bool llm_tool_change_normalize(struct json_object *msg) {
   struct json_object *body = change_body(msg);
   struct json_object *tools = body_tools(body);
   if (!tools) {
      json_object_put(body);
      return false;
   }
   struct json_object *kept = json_object_new_array();
   const size_t n = json_object_array_length(tools);
   for (size_t i = 0; kept && i < n; i++) {
      struct json_object *def = json_object_array_get_idx(tools, i);
      if (llm_tool_def_valid(def)) {
         json_object_array_add(kept, json_object_get(def));
      } else {
         OLOG_WARNING("tool defs: a stored tool change's definition '%.64s' is unusable; "
                      "left out",
                      llm_tool_def_name(def) ? llm_tool_def_name(def) : "?");
      }
   }
   const size_t nk = kept ? json_object_array_length(kept) : 0;
   if (kept && nk < n) {
      /* Rewritten once, here: every request reads it as stored. */
      json_object_object_add(body, "tools", kept);
      kept = NULL;
      json_object_object_add(
          msg, "content", json_object_new_string(json_object_to_json_string_ext(body, JSON_FLAGS)));
   }
   json_object_put(kept);
   json_object_put(body);
   return nk > 0;
}

/* The prefix a tool_change message's text starts with, by how it was stored
 * (llm_tool_change_new writes "rendered" first). */
#define STORED_INLINE_HEAD "{\"rendered\":\"inline\""
#define STORED_FOLDED_HEAD "{\"rendered\":\"folded\""

bool llm_tool_change_stored_inline(struct json_object *msg) {
   if (llm_history_kind_of(msg) != MESSAGE_KIND_TOOL_CHANGE) {
      return false;
   }
   const char *text = str_of(msg, "content");
   if (text && strncmp(text, STORED_INLINE_HEAD, sizeof(STORED_INLINE_HEAD) - 1) == 0) {
      return true;
   }
   if (!text || strncmp(text, STORED_FOLDED_HEAD, sizeof(STORED_FOLDED_HEAD) - 1) == 0) {
      return false;
   }
   struct json_object *body = change_body(msg); /* written some other way */
   const char *r = str_of(body, "rendered");
   const bool inl = r && strcmp(r, "inline") == 0;
   json_object_put(body);
   return inl;
}

bool llm_tool_defs_inline_rejected(struct json_object *history) {
   struct json_object *rec = llm_history_in_force(llm_history_prefix(history), false);
   struct json_object *flag = NULL;
   return json_object_object_get_ex(rec, LLM_TOOL_DEFS_REJECTED_KEY, &flag) &&
          json_object_get_boolean(flag);
}

static bool role_is(struct json_object *msg, const char *role) {
   const char *r = str_of(msg, "role");
   return r && strcmp(r, role) == 0;
}

bool llm_tool_change_renders_inline(struct json_object *history, size_t idx, bool inline_ok) {
   if (!inline_ok || !json_object_is_type(history, json_type_array) ||
       idx >= json_object_array_length(history)) {
      return false;
   }
   struct json_object *msg = json_object_array_get_idx(history, idx);
   if (!llm_tool_change_stored_inline(msg) || llm_tool_defs_inline_rejected(history)) {
      return false;
   }
   /* After a user turn (a question, or tool results: a user turn on the wire). */
   struct json_object *prev = NULL;
   for (size_t i = idx; i-- > 0 && !prev;) {
      struct json_object *m = json_object_array_get_idx(history, i);
      if (!role_is(m, "system")) {
         prev = m;
      }
   }
   if (!prev || !(role_is(prev, "user") || role_is(prev, "tool"))) {
      return false;
   }
   /* Followed by an assistant turn, or nothing. */
   const size_t n = json_object_array_length(history);
   for (size_t i = idx + 1; i < n; i++) {
      struct json_object *m = json_object_array_get_idx(history, i);
      if (!role_is(m, "system")) {
         return role_is(m, "assistant");
      }
   }
   return true;
}

/* Index of the definition named @p name in @p defs, or -1. */
static int find_def(struct json_object *defs, const char *name) {
   const size_t n = json_object_array_length(defs);
   for (size_t i = 0; name && i < n; i++) {
      const char *have = llm_tool_def_name(json_object_array_get_idx(defs, i));
      if (have && strcmp(have, name) == 0) {
         return (int)i;
      }
   }
   return -1;
}

void llm_tool_defs_merge(struct json_object *into, struct json_object *defs) {
   if (!json_object_is_type(into, json_type_array) || !json_object_is_type(defs, json_type_array)) {
      return;
   }
   const size_t nd = json_object_array_length(defs);
   for (size_t k = 0; k < nd; k++) {
      struct json_object *def = json_object_array_get_idx(defs, k);
      const int at = find_def(into, llm_tool_def_name(def));
      if (at >= 0) {
         json_object_array_put_idx(into, (size_t)at, json_object_get(def));
      } else {
         json_object_array_add(into, json_object_get(def));
      }
   }
}

struct json_object *llm_tool_defs_for_request(struct json_object *history, bool inline_ok) {
   struct json_object *frozen = llm_history_frozen_tools(history);
   if (!frozen) {
      return NULL;
   }
   /* Validated when frozen or loaded (llm_tool_defs_usable): read as it is. */
   struct json_object *out = json_object_new_array();
   const size_t nf = json_object_array_length(frozen);
   for (size_t i = 0; out && i < nf; i++) {
      json_object_array_add(out, json_object_get(json_object_array_get_idx(frozen, i)));
   }
   const size_t n = json_object_array_length(history);
   for (size_t i = 1; out && i < n; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      if (llm_history_kind_of(msg) != MESSAGE_KIND_TOOL_CHANGE ||
          llm_tool_change_renders_inline(history, i, inline_ok)) {
         continue;
      }
      struct json_object *defs = llm_tool_change_defs(msg);
      llm_tool_defs_merge(out, defs);
      json_object_put(defs);
   }
   return out;
}

struct json_object *llm_tool_defs_usable(struct json_object *defs) {
   if (!json_object_is_type(defs, json_type_array)) {
      return NULL;
   }
   struct json_object *out = json_object_new_array();
   const size_t n = json_object_array_length(defs);
   for (size_t i = 0; out && i < n; i++) {
      struct json_object *def = json_object_array_get_idx(defs, i);
      if (json_object_is_type(def, json_type_string) || llm_tool_def_valid(def)) {
         json_object_array_add(out, json_object_get(def));
      } else {
         OLOG_WARNING("tool defs: a frozen definition '%.64s' is unusable; left out",
                      llm_tool_def_name(def) ? llm_tool_def_name(def) : "?");
      }
   }
   return out;
}

struct json_object *llm_tool_defs_hashes(struct json_object *defs, char fp[DAWN_SHA256_HEX_LEN]) {
   if (fp) {
      fp[0] = '\0';
   }
   struct json_object *out = json_object_new_object();
   const size_t n = json_object_is_type(defs, json_type_array) ? json_object_array_length(defs) : 0;
   for (size_t i = 0; out && i < n; i++) {
      struct json_object *def = json_object_array_get_idx(defs, i);
      const char *name = llm_tool_def_name(def);
      char h[DAWN_SHA256_HEX_LEN];
      if (!name || json_object_is_type(def, json_type_string) || !llm_tool_def_hash(def, h)) {
         json_object_put(out);
         return NULL; /* no fingerprint of a set some of which can't be told */
      }
      struct json_object *v = json_object_new_string(h);
      if (!v) {
         json_object_put(out);
         return NULL;
      }
      json_object_object_add(out, name, v);
   }
   if (out && fp) {
      const char *bytes = json_object_to_json_string_ext(out, JSON_FLAGS);
      if (!bytes) {
         json_object_put(out);
         return NULL;
      }
      dawn_sha256_hex(bytes, strlen(bytes), fp);
   }
   return out;
}
