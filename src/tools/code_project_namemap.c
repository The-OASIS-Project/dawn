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
 * Code-project name-translation boundary for the cbm MCP bridge.
 *
 * cbm names every project by slugifying its absolute repo path
 * (/var/lib/dawn/source/echo -> "var-lib-dawn-source-echo") and prefixes every
 * qualified_name with that slug ("var-lib-dawn-source-echo.src.foo"). To keep the
 * filesystem layout out of the LLM's view (security) and stored conversations
 * portable across a directory move, the bridge translates the cbm namespace at the
 * boundary: the LLM only ever sees clean DAWN project names ("echo") and
 * project-relative paths.
 *
 * Projects no longer share a single source_root (link-local repos live anywhere),
 * so the mapping is per-project: DAWN's clean name <-> cbm's full graph slug,
 * captured from cbm's project list intersected with DAWN's own rows (matched by
 * root_path == local_path). qualified_names are `<slug>.<rest>`; the clean leading
 * token is everything before the first '.', and DAWN names cannot contain '.', so
 * the split is unambiguous.
 *
 * Dependency / lock invariants: only capture() touches code_project_db, and only
 * to snapshot rows (the DB lock is released before the cbm call). to_graph()/
 * scrub() are DB-free and run on the LLM hot path. Never hold s_db.mutex or s_mtx
 * across a cbm network call.
 */

#define _GNU_SOURCE

#include "tools/code_project_namemap.h"

#include <json-c/json.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/code_project_db.h"
#include "tools/mcp_bridge.h"

#define CP_NAMEMAP_LIST_TIMEOUT_MS 10000L

/* Per-project translation entry. clean = DAWN's name, graph = cbm's slug,
 * path = the absolute repo path (stripped from results so it never reaches the
 * LLM). Bounded by CODE_PROJECTS_MAX. */
typedef struct {
   char clean[CODE_PROJECT_NAME_MAX];
   char graph[CODE_PROJECT_GRAPH_NAME_MAX];
   char path[CODE_PROJECT_PATH_MAX];
} cp_map_entry_t;

static cp_map_entry_t s_map[CODE_PROJECTS_MAX];
static int s_map_count;
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;

/* malloc'd copy of @p s with every occurrence of @p needle replaced by @p repl.
 * NULL on OOM. Empty needle yields a plain copy. */
static char *replace_all(const char *s, const char *needle, const char *repl) {
   size_t slen = strlen(s);
   size_t nlen = needle != NULL ? strlen(needle) : 0;
   if (nlen == 0) {
      char *copy = malloc(slen + 1);
      if (copy != NULL) {
         memcpy(copy, s, slen + 1);
      }
      return copy;
   }
   size_t rlen = repl != NULL ? strlen(repl) : 0;
   size_t count = 0;
   for (const char *p = s; (p = strstr(p, needle)) != NULL; p += nlen) {
      count++;
   }
   /* slen >= count*nlen always (matches lie within s), so this can't underflow. */
   size_t outlen = (slen - count * nlen) + count * rlen;
   char *out = malloc(outlen + 1);
   if (out == NULL) {
      return NULL;
   }
   char *w = out;
   const char *r = s;
   const char *m;
   while ((m = strstr(r, needle)) != NULL) {
      size_t seg = (size_t)(m - r);
      memcpy(w, r, seg);
      w += seg;
      if (rlen > 0) {
         memcpy(w, repl, rlen);
         w += rlen;
      }
      r = m + nlen;
   }
   strcpy(w, r);
   return out;
}

/* Parse cbm's list_projects output, intersect with DAWN's rows, and build a fresh
 * per-project map. @rows/@nrows are DAWN's projects (already snapshotted). */
