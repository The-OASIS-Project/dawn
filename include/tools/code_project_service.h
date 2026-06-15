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
 * Code-projects orchestrator: validates imports, then runs clone + index on a
 * dedicated background thread (not worker_pool), updating code_projects status.
 */

#ifndef CODE_PROJECT_SERVICE_H
#define CODE_PROJECT_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Start the import worker thread. @return SUCCESS or FAILURE. */
int code_project_service_init(void);

/** @brief Stop the import worker thread (joins). */
void code_project_service_shutdown(void);

/**
 * @brief Validate + record a new import, then queue clone + index asynchronously.
 *
 * Returns as soon as the DB row is created (status "cloning"); progress is
 * reported via the code_project_broadcast_status_changed weak hook.
 *
 * @param requester_user_id Owner (ignored when @p global).
 * @param source_url        HTTPS clone URL (SSRF + host-allowlist checked).
 * @param desired_name      Project name (validated against the name charset).
 * @param branch            Branch to clone, or NULL/"" for the remote HEAD.
 * @param global            Make the project visible to all users.
 * @param project_id_out    On SUCCESS, the new project id.
 * @return SUCCESS or FAILURE (invalid input, duplicate name, or disabled).
 */
int code_project_import(int64_t requester_user_id,
                        const char *source_url,
                        const char *desired_name,
                        const char *branch,
                        bool global,
                        int64_t *project_id_out);

/**
 * @brief Link an existing local git checkout (no clone) and queue an index.
 *
 * The path is realpath'd, confirmed to be a directory inside an
 * [code_projects].allowed_local_roots prefix, and validated as a git repo (no
 * parent search) on the caller thread. kind='local': DAWN only ever READS the
 * tree (never clones/removes it). global is rejected (a linked tree's file
 * contents would otherwise reach every user); admin-gating is enforced by the
 * caller (WebUI/admin).
 *
 * @param requester_user_id Owner of the new project row.
 * @param local_path        Path to an existing checkout (realpath'd + contained).
 * @param desired_name      Project name (validated against the name charset).
 * @param global            Must be false; rejected for linked repos.
 * @param project_id_out    Set to 0 (no row exists until the worker creates it).
 * @return SUCCESS or FAILURE.
 */
int code_project_link(int64_t requester_user_id,
                      const char *local_path,
                      const char *desired_name,
                      bool global,
                      int64_t *project_id_out);

/** @brief Queue a cheap incremental re-index (clone: fetch tracked branch).
 *  @param project_id Row id. @return SUCCESS or FAILURE. */
int code_project_refresh(int64_t project_id);

/** @brief Queue a clean rebuild (clone: fetch; then drop the cbm graph + re-index).
 *  @param project_id Row id. @return SUCCESS or FAILURE. */
int code_project_rebuild(int64_t project_id);

/**
 * @brief Set a clone project's tracked branch and queue a rebuild to apply it.
 *        Rejected for linked local projects (branch tracks the live checkout).
 * @param project_id Row id (must be a clone-kind project).
 * @param branch     Branch name to track (validated against libgit2 ref rules).
 * @return SUCCESS or FAILURE.
 */
int code_project_set_branch(int64_t project_id, const char *branch);

/** @brief Delete a project: graph backend (best-effort) + clone + DB row.
 *         A linked local project's working tree is never removed (only the row). */
int code_project_delete(int64_t project_id);

/**
 * @brief Progress hook, overridden (strong symbol) by the WebUI to push a
 *        WebSocket status event. Default weak no-op when WebUI is absent.
 */
void code_project_broadcast_status_changed(int64_t project_id);

/**
 * @brief Failure hook for a pending import rejected before any row is created
 *        (repo not found/unreachable, or a duplicate that raced the pre-check).
 *
 * Overridden (strong symbol) by the WebUI to toast the requesting user; default
 * weak no-op when WebUI is absent. @p name and @p reason are short, caller-owned
 * strings valid only for the duration of the call.
 */
void code_project_broadcast_import_failed(int64_t user_id, const char *name, const char *reason);

#endif /* CODE_PROJECT_SERVICE_H */
