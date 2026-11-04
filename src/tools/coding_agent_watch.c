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
 * The coding-agent watcher: follows the agent gateway's events and carries
 * them onto DAWN's coding jobs.  Every event is checked against its job row
 * (a coding job of the event's user).  Ends are applied once; a step replayed
 * after a lost cursor is stored again, with the gateway's number (gw_seq) to
 * tell.  The gateway runs tasks through a DAWN restart: at start the watcher
 * first reads the events it missed, then checks the jobs still open.
 */

#include "tools/coding_agent_watch.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/conv_event.h"
#include "core/event_payload.h"
#include "core/job_manager.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/coding_agent_gateway.h"
#include "tools/mcp_bridge.h"
#include "utils/string_utils.h"

#define CW_WAIT_S 20           /* each long poll (the gateway's limit is 25) */
#define CW_POLL_MARGIN_S 5     /* a long poll ends this much inside the request timeout */
#define CW_SERVER_TIMEOUT_S 30 /* [[mcp.server]] request_timeout_seconds when unset */
#define CW_BACKOFF_S 30        /* after the gateway didn't answer; doubled each time */
#define CW_BACKOFF_MAX_S 300
#define CW_RECANCEL_MAX 32                  /* jobs whose task was asked to stop again */
#define CW_SCAN_MAX 64                      /* open coding jobs checked at start */
#define CW_NOTICE_MAX (CONV_TITLE_MAX + 64) /* the title plus DAWN's words */
#define CW_CURSOR_SUFFIX ".cursor"
#define CW_TMP_SUFFIX ".XXXXXX"
#define CW_PATH_MAX \
   (CONFIG_PATH_MAX + MCP_SERVER_ALIAS_MAX + sizeof("/coding_agent_") + sizeof(CW_CURSOR_SUFFIX))
#define CW_SEQ_TEXT_MAX 24 /* an int64 and a newline */

static pthread_t s_thread;
static bool s_running;
static atomic_bool s_stop;
static pthread_mutex_t s_wait_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_wait_cond; /* on CLOCK_MONOTONIC, made at start */
static char s_alias[MCP_SERVER_ALIAS_MAX];
static time_t s_started_at;

/* The watcher thread's own (and a test's): no lock. */
static int64_t s_recancelled[CW_RECANCEL_MAX];
static int s_recancel_next;

/* ========== Events onto jobs ========== */

static const char *const k_step_fields[] = { "tool", "brief", "text", NULL };
static const char *const k_request_fields[] = { "request_id", "request_kind", "value",
                                                "tool",       "input",        "note",
                                                "error",      "decision",     "request_ids",
                                                NULL };
static const char *const k_end_fields[] = { "summary",     "branch", "commits",  "diffstat",
                                            "uncommitted", "error",  "cost_usd", NULL };
static const char *const k_scrubbed[] = { "brief", "text", "value", "input", NULL };

/* The gateway's kinds DAWN keeps, and where; the rest (started, resumed,
 * run_end, published, ...) change nothing on a job. */
static const struct {
   const char *gw;
   const char *kind;
   const char *const *fields;
} k_kinds[] = {
   { "tool", CONV_EVENT_AGENT_STEP, k_step_fields },
   { "progress", CONV_EVENT_AGENT_STEP, k_step_fields },
   { "request", CONV_EVENT_AGENT_REQUEST, k_request_fields },
   { "reply", CONV_EVENT_AGENT_REQUEST, k_request_fields },
   { "blocked", CONV_EVENT_AGENT_REQUEST, k_request_fields },
   { "input_required", CONV_EVENT_AGENT_REQUEST, k_request_fields },
   { "completed", CONV_EVENT_AGENT_END, k_end_fields },
   { "failed", CONV_EVENT_AGENT_END, k_end_fields },
   { "cancelled", CONV_EVENT_AGENT_END, k_end_fields },
};

static bool is_end(const char *kind) {
   return strcmp(kind, "completed") == 0 || strcmp(kind, "failed") == 0 ||
          strcmp(kind, "cancelled") == 0;
}

/* Kinds that mean the agent is still at work. */
static bool is_working(const char *kind) {
   return strcmp(kind, "tool") == 0 || strcmp(kind, "progress") == 0 ||
          strcmp(kind, "request") == 0 || strcmp(kind, "input_required") == 0 ||
          strcmp(kind, "started") == 0 || strcmp(kind, "resumed") == 0;
}

/* Ask the gateway once more to stop job @p job's task: DAWN ended or dropped
 * the job, yet its task still reports.  Once per job while it stays in the
 * table's last CW_RECANCEL_MAX. */
static void recancel(int user_id, int64_t job) {
   for (int i = 0; i < CW_RECANCEL_MAX; i++) {
      if (s_recancelled[i] == job) {
         return;
      }
   }
   s_recancelled[s_recancel_next] = job;
   s_recancel_next = (s_recancel_next + 1) % CW_RECANCEL_MAX;
   OLOG_INFO("coding_agent: job %lld has ended here; asking the gateway to stop its task",
             (long long)job);
   coding_gateway_cancel(user_id, job);
}

/* The job's end from the gateway's word for it: done, or failed with why.  A
 * cancel DAWN didn't ask for (DAWN ends those itself) is a failure to the user. */
static void end_job(int64_t job, int user_id, const char *kind) {
   if (strcmp(kind, "completed") == 0) {
      job_manager_set_terminal_if_active(job, user_id, "done", NULL, time(NULL), 0);
   } else if (strcmp(kind, "failed") == 0) {
      job_manager_set_terminal_if_active(job, user_id, "failed", "the agent stopped with an error",
                                         time(NULL), 0);
   } else {
      job_manager_set_terminal_if_active(job, user_id, "failed", "stopped at the gateway",
                                         time(NULL), 0);
   }
}

/* Whether job @p job's task still waits for the user (a replayed notice of a
 * question already answered says nothing). */
static bool still_waiting(int user_id, int64_t job) {
   char ref[CODING_AGENT_REF_MAX];
   coding_gateway_ref(job, ref, sizeof(ref));
   struct json_object *args = json_object_new_object();
   json_object_object_add(args, "client_ref", json_object_new_string(ref));
   struct json_object *ans = NULL;
   char *err = NULL;
   bool waiting = true; /* unknown: say it */
   if (coding_gateway_call(user_id, "task_list", args, &ans, &err) == CODING_GW_OK) {
      struct json_object *pending = json_object_object_get(ans, "pending");
      waiting = json_object_is_type(pending, json_type_array) &&
                json_object_array_length(pending) > 0;
   }
   json_object_put(ans);
   free(err);
   return waiting;
}

void coding_watch_apply(struct json_object *event) {
   const char *kind = coding_gateway_jstr(event, "kind");
   if (coding_gateway_jstr(event, "task_id")[0] == '\0') {
      if (strcmp(kind, "update") == 0) {
         OLOG_INFO("coding_agent: the gateway's Claude Code update: %s",
                   coding_gateway_jstr(json_object_object_get(event, "data"), "result"));
      }
      return; /* a system event: nobody's job */
   }
   const int64_t job = coding_gateway_ref_job(coding_gateway_jstr(event, "client_ref"));
   struct json_object *juser = NULL;
   const int user_id = json_object_object_get_ex(event, "dawn_user_id", &juser)
                           ? json_object_get_int(juser)
                           : 0;
   if (job <= 0 || user_id <= 0) {
      return; /* a task DAWN didn't start */
   }
   job_record_t rec;
   const int rc = conv_db_job_get(job, user_id, &rec);
   if (rc == AUTH_DB_NOT_FOUND) {
      if (is_working(kind)) {
         recancel(user_id, job); /* its job was deleted */
      }
      return;
   }
   if (rc != AUTH_DB_SUCCESS || strcmp(rec.job_kind, JOB_KIND_CODING) != 0) {
      return; /* not this user's coding job */
   }
   if (strcmp(rec.job_status, "queued") != 0 && strcmp(rec.job_status, "running") != 0) {
      if (is_working(kind)) {
         recancel(user_id, job); /* DAWN ended it; its task runs on */
      }
      return;
   }

   for (size_t i = 0; i < sizeof(k_kinds) / sizeof(k_kinds[0]); i++) {
      if (strcmp(kind, k_kinds[i].gw) == 0) {
         struct json_object *jseq = json_object_object_get(event, "seq");
         struct json_object *jts = json_object_object_get(event, "ts"); /* when it happened */
         conv_event_emit(job, user_id, k_kinds[i].kind,
                         event_payload_agent(kind, json_object_get_int64(jseq),
                                             (int64_t)json_object_get_double(jts),
                                             json_object_object_get(event, "data"),
                                             k_kinds[i].fields, k_scrubbed));
         break;
      }
   }
   if (strcmp(kind, "input_required") == 0 && still_waiting(user_id, job)) {
      char text[CW_NOTICE_MAX];
      snprintf(text, sizeof(text), "Coding job #%lld (%s) is waiting for your answer.",
               (long long)job, rec.title);
      job_notify_user(user_id, text, job, 0);
   } else if (is_end(kind)) {
      end_job(job, user_id, kind);
   }
}

/* ========== The cursor ========== */

/* The cursor file for the gateway's alias, any byte outside [A-Za-z0-9_-]
 * written as '_'. */
static bool cursor_path(char *out, size_t len) {
   const char *dir = g_config.paths.data_dir_resolved;
   if (dir[0] == '\0') {
      return false;
   }
   char name[MCP_SERVER_ALIAS_MAX];
   safe_strscpy(name, s_alias);
   for (char *c = name; *c; c++) {
      if (strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-", *c) == NULL) {
         *c = '_';
      }
   }
   const int n = snprintf(out, len, "%s/coding_agent_%s" CW_CURSOR_SUFFIX, dir, name);
   return n > 0 && (size_t)n < len;
}

static int64_t cursor_load(void) {
   char path[CW_PATH_MAX];
   int64_t seq = 0;
   FILE *f = cursor_path(path, sizeof(path)) ? fopen(path, "r") : NULL;
   if (f != NULL) {
      if (fscanf(f, "%" SCNd64, &seq) != 1 || seq < 0) {
         seq = 0;
      }
      fclose(f);
   }
   return seq;
}

/* Written whole or not at all (a temporary file renamed over it), mode 600. */
static void cursor_save(int64_t seq) {
   char path[CW_PATH_MAX], tmp[CW_PATH_MAX + sizeof(CW_TMP_SUFFIX)];
   if (!cursor_path(path, sizeof(path))) {
      OLOG_WARNING("coding_agent: no data directory to keep the event cursor in");
      return;
   }
   snprintf(tmp, sizeof(tmp), "%s" CW_TMP_SUFFIX, path);
   const int fd = mkstemp(tmp);
   if (fd < 0) {
      OLOG_WARNING("coding_agent: can't save the event cursor: %s", strerror(errno));
      return;
   }
   char line[CW_SEQ_TEXT_MAX];
   const int n = snprintf(line, sizeof(line), "%" PRId64 "\n", seq);
   const bool ok = write(fd, line, (size_t)n) == n && fsync(fd) == 0;
   close(fd);
   if (!ok || rename(tmp, path) != 0) {
      OLOG_WARNING("coding_agent: can't save the event cursor");
      unlink(tmp);
   }
}

/* ========== Open jobs against the gateway ========== */

/* One open coding job, after the missed events: what the stream can't say. */
static int reconcile_job(const job_record_t *rec) {
   char ref[CODING_AGENT_REF_MAX];
   coding_gateway_ref(rec->id, ref, sizeof(ref));
   struct json_object *args = json_object_new_object();
   json_object_object_add(args, "client_ref", json_object_new_string(ref));
   struct json_object *ans = NULL;
   char *err = NULL;
   const int rc = coding_gateway_call(rec->user_id, "task_list", args, &ans, &err);
   if (rc == CODING_GW_REFUSED) {
      OLOG_WARNING("coding_agent: job %lld: %s", (long long)rec->id, err ? err : "refused");
   }
   free(err);
   if (rc != CODING_GW_OK) {
      return rc == CODING_GW_REFUSED ? SUCCESS : FAILURE; /* refused: left as it is */
   }
   struct json_object *task = json_object_object_get(ans, "task");
   const bool queued = strcmp(rec->job_status, "queued") == 0;
   if (!json_object_is_type(task, json_type_object)) {
      /* No task.  One still starting (DAWN stopped mid-start) is stopped when it
       * exists; either way the job is over. */
      job_manager_set_terminal_if_active(rec->id, rec->user_id, "failed",
                                         queued ? "it never reached the gateway"
                                                : "the gateway has no record of it",
                                         time(NULL), 0);
      coding_gateway_cancel(rec->user_id, rec->id);
   } else if (is_end(coding_gateway_jstr(task, "status"))) {
      end_job(rec->id, rec->user_id,
              coding_gateway_jstr(task, "status")); /* its end event is gone */
   } else if (queued) {
      /* It started, but DAWN stopped before recording it: the start path that
       * would have is gone, so the watcher does. */
      conv_db_job_set_running(rec->id, time(NULL));
      job_update_emit(rec->id, rec->user_id);
   }
   json_object_put(ans);
   return SUCCESS;
}

static int reconcile(void) {
   job_record_t *rows = calloc(CW_SCAN_MAX, sizeof(*rows));
   int n = 0;
   if (rows == NULL ||
       conv_db_job_scan_active_kind(JOB_KIND_CODING, rows, CW_SCAN_MAX, &n) != AUTH_DB_SUCCESS) {
      OLOG_WARNING("coding_agent: watcher: can't read the open coding jobs");
      free(rows);
      return FAILURE;
   }
   int rc = SUCCESS;
   for (int i = 0; i < n && !atomic_load(&s_stop); i++) {
      /* A job made since the watcher started is in its start path right now. */
      if (rows[i].created_at < s_started_at && reconcile_job(&rows[i]) != SUCCESS) {
         rc = FAILURE; /* the others still get their turn */
      }
   }
   free(rows);
   return rc;
}

/* ========== The thread ========== */

/* Wait @p seconds, or until stopped. */
static void pause_for(int seconds) {
   struct timespec until;
   clock_gettime(CLOCK_MONOTONIC, &until);
   until.tv_sec += seconds;
   pthread_mutex_lock(&s_wait_mutex);
   while (!atomic_load(&s_stop) &&
          pthread_cond_timedwait(&s_wait_cond, &s_wait_mutex, &until) != ETIMEDOUT) {
   }
   pthread_mutex_unlock(&s_wait_mutex);
}

/* The long poll's wait, inside the gateway server's request timeout. */
static int poll_wait_s(void) {
   int timeout = CW_SERVER_TIMEOUT_S;
   for (int i = 0; i < g_config.mcp.server_count && i < MCP_SERVERS_MAX; i++) {
      if (strcmp(g_config.mcp.servers[i].alias, s_alias) == 0 &&
          g_config.mcp.servers[i].request_timeout_seconds > 0) {
         timeout = g_config.mcp.servers[i].request_timeout_seconds;
      }
   }
   const int wait = timeout - CW_POLL_MARGIN_S;
   return wait < 1 ? 1 : (wait < CW_WAIT_S ? wait : CW_WAIT_S);
}

/* One batch of events after @p *cursor, waiting up to @p wait_s; *cursor moves
 * past what was applied.  SUCCESS with *got the number of events and *reset
 * whether the gateway's events started over, or FAILURE (no answer). */
static int poll_once(int64_t *cursor, int wait_s, size_t *got, bool *reset_out) {
   struct json_object *args = json_object_new_object();
   json_object_object_add(args, "after_seq", json_object_new_int64(*cursor));
   json_object_object_add(args, "wait_s", json_object_new_int(wait_s));
   struct json_object *ans = NULL;
   char *err = NULL;
   *got = 0;
   *reset_out = false;
   if (coding_gateway_call(0, "task_events", args, &ans, &err) != CODING_GW_OK) {
      OLOG_INFO("coding_agent: watcher: %s", err ? err : "no answer");
      free(err);
      return FAILURE;
   }
   struct json_object *events = json_object_object_get(ans, "events");
   const size_t n = json_object_is_type(events, json_type_array) ? json_object_array_length(events)
                                                                 : 0;
   int64_t applied = *cursor;
   size_t i = 0;
   for (; i < n && !atomic_load(&s_stop); i++) {
      struct json_object *ev = json_object_array_get_idx(events, i);
      const int64_t seq = json_object_get_int64(json_object_object_get(ev, "seq"));
      if (seq > applied) { /* ascending in a batch; one applied is never applied again */
         coding_watch_apply(ev);
         applied = seq;
      }
   }
   struct json_object *jlast = NULL;
   bool reset = false;
   if (n == 0 && json_object_object_get_ex(ans, "last_seq", &jlast) &&
       json_object_is_type(jlast, json_type_int) && json_object_get_int64(jlast) < *cursor) {
      OLOG_WARNING("coding_agent: the gateway's events started over; reading from the start");
      applied = 0;
      reset = true;
   }
   json_object_put(ans);
   if (n > 0 && applied == *cursor && i == n) {
      OLOG_WARNING("coding_agent: watcher: the gateway's events carry no new numbers");
      return FAILURE; /* the same batch again would repeat it */
   }
   if (applied != *cursor) {
      *cursor = applied;
      cursor_save(applied);
   }
   *got = n;
   *reset_out = reset;
   return SUCCESS;
}

static void *watch_thread(void *arg) {
   (void)arg;
   int64_t cursor = cursor_load();
   bool caught_up = false, reconciled = false;
   int backoff = CW_BACKOFF_S;
   struct timespec tried = { 0, 0 }; /* the last reconcile, CLOCK_MONOTONIC */
   while (!atomic_load(&s_stop)) {
      size_t got = 0;
      bool reset = false;
      int rc = poll_once(&cursor, caught_up ? poll_wait_s() : 0, &got, &reset);
      if (reset) {
         /* Its store was likely wiped: read it all again, then check the open jobs
          * again (their tasks may be gone). */
         caught_up = reconciled = false;
         tried.tv_sec = 0;
      } else if (rc == SUCCESS && !caught_up && got == 0) {
         caught_up = true; /* the events missed while DAWN was down are in */
      }
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (rc == SUCCESS && caught_up && !reconciled &&
          (tried.tv_sec == 0 || now.tv_sec - tried.tv_sec >= CW_BACKOFF_MAX_S)) {
         tried = now;
         reconciled = reconcile() == SUCCESS; /* not all reached: again in a while */
      }
      if (rc != SUCCESS) {
         pause_for(backoff);
         backoff = backoff * 2 > CW_BACKOFF_MAX_S ? CW_BACKOFF_MAX_S : backoff * 2;
      } else {
         backoff = CW_BACKOFF_S;
      }
   }
   return NULL;
}

int coding_watch_start(void) {
   const char *alias = coding_gateway_alias();
   if (alias == NULL || s_running) {
      return SUCCESS;
   }
   safe_strscpy(s_alias, alias);
   s_started_at = time(NULL);
   atomic_store(&s_stop, false);
   pthread_condattr_t attr;
   pthread_condattr_init(&attr);
   pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
   pthread_cond_init(&s_wait_cond, &attr);
   pthread_condattr_destroy(&attr);
   if (pthread_create(&s_thread, NULL, watch_thread, NULL) != 0) {
      OLOG_ERROR("coding_agent: couldn't start the watcher");
      pthread_cond_destroy(&s_wait_cond);
      return FAILURE;
   }
   s_running = true;
   return SUCCESS;
}

void coding_watch_stop(void) {
   if (!s_running) {
      return;
   }
   atomic_store(&s_stop, true);
   pthread_mutex_lock(&s_wait_mutex);
   pthread_cond_broadcast(&s_wait_cond);
   pthread_mutex_unlock(&s_wait_mutex);
   mcp_bridge_interrupt(s_alias); /* cuts its long poll short */
   pthread_join(s_thread, NULL);
   pthread_cond_destroy(&s_wait_cond);
   s_running = false;
}
