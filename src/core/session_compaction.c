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
 * A session's compaction, summarized ahead and applied at a turn seam
 * (session_compaction.h).
 */

#include "core/session_compaction.h"

#include <json-c/json.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/prefix_in_force.h"
#include "core/session_history.h"
#include "core/session_prefix.h"
#include "llm/llm_compaction.h"
#include "llm/llm_compaction_range.h"
#include "llm/llm_context.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_history_rows.h"
#include "llm/llm_turn_blocks.h"
#include "logging.h"
#include "tts/text_to_speech.h"

/* Seconds after one compaction finishes before the next may start: a history
 * that keeps missing would otherwise pay a summary every turn. */
#define COMPACTION_COOLDOWN_SEC 60

/* Fewer messages than this past the system prompt aren't worth summarizing. */
#define COMPACTION_MIN_MESSAGES 4

/* How often a turn waiting on a running summary looks at its own cancel. */
#define COMPACTION_WAIT_TICK_MS 50

/* The worker's stack: the summarizer's request is built on the heap. */
#define COMPACTION_WORKER_STACK (256 * 1024)

void session_compaction_commit_free(session_compaction_commit_t *c) {
   if (!c) {
      return;
   }
   free(c->summary);
   json_object_put(c->tail_calls);
   memset(c, 0, sizeof(*c));
}

/* The client's "context compacted" marker.  The WebUI replaces this (a surface
 * without one has nothing to show it on). */
__attribute__((weak)) void session_compaction_client_notice(session_t *session,
                                                            int64_t conv_id,
                                                            int tokens_before,
                                                            int tokens_after,
                                                            int count,
                                                            const char *summary,
                                                            int level) {
   (void)session;
   (void)conv_id;
   (void)tokens_before;
   (void)tokens_after;
   (void)count;
   (void)summary;
   (void)level;
}

/* What one summary's runner (the worker, or a turn summarizing first) owns:
 * nothing of it is the session's, so nothing is freed from under it. */
typedef struct {
   session_t *session;
   struct json_object *input;        /* the range, copied, with an earlier summary first */
   int kept_tokens;                  /* what the history keeps beside the summary */
   int window_tokens;                /* the window the history must fit: the next turn's model */
   llm_compaction_calibration_t cal; /* how that model reads this conversation */
   char tag[32];
   session_llm_config_t config; /* the summarizer */
   llm_type_t type;
   cloud_provider_t provider;
   char model[64];
} compaction_run_t;

static void run_free(compaction_run_t *run) {
   if (run) {
      json_object_put(run->input);
      free(run);
   }
}

/* Where @p hist's messages start: past its leading system messages. */
static int messages_start(struct json_object *hist) {
   const int len = (int)json_object_array_length(hist);
   int start = 0;
   while (start < len && llm_history_role_is(json_object_array_get_idx(hist, start), "system")) {
      start++;
   }
   return start;
}

/* A turn opens here: the user's question, or DAWN's (a continuation's). */
static bool is_question_at(struct json_object *hist, int i) {
   return llm_history_is_question(json_object_array_get_idx(hist, i));
}

/* The newest question in (@p start, @p from], or @p start. */
static int question_at_or_before(struct json_object *hist, int start, int from) {
   for (int j = from; j > start; j--) {
      if (is_question_at(hist, j)) {
         return j;
      }
   }
   return start;
}

/* The text of a summary part @p msg carries (an earlier compaction's), or NULL. */
static const char *summary_part_text(struct json_object *msg) {
   struct json_object *content = NULL;
   if (!json_object_object_get_ex(msg, "content", &content) ||
       !json_object_is_type(content, json_type_array)) {
      return NULL;
   }
   const size_t n = json_object_array_length(content);
   for (size_t i = 0; i < n; i++) {
      struct json_object *part = json_object_array_get_idx(content, i);
      struct json_object *text = NULL;
      if (llm_history_kind_of(part) == MESSAGE_KIND_SUMMARY &&
          json_object_object_get_ex(part, "text", &text)) {
         return json_object_get_string(text);
      }
   }
   return NULL;
}

/* @p msg for the summarizer: its reasoning, images and earlier summary left out
 * (the summary is given once, first).  NULL when out of memory. */
