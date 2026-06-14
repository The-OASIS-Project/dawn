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
#include <string.h>
#include <sys/resource.h>
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
   CP_JOB_IMPORT, /* pending import: probe → create row → clone → index */
   CP_JOB_REFRESH /* existing row: re-index */
} cp_job_op_t;
typedef struct {
   cp_job_op_t op;
   int64_t project_id; /* REFRESH: the existing row */
   /* IMPORT carries its params until the worker validates the repo and creates
    * the row (so a nonexistent URL never produces a phantom row). */
   char source_url[CODE_PROJECT_URL_MAX];
   char name[CODE_PROJECT_NAME_MAX];
   int64_t user_id; /* requester; also the project owner unless global */
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
         if (regcomp(&re, anchored, REG_EXTENDED | REG_NOSUB) == 0) {
            ok = (regexec(&re, host, 0, NULL, 0) == 0);
            regfree(&re);
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
   /* Refresh the cbm name-translation prefix: this may be the first indexed
    * project, in which case the bridge had nothing to capture from at startup. */
   code_project_namemap_capture();
   set_status(p->id, "ready", "");
}

static void worker_do_clone(const code_project_t *p) {
   const dawn_config_t *cfg = config_get();
   set_status(p->id, "cloning", "");
   code_git_clone_opts_t co = {
      .source_url = p->source_url,
      .local_path = p->local_path,
      .branch = NULL,
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

static void process_job(const cp_job_t *job) {
   if (job->op == CP_JOB_IMPORT) {
      worker_do_import(job);
      return;
   }
   code_project_t p;
   if (code_project_db_get(job->project_id, &p) != AUTH_DB_SUCCESS) {
      return;
   }
   worker_do_index(&p); /* refresh re-indexes the existing clone */
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

int code_project_service_init(void) {
   if (s_worker_running) {
      return SUCCESS;
   }
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
   job.op = CP_JOB_IMPORT;
   snprintf(job.source_url, sizeof(job.source_url), "%s", source_url);
   snprintf(job.name, sizeof(job.name), "%s", desired_name);
   job.user_id = requester_user_id;
   job.global = global;
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

int code_project_delete(int64_t project_id) {
   code_project_t p;
   if (code_project_db_get(project_id, &p) != AUTH_DB_SUCCESS) {
      return FAILURE;
   }
   code_graph_provider_cbm.delete_project(p.name); /* best-effort */
   code_project_git_remove(p.local_path);
   int rc = code_project_db_delete(project_id);
   code_project_broadcast_status_changed(project_id);
   return rc == AUTH_DB_SUCCESS ? SUCCESS : FAILURE;
}
