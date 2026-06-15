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
 * Code-projects orchestrator. A single dedicated thread drains an import/refresh
 * queue (serialized so the non-reentrant clone sweep is safe) and runs
 * clone -> index -> ready, updating code_projects status. Imports are validated
 * (name charset, HTTPS scheme, SSRF + host allowlist) before any work starts.
 */

#define _GNU_SOURCE

#include "tools/code_project_service.h"

#include <ctype.h>
#include <curl/curl.h>
#include <pthread.h>
#include <regex.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/code_graph_provider.h"
#include "tools/code_project_db.h"
#include "tools/code_project_git.h"
#include "tools/code_project_namemap.h"
#include "tools/url_fetcher.h"

#define CP_JOB_QUEUE_MAX 32
#define CP_INDEX_MODE_DEFAULT "full"

typedef enum {
   CP_JOB_IMPORT,  /* pending import: probe → create row → clone → index */
   CP_JOB_REFRESH, /* existing row: fetch (clone kind) + incremental re-index */
   CP_JOB_REBUILD, /* existing row: fetch (clone kind) + delete graph + re-index */
   CP_JOB_LINK     /* pending link: validated local path → create row → index */
} cp_job_op_t;
typedef struct {
   cp_job_op_t op;
   int64_t project_id; /* REFRESH / REBUILD: the existing row */
   /* IMPORT/LINK carry their params until the worker creates the row (so a bad
    * URL/path never produces a phantom row). */
   char source_url[CODE_PROJECT_URL_MAX];
   char name[CODE_PROJECT_NAME_MAX];
   char branch[CODE_PROJECT_BRANCH_MAX];   /* IMPORT: branch to clone ("" = HEAD) */
   char local_path[CODE_PROJECT_PATH_MAX]; /* LINK: validated absolute repo path */
   char kind[CODE_PROJECT_KIND_MAX];       /* "" → clone; LINK sets "local" */
   int64_t user_id;                        /* requester; also the project owner unless global */
   bool global;
} cp_job_t;

static cp_job_t s_jobs[CP_JOB_QUEUE_MAX];
static int s_job_head;
static int s_job_count;
static pthread_mutex_t s_job_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_job_cv = PTHREAD_COND_INITIALIZER;
static pthread_t s_worker;
static bool s_worker_running;
static volatile sig_atomic_t s_shutdown;

/* Weak default; the WebUI provides a strong override that pushes a WS event. */
void code_project_broadcast_status_changed(int64_t project_id) __attribute__((weak));
void code_project_broadcast_status_changed(int64_t project_id) {
   (void)project_id;
}

/* Weak default; the WebUI strong override toasts the requesting user when a
 * pending import is rejected before any row is created. */
void code_project_broadcast_import_failed(int64_t user_id, const char *name, const char *reason)
    __attribute__((weak));
void code_project_broadcast_import_failed(int64_t user_id, const char *name, const char *reason) {
   (void)user_id;
   (void)name;
   (void)reason;
}

/* --------------------------------------------------------------------------
 * Validation
 * -------------------------------------------------------------------------- */

/* True if @p resolved (an already-realpath'd absolute dir) is equal to, or nested
 * under, one of the configured allowed_local_roots. Trailing '/' on a root is
 * tolerated; the boundary check requires a '/' separator so "/srv/codex" does not
 * match an allowed "/srv/code". */
