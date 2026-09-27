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
 * Per-call prompt-cache telemetry.  Every provider path reports its usage
 * through llm_context_update_usage(), which hands it here; the request the
 * call sent was noted on the same thread just before (the send and the usage
 * callback run on one thread), so the record carries the model and a hash of
 * the request's cacheable prefix without plumbing either through each path.
 */

#include "llm/llm_cache_monitor.h"

#include <json-c/json.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "core/session_manager.h"
#include "llm/llm_cache_monitor_internal.h"
#include "llm/llm_interface.h"
#include "logging.h"
#include "utils/string_utils.h"

/* Prompt-cache lifetimes the expected-read math assumes.  Anthropic's default
 * cache lives 5 minutes from its last use; the OpenAI-shape providers keep
 * theirs for minutes as well; a local server keeps its KV cache until evicted. */
#define CACHE_TTL_CLOUD_MS (5 * 60 * 1000)
#define CACHE_TTL_LOCAL_MS (24 * 60 * 60 * 1000)

/* Cache keys (a conversation on a session) whose previous call is remembered
 * for the expected-read math; the least recently used is replaced.  Sized for
 * the background-job pool (256) plus the interactive sessions. */
#define CACHE_KEYS_MAX 320

static const char *const s_kind_names[LLM_CALL_KIND_COUNT] = {
   [LLM_CALL_TURN] = "turn",
   [LLM_CALL_TOOL_ITER] = "tool_iter",
   [LLM_CALL_JOB] = "job",
   [LLM_CALL_RESEARCH] = "research",
   [LLM_CALL_EXTRACTION] = "extraction",
   [LLM_CALL_COMPACTION] = "compaction",
   [LLM_CALL_BRIEFING] = "briefing",
   [LLM_CALL_OBSERVE] = "observe",
   [LLM_CALL_SUMMARIZER] = "summarizer",
   [LLM_CALL_SYNTHESIS] = "synthesis",
   [LLM_CALL_OTHER] = "other",
};

static const char *const s_state_names[] = {
   [LLM_CACHE_WARM] = "warm",
   [LLM_CACHE_COLD_FIRST] = "first",
   [LLM_CACHE_COLD_TTL] = "ttl",
   [LLM_CACHE_COLD_TOOLS] = "tools",
   [LLM_CACHE_COLD_SYSTEM] = "system",
   [LLM_CACHE_COLD_MODEL] = "model",
   [LLM_CACHE_COLD_THINKING] = "thinking",
};

/* The calling thread's tag, iteration and noted request (-1 = none). */
static __thread int t_kind = -1;
static __thread int t_iteration = -1;
static __thread bool t_local_mic = false;
static __thread struct {
   bool valid;
   llm_cache_prefix_t prefix;
   char model[LLM_CACHE_MODEL_MAX];
   char thinking[LLM_CACHE_THINKING_MAX];
   bool images;
} t_note;

/* The previous conversation call per cache key, for the expected read. */
typedef struct {
   uint32_t session_id; /* 0 = free */
   int64_t conversation_id;
   uint64_t last_ms;
   llm_cache_prefix_t prefix;
   int cached_after;    /* read + write: what the next call on this key can read */
   char message_id[64]; /* the response id, for Anthropic cache diagnostics */
} cache_key_state_t;

static pthread_mutex_t s_keys_mutex = PTHREAD_MUTEX_INITIALIZER; /* leaf */
static cache_key_state_t s_keys[CACHE_KEYS_MAX];

/* Records waiting for llm_cache_monitor_flush().  A few calls a second at most,
 * flushed every second; a failed write leaves them queued for the next flush,
 * so the ring only fills while the database is unavailable, and then the
 * oldest are dropped (and counted). */