static struct json_object *input_copy(struct json_object *msg) {
   struct json_object *copy = NULL;
   if (json_object_deep_copy(msg, &copy, NULL) != 0 || !copy) {
      return NULL;
   }
   struct json_object *content = NULL;
   if (json_object_object_get_ex(copy, "content", &content) &&
       json_object_is_type(content, json_type_array)) {
      for (int i = (int)json_object_array_length(content) - 1; i >= 0; i--) {
         if (llm_history_kind_of(json_object_array_get_idx(content, i)) == MESSAGE_KIND_SUMMARY) {
            json_object_array_del_idx(content, (size_t)i, 1);
         }
      }
   }
   return copy;
}

/* Caller holds history_mutex.  Release the range's refs. */
static void clear_range_locked(session_t *session) {
   session_compaction_t *c = &session->compaction;
   session_release_ref_locked(session, c->first);
   session_release_ref_locked(session, c->last);
   session_release_ref_locked(session, c->hist);
   c->first = c->last = c->hist = NULL;
   c->count = 0;
}

/* Caller holds history_mutex.  The range of @p hist to summarize, pinned, and
 * copied into @p run for the summarizer: from its first message to where the
 * kept part starts (a turn; with nothing to keep, the newest question).  A
 * surface that saves turn by turn summarizes only rows already saved: a message
 * a turn record still holds ends the range at the question before it, and the
 * range must end on a row whose id is known (or a tool exchange, whose rows are
 * found by call id). */
static bool plan_locked(session_t *session, struct json_object *hist, compaction_run_t *run) {
   session_compaction_t *c = &session->compaction;
   const int len = json_object_is_type(hist, json_type_array) ? (int)json_object_array_length(hist)
                                                              : 0;
   const int start = messages_start(hist);
   if (len - start < COMPACTION_MIN_MESSAGES) {
      return false;
   }
   int end = llm_compaction_keep_start(hist, start, LLM_COMPACTION_KEEP_EXCHANGES * 2);
   if (end <= start) {
      /* A tool exchange fills what would be kept: keep the newest question on. */
      end = question_at_or_before(hist, start, len - 1);
   }
   if (!session_saved_whole(session)) {
      for (int i = start; i < end; i++) {
         if (session_prefix_owns_locked(session, json_object_array_get_idx(hist, i))) {
            end = question_at_or_before(hist, start, i);
            break;
         }
      }
      /* The range ends on a saved row, or on a tool exchange (its rows are
       * found by call id): past anything else unsaved, a reload couldn't tell
       * where it ends, so it ends at the turn before. */
      while (end > start) {
         struct json_object *last = json_object_array_get_idx(hist, end - 1);
         if (llm_compaction_row_id(last) > 0 || llm_compaction_is_tool_exchange(last)) {
            break;
         }
         end = question_at_or_before(hist, start, end - 1);
      }
      int64_t first_id = 0;
      int64_t last_id = 0;
      llm_compaction_summary_ids(hist, start, end, &first_id, &last_id);
      if (last_id <= 0) {
         return false;
      }
   }
   if (end - start < 2) {
      return false;
   }

   struct json_object *input = json_object_new_array();
   if (!input) {
      return false;
   }
   /* What an earlier compaction summarized is in the summary this range opens
    * with: the new one covers it too. */
   const char *earlier = summary_part_text(json_object_array_get_idx(hist, start));
   if (earlier) {
      struct json_object *m = json_object_new_object();
      json_object_object_add(m, "role", json_object_new_string("user"));
      json_object_object_add(m, "content", json_object_new_string(earlier));
      json_object_array_add(input, m);
   }
   for (int i = start; i < end; i++) {
      struct json_object *copy = input_copy(json_object_array_get_idx(hist, i));
      if (!copy) {
         json_object_put(input);
         return false;
      }
      json_object_array_add(input, copy);
   }
   run->input = input;
   run->kept_tokens = llm_compaction_estimate_range(hist, 0, start) +
                      llm_compaction_estimate_range(hist, end, len);
   const char *tag = llm_history_tag(hist);
   snprintf(run->tag, sizeof(run->tag), "%s", tag ? tag : "");
   snprintf(c->tag, sizeof(c->tag), "%s", run->tag);
   c->hist = json_object_get(hist);
   c->first = json_object_get(json_object_array_get_idx(hist, start));
   c->last = json_object_get(json_object_array_get_idx(hist, end - 1));
   c->count = end - start;
   return true;
}

