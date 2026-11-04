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
 * The report-back on a finished coding job, posted into the conversation the
 * task came from (core/job_report.h, as deep research posts its take).  The
 * take is written in a turn with no tools, so the agent's report (its words
 * about code and pages it read) can shape words there, never actions; a
 * report the injection filter flags, or any other end, gets a fixed line.
 */

#include "tools/coding_agent_report.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "core/job_dispatch.h"
#include "core/job_manager.h"
#include "core/job_report.h"
#include "core/memory_filter.h"
#include "core/session_manager.h"
#include "core/strbuf.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_context_text.h"
#include "llm/llm_third_party.h"
#include "logging.h"
#include "prompts.h"
#include "tools/coding_agent_gateway.h"
#include "tools/tool_registry.h"
#include "utils/string_utils.h"

#define CR_KIND_MAX 16                     /* a gateway end: completed, failed, cancelled */
#define CR_LINE_MAX (CONV_TITLE_MAX + 256) /* a fixed line: the title plus DAWN's words */
#define CR_REPORT_MAX (16 * 1024)          /* the agent's words put to the take */
#define CR_FACTS_MAX 256
#define CR_STOP_WAIT_MS 5000 /* stop's wait for report-backs in flight */
#define CR_STOP_POLL_MS 50

static atomic_bool s_stopping;
static atomic_int s_in_flight;
static pthread_mutex_t s_take_mutex = PTHREAD_MUTEX_INITIALIZER;
static session_t *s_take; /* the take running now, if any (stop cancels it) */

typedef struct {
   int64_t job;
   int user_id;
   char kind[CR_KIND_MAX];
   struct json_object *data; /* the end's facts and the agent's report (owned; may be NULL) */
} cr_work_t;

/* A string field, or a list of strings one per line; "" when absent. */
static char *field_text(struct json_object *data, const char *key) {
   struct json_object *v = json_object_object_get(data, key);
   strbuf_t sb;
   strbuf_init(&sb, 256);
   if (json_object_is_type(v, json_type_array)) {
      for (size_t i = 0; i < json_object_array_length(v); i++) {
         struct json_object *el = json_object_array_get_idx(v, i);
         if (json_object_is_type(el, json_type_string)) {
            strbuf_appendf(&sb, "%s%s", strbuf_len(&sb) ? "\n" : "", json_object_get_string(el));
         }
      }
   } else if (json_object_is_type(v, json_type_string)) {
      strbuf_append(&sb, json_object_get_string(v));
   }
   return strbuf_steal(&sb);
}

/* The agent's words for the take (its report, commits, changed files), cut and
 * framed AGENT CONTENT; NULL when there's no report, or the injection filter
 * flags it (the fixed line then). */
static char *framed_report(struct json_object *data) {
   char *summary = field_text(data, "summary");
   char *commits = field_text(data, "commits");
   char *diffstat = field_text(data, "diffstat");
   char *framed = NULL;
   if (summary && summary[0] != '\0' && !memory_filter_check_injection_commands(summary) &&
       !(commits && memory_filter_check_injection_commands(commits))) {
      strbuf_t sb;
      strbuf_init(&sb, strlen(summary) + 256);
      strbuf_appendf(&sb, "Report:\n%s\n", summary);
      if (commits && commits[0] != '\0') {
         strbuf_appendf(&sb, "Commits:\n%s\n", commits);
      }
      if (diffstat && diffstat[0] != '\0') {
         strbuf_appendf(&sb, "Changed:\n%s\n", diffstat);
      }
      char *body = strbuf_steal(&sb);
      if (body != NULL && strlen(body) > CR_REPORT_MAX) {
         body[CR_REPORT_MAX] = '\0';
         utf8_trim_incomplete(body);
      }
      body = llm_context_neutralize_owned(body);
      framed = body ? llm_third_party_frame(TOOL_FRAME_AGENT, NULL, body) : NULL;
      free(body);
   }
   free(summary);
   free(commits);
   free(diffstat);
   return framed;
}

/* DAWN's facts about the result, only those the gateway sent (the reconcile
 * path's task record has no commit or uncommitted counts). */
static void facts(char *out, size_t len, struct json_object *data) {
   strbuf_t sb;
   strbuf_init(&sb, CR_FACTS_MAX);
   const char *branch = coding_gateway_jstr(data, "branch");
   if (branch[0] != '\0') {
      strbuf_appendf(&sb, "It finished on branch %s.", branch);
   }
   struct json_object *v = NULL;
   if (json_object_object_get_ex(data, "uncommitted", &v) &&
       json_object_is_type(v, json_type_int)) {
      const int n = json_object_get_int(v);
      if (n > 0) {
         strbuf_appendf(&sb, " %d changed file%s left uncommitted.", n, n == 1 ? " was" : "s were");
      } else {
         strbuf_append(&sb, " Nothing was left uncommitted.");
      }
   }
   if (json_object_object_get_ex(data, "cost_usd", &v) && json_object_get_double(v) > 0.0) {
      strbuf_appendf(&sb, " It cost $%.2f.", json_object_get_double(v));
   }
   snprintf(out, len, "%s", strbuf_len(&sb) ? strbuf_str(&sb) : "");
   strbuf_free(&sb);
}