static bool path_within_allowed(const char *resolved) {
   const dawn_config_t *cfg = config_get();
   if (cfg == NULL) {
      return false;
   }
   for (int i = 0; i < cfg->code_projects.allowed_local_roots_count; i++) {
      /* Canonicalize the configured root so a symlinked root component (e.g.
       * /home -> /mnt/home) still matches a realpath'd candidate; fall back to the
       * literal value if it can't resolve (e.g. the dir doesn't exist yet). */
      char *canon = realpath(cfg->code_projects.allowed_local_roots[i], NULL);
      char root[CONFIG_PATH_MAX];
      snprintf(root, sizeof(root), "%s",
               canon != NULL ? canon : cfg->code_projects.allowed_local_roots[i]);
      free(canon);
      size_t rl = strlen(root);
      while (rl > 1 && root[rl - 1] == '/') {
         root[--rl] = '\0';
      }
      if (rl == 0) {
         continue;
      }
      if (strcmp(resolved, root) == 0 ||
          (strncmp(resolved, root, rl) == 0 && resolved[rl] == '/')) {
         return true;
      }
   }
   return false;
}

/* Resolve a link path and validate it: realpath → fits → directory → inside an
 * allowed root → a git repo (NO_SEARCH). Writes the canonical absolute path to
 * @p out on SUCCESS. realpath(.,NULL) returns a heap buffer (unknown static size),
 * which also keeps the copy into @p out free of a -Wformat-truncation warning. */
static int resolve_link_path(const char *local_path, char *out, size_t out_sz) {
   char *resolved = realpath(local_path, NULL);
   if (resolved == NULL) {
      OLOG_WARNING("code_project: link path does not resolve");
      return FAILURE;
   }
   int rc = FAILURE;
   struct stat st;
   if (strlen(resolved) >= out_sz) {
      OLOG_WARNING("code_project: link path too long");
   } else if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) {
      OLOG_WARNING("code_project: link path is not a directory");
   } else if (!path_within_allowed(resolved)) {
      OLOG_WARNING("code_project: link path is outside allowed_local_roots");
   } else if (code_project_git_open_validate(resolved) != SUCCESS) {
      OLOG_WARNING("code_project: link path is not a git repository (NO_SEARCH)");
   } else {
      snprintf(out, out_sz, "%s", resolved);
      rc = SUCCESS;
   }
   free(resolved);
   return rc;
}

/* Name charset: ^[A-Za-z0-9][A-Za-z0-9_-]{0,62}$ (also blocks '/' and "..", so
 * the derived local_path stays inside source_root). Mixed case is allowed so
 * repo names like "Hello-World" import without mangling. */
static bool valid_name(const char *name) {
   if (name == NULL) {
      return false;
   }
   size_t n = strlen(name);
   if (n == 0 || n > 63) {
      return false;
   }
   if (!isalnum((unsigned char)name[0])) {
      return false;
   }
   for (size_t i = 1; i < n; i++) {
      char c = name[i];
      if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) {
         return false;
      }
   }
   return true;
}

/* HTTPS scheme + not SSRF-blocked + host matches the configured allowlist. */
static bool valid_url(const char *url, const char *allowed_host_pattern) {
   if (url == NULL || strncmp(url, "https://", 8) != 0) {
      return false;
   }
   if (url_is_blocked(url)) {
      OLOG_WARNING("code_project: URL blocked by SSRF guard: %s", url);
      return false;
   }

   CURLU *h = curl_url();
   if (h == NULL) {
      return false;
   }
   bool ok = false;
   char *host = NULL;
   char *user = NULL;
   if (curl_url_set(h, CURLUPART_URL, url, 0) == CURLUE_OK &&
       curl_url_get(h, CURLUPART_HOST, &host, 0) == CURLUE_OK && host != NULL) {
      if (curl_url_get(h, CURLUPART_USER, &user, 0) == CURLUE_OK && user != NULL) {
         /* Reject embedded credentials (https://user:pass@host/...): public-repo
          * clones never need userinfo, and it would leak into logs/DB (sec-S2). */
         OLOG_WARNING("code_project: URL with embedded credentials rejected");
      } else if (allowed_host_pattern == NULL || allowed_host_pattern[0] == '\0') {
         ok = true; /* no allowlist configured */
      } else {
         /* Anchor the operator pattern so 'github\.com' can't match
          * 'github.com.evil.net' or 'notgithub.com' (sec-S2). */
         char anchored[256];
         snprintf(anchored, sizeof(anchored), "^(%s)$", allowed_host_pattern);
         regex_t re;
         int crc = regcomp(&re, anchored, REG_EXTENDED | REG_NOSUB);
         if (crc == 0) {
            ok = (regexec(&re, host, 0, NULL, 0) == 0);
            regfree(&re);
         } else {
            /* Fail closed (ok stays false), but make the misconfiguration
             * diagnosable — otherwise every import silently "fails the allowlist". */
            OLOG_ERROR("code_project: allowed_host_pattern failed to compile (rc=%d) — "
                       "rejecting all imports until fixed",
                       crc);
         }
         if (!ok) {
            OLOG_WARNING("code_project: host '%s' not in allowlist", host);
         }
      }
   }
   curl_free(user);
   curl_free(host);
   curl_url_cleanup(h);
   return ok;
}

