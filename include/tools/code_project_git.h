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

typedef struct {
   const char *local_path;
   const char *branch;      /* required: branch to fetch + check out */
   int clone_depth;         /* preserve the original shallow depth (0 = full) */
   size_t max_size_bytes;   /* 0 = unlimited */
   uint32_t max_file_count; /* 0 = unlimited */
   uint8_t max_path_depth;  /* 0 = unlimited */
} code_git_fetch_opts_t;

/**
 * @brief Fetch origin and hard-check-out @p branch on a DAWN-managed clone.
 *
 * Fetches origin (preserving the original shallow depth), resolves
 * refs/remotes/origin/<branch> (creating a tracking local branch if needed),
 * force-checks-out that tree, points HEAD at it, and hard-resets — so the working
 * tree matches the remote branch exactly. Re-runs the post-checkout sweep
 * (symlink strip + size/file/depth caps). On failure the clone is left in place
 * (the caller marks the project errored rather than destroying it).
 *
 * Clone kind only — NEVER call on a linked local repo (would stomp the user's
 * working tree). Not reentrant (shares the sweep's file-scope state); serialize
 * with clones on the one worker thread.
 *
 * @param opts Fetch + checkout options (local_path + branch required; the cap
 *             fields 0 = unlimited; clone_depth preserves a shallow clone).
 * @return SUCCESS or FAILURE.
 */
int code_project_git_fetch_checkout(const code_git_fetch_opts_t *opts);

/**
 * @brief Validate a git branch name (via libgit2's ref-name rules) before it is
 *        interpolated into refs/heads/<branch> / refs/remotes/origin/<branch>.
 * @param branch Candidate branch name.
 * @return SUCCESS if a valid branch name, FAILURE otherwise (incl. NULL/empty).
 */
int code_project_git_branch_valid(const char *branch);

/**
 * @brief Write the currently checked-out branch shorthand of the repo at
 *        @p local_path into @p out (e.g. "main"); "(detached <sha>)" when
 *        detached, empty for an unborn branch.
 * @return SUCCESS if the repo opened, FAILURE otherwise.
 */
int code_project_git_current_branch(const char *local_path, char *out, size_t out_sz);

/**
 * @brief Validate that @p local_path is itself a git repository (no parent
 *        search — GIT_REPOSITORY_OPEN_NO_SEARCH), used before linking a local repo
 *        so a subdir can't silently attach to an ancestor's .git.
 * @return SUCCESS if a repo exists at exactly @p local_path, FAILURE otherwise.
 */
int code_project_git_open_validate(const char *local_path);

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
