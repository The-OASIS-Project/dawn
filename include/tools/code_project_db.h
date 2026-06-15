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
 * CRUD + visibility queries for the code_projects table (created in schema v65;
 * branch/kind/graph_name columns added in v66). Parallels document_db.c; uses the
 * shared auth_db handle.
 */

#ifndef CODE_PROJECT_DB_H
#define CODE_PROJECT_DB_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define CODE_PROJECT_NAME_MAX 64
#define CODE_PROJECT_URL_MAX 512
#define CODE_PROJECT_PATH_MAX 512
#define CODE_PROJECT_STATUS_MAX 32
#define CODE_PROJECT_MSG_MAX 256
#define CODE_PROJECT_BRANCH_MAX 128
#define CODE_PROJECT_KIND_MAX 8         /* "clone" | "local" */
#define CODE_PROJECT_GRAPH_NAME_MAX 256 /* cbm's path-derived slug (can be long) */
#define CODE_PROJECTS_MAX 64            /* upper bound on rows returned by a single list query */

/* code_projects.kind values. Validated in C (code_project_db_create), not via a
 * SQLite CHECK constraint (avoids a table rebuild) — all writes must go through
 * code_project_db_create so the invariant holds. */
#define CODE_PROJECT_KIND_CLONE "clone" /* DAWN-managed clone under source_root */
#define CODE_PROJECT_KIND_LOCAL "local" /* linked existing local checkout (no clone) */

typedef struct {
   int64_t id;
   char name[CODE_PROJECT_NAME_MAX];
   char source_url[CODE_PROJECT_URL_MAX]; /* "" for kind=local (no remote) */
   char local_path[CODE_PROJECT_PATH_MAX];
   int64_t user_id; /* 0 = no owner */
   bool is_global;
   char status[CODE_PROJECT_STATUS_MAX];
   char status_msg[CODE_PROJECT_MSG_MAX];
   int64_t created_at;
   int64_t updated_at;
   int64_t indexed_at;                           /* 0 = never */
   int64_t imported_by;                          /* 0 = unknown */
   char branch[CODE_PROJECT_BRANCH_MAX];         /* tracked branch; "" = HEAD / as-checked-out */
   char kind[CODE_PROJECT_KIND_MAX];             /* "clone" | "local" */
   char graph_name[CODE_PROJECT_GRAPH_NAME_MAX]; /* cbm graph slug; "" = not yet indexed */
} code_project_t;

/* All functions below return AUTH_DB_SUCCESS / AUTH_DB_NOT_FOUND / AUTH_DB_FAILURE
 * unless noted otherwise. */

/** @brief Insert a new project row (validates @p p->kind in C).
 *  @param p      Project to insert (kind defaults to "clone" if empty).
 *  @param id_out If non-NULL, set to the new row id. */
int code_project_db_create(const code_project_t *p, int64_t *id_out);

/** @brief Update a project's status string and optional status message.
 *  @param id @param status New status. @param msg Optional detail (NULL = ""). */
int code_project_db_update_status(int64_t id, const char *status, const char *msg);

/** @brief Stamp the last-indexed time on a project. @param id @param when Unix time. */
int code_project_db_set_indexed_at(int64_t id, time_t when);

/** @brief Set a project's tracked branch (clone kind). @param id @param branch
 *  Branch name; empty/NULL clears it. */
int code_project_db_set_branch(int64_t id, const char *branch);

/** @brief Set a project's persisted cbm graph slug. @param id @param graph_name
 *  Slug; empty/NULL clears it. */
int code_project_db_set_graph_name(int64_t id, const char *graph_name);

/** @brief Fetch a project by id. @param id @param out Filled on success. */
int code_project_db_get(int64_t id, code_project_t *out);

/** @brief Fetch a project by unique name. @param name @param out Filled on success. */
int code_project_db_get_by_name(const char *name, code_project_t *out);

/**
 * @brief List projects visible to @p user_id (owned or global), sorted by name.
 * @param out      Caller array of @p max elements.
 * @param max      Capacity of @p out.
 * @param count_out Set to the number of rows written.
 */
int code_project_db_list_visible(int64_t user_id, code_project_t *out, int max, int *count_out);

/**
 * @brief List all projects (operator/admin view), sorted by name.
 * @param out      Caller array of @p max elements.
 * @param max      Capacity of @p out.
 * @param count_out Set to the number of rows written.
 */
int code_project_db_list_all(code_project_t *out, int max, int *count_out);

/**
 * @brief Per-call visibility re-check for a named project.
 * @param out Set to true if @p name is owned by @p user_id or global, else false.
 * @return AUTH_DB_SUCCESS when the lookup ran (inspect @p out for the result),
 *         AUTH_DB_FAILURE on error.
 */
int code_project_db_check_visible(int64_t user_id, const char *name, bool *out);

/** @brief Set or clear a project's global (shared) flag. @param id @param is_global */
int code_project_db_set_global(int64_t id, bool is_global);

/** @brief Delete a project row by id. @param id */
int code_project_db_delete(int64_t id);

#endif /* CODE_PROJECT_DB_H */
