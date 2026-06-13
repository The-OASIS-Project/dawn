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
 * In-process git client (libgit2) for cloning imported repositories, with size,
 * file-count, path-depth, and symlink-containment guards.
 */

#ifndef CODE_PROJECT_GIT_H
#define CODE_PROJECT_GIT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
   const char *source_url;
   const char *local_path;
   const char *branch;      /* NULL = remote HEAD */
   size_t max_size_bytes;   /* 0 = unlimited */
   uint32_t max_file_count; /* 0 = unlimited */
   uint8_t max_path_depth;  /* 0 = unlimited */
   int clone_depth;         /* 0 = full history, 1 = shallow */
   void (*progress_cb)(void *user, int percent, const char *phase);
   void *progress_user;
} code_git_clone_opts_t;

/**
 * @brief Clone a repository to opts->local_path with hardening.
 *
 * Aborts (and removes the partial clone) if the transfer exceeds the size or
 * file-count caps; after checkout, removes any symlinks (containment) and fails
 * if the path depth exceeds the cap. libgit2 negative codes are translated to
 * FAILURE; the detail is logged with credentials redacted.
 *
 * NOTE: not reentrant — the post-clone directory sweep uses file-scope state, so
 * callers must serialize clones (the orchestrator runs them on one thread).
 *
 * @return SUCCESS or FAILURE.
 */
int code_project_git_clone(const code_git_clone_opts_t *opts);

/** @brief Recursively remove a cloned tree (nftw FTW_PHYS, in-process). */
int code_project_git_remove(const char *local_path);

/**
 * @brief Initialize libgit2's process-global state. Call once at service start.
 *
 * libgit2's init/shutdown is a refcount; calling it per-clone would tear down
 * library state shared with any other user. Pair with the shutdown below.
 */
void code_project_git_global_init(void);

/** @brief Tear down libgit2's process-global state. Call once at service stop. */
void code_project_git_global_shutdown(void);

/**
 * @brief Probe whether a remote repository exists and is reachable, without
 *        cloning (in-process libgit2 ref negotiation; redirects disabled).
 *
 * Used to validate an import before a DB row is created, so a nonexistent or
 * unreachable URL is rejected rather than left as a phantom error row.
 *
 * @return SUCCESS if the remote advertises refs; FAILURE on not-found,
 *         unreachable, or auth-required (Phase 1 targets public repos).
 */
int code_project_git_remote_probe(const char *url);

#endif /* CODE_PROJECT_GIT_H */