/* --------------------------------------------------------------------------
 * Worker
 * -------------------------------------------------------------------------- */

static void set_status(int64_t id, const char *status, const char *msg) {
   code_project_db_update_status(id, status, msg);
   code_project_broadcast_status_changed(id);
}

static void worker_do_index(const code_project_t *p) {
   /* Distinguish "no code server connected" from a real index failure: without
    * this, a clone with cbm-mcp absent reports a bare "indexing failed" that
    * reads like a bug rather than a missing backend. */
   if (code_graph_provider_cbm.is_available != NULL &&
       code_graph_provider_cbm.is_available() != SUCCESS) {
      set_status(p->id, "error",
                 "clone ready, but no code server connected — start cbm-mcp, then re-index");
      return;
   }
   set_status(p->id, "indexing", "");
   int64_t job_id = 0;
   if (code_graph_provider_cbm.index_start(p->name, p->local_path, CP_INDEX_MODE_DEFAULT,
                                           &job_id) != SUCCESS) {
      set_status(p->id, "error", "indexing failed");
      return;
   }
   code_project_db_set_indexed_at(p->id, time(NULL));
   /* Refresh the cbm name-translation map: this may be the first indexed project,
    * in which case the bridge had nothing to capture from at startup. */
   code_project_namemap_capture();
   /* Persist cbm's graph slug so delete/rebuild can target the on-disk .db
    * without a live cbm round-trip (and so it survives a cbm-list outage). Only
    * store a real slug (translation actually applied), never the bare name — and
    * never a truncated one: a partial slug wouldn't match cbm's real graph name,
    * so delete/rebuild would silently no-op (the exact bug this column fixes). On
    * truncation, leave it empty so the delete path falls back to a live namemap
    * translation. */
   char graph[CODE_PROJECT_GRAPH_NAME_MAX];
   code_project_namemap_to_graph(p->name, graph, sizeof(graph));
   if (strlen(graph) >= sizeof(graph) - 1) {
      OLOG_WARNING("code_project: graph slug for '%s' too long to persist; "
                   "delete/rebuild will resolve it live",
                   p->name);
   } else if (graph[0] != '\0' && strcmp(graph, p->name) != 0) {
      code_project_db_set_graph_name(p->id, graph);
   }
   set_status(p->id, "ready", "");
}

static void worker_do_clone(const code_project_t *p) {
   const dawn_config_t *cfg = config_get();
   set_status(p->id, "cloning", "");
   code_git_clone_opts_t co = {
      .source_url = p->source_url,
      .local_path = p->local_path,
      .branch = (p->branch[0] != '\0') ? p->branch : NULL, /* NULL = remote HEAD */
      .max_size_bytes = (size_t)cfg->code_projects.max_repo_size_mb * 1024 * 1024,
      .max_file_count = (uint32_t)cfg->code_projects.max_file_count,
      .max_path_depth = (uint8_t)cfg->code_projects.max_path_depth,
      .clone_depth = cfg->code_projects.clone_depth,
   };
   if (code_project_git_clone(&co) != SUCCESS) {
      set_status(p->id, "error", "clone failed");
      return;
   }
   worker_do_index(p);
}