/* Caller holds history_mutex.  Take the session to RUNNING with a plan of @p
 * hist in @p run, or leave it as it was (false). */
static bool start_locked(session_t *session, struct json_object *hist, compaction_run_t *run) {
   session_compaction_t *c = &session->compaction;
   if (c->closed) {
      return false;
   }
   int expected = COMPACTION_IDLE;
   if (!atomic_compare_exchange_strong(&c->state, &expected, COMPACTION_RUNNING)) {
      return false;
   }
   atomic_store(&c->cancel, false);
   if (!plan_locked(session, hist, run)) {
      atomic_store(&c->state, COMPACTION_IDLE);
      c->last_at = time(NULL);
      return false;
   }
   return true;
}

/* Summarize the planned range (no lock held).  Returns the summary, or NULL
 * (cancelled, or every level failed). */
static char *summarize(const compaction_run_t *run, const atomic_bool *cancel, int *level_out) {
   llm_compaction_level_t level = LLM_COMPACT_NORMAL;
   char *summary = llm_context_summarize(run->input, run->kept_tokens, run->window_tokens,
                                         &run->cal, run->tag[0] ? run->tag : NULL, &run->config,
                                         cancel, &level);
   *level_out = (int)level;
   return summary;
}

/* The end of a summary, run or sync: ready to apply, or given up. */
static void finish(session_t *session, char *summary, int level) {
   session_compaction_t *c = &session->compaction;
   pthread_mutex_lock(&session->history_mutex);
   if (summary && !atomic_load(&c->cancel) && !c->closed && c->hist) {
      c->summary = summary;
      c->level = level;
      atomic_store(&c->state, COMPACTION_READY);
      OLOG_INFO("Session %u: a summary of %d message(s) is ready (L%d)", session->session_id,
                c->count, level + 1);
   } else {
      free(summary);
      clear_range_locked(session);
      atomic_store(&c->state, COMPACTION_IDLE);
      c->last_at = time(NULL);
   }
   pthread_mutex_unlock(&session->history_mutex);
}

static void *worker(void *arg) {
   compaction_run_t *run = arg;
   session_t *session = run->session;
   session_compaction_t *c = &session->compaction;
   /* Its own cancel: a teardown's, never the turn's (the summary outlives it). */
   llm_set_cancel_flag_ex(&c->cancel, false);

   int level = 0;
   char *summary = atomic_load(&c->cancel) ? NULL : summarize(run, &c->cancel, &level);
   finish(session, summary, level);

   llm_set_cancel_flag(NULL);
   run_free(run);
   session_release(session);
   return NULL;
}

/* A worker that finished (or never ran) is joined before another starts. */
static void join_finished(session_t *session) {
   if (atomic_exchange(&session->compaction.thread_active, false)) {
      pthread_join(session->compaction.thread_id, NULL);
   }
}

/* The summarizer and the window of the model @p cfg resolves to. */
static bool run_model(compaction_run_t *run, const session_llm_config_t *cfg) {
   llm_resolved_config_t resolved;
   if (llm_resolve_config(cfg, &resolved) != 0) {
      return false;
   }
   run->config = *cfg;
   run->type = resolved.type;
   run->provider = resolved.cloud_provider;
   snprintf(run->model, sizeof(run->model), "%s", resolved.model ? resolved.model : "");
   return true;
}

