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
 * MCP JSON Schema -> DAWN treg_param_t translator with hardening caps.
 */

#include "tools/mcp_bridge_schema.h"

#include <json-c/json.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "dawn_error.h"
#include "logging.h"
#include "tools/tool_registry.h"

/* Property cap is the registry's documented per-tool parameter limit (12), NOT
 * the design's original 32: TOOL_PARAM_MAX is the binding contract here. */
#define MCP_SCHEMA_MAX_PROPERTIES TOOL_PARAM_MAX
#define MCP_SCHEMA_MAX_ENUM TOOL_PARAM_ENUM_MAX
#define MCP_SCHEMA_MAX_ONEOF_DEPTH 2
#define MCP_SCHEMA_REF_SCAN_BUDGET 8

/* --------------------------------------------------------------------------
 * Sanitization
 * -------------------------------------------------------------------------- */

/*
 * Copy @p src into @p dst, dropping ASCII control chars (except \n/\t and
 * keeping no DEL) and the common zero-width / bidi UTF-8 sequences:
 *   U+200B..U+200F, U+202A..U+202E  (E2 80 8B..8F, E2 80 AA..AE)
 *   U+2066..U+2069                  (E2 81 A6..A9)
 *   U+FEFF                          (EF BB BF)
 * Writes at most max_copy bytes (plus NUL) and at most dst_cap-1. Returns the
 * written length. Short-circuit reads keep multi-byte checks in-bounds because
 * src is NUL-terminated.
 */
static size_t sanitize_into(char *dst, size_t dst_cap, const char *src, size_t max_copy) {
   size_t di = 0;
   const unsigned char *p = (const unsigned char *)src;
   while (*p != '\0' && di < max_copy && di < dst_cap - 1) {
      unsigned char ch = *p;
      if (ch == 0xE2 && p[1] == 0x80 && p[2] >= 0x8B && p[2] <= 0x8F) {
         p += 3;
         continue;
      }
      if (ch == 0xE2 && p[1] == 0x80 && p[2] >= 0xAA && p[2] <= 0xAE) {
         p += 3;
         continue;
      }
      if (ch == 0xE2 && p[1] == 0x81 && p[2] >= 0xA6 && p[2] <= 0xA9) {
         p += 3;
         continue;
      }
      if (ch == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
         p += 3;
         continue;
      }
      if ((ch < 0x20 && ch != '\n' && ch != '\t') || ch == 0x7F) {
         p += 1;
         continue;
      }
      dst[di++] = (char)ch;
      p += 1;
   }
   dst[di] = '\0';
   return di;
}

char *mcp_schema_wrap_description(const char *server_alias, const char *raw_description) {
   if (server_alias == NULL) {
      server_alias = "unknown";
   }
   if (raw_description == NULL) {
      raw_description = "";
   }

   char begin[128];
   snprintf(begin, sizeof(begin), "[BEGIN UNTRUSTED MCP TOOL DESCRIPTION from server '%s']\n",
            server_alias);
   const char *end = "\n[END UNTRUSTED MCP TOOL DESCRIPTION]";

   char *out = malloc(TOOL_DESC_MAX);
   if (out == NULL) {
      return NULL;
   }
   size_t bl = strlen(begin);
   size_t el = strlen(end);
   if (bl + el + 1 >= TOOL_DESC_MAX) {
      /* Pathological alias length; markers alone fill the budget. */
      snprintf(out, TOOL_DESC_MAX, "%s", begin);
      return out;
   }

   memcpy(out, begin, bl);
   size_t room = TOOL_DESC_MAX - bl - el - 1;
   size_t cl = sanitize_into(out + bl, room + 1, raw_description, room);
   memcpy(out + bl + cl, end, el);
   out[bl + cl + el] = '\0';
   return out;
}

/* --------------------------------------------------------------------------
 * Schema inspection helpers
 * -------------------------------------------------------------------------- */

/* True if a "$ref" appears anywhere within @p obj up to the recursion budget. */
static bool schema_contains_ref(struct json_object *obj, int budget) {
   if (obj == NULL || budget < 0) {
      return false;
   }
   if (json_object_is_type(obj, json_type_object)) {
      struct json_object *ref = NULL;
      if (json_object_object_get_ex(obj, "$ref", &ref)) {
         return true;
      }
      json_object_object_foreach(obj, k, v) {
         (void)k;
         if (schema_contains_ref(v, budget - 1)) {
            return true;
         }
      }
   } else if (json_object_is_type(obj, json_type_array)) {
      int n = json_object_array_length(obj);
      for (int i = 0; i < n; i++) {
         if (schema_contains_ref(json_object_array_get_idx(obj, i), budget - 1)) {
            return true;
         }
      }
   }
   return false;
}