#define USAGE_QUEUE_MAX 256
static pthread_mutex_t s_queue_mutex = PTHREAD_MUTEX_INITIALIZER; /* leaf */
static llm_usage_row_t s_queue[USAGE_QUEUE_MAX];
static int s_queue_head; /* oldest */
static int s_queue_count;
static int s_queue_dropped;       /* since the last flush */
static uint64_t s_queue_head_seq; /* sequence number of the oldest (each row gets the next) */
static llm_usage_row_t s_flush_batch[USAGE_QUEUE_MAX]; /* flush thread only */

const char *llm_call_kind_name(llm_call_kind_t kind) {
   return (kind >= 0 && kind < LLM_CALL_KIND_COUNT) ? s_kind_names[kind] : "other";
}

bool llm_call_kind_is_conversation(llm_call_kind_t kind) {
   return kind == LLM_CALL_TURN || kind == LLM_CALL_TOOL_ITER || kind == LLM_CALL_JOB ||
          kind == LLM_CALL_RESEARCH;
}

bool llm_call_kind_sets_context(llm_call_kind_t kind) {
   return llm_call_kind_is_conversation(kind) || kind == LLM_CALL_OTHER;
}

const char *llm_cache_state_name(llm_cache_state_t state) {
   return (state >= LLM_CACHE_WARM && state <= LLM_CACHE_COLD_THINKING) ? s_state_names[state]
                                                                        : "?";
}

int llm_cache_monitor_push_kind(llm_call_kind_t kind) {
   const int previous = t_kind;
   t_kind = kind;
   return previous;
}

void llm_cache_monitor_pop_kind(int previous) {
   t_kind = previous;
}

bool llm_cache_monitor_in_side_call(void) {
   return t_kind >= 0 && t_kind < LLM_CALL_KIND_COUNT &&
          !llm_call_kind_sets_context((llm_call_kind_t)t_kind);
}

void llm_cache_monitor_set_iteration(int iteration) {
   t_iteration = iteration;
}

int llm_cache_monitor_get_iteration(void) {
   return t_iteration;
}

void llm_cache_monitor_set_local_mic(bool local_mic) {
   t_local_mic = local_mic;
}

int llm_cache_monitor_ttl_ms(llm_type_t type, cloud_provider_t provider) {
   (void)provider;
   return type == LLM_LOCAL ? CACHE_TTL_LOCAL_MS : CACHE_TTL_CLOUD_MS;
}

static uint32_t fnv1a(uint32_t h, const char *s) {
   for (; s && *s; s++) {
      h ^= (unsigned char)*s;
      h *= 16777619u;
   }
   return h;
}

#define FNV_OFFSET 2166136261u

/* The request's stable system text: Anthropic's first system block, the
 * Responses instructions, or a Chat Completions leading system message. */
static const char *stable_system_text(struct json_object *request) {
   struct json_object *v = NULL;
   if (json_object_object_get_ex(request, "system", &v)) {
      if (json_object_is_type(v, json_type_array)) {
         struct json_object *first = json_object_array_get_idx(v, 0);
         struct json_object *text = NULL;
         return (first && json_object_object_get_ex(first, "text", &text))
                    ? json_object_get_string(text)
                    : NULL;
      }
      return json_object_get_string(v);
   }
   if (json_object_object_get_ex(request, "instructions", &v)) {
      return json_object_get_string(v);
   }
   struct json_object *messages = NULL;
   if (json_object_object_get_ex(request, "messages", &messages) &&
       json_object_is_type(messages, json_type_array)) {
      struct json_object *first = json_object_array_get_idx(messages, 0);
      struct json_object *role = NULL;
      struct json_object *content = NULL;
      if (first && json_object_object_get_ex(first, "role", &role) &&
          strcmp(json_object_get_string(role), "system") == 0 &&
          json_object_object_get_ex(first, "content", &content)) {
         return json_object_is_type(content, json_type_string)
                    ? json_object_get_string(content)
                    : json_object_to_json_string_ext(content, JSON_C_TO_STRING_PLAIN);
      }
   }
   return NULL;
}

