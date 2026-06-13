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
 * libgit2 clone wrapper with size / file-count / depth caps and symlink
 * containment. In-process only (no git subprocess); nftw() for the directory
 * sweep and removal is an in-process POSIX walk (allowed by the CI invariant).
 */

#define _GNU_SOURCE

#include "tools/code_project_git.h"

#include <ftw.h>
#include <git2.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dawn_error.h"
#include "logging.h"

#define GIT_SWEEP_MAX_FDS 16

typedef struct {
   const code_git_clone_opts_t *opts;
   int last_transfer_pct; /* throttle: only fire progress_cb on whole-percent change */
   int last_checkout_pct;
} git_cb_ctx_t;

/* Overwrite any "scheme://user:pass@" userinfo in @p msg with '*' (in place,
 * same length) so credentials never reach the logs. */
static void redact_credentials(char *out, size_t out_sz, const char *msg) {
   snprintf(out, out_sz, "%s", msg != NULL ? msg : "");
   char *scheme = strstr(out, "://");
   if (scheme == NULL) {
      return;
   }
   char *userinfo = scheme + 3;
   char *at = strchr(userinfo, '@');
   char *slash = strchr(userinfo, '/');
   if (at != NULL && (slash == NULL || at < slash)) {
      for (char *q = userinfo; q < at; q++) {
         *q = '*';
      }
   }
}

static int transfer_progress_cb(const git_indexer_progress *stats, void *payload) {
   git_cb_ctx_t *ctx = (git_cb_ctx_t *)payload;
   const code_git_clone_opts_t *o = ctx->opts;
   /* Best-effort in-flight caps. total_objects is server-advertised and can be
    * understated; the authoritative on-disk tally runs in sweep_cb (sec-S5). */
   if (o->max_file_count > 0 && stats->total_objects > o->max_file_count) {
      OLOG_ERROR("code_git: object count %u exceeds cap %u", stats->total_objects,
                 o->max_file_count);
      return FAILURE; /* any nonzero return aborts the fetch (libgit2 contract) */
   }
   if (o->max_size_bytes > 0 && stats->received_bytes > o->max_size_bytes) {
      OLOG_ERROR("code_git: received bytes exceed cap %zu", o->max_size_bytes);
      return FAILURE;
   }
   if (o->progress_cb != NULL && stats->total_objects > 0) {
      int pct = (int)(((uint64_t)stats->received_objects * 100) / stats->total_objects);
      if (pct != ctx->last_transfer_pct) {
         ctx->last_transfer_pct = pct;
         o->progress_cb(o->progress_user, pct, "cloning");
      }
   }
   return 0;
}

static void checkout_progress_cb(const char *path, size_t completed, size_t total, void *payload) {
   (void)path;
   git_cb_ctx_t *ctx = (git_cb_ctx_t *)payload;
   const code_git_clone_opts_t *o = ctx->opts;
   if (o->progress_cb != NULL && total > 0) {
      int pct = (int)((completed * 100) / total);
      if (pct != ctx->last_checkout_pct) {
         ctx->last_checkout_pct = pct;
         o->progress_cb(o->progress_user, pct, "checkout");
      }
   }
}

/* Post-clone sweep state (single-threaded use; see header note). */
static uint8_t s_sweep_max_depth;
static size_t s_sweep_max_bytes;
static uint32_t s_sweep_max_files;
static uint64_t s_sweep_bytes;
static uint32_t s_sweep_files;
static int s_sweep_failed;

static int sweep_cb(const char *path, const struct stat *sb, int typeflag, struct FTW *ftw) {
   (void)typeflag;
   if (S_ISLNK(sb->st_mode)) {
      OLOG_WARNING("code_git: removing symlink from clone: %s", path);
      if (unlink(path) != 0) {
         s_sweep_failed = 1;
      }
      return 0;
   }
   if (s_sweep_max_depth > 0 && ftw->level > (int)s_sweep_max_depth) {
      OLOG_ERROR("code_git: path depth %d exceeds cap %u: %s", ftw->level, s_sweep_max_depth, path);
      s_sweep_failed = 1;
   }
   /* Authoritative on-disk size/count tally (sec-S5): transfer_progress_cb's
    * caps trust the server-advertised object count, which a hostile server can
    * understate. Enforce the real working-tree totals here. */
   if (S_ISREG(sb->st_mode)) {
      s_sweep_files++;
      s_sweep_bytes += (uint64_t)sb->st_size;
      if (s_sweep_max_files > 0 && s_sweep_files > s_sweep_max_files) {
         OLOG_ERROR("code_git: file count %u exceeds cap %u", s_sweep_files, s_sweep_max_files);
         s_sweep_failed = 1;
      }
      if (s_sweep_max_bytes > 0 && s_sweep_bytes > s_sweep_max_bytes) {
         OLOG_ERROR("code_git: working-tree size exceeds cap %zu", s_sweep_max_bytes);
         s_sweep_failed = 1;
      }
   }
   return 0;
}

/* libgit2's global init/shutdown is a process-wide refcount; pairing it
 * per-clone would tear down library state for any other user. Call these once
 * at service start/stop instead (sec-S6). */