/* Max nesting depth of oneOf/anyOf keywords within @p obj (0 = none present). */
static int oneof_depth(struct json_object *obj, int budget) {
   if (obj == NULL || budget <= 0) {
      return 0;
   }
   int best = 0;
   if (json_object_is_type(obj, json_type_object)) {
      struct json_object *sub = NULL;
      bool here = json_object_object_get_ex(obj, "oneOf", &sub) ||
                  json_object_object_get_ex(obj, "anyOf", &sub);
      json_object_object_foreach(obj, k, v) {
         (void)k;
         int d = oneof_depth(v, budget - 1);
         if (d > best) {
            best = d;
         }
      }
      return (here ? 1 : 0) + best;
   }
   if (json_object_is_type(obj, json_type_array)) {
      int n = json_object_array_length(obj);
      for (int i = 0; i < n; i++) {
         int d = oneof_depth(json_object_array_get_idx(obj, i), budget - 1);
         if (d > best) {
            best = d;
         }
      }
   }
   return best;
}

/* Resolve a property's JSON Schema "type" string, picking the first non-null
 * entry if "type" is an array (e.g. ["string","null"]). NULL if absent. */
static const char *schema_type_string(struct json_object *val) {
   struct json_object *t = NULL;
   if (!json_object_object_get_ex(val, "type", &t)) {
      return NULL;
   }
   if (json_object_is_type(t, json_type_string)) {
      return json_object_get_string(t);
   }
   if (json_object_is_type(t, json_type_array)) {
      int n = json_object_array_length(t);
      for (int i = 0; i < n; i++) {
         const char *s = json_object_get_string(json_object_array_get_idx(t, i));
         if (s != NULL && strcmp(s, "null") != 0) {
            return s;
         }
      }
   }
   return NULL;
}

static bool name_in_required(struct json_object *required, const char *key) {
   if (required == NULL || !json_object_is_type(required, json_type_array)) {
      return false;
   }
   int n = json_object_array_length(required);
   for (int i = 0; i < n; i++) {
      const char *s = json_object_get_string(json_object_array_get_idx(required, i));
      if (s != NULL && strcmp(s, key) == 0) {
         return true;
      }
   }
   return false;
}

/* --------------------------------------------------------------------------
 * Parameter construction
 * -------------------------------------------------------------------------- */

static void free_param(treg_param_t *p) {
   free((char *)p->name);
   free((char *)p->description);
   free((char *)p->field_name);
   free((char *)p->unit);
   for (int i = 0; i < p->enum_count; i++) {
      free((char *)p->enum_values[i]);
   }
}

static void mark_opaque_json(treg_param_t *p,
                             const char *tool_name,
                             const char *key,
                             const char *kind) {
   p->type = TOOL_PARAM_TYPE_STRING;
   p->unit = strdup("json");
   OLOG_WARNING("MCP schema: tool '%s' param '%s' is %s -> opaque JSON passthrough", tool_name, key,
                kind);
}