/* Pending import (no row yet): confirm the remote exists, then create the row and
 * clone+index it. A nonexistent/unreachable repo is rejected with no phantom row
 * left behind (a failure toast is pushed instead). A repo that exists but whose
 * clone is later rejected (size/depth caps) keeps its error row — that's a
 * settings problem the user can fix and refresh. */
static void worker_do_import(const cp_job_t *job) {
   if (code_project_git_remote_probe(job->source_url) != SUCCESS) {
      OLOG_WARNING("code_project: import rejected — repo not found/unreachable");
      code_project_broadcast_import_failed(job->user_id, job->name,
                                           "Repository not found or unreachable.");
      return;
   }

   const dawn_config_t *cfg = config_get();
   code_project_t p;
   memset(&p, 0, sizeof(p));
   snprintf(p.name, sizeof(p.name), "%s", job->name);
   snprintf(p.source_url, sizeof(p.source_url), "%s", job->source_url);
   snprintf(p.local_path, sizeof(p.local_path), "%s/%s", cfg->code_projects.source_root, job->name);
   snprintf(p.branch, sizeof(p.branch), "%s", job->branch); /* "" = remote HEAD */
   snprintf(p.kind, sizeof(p.kind), "%s", CODE_PROJECT_KIND_CLONE);
   p.user_id = job->global ? 0 : job->user_id;
   p.is_global = job->global;
   p.imported_by = job->user_id;
   snprintf(p.status, sizeof(p.status), "cloning");

   int64_t id = 0;
   /* UNIQUE(name) is the authoritative guard against a duplicate that slipped
    * past the pre-enqueue check (concurrent import of the same name). */
   if (code_project_db_create(&p, &id) != AUTH_DB_SUCCESS) {
      OLOG_WARNING("code_project: create failed (duplicate name '%s'?)", job->name);
      code_project_broadcast_import_failed(job->user_id, job->name,
                                           "A project with that name already exists.");
      return;
   }
   p.id = id;
   code_project_broadcast_status_changed(id); /* row is now visible (cloning) */
   worker_do_clone(&p);
}

/* Build fetch options for a clone-kind project from current config. */
static void build_fetch_opts(const code_project_t *p, code_git_fetch_opts_t *fo) {
   const dawn_config_t *cfg = config_get();
   memset(fo, 0, sizeof(*fo));
   fo->local_path = p->local_path;
   fo->branch = p->branch; /* overridden by cp_fetch_tracked when "" */
   fo->clone_depth = cfg->code_projects.clone_depth;
   fo->max_size_bytes = (size_t)cfg->code_projects.max_repo_size_mb * 1024 * 1024;
   fo->max_file_count = (uint32_t)cfg->code_projects.max_file_count;
   fo->max_path_depth = (uint8_t)cfg->code_projects.max_path_depth;
}

/* Fetch + checkout a clone project's tracked branch (or its current branch when
 * none is tracked). A repo with no usable branch (detached/empty) is left as-is
 * and reindexed. @return SUCCESS (incl. the skip case) or FAILURE on a real
 * fetch/checkout error. */
static int cp_fetch_tracked(const code_project_t *p) {
   char branch[CODE_PROJECT_BRANCH_MAX];
   if (p->branch[0] != '\0') {
      snprintf(branch, sizeof(branch), "%s", p->branch);
   } else if (code_project_git_current_branch(p->local_path, branch, sizeof(branch)) != SUCCESS ||
              branch[0] == '\0' || branch[0] == '(') {
      OLOG_WARNING("code_project: '%s' has no tracked branch; reindexing without fetch", p->name);
      return SUCCESS;
   }
   code_git_fetch_opts_t fo;
   build_fetch_opts(p, &fo);
   fo.branch = branch;
   return code_project_git_fetch_checkout(&fo);
}