/* The request's thinking settings, as "type/effort" (either part may be empty):
 * Anthropic thinking.type + output_config.effort, OpenAI reasoning.effort or
 * reasoning_effort. */
static void thinking_text(struct json_object *request, char *out, size_t out_len) {
   const char *type = "";
   const char *effort = "";
   struct json_object *v = NULL;
   struct json_object *f = NULL;
   if (json_object_object_get_ex(request, "thinking", &v) &&
       json_object_object_get_ex(v, "type", &f)) {
      type = json_object_get_string(f);
   }
   if ((json_object_object_get_ex(request, "output_config", &v) ||
        json_object_object_get_ex(request, "reasoning", &v)) &&
       json_object_object_get_ex(v, "effort", &f)) {
      effort = json_object_get_string(f);
   } else if (json_object_object_get_ex(request, "reasoning_effort", &f)) {
      effort = json_object_get_string(f);
   }
   if (type[0] || effort[0]) {
      snprintf(out, out_len, "%s/%s", type, effort);
   } else {
      out[0] = '\0';
   }
}

/* Whether the newest message (the one this call adds) carries an image: its
 * tokens are new, so an image call is never a warm miss. */
static bool newest_message_has_image(struct json_object *request) {
   struct json_object *items = NULL;
   if (!json_object_object_get_ex(request, "messages", &items) &&
       !json_object_object_get_ex(request, "input", &items)) {
      return false;
   }
   const size_t n = json_object_is_type(items, json_type_array) ? json_object_array_length(items)
                                                                : 0;
   struct json_object *content = NULL;
   if (n == 0 ||
       !json_object_object_get_ex(json_object_array_get_idx(items, n - 1), "content", &content) ||
       !json_object_is_type(content, json_type_array)) {
      return false;
   }
   for (size_t i = 0; i < json_object_array_length(content); i++) {
      struct json_object *type = NULL;
      if (json_object_object_get_ex(json_object_array_get_idx(content, i), "type", &type)) {
         const char *t = json_object_get_string(type);
         if (strcmp(t, "image") == 0 || strcmp(t, "image_url") == 0 ||
             strcmp(t, "input_image") == 0) {
            return true;
         }
      }
   }
   return false;
}

void llm_cache_monitor_note_request(struct json_object *request) {
   t_note.valid = false;
   if (!request) {
      return;
   }
   struct json_object *v = NULL;
   t_note.model[0] = '\0';
   if (json_object_object_get_ex(request, "model", &v)) {
      safe_strscpy(t_note.model, json_object_get_string(v));
   }
   t_note.prefix.tools = 0;
   if (json_object_object_get_ex(request, "tools", &v)) {
      t_note.prefix.tools = fnv1a(FNV_OFFSET,
                                  json_object_to_json_string_ext(v, JSON_C_TO_STRING_PLAIN));
   }
   t_note.prefix.system = fnv1a(FNV_OFFSET, stable_system_text(request));
   t_note.prefix.model = fnv1a(FNV_OFFSET, t_note.model);
   thinking_text(request, t_note.thinking, sizeof(t_note.thinking));
   t_note.prefix.thinking = fnv1a(FNV_OFFSET, t_note.thinking);
   t_note.images = newest_message_has_image(request);
   t_note.valid = true;
}

bool llm_cache_monitor_noted(llm_cache_prefix_t *prefix, const char **model) {
   if (prefix) {
      *prefix = t_note.prefix;
   }
   if (model) {
      *model = t_note.model;
   }
   return t_note.valid;
}

void llm_cache_monitor_reset_keys(void) {
   pthread_mutex_lock(&s_keys_mutex);
   memset(s_keys, 0, sizeof(s_keys));
   pthread_mutex_unlock(&s_keys_mutex);
}