void session_compaction_trigger(session_t *session,
                                struct json_object *hist,
                                llm_type_t type,
                                cloud_provider_t provider,
                                const char *model) {
   if (!session || !hist || session->type == SESSION_TYPE_JOB) {
      return;
   }
   session_compaction_t *c = &session->compaction;
   /* A summary waiting for a history the session no longer runs won't be
    * applied: it goes, and this one may start. */
   if (atomic_load(&c->state) == COMPACTION_READY) {
      pthread_mutex_lock(&session->history_mutex);
      if (c->hist != session->conversation_history && c->hist != session->turn_history) {
         session_compaction_drop_locked(session);
      }
      pthread_mutex_unlock(&session->history_mutex);
   }
   /* A switch waits for the next seam, which judges it against the new model. */
   if (atomic_load(&c->state) != COMPACTION_IDLE || atomic_load(&c->switched) ||
       (c->last_at > 0 && time(NULL) - c->last_at < COMPACTION_COOLDOWN_SEC)) {
      return;
   }
   if (!llm_context_over_threshold(session->session_id, hist, 0, type, provider, model,
                                   g_config.llm.compact_soft_threshold)) {
      return;
   }
   compaction_run_t *run = calloc(1, sizeof(*run));
   session_llm_config_t cfg;
   session_get_llm_config(session, &cfg);
   if (!run || !run_model(run, &cfg)) {
      free(run);
      return;
   }
   run->session = session;
   run->window_tokens = llm_context_get_size(type, provider, model);
   llm_context_calibration(session->session_id, type, provider, model, &run->cal);
   join_finished(session);

   /* Planned and started under the lock, so a teardown either sees the worker
    * (and joins it) or closes before it starts. */
   pthread_mutex_lock(&session->history_mutex);
   if (!start_locked(session, hist, run)) {
      pthread_mutex_unlock(&session->history_mutex);
      run_free(run);
      return;
   }
   const int count = c->count;
   session_retain(session);
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setstacksize(&attr, COMPACTION_WORKER_STACK);
   const bool started = pthread_create(&c->thread_id, &attr, worker, run) == 0;
   pthread_attr_destroy(&attr);
   if (started) {
      atomic_store(&c->thread_active, true);
   }
   pthread_mutex_unlock(&session->history_mutex);
   if (!started) {
      OLOG_ERROR("Session %u: couldn't start the compaction worker", session->session_id);
      run_free(run);
      finish(session, NULL, 0);
      session_release(session);
      return;
   }
   OLOG_INFO("Session %u: summarizing %d message(s) ahead (soft threshold %.0f%%)",
             session->session_id, count, g_config.llm.compact_soft_threshold * 100.0f);
}

/* The history a turn about to begin will run on. */
static struct json_object *turn_history_locked(session_t *session) {
   return session->turn_history ? session->turn_history : session->conversation_history;
}

void session_compaction_prepare(session_t *session, int extra_tokens) {
   if (!session) {
      return;
   }
   session_compaction_t *c = &session->compaction;
   session_llm_config_t cfg;
   session_get_llm_config(session, &cfg);
   compaction_run_t *run = calloc(1, sizeof(*run));
   if (!run || !run_model(run, &cfg)) {
      free(run);
      return;
   }
   run->session = session;
   /* The history must fit the model this turn runs. */
   const llm_type_t type = run->type;
   const cloud_provider_t provider = run->provider;
   char model[sizeof(run->model)];
   snprintf(model, sizeof(model), "%s", run->model);
   const char *m = model[0] ? model : NULL;
   run->window_tokens = llm_context_get_size(type, provider, m);
   llm_context_calibration(session->session_id, type, provider, m, &run->cal);

   /* A switch made last turn is judged at this seam and no later: it is
    * summarized (if it must be) with the model switched from, which the
    * history fits. */
   pthread_mutex_lock(&session->history_mutex);
   const bool switched = atomic_exchange(&c->switched, false);
   session_llm_config_t from = c->switched_from;
   struct json_object *hist = json_object_get(turn_history_locked(session));
   pthread_mutex_unlock(&session->history_mutex);

   const bool over = hist &&
                     llm_context_over_threshold(session->session_id, hist, extra_tokens, type,
                                                provider, m, llm_context_hard_threshold());
   if (!over) {
      session_release_ref(session, hist);
      run_free(run);
      return;
   }
   if (switched && !run_model(run, &from)) {
      (void)run_model(run, &cfg);
   }
   /* A summary on its way is waited for, as long as this turn is: a Stop, a
    * barge-in or a teardown ends the wait, and the summary with it (the turn
    * that needed it is going). */
   if (atomic_load(&c->state) == COMPACTION_RUNNING && atomic_load(&c->thread_active)) {
      /* The global interrupt is the local mic's barge-in: no other surface's. */
      const bool barge_in = session->type == SESSION_TYPE_LOCAL;
      while (atomic_load(&c->state) == COMPACTION_RUNNING) {
         if (atomic_load(&session->cancel_requested) ||
             (barge_in && llm_is_interrupt_requested())) {
            atomic_store(&c->cancel, true);
            session_release_ref(session, hist);
            run_free(run);
            return;
         }
         const struct timespec tick = { 0, COMPACTION_WAIT_TICK_MS * 1000000L };
         nanosleep(&tick, NULL);
      }
      join_finished(session);
   }
   pthread_mutex_lock(&session->history_mutex);
   if (atomic_load(&c->state) == COMPACTION_READY && c->hist == hist && !switched) {
      pthread_mutex_unlock(&session->history_mutex);
      session_release_ref(session, hist);
      run_free(run);
      return;
   }
   if (atomic_load(&c->state) == COMPACTION_READY) {
      /* Of another history, or made to fit the model switched from. */
      session_compaction_drop_locked(session);
   }
   const bool planned = start_locked(session, hist, run);
   const int count = c->count;
   pthread_mutex_unlock(&session->history_mutex);
   session_release_ref(session, hist);
   if (!planned) {
      run_free(run);
      return;
   }

   OLOG_WARNING("Session %u: the history reaches its window; summarizing %d message(s) first",
                session->session_id, count);
   if (session->type == SESSION_TYPE_LOCAL) {
      text_to_speech((char *)"Compacting my memory. Just a moment.");
   }
   /* On this turn's thread, and the turn's cancel (a Stop, a teardown, and for
    * the local mic a barge-in) stops it, transfer and all. */
   void *prev_cancel = llm_get_cancel_flag();
   llm_set_cancel_flag_ex(&session->cancel_requested, session->type == SESSION_TYPE_LOCAL);
   int level = 0;
   char *summary = summarize(run, &session->cancel_requested, &level);
   llm_set_cancel_flag_ex(prev_cancel, !session_is_background(session));
   run_free(run);
   finish(session, summary, level);
}