/* For a local (linked) project, refresh the stored branch from the live working
 * tree (the user/Claude Code controls git here; DAWN never checks out). */
static void cp_sync_local_branch(const code_project_t *p) {
   char cur[CODE_PROJECT_BRANCH_MAX];
   if (code_project_git_current_branch(p->local_path, cur, sizeof(cur)) == SUCCESS &&
       cur[0] != '\0' && strcmp(cur, p->branch) != 0) {
      code_project_db_set_branch(p->id, cur);
   }
}

/* Refresh: cheap incremental re-index. Clone → fetch tracked branch; local →
 * sync the stored branch from the live tree. */
static void worker_do_refresh(const code_project_t *p) {
   if (strcmp(p->kind, CODE_PROJECT_KIND_LOCAL) == 0) {
      cp_sync_local_branch(p);
   } else if (cp_fetch_tracked(p) != SUCCESS) {
      set_status(p->id, "error", "fetch/checkout failed");
      return;
   }
   worker_do_index(p);
}

/* Rebuild: clean re-index. Same git step as refresh, then drop the cbm graph so
 * the re-index is a fresh build (not cbm's incremental update, which would keep
 * stale nodes after a cbm upgrade or a branch switch). */
static void worker_do_rebuild(const code_project_t *p) {
   if (strcmp(p->kind, CODE_PROJECT_KIND_LOCAL) == 0) {
      cp_sync_local_branch(p);
   } else if (cp_fetch_tracked(p) != SUCCESS) {
      set_status(p->id, "error", "fetch/checkout failed");
      return;
   }
   if (code_graph_provider_cbm.is_available != NULL &&
       code_graph_provider_cbm.is_available() == SUCCESS) {
      char graph[CODE_PROJECT_GRAPH_NAME_MAX];
      if (p->graph_name[0] != '\0') {
         snprintf(graph, sizeof(graph), "%s", p->graph_name);
      } else {
         code_project_namemap_to_graph(p->name, graph, sizeof(graph));
      }
      if (graph[0] != '\0') {
         code_graph_provider_cbm.delete_project(graph); /* best-effort */
      }
   }
   worker_do_index(p); /* re-indexes, recaptures, writes graph_name back */
}

/* Pending link (no row yet): the path was already realpath'd, contained, and
 * git-validated on the caller thread, so just create the row and index in place.
 * The working tree is the user's — DAWN only reads it (never clones/removes). */
static void worker_do_link(const cp_job_t *job) {
   code_project_t p;
   memset(&p, 0, sizeof(p));
   snprintf(p.name, sizeof(p.name), "%s", job->name);
   p.source_url[0] = '\0'; /* no remote */
   snprintf(p.local_path, sizeof(p.local_path), "%s", job->local_path);
   snprintf(p.kind, sizeof(p.kind), "%s", CODE_PROJECT_KIND_LOCAL);
   /* Record whatever branch is currently checked out (display only; refresh keeps
    * it in sync). */
   code_project_git_current_branch(p.local_path, p.branch, sizeof(p.branch));
   p.user_id = job->global ? 0 : job->user_id; /* link forbids global (rejected upstream) */
   p.is_global = job->global;
   p.imported_by = job->user_id;
   snprintf(p.status, sizeof(p.status), "indexing");

   int64_t id = 0;
   if (code_project_db_create(&p, &id) != AUTH_DB_SUCCESS) {
      OLOG_WARNING("code_project: link create failed (duplicate name '%s'?)", job->name);
      code_project_broadcast_import_failed(job->user_id, job->name,
                                           "A project with that name already exists.");
      return;
   }
   p.id = id;
   code_project_broadcast_status_changed(id); /* row is now visible */
   worker_do_index(&p);
}