static void build_map_from_cbm(const char *cbm_result, const code_project_t *rows, int nrows) {
   /* cbm wraps tool output as MCP content: {"content":[{"text":"<json>"}]} */
   struct json_object *outer = json_tokener_parse(cbm_result);
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

   cp_map_entry_t tmp[CODE_PROJECTS_MAX];
   int tn = 0;
   struct json_object *projects = NULL;
   if (inner != NULL && json_object_object_get_ex(inner, "projects", &projects) &&
       json_object_is_type(projects, json_type_array)) {
      size_t np = json_object_array_length(projects);
      for (size_t i = 0; i < np && tn < CODE_PROJECTS_MAX; i++) {
         struct json_object *pj = json_object_array_get_idx(projects, i);
         struct json_object *jn = NULL;
         struct json_object *jr = NULL;
         if (pj == NULL || !json_object_object_get_ex(pj, "name", &jn) ||
             !json_object_object_get_ex(pj, "root_path", &jr)) {
            continue;
         }
         const char *graph = json_object_get_string(jn);
         const char *rpath = json_object_get_string(jr);
         if (graph == NULL || rpath == NULL) {
            continue;
         }
         /* Match this cbm project to a DAWN row by exact path. cbm stores the
          * repo_path DAWN passed verbatim, and DAWN builds local_path canonically
          * (source_root/name for clones, realpath for links), so == holds.
          * Projects with no matching DAWN row (e.g. another tool's, in a shared
          * cache) are deliberately ignored. */
         for (int r = 0; r < nrows; r++) {
            if (strcmp(rows[r].local_path, rpath) == 0) {
               snprintf(tmp[tn].clean, sizeof(tmp[tn].clean), "%s", rows[r].name);
               snprintf(tmp[tn].graph, sizeof(tmp[tn].graph), "%s", graph);
               snprintf(tmp[tn].path, sizeof(tmp[tn].path), "%s", rpath);
               tn++;
               break;
            }
         }
      }
   }

   pthread_mutex_lock(&s_mtx);
   memcpy(s_map, tmp, (size_t)tn * sizeof(tmp[0]));
   s_map_count = tn;
   pthread_mutex_unlock(&s_mtx);
   OLOG_INFO("code_project namemap: captured %d project mapping(s)", tn);

   if (inner != NULL) {
      json_object_put(inner);
   }
   json_object_put(outer);
}

void code_project_namemap_capture(void) {
   /* 1) Snapshot DAWN rows (list_all takes/releases s_db.mutex internally). */
   code_project_t rows[CODE_PROJECTS_MAX];
   int nrows = 0;
   if (code_project_db_list_all(rows, CODE_PROJECTS_MAX, &nrows) != AUTH_DB_SUCCESS) {
      return;
   }
   if (nrows == 0) {
      pthread_mutex_lock(&s_mtx);
      s_map_count = 0;
      pthread_mutex_unlock(&s_mtx);
      return;
   }

   /* 2) Ask cbm for its project list (no DAWN lock held across this call). */
   char *result = NULL;
   if (mcp_bridge_call_tool("cbm", "list_projects", "{}", CP_NAMEMAP_LIST_TIMEOUT_MS, &result) !=
           SUCCESS ||
       result == NULL) {
      free(result);
      return;
   }

   /* 3) Build + publish the map. */
   build_map_from_cbm(result, rows, nrows);
   free(result);
}

void code_project_namemap_to_graph(const char *clean_value, char *out, size_t out_sz) {
   if (out == NULL || out_sz == 0) {
      return;
   }
   out[0] = '\0';
   if (clean_value == NULL) {
      return;
   }
   /* The clean leading token is everything up to the first '.' (a project name —
    * which cannot contain '.'); the remainder is the symbol path, passed verbatim. */
   const char *dot = strchr(clean_value, '.');
   size_t token_len = dot != NULL ? (size_t)(dot - clean_value) : strlen(clean_value);

   char graph[CODE_PROJECT_GRAPH_NAME_MAX];
   graph[0] = '\0';
   pthread_mutex_lock(&s_mtx);
   for (int i = 0; i < s_map_count; i++) {
      if (strlen(s_map[i].clean) == token_len &&
          strncmp(s_map[i].clean, clean_value, token_len) == 0) {
         snprintf(graph, sizeof(graph), "%s", s_map[i].graph);
         break;
      }
   }
   pthread_mutex_unlock(&s_mtx);

   if (graph[0] == '\0') {
      /* No mapping (project not indexed yet, or an unknown identifier). Pass
       * through unchanged so cbm 404s visibly rather than silently mis-routing. */
      snprintf(out, out_sz, "%s", clean_value);
      OLOG_WARNING("code_project namemap: no graph mapping for '%.*s' (passing through)",
                   (int)token_len, clean_value);
      return;
   }
   snprintf(out, out_sz, "%s%s", graph, dot != NULL ? dot : "");
}

