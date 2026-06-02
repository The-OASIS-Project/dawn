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
 * Code-graph provider vtable: abstracts the indexing backend (cbm-mcp in
 * Phase 1) so the code-projects orchestrator is backend-agnostic.
 */

#ifndef CODE_GRAPH_PROVIDER_H
#define CODE_GRAPH_PROVIDER_H

#include <stdint.h>

typedef enum {
   CODE_GRAPH_PENDING,
   CODE_GRAPH_INDEXING,
   CODE_GRAPH_READY,
   CODE_GRAPH_ERROR,
} code_graph_state_t;

typedef struct {
   code_graph_state_t state;
   int percent; /* 0-100, best-effort (cbm reports 0 or 100) */
   char message[256];
} code_graph_status_t;

typedef struct code_graph_provider {
   const char *name; /* "cbm", future "github_mcp", etc. */

   /**
    * Index a repository. BLOCKING in Phase 1 (cbm's index_repository runs to
    * completion before returning), so a SUCCESS return means indexing is done.
    * @param job_id_out optional; set to 0 by synchronous providers.
    * @return SUCCESS or FAILURE.
    */
   int (*index_start)(const char *project_name,
                      const char *repo_path,
                      const char *mode,
                      int64_t *job_id_out);

   /** Poll index status. For synchronous providers this reports READY once
    *  index_start has returned. @return SUCCESS or FAILURE. */
   int (*index_poll_status)(const char *project_name, code_graph_status_t *out);

   /** Remove a project from the graph backend (best-effort). */
   int (*delete_project)(const char *project_name);
} code_graph_provider_t;

/** Phase 1 provider backed by cbm-mcp via the MCP bridge. */
extern const code_graph_provider_t code_graph_provider_cbm;

#endif /* CODE_GRAPH_PROVIDER_H */