static void process_job(const cp_job_t *job) {
   if (job->op == CP_JOB_IMPORT) {
      worker_do_import(job);
      return;
   }
   if (job->op == CP_JOB_LINK) {
      worker_do_link(job);
      return;
   }
   code_project_t p;
   if (code_project_db_get(job->project_id, &p) != AUTH_DB_SUCCESS) {
      return;
   }
   if (job->op == CP_JOB_REBUILD) {
      worker_do_rebuild(&p);
      return;
   }
   worker_do_refresh(&p); /* CP_JOB_REFRESH */
}

static void *worker_main(void *arg) {
   (void)arg;
   setpriority(PRIO_PROCESS, 0, 10); /* nice 10 — background work */
   pthread_setname_np(pthread_self(), "dawn-code-import");

   for (;;) {
      pthread_mutex_lock(&s_job_mtx);
      while (s_job_count == 0 && !s_shutdown) {
         pthread_cond_wait(&s_job_cv, &s_job_mtx);
      }
      if (s_shutdown) {
         pthread_mutex_unlock(&s_job_mtx);
         break;
      }
      cp_job_t job = s_jobs[s_job_head];
      s_job_head = (s_job_head + 1) % CP_JOB_QUEUE_MAX;
      s_job_count--;
      pthread_mutex_unlock(&s_job_mtx);

      process_job(&job);
   }
   return NULL;
}

static int enqueue_job(const cp_job_t *job) {
   pthread_mutex_lock(&s_job_mtx);
   if (s_job_count >= CP_JOB_QUEUE_MAX) {
      pthread_mutex_unlock(&s_job_mtx);
      OLOG_ERROR("code_project: job queue full");
      return FAILURE;
   }
   int tail = (s_job_head + s_job_count) % CP_JOB_QUEUE_MAX;
   s_jobs[tail] = *job;
   s_job_count++;
   pthread_cond_signal(&s_job_cv);
   pthread_mutex_unlock(&s_job_mtx);
   return SUCCESS;
}

/* --------------------------------------------------------------------------
 * Lifecycle + API
 * -------------------------------------------------------------------------- */

/* Reconcile rows left in a transient state by a crash/restart mid-clone or
 * mid-rebuild. index_repository is synchronous, so an interrupted job leaves no
 * runner to recover from; without this such a row would advertise "indexing"
 * forever (and rebuild may have already dropped its graph). Mark them errored so
 * the user can rebuild to retry. Best-effort; a DB-not-ready failure is a no-op. */
static void recover_interrupted(void) {
   code_project_t rows[CODE_PROJECTS_MAX];
   int n = 0;
   if (code_project_db_list_all(rows, CODE_PROJECTS_MAX, &n) != AUTH_DB_SUCCESS) {
      return;
   }
   for (int i = 0; i < n; i++) {
      if (strcmp(rows[i].status, "indexing") == 0 || strcmp(rows[i].status, "cloning") == 0) {
         OLOG_WARNING("code_project: '%s' was '%s' at shutdown — marking interrupted", rows[i].name,
                      rows[i].status);
         code_project_db_update_status(rows[i].id, "error", "interrupted — rebuild to retry");
      }
   }
}

int code_project_service_init(void) {
   if (s_worker_running) {
      return SUCCESS;
   }
   recover_interrupted();          /* heal rows stuck mid-clone/rebuild from a crash */
   code_project_git_global_init(); /* libgit2 refcount: once per process (sec-S6) */
   s_shutdown = 0;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   /* 512 KB: libgit2's checkout + pack-index call chains have deep frames on
    * ARM64; 256 KB risked a silent overflow on large repos (eff-E1). */
   pthread_attr_setstacksize(&attr, 512 * 1024);
   int rc = pthread_create(&s_worker, &attr, worker_main, NULL);
   pthread_attr_destroy(&attr);
   if (rc != 0) {
      OLOG_ERROR("code_project: failed to start import worker");
      code_project_git_global_shutdown(); /* undo the init above */
      return FAILURE;
   }
   s_worker_running = true;
   return SUCCESS;
}

