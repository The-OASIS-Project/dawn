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
 * cbm-mcp implementation of the code-graph provider. Calls the cbm server's
 * tools (index_repository, delete_project) over the MCP bridge. The cbm server
 * is expected to be configured under the alias "cbm".
 */

#include <json-c/json.h>
#include <stdlib.h>

#include "dawn_error.h"
#include "logging.h"
#include "tools/code_graph_provider.h"
#include "tools/mcp_bridge.h"

#define CBM_ALIAS "cbm"
#define CBM_INDEX_TIMEOUT_MS 600000L /* 10 min: index_repository is synchronous */
#define CBM_DELETE_TIMEOUT_MS 30000L

/* Build a one-key JSON object string {"<key>":"<value>"} (json-c escapes). */
static char *cbm_args_one(const char *key, const char *value) {
   struct json_object *o = json_object_new_object();
   json_object_object_add(o, key, json_object_new_string(value != NULL ? value : ""));
   char *s = strdup(json_object_to_json_string(o));
   json_object_put(o);
   if (s == NULL) {
      OLOG_ERROR("cbm provider: strdup failed building args");
   }
   return s;
}

static int cbm_index_start(const char *project_name,
                           const char *repo_path,
                           const char *mode,
                           int64_t *job_id_out) {
   (void)project_name; /* cbm derives the project from the repo path */
   (void)mode;
   if (job_id_out != NULL) {
      *job_id_out = 0;
   }
   if (repo_path == NULL || repo_path[0] == '\0') {
      return FAILURE;
   }

   char *args = cbm_args_one("repo_path", repo_path);
   if (args == NULL) {
      return FAILURE;
   }
   char *result = NULL;
   int rc = mcp_bridge_call_tool(CBM_ALIAS, "index_repository", args, CBM_INDEX_TIMEOUT_MS,
                                 &result);
   free(args);
   free(result);
   if (rc != SUCCESS) {
      OLOG_ERROR("cbm provider: index_repository failed for %s", repo_path);
   }
   return rc;
}

static int cbm_index_poll_status(const char *project_name, code_graph_status_t *out) {
   (void)project_name;
   if (out == NULL) {
      return FAILURE;
   }
   /* index_start is synchronous; if the orchestrator reached this point the
    * index call already returned, so the graph is ready. */
   out->state = CODE_GRAPH_READY;
   out->percent = 100;
   out->message[0] = '\0';
   return SUCCESS;
}

static int cbm_delete_project(const char *graph_name) {
   if (graph_name == NULL || graph_name[0] == '\0') {
      return FAILURE;
   }
   /* cbm's delete_project tool keys on "project" = the path-derived graph slug
    * (NOT DAWN's clean name). The caller resolves the slug; we just pass it under
    * the key cbm expects. (Both the key and the value were wrong before — the
    * delete silently no-op'd and the on-disk .db was never removed.) */
   char *args = cbm_args_one("project", graph_name);
   if (args == NULL) {
      return FAILURE;
   }
   char *result = NULL;
   int rc = mcp_bridge_call_tool(CBM_ALIAS, "delete_project", args, CBM_DELETE_TIMEOUT_MS, &result);
   free(args);
   free(result);
   /* Best-effort: a missing delete_project tool should not block project teardown. */
   return rc;
}

static int cbm_is_available(void) {
   return mcp_bridge_server_connected(CBM_ALIAS);
}

const code_graph_provider_t code_graph_provider_cbm = {
   .name = "cbm",
   .index_start = cbm_index_start,
   .index_poll_status = cbm_index_poll_status,
   .delete_project = cbm_delete_project,
   .is_available = cbm_is_available,
};