bool session_compaction_apply_locked(session_t *session,
                                     struct json_object *hist,
                                     session_compaction_commit_t *out) {
   memset(out, 0, sizeof(*out));
   if (!session || !hist) {
      return false;
   }
   session_compaction_t *c = &session->compaction;
   if (atomic_load(&c->state) != COMPACTION_READY || c->hist != hist) {
      return false; /* none, or another history's: it waits for that one */
   }
   /* The range must still be where it was: its first message the first past
    * the system prompt, its last where it ended. */
   const int len = (int)json_object_array_length(hist);
   const int start = messages_start(hist);
   const int end = start + c->count;
   if (end > len || json_object_array_get_idx(hist, start) != c->first ||
       json_object_array_get_idx(hist, end - 1) != c->last) {
      OLOG_INFO("Session %u: the history changed under its summary; dropped", session->session_id);
      session_compaction_drop_locked(session);
      return false;
   }

   out->summary = c->summary;
   c->summary = NULL;
   out->level = c->level;
   out->count = c->count;
   out->tokens_before = llm_context_estimate_tokens(hist);
   llm_compaction_summary_ids(hist, start, end, &out->first_id, &out->last_id);
   out->tail_calls = llm_compaction_tail_call_ids(hist, start, end);
   out->kept_first_id = llm_compaction_kept_first_id(hist, end);

   if (session_saved_whole(session)) {
      /* Saved at the voice save, before what is kept: the rows each message
       * becomes, without the reasoning a compaction leaves behind anyway (and
       * without the images the rows never carry). */
      if (!c->voice_removed) {
         c->voice_removed = json_object_new_array();
      }
      for (int i = start; c->voice_removed && i < end; i++) {
         struct json_object *msg = json_object_array_get_idx(hist, i);
         if (llm_history_role_is(msg, "system") && llm_history_kind_of(msg) == MESSAGE_KIND_NONE) {
            continue; /* a prompt is saved with the conversation, not as a row */
         }
         struct json_object *rows = json_object_new_array();
         if (!rows) {
            continue;
         }
         (void)llm_history_rows_append_text(msg, rows);
         json_object_array_add(c->voice_removed, rows);
      }
      free(c->voice_summary);
      c->voice_summary = strdup(out->summary);
      c->voice_level = out->level;
   }
   json_object_array_del_idx(hist, (size_t)start, (size_t)c->count);
   if (llm_history_attach_summary(hist, start, out->summary, c->tag[0] ? c->tag : NULL) < 0) {
      OLOG_ERROR("Session %u: out of memory putting the summary in place", session->session_id);
   }
   /* A declared boundary: nothing before it reads as it did. */
   (void)llm_history_drop_turn_blocks(hist);
   prefix_in_force_reset_to_history(hist);
   out->tokens_after = llm_context_estimate_tokens(hist);

   OLOG_INFO("Session %u: compaction applied: %d message(s) summarized (L%d)", session->session_id,
             c->count, out->level + 1);
   clear_range_locked(session);
   atomic_store(&c->state, COMPACTION_IDLE);
   atomic_fetch_add(&c->applied, 1);
   c->last_at = time(NULL);
   return true;
}