void code_project_service_shutdown(void) {
   if (!s_worker_running) {
      return;
   }
   pthread_mutex_lock(&s_job_mtx);
   s_shutdown = 1;
   pthread_cond_signal(&s_job_cv);
   pthread_mutex_unlock(&s_job_mtx);
   pthread_join(s_worker, NULL);
   s_worker_running = false;
   code_project_git_global_shutdown(); /* libgit2 refcount: once per process (sec-S6) */
}

int code_project_import(int64_t requester_user_id,
                        const char *source_url,
                        const char *desired_name,
                        const char *branch,
                        bool global,
                        int64_t *project_id_out) {
   if (project_id_out != NULL) {
      *project_id_out = 0; /* no row exists until the worker confirms the repo */
   }
   const dawn_config_t *cfg = config_get();
   if (cfg == NULL || !cfg->code_projects.enabled) {
      return FAILURE;
   }
   if (!valid_name(desired_name)) {
      OLOG_WARNING("code_project: invalid project name");
      return FAILURE;
   }
   if (!valid_url(source_url, cfg->code_projects.allowed_host_pattern)) {
      return FAILURE;
   }
   /* Fast-path duplicate check for immediate feedback; the worker's db_create
    * UNIQUE(name) is the authoritative guard against a concurrent duplicate. */
   code_project_t existing;
   if (code_project_db_get_by_name(desired_name, &existing) == AUTH_DB_SUCCESS) {
      OLOG_WARNING("code_project: duplicate name '%s'", desired_name);
      return FAILURE;
   }

   /* Defer the remote-existence probe + clone to the worker thread. The caller
    * here is the WebUI lws service thread, which carries live audio and must not
    * block on the network. The DB row is created only once the worker confirms
    * the repo exists (so a typo'd URL leaves no phantom error row). */
   cp_job_t job;
   memset(&job, 0, sizeof(job));
   /* A specified branch is interpolated into git refspecs — validate it (libgit2
    * ref-name rules) rather than trust caller input. Empty/NULL = remote HEAD. */
   if (branch != NULL && branch[0] != '\0' && code_project_git_branch_valid(branch) != SUCCESS) {
      OLOG_WARNING("code_project: invalid branch name");
      return FAILURE;
   }
   job.op = CP_JOB_IMPORT;
   snprintf(job.source_url, sizeof(job.source_url), "%s", source_url);
   snprintf(job.name, sizeof(job.name), "%s", desired_name);
   if (branch != NULL) {
      snprintf(job.branch, sizeof(job.branch), "%s", branch);
   }
   job.user_id = requester_user_id;
   job.global = global;
   return enqueue_job(&job);
}

int code_project_link(int64_t requester_user_id,
                      const char *local_path,
                      const char *desired_name,
                      bool global,
                      int64_t *project_id_out) {
   if (project_id_out != NULL) {
      *project_id_out = 0;
   }
   const dawn_config_t *cfg = config_get();
   if (cfg == NULL || !cfg->code_projects.enabled) {
      return FAILURE;
   }
   /* Linking exposes the tree's file CONTENTS to the LLM — never make a linked
    * repo visible to all users. Owner-visible only in v1. */
   if (global) {
      OLOG_WARNING("code_project: link rejected — global is not allowed for local repos");
      return FAILURE;
   }
   if (!valid_name(desired_name)) {
      OLOG_WARNING("code_project: invalid project name");
      return FAILURE;
   }
   if (local_path == NULL || local_path[0] == '\0') {
      return FAILURE;
   }
   if (cfg->code_projects.allowed_local_roots_count == 0) {
      OLOG_WARNING("code_project: link rejected — no allowed_local_roots configured");
      return FAILURE;
   }
   /* Resolve + contain + git-validate on THIS (caller) thread so a bad path gives
    * synchronous feedback and the worker trusts the enqueued absolute path. */
   cp_job_t job;
   memset(&job, 0, sizeof(job));
   if (resolve_link_path(local_path, job.local_path, sizeof(job.local_path)) != SUCCESS) {
      return FAILURE;
   }
   code_project_t existing;
   if (code_project_db_get_by_name(desired_name, &existing) == AUTH_DB_SUCCESS) {
      OLOG_WARNING("code_project: duplicate name '%s'", desired_name);
      return FAILURE;
   }

   job.op = CP_JOB_LINK;
   snprintf(job.name, sizeof(job.name), "%s", desired_name);
   snprintf(job.kind, sizeof(job.kind), "%s", CODE_PROJECT_KIND_LOCAL);
   job.user_id = requester_user_id;
   job.global = false;
   return enqueue_job(&job);
}