char *code_project_namemap_scrub(const char *cbm_result) {
   if (cbm_result == NULL) {
      return NULL;
   }
   /* Snapshot the map so the cbm hot path never holds s_mtx during the rewrites.
    * Stack note: snap[CODE_PROJECTS_MAX] (~53 KB) assumes a default-stack thread —
    * cbm tools run sequentially on the LLM tool-loop driver, NOT the 512 KB
    * parallel-tool thread; keep cbm sequential or heap-allocate this if that changes.
    * Cost note: this does ~3N+1 full-result allocations/copies per cbm call
    * (slug + path plain/escaped per project). Fine at small N; if per-user project
    * count crosses ~20 OR cbm results routinely exceed ~64 KB, switch to a
    * single-pass multi-needle rewrite (pre-sorted needle table built at capture). */
   cp_map_entry_t snap[CODE_PROJECTS_MAX];
   int n;
   pthread_mutex_lock(&s_mtx);
   n = s_map_count;
   memcpy(snap, s_map, (size_t)n * sizeof(snap[0]));
   pthread_mutex_unlock(&s_mtx);

   char *cur = malloc(strlen(cbm_result) + 1);
   if (cur == NULL) {
      return NULL;
   }
   strcpy(cur, cbm_result);
   if (n == 0) {
      return cur;
   }

   /* Replace each graph slug with its clean name, longest slug first so a shorter
    * slug that is a prefix of a longer one (e.g. "<root>-echo" vs "<root>-echo-2")
    * can't corrupt the longer match. n <= 64, so an O(n^2) selection over indices
    * is fine. */
   int order[CODE_PROJECTS_MAX];
   for (int i = 0; i < n; i++) {
      order[i] = i;
   }
   for (int a = 0; a < n; a++) {
      for (int b = a + 1; b < n; b++) {
         if (strlen(snap[order[b]].graph) > strlen(snap[order[a]].graph)) {
            int t = order[a];
            order[a] = order[b];
            order[b] = t;
         }
      }
   }
   for (int i = 0; i < n; i++) {
      char *next = replace_all(cur, snap[order[i]].graph, snap[order[i]].clean);
      free(cur);
      if (next == NULL) {
         return NULL;
      }
      cur = next;
   }

   /* Strip each absolute repo path (plain + JSON-escaped '\/'), longest first. */
   for (int a = 0; a < n; a++) {
      for (int b = a + 1; b < n; b++) {
         if (strlen(snap[order[b]].path) > strlen(snap[order[a]].path)) {
            int t = order[a];
            order[a] = order[b];
            order[b] = t;
         }
      }
   }
   for (int i = 0; i < n; i++) {
      const char *root = snap[order[i]].path;
      if (root[0] == '\0') {
         continue;
      }
      char path_plain[CODE_PROJECT_PATH_MAX + 2];
      snprintf(path_plain, sizeof(path_plain), "%s/", root);
      char path_esc[2 * CODE_PROJECT_PATH_MAX + 4];
      size_t j = 0;
      for (size_t k = 0; path_plain[k] != '\0' && j + 2 < sizeof(path_esc); k++) {
         if (path_plain[k] == '/') {
            path_esc[j++] = '\\';
            path_esc[j++] = '/';
         } else {
            path_esc[j++] = path_plain[k];
         }
      }
      path_esc[j] = '\0';

      char *step = replace_all(cur, path_plain, "");
      free(cur);
      if (step == NULL) {
         return NULL;
      }
      char *step2 = replace_all(step, path_esc, "");
      free(step);
      if (step2 == NULL) {
         return NULL;
      }
      cur = step2;
   }
   return cur;
}
