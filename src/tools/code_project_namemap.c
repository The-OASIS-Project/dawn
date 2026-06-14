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
 * Code-project name-translation boundary for the cbm MCP bridge. Caches cbm's
 * single path-derived graph-name prefix (captured from cbm's own project list)
 * and uses it to translate the LLM's clean identifiers to cbm's namespace
 * outbound, and to strip every slug/path back out of cbm's results inbound. See
 * code_project_namemap.h for the rationale.
 */

#define _GNU_SOURCE

#include "tools/code_project_namemap.h"

#include <json-c/json.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/mcp_bridge.h"

#define CP_NAMEMAP_PREFIX_MAX 128
#define CP_NAMEMAP_LIST_TIMEOUT_MS 10000L

/* cbm's graph-name prefix, e.g. "var-lib-dawn-source-" (the slugified source_root
 * plus its trailing separator), and the literal source_root path. Both derived
 * once from cbm and reused for every translation. */
static char s_graph_prefix[CP_NAMEMAP_PREFIX_MAX];
static char s_source_root[CONFIG_PATH_MAX];
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Return a malloc'd copy of @p s with every occurrence of @p needle removed.
 * NULL on OOM. An empty needle yields a plain copy. */
static char *remove_all(const char *s, const char *needle) {
   size_t nlen = needle != NULL ? strlen(needle) : 0;
   size_t slen = strlen(s);
   if (nlen == 0) {
      char *copy = malloc(slen + 1);
      if (copy != NULL) {
         memcpy(copy, s, slen + 1);
      }
      return copy;
   }
   char *out = malloc(slen + 1);
   if (out == NULL) {
      return NULL;
   }
   size_t oi = 0;
   for (size_t i = 0; i < slen;) {
      if (i + nlen <= slen && memcmp(s + i, needle, nlen) == 0) {
         i += nlen; /* drop the needle */
      } else {
         out[oi++] = s[i++];
      }
   }
   out[oi] = '\0';
   return out;
}

void code_project_namemap_capture(void) {
   const dawn_config_t *cfg = config_get();
   if (cfg == NULL || cfg->code_projects.source_root[0] == '\0') {
      return;
   }
   const char *root = cfg->code_projects.source_root;

   char *result = NULL;
   if (mcp_bridge_call_tool("cbm", "list_projects", "{}", CP_NAMEMAP_LIST_TIMEOUT_MS, &result) !=
           SUCCESS ||
       result == NULL) {
      free(result);
      return;
   }

   /* cbm wraps its tool output as MCP content: {"content":[{"text":"<json>"}]} */
   struct json_object *outer = json_tokener_parse(result);
   free(result);
   if (outer == NULL) {
      return;
   }

   struct json_object *content = NULL;
   struct json_object *inner = NULL;
   const char *inner_str = NULL;
   if (json_object_object_get_ex(outer, "content", &content) &&
       json_object_is_type(content, json_type_array) && json_object_array_length(content) > 0) {
      struct json_object *c0 = json_object_array_get_idx(content, 0);
      struct json_object *text = NULL;
      if (c0 != NULL && json_object_object_get_ex(c0, "text", &text)) {
         inner_str = json_object_get_string(text);
      }
   }
   if (inner_str != NULL) {
      inner = json_tokener_parse(inner_str);
   }

   struct json_object *projects = NULL;
   if (inner != NULL && json_object_object_get_ex(inner, "projects", &projects) &&
       json_object_is_type(projects, json_type_array)) {
      size_t root_len = strlen(root);
      size_t n = json_object_array_length(projects);
      for (size_t i = 0; i < n; i++) {
         struct json_object *pj = json_object_array_get_idx(projects, i);
         struct json_object *jn = NULL;
         struct json_object *jr = NULL;
         if (pj == NULL || !json_object_object_get_ex(pj, "name", &jn) ||
             !json_object_object_get_ex(pj, "root_path", &jr)) {
            continue;
         }
         const char *graph = json_object_get_string(jn);
         const char *rpath = json_object_get_string(jr);
         if (graph == NULL || rpath == NULL || strncmp(rpath, root, root_len) != 0) {
            continue;
         }
         /* prefix = graph_name with its trailing basename removed, so it holds
          * regardless of how cbm slugifies the leading path. */
         const char *slash = strrchr(rpath, '/');
         const char *base = (slash != NULL) ? slash + 1 : rpath;
         size_t blen = strlen(base);
         size_t glen = strlen(graph);
         if (blen == 0 || glen <= blen || strcmp(graph + (glen - blen), base) != 0) {
            continue;
         }
         pthread_mutex_lock(&s_mtx);
         snprintf(s_graph_prefix, sizeof(s_graph_prefix), "%.*s", (int)(glen - blen), graph);
         snprintf(s_source_root, sizeof(s_source_root), "%s", root);
         pthread_mutex_unlock(&s_mtx);
         OLOG_INFO("code_project namemap: captured cbm prefix '%.*s'", (int)(glen - blen), graph);
         break;
      }
   }

   if (inner != NULL) {
      json_object_put(inner);
   }
   json_object_put(outer);
}

void code_project_namemap_to_graph(const char *clean_value, char *out, size_t out_sz) {
   if (out == NULL || out_sz == 0) {
      return;
   }
   out[0] = '\0';
   if (clean_value == NULL) {
      return;
   }
   char prefix[CP_NAMEMAP_PREFIX_MAX];
   pthread_mutex_lock(&s_mtx);
   snprintf(prefix, sizeof(prefix), "%s", s_graph_prefix);
   pthread_mutex_unlock(&s_mtx);

   size_t plen = strlen(prefix);
   if (plen > 0 && strncmp(clean_value, prefix, plen) != 0) {
      snprintf(out, out_sz, "%s%s", prefix, clean_value);
   } else {
      snprintf(out, out_sz, "%s", clean_value);
   }
}

char *code_project_namemap_scrub(const char *cbm_result) {
   if (cbm_result == NULL) {
      return NULL;
   }
   char prefix[CP_NAMEMAP_PREFIX_MAX];
   char root[CONFIG_PATH_MAX];
   pthread_mutex_lock(&s_mtx);
   snprintf(prefix, sizeof(prefix), "%s", s_graph_prefix);
   snprintf(root, sizeof(root), "%s", s_source_root);
   pthread_mutex_unlock(&s_mtx);

   /* 1) graph-name prefix (dash-joined slug, no slashes — escaping-agnostic). */
   char *step = (prefix[0] != '\0') ? remove_all(cbm_result, prefix) : remove_all(cbm_result, "");
   if (step == NULL || root[0] == '\0') {
      return step;
   }

   /* 2) source_root path prefix. cbm's serializer escapes '/' as '\/' inside the
    * tool-output text, so strip both the plain and the escaped form; whichever
    * the result actually uses is the one that matches. */
   char path_plain[CONFIG_PATH_MAX + 2];
   snprintf(path_plain, sizeof(path_plain), "%s/", root);
   char path_esc[2 * CONFIG_PATH_MAX + 4];
   size_t j = 0;
   for (size_t i = 0; path_plain[i] != '\0' && j + 2 < sizeof(path_esc); i++) {
      if (path_plain[i] == '/') {
         path_esc[j++] = '\\';
         path_esc[j++] = '/';
      } else {
         path_esc[j++] = path_plain[i];
      }
   }
   path_esc[j] = '\0';

   char *step2 = remove_all(step, path_plain);
   free(step);
   if (step2 == NULL) {
      return NULL;
   }
   char *step3 = remove_all(step2, path_esc);
   free(step2);
   return step3;
}