int code_project_refresh(int64_t project_id) {
   code_project_t p;
   if (code_project_db_get(project_id, &p) != AUTH_DB_SUCCESS) {
      return FAILURE;
   }
   cp_job_t job;
   memset(&job, 0, sizeof(job));
   job.op = CP_JOB_REFRESH;
   job.project_id = project_id;
   return enqueue_job(&job);
}

int code_project_rebuild(int64_t project_id) {
   code_project_t p;
   if (code_project_db_get(project_id, &p) != AUTH_DB_SUCCESS) {
      return FAILURE;
   }
   cp_job_t job;
   memset(&job, 0, sizeof(job));
   job.op = CP_JOB_REBUILD;
   job.project_id = project_id;
   return enqueue_job(&job);
}

int code_project_set_branch(int64_t project_id, const char *branch) {
   code_project_t p;
   if (code_project_db_get(project_id, &p) != AUTH_DB_SUCCESS) {
      return FAILURE;
   }
   /* Branch is read-only for a linked local project — it tracks whatever the user
    * has checked out; DAWN must not switch it. */
   if (strcmp(p.kind, CODE_PROJECT_KIND_LOCAL) == 0) {
      OLOG_WARNING("code_project: cannot set branch on linked project '%s'", p.name);
      return FAILURE;
   }
   if (code_project_git_branch_valid(branch) != SUCCESS) {
      OLOG_WARNING("code_project: invalid branch name for '%s'", p.name);
      return FAILURE;
   }
   if (code_project_db_set_branch(project_id, branch != NULL ? branch : "") != AUTH_DB_SUCCESS) {
      return FAILURE;
   }
   /* A branch switch rewrites the code — apply it as a clean rebuild. */
   return code_project_rebuild(project_id);
}

int code_project_delete(int64_t project_id) {
   code_project_t p;
   if (code_project_db_get(project_id, &p) != AUTH_DB_SUCCESS) {
      return FAILURE;
   }
   /* Resolve cbm's graph slug: the persisted graph_name is authoritative; fall
    * back to a namemap translation of the clean name (covers pre-v66 rows / never
    * fully indexed). Empty → skip the cbm delete (best-effort; still tear down
    * the rest). */
   char graph[CODE_PROJECT_GRAPH_NAME_MAX];
   if (p.graph_name[0] != '\0') {
      snprintf(graph, sizeof(graph), "%s", p.graph_name);
   } else {
      code_project_namemap_to_graph(p.name, graph, sizeof(graph));
   }
   if (graph[0] != '\0') {
      code_graph_provider_cbm.delete_project(graph); /* best-effort */
   }
   /* kind guard: only remove a DAWN-managed clone. NEVER touch a linked local
    * working tree (kind=local) — that's the user's live repo. */
   if (strcmp(p.kind, CODE_PROJECT_KIND_LOCAL) != 0) {
      code_project_git_remove(p.local_path);
   }
   int rc = code_project_db_delete(project_id);
   code_project_broadcast_status_changed(project_id);
   return rc == AUTH_DB_SUCCESS ? SUCCESS : FAILURE;
}