void session_compaction_rebind_locked(session_t *session,
                                      struct json_object *from,
                                      struct json_object *to) {
   session_compaction_t *c = &session->compaction;
   if (from && to && c->hist == from) {
      c->hist = json_object_get(to);
      session_release_ref_locked(session, from);
   }
}

void session_compaction_notify(session_t *session,
                               int64_t conv_id,
                               const session_compaction_commit_t *c) {
   if (session && c && c->summary) {
      session_compaction_client_notice(session, conv_id, c->tokens_before, c->tokens_after,
                                       c->count, c->summary, c->level);
   }
}

void session_compaction_drop_locked(session_t *session) {
   if (!session) {
      return;
   }
   session_compaction_t *c = &session->compaction;
   atomic_store(&c->cancel, true);
   if (atomic_load(&c->state) == COMPACTION_READY) {
      free(c->summary);
      c->summary = NULL;
      clear_range_locked(session);
      atomic_store(&c->state, COMPACTION_IDLE);
   }
   /* A running summary sees the cancel and gives up (finish). */
}

void session_compaction_reset_locked(session_t *session) {
   if (!session) {
      return;
   }
   session_compaction_t *c = &session->compaction;
   session_compaction_drop_locked(session);
   json_object_put(c->voice_removed);
   c->voice_removed = NULL;
   free(c->voice_summary);
   c->voice_summary = NULL;
}

typedef struct {
   struct json_object *calls;
   int64_t last;
   int64_t below;
} tail_rows_t;

static int tail_row_cb(const conversation_message_t *msg, void *ctx) {
   tail_rows_t *t = ctx;
   if (t->below > 0 && msg->id >= t->below) {
      return 1; /* past the summarized part: the kept part starts here */
   }
   if (llm_compaction_row_in_calls(msg->role, msg->tool_calls, msg->tool_call_id, t->calls) &&
       msg->id > t->last) {
      t->last = msg->id;
   }
   return 0;
}

int64_t session_compaction_watermark(int64_t conv_id,
                                     int user_id,
                                     const session_compaction_commit_t *c) {
   if (!c || c->last_id <= 0 || conv_id <= 0 || user_id <= 0) {
      return c ? c->last_id : 0;
   }
   tail_rows_t t = { .calls = c->tail_calls, .last = c->last_id, .below = c->kept_first_id };
   if (t.calls && json_object_array_length(t.calls) > 0) {
      (void)conv_db_get_messages_after(conv_id, user_id, c->last_id, tail_row_cb, &t);
   }
   return t.last;
}

void session_compaction_note_switch(session_t *session, const session_llm_config_t *from) {
   if (!session || !from) {
      return;
   }
   session_compaction_t *c = &session->compaction;
   pthread_mutex_lock(&session->history_mutex);
   c->switched_from = *from;
   atomic_store(&c->switched, true);
   pthread_mutex_unlock(&session->history_mutex);
}

void session_compaction_teardown(session_t *session) {
   if (!session) {
      return;
   }
   session_compaction_t *c = &session->compaction;
   /* Closed first, under the lock a start takes: none starts after this, and
    * one started before it is seen and joined. */
   pthread_mutex_lock(&session->history_mutex);
   c->closed = true;
   atomic_store(&c->cancel, true);
   pthread_mutex_unlock(&session->history_mutex);
   join_finished(session);
   pthread_mutex_lock(&session->history_mutex);
   free(c->summary);
   c->summary = NULL;
   clear_range_locked(session);
   atomic_store(&c->state, COMPACTION_IDLE);
   json_object_put(c->voice_removed);
   c->voice_removed = NULL;
   free(c->voice_summary);
   c->voice_summary = NULL;
   pthread_mutex_unlock(&session->history_mutex);
}