static uint64_t now_ms(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* The call's kind: an explicit tag, else from its session and loop iteration. */
static llm_call_kind_t call_kind(session_t *session) {
   if (t_kind >= 0 && t_kind < LLM_CALL_KIND_COUNT) {
      return (llm_call_kind_t)t_kind;
   }
   if (!session) {
      /* No session: the local microphone's turn (its worker says so) runs its
       * tool loop without one; anything else untagged is unattributed. */
      if (!t_local_mic) {
         return LLM_CALL_OTHER;
      }
      return t_iteration > 0 ? LLM_CALL_TOOL_ITER : LLM_CALL_TURN;
   }
   if (session->type == SESSION_TYPE_JOB) {
      return atomic_load(&session->research_run_id) > 0 ? LLM_CALL_RESEARCH : LLM_CALL_JOB;
   }
   return t_iteration > 0 ? LLM_CALL_TOOL_ITER : LLM_CALL_TURN;
}

/* Which part of the prefix differs (LLM_CACHE_WARM when none). */
static llm_cache_state_t prefix_change(const llm_cache_prefix_t *was,
                                       const llm_cache_prefix_t *now) {
   if (was->model != now->model) {
      return LLM_CACHE_COLD_MODEL;
   }
   if (was->tools != now->tools) {
      return LLM_CACHE_COLD_TOOLS;
   }
   if (was->system != now->system) {
      return LLM_CACHE_COLD_SYSTEM;
   }
   if (was->thinking != now->thinking) {
      return LLM_CACHE_COLD_THINKING;
   }
   return LLM_CACHE_WARM;
}

int llm_cache_monitor_expected_read_at(uint32_t session_id,
                                       int64_t conversation_id,
                                       const llm_cache_prefix_t *prefix,
                                       int ttl_ms,
                                       int cached_after,
                                       const char *message_id,
                                       uint64_t now,
                                       uint64_t *gap_ms,
                                       llm_cache_state_t *state) {
   *gap_ms = 0;
   *state = LLM_CACHE_COLD_FIRST;
   if (session_id == 0) {
      return 0;
   }
   int expected = 0;
   pthread_mutex_lock(&s_keys_mutex);
   cache_key_state_t *slot = NULL;
   cache_key_state_t *oldest = &s_keys[0];
   for (int i = 0; i < CACHE_KEYS_MAX; i++) {
      if (s_keys[i].session_id == session_id && s_keys[i].conversation_id == conversation_id) {
         slot = &s_keys[i];
         break;
      }
      if (s_keys[i].session_id == 0 || s_keys[i].last_ms < oldest->last_ms) {
         oldest = &s_keys[i];
      }
   }
   if (slot) {
      *gap_ms = now - slot->last_ms;
      if (*gap_ms >= (uint64_t)ttl_ms) {
         *state = LLM_CACHE_COLD_TTL;
      } else {
         *state = prefix_change(&slot->prefix, prefix);
         if (*state == LLM_CACHE_WARM) {
            expected = slot->cached_after;
         }
      }
   } else {
      slot = oldest;
      slot->session_id = session_id;
      slot->conversation_id = conversation_id;
   }
   slot->last_ms = now;
   slot->prefix = *prefix;
   slot->cached_after = cached_after;
   snprintf(slot->message_id, sizeof(slot->message_id), "%s", message_id ? message_id : "");
   pthread_mutex_unlock(&s_keys_mutex);
   return expected;
}

bool llm_cache_monitor_previous_message_id(uint32_t session_id,
                                           int64_t conversation_id,
                                           char *out,
                                           size_t out_len) {
   if (!out || out_len == 0 || session_id == 0) {
      return false;
   }
   out[0] = '\0';
   pthread_mutex_lock(&s_keys_mutex);
   for (int i = 0; i < CACHE_KEYS_MAX; i++) {
      if (s_keys[i].session_id == session_id && s_keys[i].conversation_id == conversation_id) {
         snprintf(out, out_len, "%s", s_keys[i].message_id);
         break;
      }
   }
   pthread_mutex_unlock(&s_keys_mutex);
   return out[0] != '\0';
}

/* Queue @p rec for the llm_usage_log table (see llm_cache_monitor_flush). */
static void enqueue_row(const llm_cache_record_t *rec, session_t *session, const char *provider) {
   llm_usage_row_t row = {
      .created_at = (int64_t)time(NULL),
      .conversation_id = rec->conversation_id,
      .iteration = rec->iteration,
      .prompt_tokens = rec->prompt,
      .cache_read_tokens = rec->read,
      .cache_write_tokens = rec->write,
      .uncached_tokens = rec->uncached,
      .output_tokens = rec->output,
      .expected_read = rec->expected,
      .gap_ms = (int64_t)rec->gap_ms,
      .tools_hash = rec->tools_hash,
      .system_hash = rec->system_hash,
      .images = rec->images,
   };
   /* Whose call: the session's user; the local microphone's turn (no session)
    * is the default voice user's; anything else, nobody's. */
   if (session) {
      row.user_id = session_effective_user_id(session);
   } else if (t_local_mic) {
      row.user_id = session_default_voice_user_id();
   }
   safe_strscpy(row.provider, provider ? provider : "?");
   safe_strscpy(row.model, rec->model);
   safe_strscpy(row.kind, llm_call_kind_name(rec->kind));
   safe_strscpy(row.cache_state, llm_cache_state_name(rec->state));
   safe_strscpy(row.thinking, rec->thinking);
   safe_strscpy(row.cache_miss_reason, rec->miss_reason);
   row.cache_missed_tokens = rec->missed_tokens;

   pthread_mutex_lock(&s_queue_mutex);
   if (s_queue_count == USAGE_QUEUE_MAX) {
      s_queue_head = (s_queue_head + 1) % USAGE_QUEUE_MAX; /* drop the oldest */
      s_queue_head_seq++;
      s_queue_count--;
      s_queue_dropped++;
   }
   s_queue[(s_queue_head + s_queue_count) % USAGE_QUEUE_MAX] = row;
   s_queue_count++;
   pthread_mutex_unlock(&s_queue_mutex);
}

void llm_cache_monitor_record(uint32_t session_id,
                              const llm_usage_report_t *usage,
                              llm_cache_record_t *out) {
   if (!usage) {
      return;
   }
   llm_cache_record_t rec = { .type = usage->type,
                              .provider = usage->provider,
                              .iteration = t_iteration,
                              .expected = -1,
                              .state = LLM_CACHE_WARM };
   rec.prompt = usage->prompt_tokens > 0 ? usage->prompt_tokens : 0;
   rec.read = usage->cached_tokens > 0 ? usage->cached_tokens : 0;
   rec.write = usage->cache_write_tokens > 0 ? usage->cache_write_tokens : 0;
   rec.output = usage->completion_tokens > 0 ? usage->completion_tokens : 0;
   rec.uncached = rec.prompt - rec.read - rec.write;
   if (rec.uncached < 0) {
      rec.uncached = 0;
   }

   session_t *session = session_get_command_context();
   rec.kind = call_kind(session);
   rec.conversation_id = session ? atomic_load(&session->stream_conversation_id) : 0;
   const bool noted = t_note.valid;
   t_note.valid = false; /* one record per noted request */
   if (noted) {
      safe_strscpy(rec.model, t_note.model);
      safe_strscpy(rec.thinking, t_note.thinking);
      rec.tools_hash = t_note.prefix.tools;
      rec.system_hash = t_note.prefix.system;
      rec.images = t_note.images;
   }

   /* Expected read: for a conversation's own calls on Anthropic, where the
    * provider's counts define it (the next call reads what this one read or
    * wrote, while the prefix and the TTL hold).  Side calls have their own
    * prefixes and don't disturb a conversation's key.  The implicit caches
    * (OpenAI, Gemini, local) are best-effort: a request routed elsewhere can miss
    * with an identical prefix, so a read below the prior span is not a defect and
    * there is no expected read to hold them to. */
   if (noted && session_id > 0 && usage->type == LLM_CLOUD &&
       usage->provider == CLOUD_PROVIDER_CLAUDE && llm_call_kind_is_conversation(rec.kind)) {
      rec.expected = llm_cache_monitor_expected_read_at(
          session_id, rec.conversation_id, &t_note.prefix,
          llm_cache_monitor_ttl_ms(usage->type, usage->provider), rec.read + rec.write,
          usage->message_id, now_ms(), &rec.gap_ms, &rec.state);
   }

   if (usage->cache_miss_reason && usage->cache_miss_reason[0]) {
      safe_strscpy(rec.miss_reason, usage->cache_miss_reason);
      rec.missed_tokens = usage->cache_missed_tokens;
   }

   const char *provider = rec.type == LLM_LOCAL ? "local" : cloud_provider_to_string(rec.provider);
   const int coverage = rec.prompt > 0 ? (int)((int64_t)rec.read * 100 / rec.prompt) : 0;
   char expected_str[24] = "n/a";
   if (rec.expected >= 0) {
      snprintf(expected_str, sizeof(expected_str), "%d", rec.expected);
   }
   OLOG_INFO("LLM cache: provider=%s model=%s kind=%s conv=%lld iter=%d prompt=%d read=%d "
             "write=%d uncached=%d coverage=%d%% expected=%s state=%s gap=%.1fs tools=%08x "
             "system=%08x thinking=%s images=%d miss=%s/%d output=%d",
             provider ? provider : "?", rec.model[0] ? rec.model : "?",
             llm_call_kind_name(rec.kind), (long long)rec.conversation_id, rec.iteration,
             rec.prompt, rec.read, rec.write, rec.uncached, coverage, expected_str,
             rec.expected >= 0 ? llm_cache_state_name(rec.state) : "n/a",
             (double)rec.gap_ms / 1000.0, rec.tools_hash, rec.system_hash,
             rec.thinking[0] ? rec.thinking : "-", rec.images ? 1 : 0,
             rec.miss_reason[0] ? rec.miss_reason : "-", rec.missed_tokens, rec.output);
   enqueue_row(&rec, session, provider);
   if (out) {
      *out = rec;
   }
}

void llm_cache_monitor_flush(void) {
   /* Copy the queued rows (they stay queued), write them, and only then remove
    * them: a failed write keeps them for the next flush.  Rows dropped from the
    * full ring while writing are counted by sequence, not position. */
   pthread_mutex_lock(&s_queue_mutex);
   const int n = s_queue_count;
   const uint64_t first_seq = s_queue_head_seq;
   for (int i = 0; i < n; i++) {
      s_flush_batch[i] = s_queue[(s_queue_head + i) % USAGE_QUEUE_MAX];
   }
   const int dropped = s_queue_dropped;
   s_queue_dropped = 0;
   pthread_mutex_unlock(&s_queue_mutex);
   if (dropped > 0) {
      OLOG_WARNING("LLM cache: %d usage record(s) dropped before they could be written", dropped);
   }
   if (n == 0) {
      return;
   }
   if (auth_db_llm_usage_insert(s_flush_batch, n) != AUTH_DB_SUCCESS) {
      OLOG_WARNING("LLM cache: %d usage record(s) not written yet; kept for the next try", n);
      return;
   }
   pthread_mutex_lock(&s_queue_mutex);
   const uint64_t written_end = first_seq + (uint64_t)n;
   if (s_queue_head_seq < written_end) {
      int remove = (int)(written_end - s_queue_head_seq);
      if (remove > s_queue_count) {
         remove = s_queue_count;
      }
      s_queue_head = (s_queue_head + remove) % USAGE_QUEUE_MAX;
      s_queue_head_seq += (uint64_t)remove;
      s_queue_count -= remove;
   }
   pthread_mutex_unlock(&s_queue_mutex);
}