void code_project_git_global_init(void) {
   git_libgit2_init();
   /* Bound network ops so a hung/slow git server can't pin the import worker
    * (the probe + clone both run there). */
   git_libgit2_opts(GIT_OPT_SET_SERVER_CONNECT_TIMEOUT, 10000); /* ms */
   git_libgit2_opts(GIT_OPT_SET_SERVER_TIMEOUT, 30000);         /* ms */
}

/* Lightweight "does this repo exist / is it reachable" check — the in-process
 * equivalent of `git ls-remote`: negotiate refs without transferring objects or
 * touching disk. Redirects are disabled (same SSRF stance as the clone), so a
 * validated host can't bounce us to an internal target. Returns SUCCESS when the
 * remote is reachable and advertises refs, FAILURE for not-found / unreachable /
 * auth-required (Phase 1 is public repos). */
int code_project_git_remote_probe(const char *url) {
   if (url == NULL || url[0] == '\0') {
      return FAILURE;
   }
   git_remote *remote = NULL;
   if (git_remote_create_detached(&remote, url) != 0) {
      return FAILURE;
   }
   git_remote_connect_options opts;
   git_remote_connect_options_init(&opts, GIT_REMOTE_CONNECT_OPTIONS_VERSION);
   opts.follow_redirects = GIT_REMOTE_REDIRECT_NONE;
   int rc = git_remote_connect_ext(remote, GIT_DIRECTION_FETCH, &opts);
   if (rc == 0) {
      git_remote_disconnect(remote);
   }
   git_remote_free(remote);
   return (rc == 0) ? SUCCESS : FAILURE;
}

void code_project_git_global_shutdown(void) {
   git_libgit2_shutdown();
}

static int remove_cb(const char *path, const struct stat *sb, int typeflag, struct FTW *ftw) {
   (void)sb;
   (void)typeflag;
   (void)ftw;
   remove(path);
   return 0;
}

int code_project_git_remove(const char *local_path) {
   if (local_path == NULL || local_path[0] == '\0') {
      return FAILURE;
   }
   /* FTW_DEPTH: visit children before the directory so remove() succeeds. */
   nftw(local_path, remove_cb, GIT_SWEEP_MAX_FDS, FTW_DEPTH | FTW_PHYS);
   return SUCCESS;
}

int code_project_git_clone(const code_git_clone_opts_t *opts) {
   if (opts == NULL || opts->source_url == NULL || opts->local_path == NULL) {
      return FAILURE;
   }

   /* Ensure a clean target dir: a stale/partial tree from a prior aborted clone
    * (or a delete that raced this import) would make git_clone fail (sec-S7). */
   code_project_git_remove(opts->local_path);

   git_cb_ctx_t ctx = { .opts = opts, .last_transfer_pct = -1, .last_checkout_pct = -1 };
   git_clone_options co;
   git_clone_options_init(&co, GIT_CLONE_OPTIONS_VERSION);
   co.fetch_opts.callbacks.transfer_progress = transfer_progress_cb;
   co.fetch_opts.callbacks.payload = &ctx;
   /* The SSRF/allowlist check validated the original URL's host; refuse to let
    * libgit2 follow a redirect to an unvalidated (possibly internal) host (sec-S2). */
   co.fetch_opts.follow_redirects = GIT_REMOTE_REDIRECT_NONE;
   co.checkout_opts.checkout_strategy = GIT_CHECKOUT_SAFE;
   co.checkout_opts.progress_cb = checkout_progress_cb;
   co.checkout_opts.progress_payload = &ctx;
   if (opts->clone_depth > 0) {
      co.fetch_opts.depth = opts->clone_depth;
   }
   if (opts->branch != NULL && opts->branch[0] != '\0') {
      co.checkout_branch = opts->branch;
   }

   git_repository *repo = NULL;
   int err = git_clone(&repo, opts->source_url, opts->local_path, &co);
   if (repo != NULL) {
      git_repository_free(repo);
   }

   if (err < 0) {
      const git_error *e = git_error_last();
      char redacted[256];
      char redacted_url[256];
      redact_credentials(redacted, sizeof(redacted),
                         (e != NULL && e->message != NULL) ? e->message : "clone failed");
      redact_credentials(redacted_url, sizeof(redacted_url), opts->source_url);
      OLOG_ERROR("code_git: clone of '%s' failed: %s", redacted_url, redacted);
      code_project_git_remove(opts->local_path); /* drop any partial tree */
      return FAILURE;
   }

   /* Defense in depth: strip symlinks, enforce the path-depth cap, and tally the
    * authoritative on-disk size/file count against the caps (sec-S5). */
   s_sweep_max_depth = opts->max_path_depth;
   s_sweep_max_bytes = opts->max_size_bytes;
   s_sweep_max_files = opts->max_file_count;
   s_sweep_bytes = 0;
   s_sweep_files = 0;
   s_sweep_failed = 0;
   nftw(opts->local_path, sweep_cb, GIT_SWEEP_MAX_FDS, FTW_PHYS);
   if (s_sweep_failed) {
      OLOG_ERROR("code_git: post-clone sweep failed; removing clone at %s", opts->local_path);
      code_project_git_remove(opts->local_path);
      return FAILURE;
   }

   return SUCCESS;
}