/* Translate one property schema into @p p. Returns SUCCESS or FAILURE (reject). */
static int translate_one(const char *key,
                         struct json_object *val,
                         const char *tool_name,
                         treg_param_t *p) {
   p->name = strdup(key);
   p->maps_to = TOOL_MAPS_TO_CUSTOM;
   p->field_name = strdup(key);
   if (p->name == NULL || p->field_name == NULL) {
      return FAILURE; /* caller's free_param reclaims the partial entry */
   }

   struct json_object *d = NULL;
   const char *desc = json_object_object_get_ex(val, "description", &d) ? json_object_get_string(d)
                                                                        : "";
   char dbuf[TOOL_DESC_MAX];
   sanitize_into(dbuf, sizeof(dbuf), desc != NULL ? desc : "", sizeof(dbuf) - 1);
   p->description = strdup(dbuf);
   if (p->description == NULL) {
      return FAILURE;
   }

   if (schema_contains_ref(val, MCP_SCHEMA_REF_SCAN_BUDGET)) {
      OLOG_WARNING("MCP schema: tool '%s' param '%s' uses $ref -> rejected", tool_name, key);
      return FAILURE;
   }
   if (oneof_depth(val, MCP_SCHEMA_REF_SCAN_BUDGET) > MCP_SCHEMA_MAX_ONEOF_DEPTH) {
      OLOG_WARNING("MCP schema: tool '%s' param '%s' nests oneOf/anyOf too deep -> rejected",
                   tool_name, key);
      return FAILURE;
   }

   const char *type = schema_type_string(val);

   if (type != NULL && strcmp(type, "string") == 0) {
      struct json_object *en = NULL;
      bool has_enum = json_object_object_get_ex(val, "enum", &en) &&
                      json_object_is_type(en, json_type_array);
      if (has_enum) {
         int ec = json_object_array_length(en);
         if (ec > MCP_SCHEMA_MAX_ENUM) {
            OLOG_WARNING("MCP schema: tool '%s' param '%s' enum has %d values (>%d) -> rejected",
                         tool_name, key, ec, MCP_SCHEMA_MAX_ENUM);
            return FAILURE;
         }
         p->type = TOOL_PARAM_TYPE_ENUM;
         for (int i = 0; i < ec; i++) {
            const char *ev = json_object_get_string(json_object_array_get_idx(en, i));
            p->enum_values[i] = strdup(ev != NULL ? ev : "");
            if (p->enum_values[i] == NULL) {
               p->enum_count = i; /* free_param reclaims [0,i) */
               return FAILURE;
            }
         }
         p->enum_count = ec;
      } else {
         p->type = TOOL_PARAM_TYPE_STRING;
      }
   } else if (type != NULL && strcmp(type, "integer") == 0) {
      p->type = TOOL_PARAM_TYPE_INT;
   } else if (type != NULL && strcmp(type, "number") == 0) {
      p->type = TOOL_PARAM_TYPE_NUMBER;
   } else if (type != NULL && strcmp(type, "boolean") == 0) {
      p->type = TOOL_PARAM_TYPE_BOOL;
   } else if (type != NULL && strcmp(type, "array") == 0) {
      mark_opaque_json(p, tool_name, key, "an array");
   } else {
      /* object / oneOf / anyOf / mixed / missing type */
      mark_opaque_json(p, tool_name, key, "a complex/object type");
   }
   return SUCCESS;
}

int mcp_schema_translate(struct json_object *input_schema,
                         const char *tool_name,
                         mcp_param_set_t *out) {
   if (out == NULL) {
      return FAILURE;
   }
   out->params = NULL;
   out->param_count = 0;
   if (tool_name == NULL) {
      tool_name = "(unknown)";
   }

   if (input_schema == NULL || !json_object_is_type(input_schema, json_type_object)) {
      return SUCCESS; /* no schema -> no-argument tool */
   }
   struct json_object *props = NULL;
   if (!json_object_object_get_ex(input_schema, "properties", &props) ||
       !json_object_is_type(props, json_type_object)) {
      return SUCCESS; /* no properties -> no-argument tool */
   }

   int nprops = json_object_object_length(props);
   if (nprops > MCP_SCHEMA_MAX_PROPERTIES) {
      OLOG_WARNING("MCP schema: tool '%s' has %d properties (>%d) -> rejected", tool_name, nprops,
                   MCP_SCHEMA_MAX_PROPERTIES);
      return FAILURE;
   }
   if (nprops == 0) {
      return SUCCESS;
   }

   struct json_object *required = NULL;
   json_object_object_get_ex(input_schema, "required", &required);

   treg_param_t *params = calloc((size_t)nprops, sizeof(treg_param_t));
   if (params == NULL) {
      return FAILURE;
   }

   int count = 0;
   int rc = SUCCESS;
   json_object_object_foreach(props, key, val) {
      if (rc != SUCCESS) {
         break;
      }
      treg_param_t *p = &params[count];
      rc = translate_one(key, val, tool_name, p);
      if (rc != SUCCESS) {
         break;
      }
      p->required = name_in_required(required, key);
      count++;
   }

   if (rc != SUCCESS) {
      for (int i = 0; i < nprops; i++) {
         free_param(&params[i]); /* safe on the partial/zeroed trailing entries */
      }
      free(params);
      return FAILURE;
   }

   out->params = params;
   out->param_count = count;
   return SUCCESS;
}

void mcp_param_set_free(mcp_param_set_t *out) {
   if (out == NULL || out->params == NULL) {
      return;
   }
   for (int i = 0; i < out->param_count; i++) {
      free_param(&out->params[i]);
   }
   free(out->params);
   out->params = NULL;
   out->param_count = 0;
}
