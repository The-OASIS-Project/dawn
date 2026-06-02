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
 * @param global            Make the project visible to all users.
 * @param project_id_out    On SUCCESS, the new project id.
 * @return SUCCESS or FAILURE (invalid input, duplicate name, or disabled).
 */
int code_project_import(int64_t requester_user_id,
                        const char *source_url,
                        const char *desired_name,
                        bool global,
                        int64_t *project_id_out);

/** @brief Queue a re-index of an existing project. */
int code_project_refresh(int64_t project_id);

/** @brief Delete a project: graph backend (best-effort) + clone + DB row. */
int code_project_delete(int64_t project_id);

/**
 * @brief Progress hook, overridden (strong symbol) by the WebUI to push a
 *        WebSocket status event. Default weak no-op when WebUI is absent.
 */
void code_project_broadcast_status_changed(int64_t project_id);

#endif /* CODE_PROJECT_SERVICE_H */