/* The take on a completed job, written with no tools; NULL on any failure. */
static char *write_take(const cr_work_t *w, const job_record_t *rec) {
   char *framed = framed_report(w->data);
   if (framed == NULL) {
      return NULL; /* no report, or one the filter flags */
   }
   session_t *s = NULL;
   /* DAWN's side call, not the user's job: it doesn't count against their cap. */
   if (job_manager_begin_ex(w->user_id, w->job, job_provider_from_default(), false, &s) !=
       JOB_MGR_OK) {
      free(framed);
      return NULL;
   }
   atomic_store(&s->stream_conversation_id, 0); /* nothing streams into the job's own panel */
   pthread_mutex_lock(&s_take_mutex);
   s_take = s;
   pthread_mutex_unlock(&s_take_mutex);

   char persona[LLM_PERSONA_MAX];
   job_report_persona(persona, sizeof(persona));
   char *goal = NULL;
   if (conv_db_job_get_goal(w->job, w->user_id, &goal) != AUTH_DB_SUCCESS) {
      goal = NULL;
   }
   char fact_line[CR_FACTS_MAX];
   facts(fact_line, sizeof(fact_line), w->data);
   strbuf_t sys, ask;
   strbuf_init(&sys, strlen(persona) + sizeof(CODING_COMMENTARY_PROMPT_TEMPLATE));
   strbuf_appendf(&sys, CODING_COMMENTARY_PROMPT_TEMPLATE, persona);
   strbuf_init(&ask, strlen(framed) + CR_LINE_MAX);
   strbuf_appendf(&ask, CODING_COMMENTARY_ASK_TEMPLATE, goal ? goal : rec->title, fact_line,
                  framed);
   free(goal);
   free(framed);

   char *take = NULL;
   if (!strbuf_oom(&sys) && !strbuf_oom(&ask) && !atomic_load(&s_stopping)) {
      take = job_report_take(s, strbuf_str(&sys), strbuf_str(&ask), w->user_id);
   }
   strbuf_free(&sys);
   strbuf_free(&ask);
   pthread_mutex_lock(&s_take_mutex);
   s_take = NULL;
   pthread_mutex_unlock(&s_take_mutex);
   job_manager_end(s);
   return take;
}

/* DAWN's own line for a job's end: nothing in it is the agent's. */
static void fixed_line(char *out, size_t len, const cr_work_t *w, const char *title) {
   if (strcmp(w->kind, "completed") == 0) {
      snprintf(out, len,
               "The coding task \"%s\" (job #%lld) is done. Ask me for what it did and how it "
               "checked it.",
               title, (long long)w->job);
   } else if (strcmp(w->kind, "failed") == 0) {
      snprintf(out, len,
               "The coding task \"%s\" (job #%lld) stopped with an error. Ask me for its status.",
               title, (long long)w->job);
   } else {
      snprintf(out, len, "The coding task \"%s\" (job #%lld) was stopped at the gateway.", title,
               (long long)w->job);
   }
}

static void *report_thread(void *arg) {
   cr_work_t *w = arg;
   job_record_t rec;
   if (conv_db_job_get(w->job, w->user_id, &rec) == AUTH_DB_SUCCESS && rec.parent_id > 0) {
      char *take = (strcmp(w->kind, "completed") == 0 && !atomic_load(&s_stopping))
                       ? write_take(w, &rec)
                       : NULL;
      char *title = llm_context_neutralize(rec.title);
      strbuf_t post;
      strbuf_init(&post, CR_LINE_MAX + (take ? strlen(take) : 0));
      if (take != NULL) {
         /* DAWN's lead: where the words below came from. */
         strbuf_appendf(&post, "Coding job #%lld (%s) finished.\n\n%s", (long long)w->job,
                        title ? title : rec.title, take);
      } else {
         char line[CR_LINE_MAX];
         fixed_line(line, sizeof(line), w, title ? title : rec.title);
         strbuf_append(&post, line);
      }
      if (!strbuf_oom(&post)) {
         job_report_post(rec.parent_id, w->user_id, strbuf_str(&post));
      }
      strbuf_free(&post);
      free(title);
      free(take);
   }
   json_object_put(w->data);
   free(w);
   atomic_fetch_sub(&s_in_flight, 1);
   return NULL;
}

void coding_report_job_end(int64_t job,
                           int user_id,
                           const char *gw_kind,
                           struct json_object *data) {
   if (gw_kind == NULL) {
      return;
   }
   /* Counted before the check, so stop either waits for this one or this one
    * sees it stopping. */
   atomic_fetch_add(&s_in_flight, 1);
   cr_work_t *w = atomic_load(&s_stopping) ? NULL : calloc(1, sizeof(*w));
   pthread_attr_t attr;
   if (w == NULL || pthread_attr_init(&attr) != 0) {
      free(w);
      atomic_fetch_sub(&s_in_flight, 1);
      return;
   }
   w->job = job;
   w->user_id = user_id;
   safe_strscpy(w->kind, gw_kind);
   w->data = NULL;
   if (data != NULL && json_object_deep_copy(data, &w->data, NULL) != 0) {
      w->data = NULL; /* the fixed line, then */
   }
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   pthread_t th;
   if (pthread_create(&th, &attr, report_thread, w) != 0) {
      OLOG_WARNING("coding_agent: couldn't report job %lld's end", (long long)job);
      json_object_put(w->data);
      free(w);
      atomic_fetch_sub(&s_in_flight, 1);
   }
   pthread_attr_destroy(&attr);
}

void coding_report_stop(void) {
   atomic_store(&s_stopping, true);
   pthread_mutex_lock(&s_take_mutex);
   if (s_take != NULL) {
      session_cancel_turn(s_take); /* its thread then posts the fixed line */
   }
   pthread_mutex_unlock(&s_take_mutex);
   for (int waited = 0; atomic_load(&s_in_flight) > 0 && waited < CR_STOP_WAIT_MS;
        waited += CR_STOP_POLL_MS) {
      usleep(CR_STOP_POLL_MS * 1000);
   }
}
