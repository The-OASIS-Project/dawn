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
 * A session's own history, after each kind of turn it runs, is the request a
 * reload of its conversation builds: the turn runs through the real dispatch,
 * session unit, append-only prefix and LLM call finalizer (the model is a
 * fake), saves its rows as the WebUI does, and the live history and
 * memory_history_request_context() are rendered by the Claude and OpenAI
 * Responses request builders and compared byte for byte.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <json-c/json.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_conv_prefix.h"
#include "auth/auth_db_internal.h"
#include "auth/auth_db_messages.h"
#include "config/dawn_config.h"
#include "core/llm_response_finalize.h"
#include "core/prompt_parts.h"
#include "core/session_compaction.h"
#include "core/session_manager.h"
#include "core/session_prefix.h"
#include "core/text_input_dispatch.h"
#include "dawn_error.h"
#include "llm/llm_claude_format.h"
#include "llm/llm_compaction.h"
#include "llm/llm_context.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_history_rows.h"
#include "llm/llm_interface.h"
#include "llm/llm_local_provider.h"
#include "llm/llm_openai_responses_input.h"
#include "llm/llm_tool_history.h"
#include "llm/llm_tool_images.h"
#include "llm/llm_tools.h"
#include "llm/llm_turn_blocks.h"
#include "memory/memory_history_loader.h"
#include "unity.h"

dawn_config_t g_config;

#define CARRIER "api.anthropic.com#0123456789abcdef"
#define MODEL "claude-opus-5-5"
#define FLAGS (JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE)

/* ---- the model: one scripted response per call ---- */

typedef struct {
   const char *reply;     /* the final answer; NULL = the call fails */
   bool thinking;         /* the answer comes with signed reasoning */
   const char *tool_call; /* a tool exchange runs first: the call's id */
   bool openai;           /* ... on an OpenAI path (the history in OpenAI shape) */
   const char *visual;    /* a visual the turn rendered (render_visual's pending_visual) */
   bool cancel_after;     /* the user's stop lands once the answer is complete */
   bool cancel_during;    /* the stop lands mid-stream: no answer */
} script_t;

static script_t s_script;
static session_t *s;
static int s_user;

static struct json_object *signed_blocks(const char *text) {
   struct json_object *blocks = llm_turn_blocks_new();
   llm_turn_blocks_add_reasoning(blocks, CARRIER, LLM_FORMAT_ANTHROPIC, MODEL,
                                 json_tokener_parse("{\"type\":\"thinking\",\"thinking\":"
                                                    "\"Let me see.\",\"signature\":\"SIG\"}"));
   llm_turn_blocks_add_text(blocks, text);
   return blocks;
}

/* A tool exchange as the tool loop appends it (llm_tool_history.h): on a
 * Claude path the call's blocks rendered and one tool_result message, on an
 * OpenAI path the call with its tool_calls and a role:tool message.  Saved as
 * the loop saves it: each appended message's rows, through the turn's
 * persist hook. */
static void run_tool_exchange(struct json_object *history, const char *call_id, bool openai) {
   const int before = (int)json_object_array_length(history);
   llm_tool_response_t response;
   memset(&response, 0, sizeof(response));
   response.text = "Checking.";
   response.tool_calls.count = 1;
   tool_call_t *tc = &response.tool_calls.calls[0];
   snprintf(tc->id, sizeof(tc->id), "%s", call_id);
   snprintf(tc->name, sizeof(tc->name), "weather");
   snprintf(tc->arguments, sizeof(tc->arguments), "{\"city\":\"Paris\"}");
   response.has_tool_calls = true;
   if (!openai) {
      response.blocks = llm_turn_blocks_new();
      llm_turn_blocks_add_reasoning(response.blocks, CARRIER, LLM_FORMAT_ANTHROPIC, MODEL,
                                    json_tokener_parse("{\"type\":\"thinking\",\"thinking\":"
                                                       "\"A tool.\",\"signature\":\"TSIG\"}"));
      llm_turn_blocks_add_text(response.blocks, "Checking.");
      llm_turn_blocks_add_tool_call(response.blocks, call_id, "weather", "{\"city\":\"Paris\"}");
   }
   tool_result_list_t *results = calloc(1, sizeof(*results));
   TEST_ASSERT_NOT_NULL(results);
   results->count = 1;
   snprintf(results->results[0].tool_call_id, sizeof(results->results[0].tool_call_id), "%s",
            call_id);
   snprintf(results->results[0].result, sizeof(results->results[0].result), "Sunny, 21C");
   results->results[0].success = true;
   if (openai) {
      llm_tool_history_append_openai(history, &response, results);
   } else {
      llm_tool_history_append_claude(history, &response, results, CARRIER);
   }
   json_object_put(response.blocks);
   free(results);

   struct json_object *rows = json_object_new_array();
   for (int i = before; i < (int)json_object_array_length(history); i++) {
      llm_history_rows_append(json_object_array_get_idx(history, i), rows);
   }
   for (int i = 0; i < (int)json_object_array_length(rows); i++) {
      struct json_object *m = json_object_array_get_idx(rows, i);
      struct json_object *v = NULL;
#define FIELD(k) (json_object_object_get_ex(m, (k), &v) ? json_object_get_string(v) : NULL)
      struct json_object *tc_obj = NULL;
      const session_tool_row_t row = {
         .role = FIELD("role"),
         .content = FIELD("content"),
         .tool_calls = json_object_object_get_ex(m, "tool_calls", &tc_obj)
                           ? json_object_to_json_string(tc_obj)
                           : NULL,
         .tool_call_id = FIELD("tool_call_id"),
         .llm_blocks = FIELD(LLM_HISTORY_ROW_STORED_KEY),
      };
#undef FIELD
      (void)s->tool_persist_cb(s->tool_persist_userdata, &row);
   }
   json_object_put(rows);
}

char *llm_chat_completion_streaming_with_config(struct json_object *conversation_history,
                                                const char *input_text,
                                                llm_text_chunk_callback chunk_callback,
                                                void *callback_userdata,
                                                const llm_resolved_config_t *config) {
   (void)input_text, (void)chunk_callback, (void)callback_userdata, (void)config;
   if (s_script.tool_call) {
      run_tool_exchange(conversation_history, s_script.tool_call, s_script.openai);
   }
   if (s_script.visual) {
      /* What the render_visual tool leaves for the final answer's save. */
      pthread_mutex_lock(&s->tools_mutex);
      free(s->pending_visual);
      s->pending_visual = strdup(s_script.visual);
      pthread_mutex_unlock(&s->tools_mutex);
   }
   if (s_script.cancel_during) {
      atomic_store(&s->cancel_requested, true);
      return NULL;
   }
   if (!s_script.reply) {
      return NULL;
   }
   if (s_script.thinking) {
      /* The tool loop's stash of the final answer's blocks. */
      s->final_answer.blocks = signed_blocks(s_script.reply);
   }
   if (s_script.cancel_after) {
      atomic_store(&s->cancel_requested, true);
   }
   return strdup(s_script.reply);
}

/* The finalizer's own work (tags, citations) is tested elsewhere: the text as given. */
int llm_response_finalize(session_t *session, const char *raw_response, response_final_t *out) {
   (void)session;
   out->text = strdup(raw_response);
   return out->text ? SUCCESS : FAILURE;
}

/* ---- the prompt each turn applies (session_dispatch_user_turn_ex) ---- */

static const char *s_persona = "P1";
static const char *s_directive = "D1";
static const char *s_time = "09:00";

int session_dispatch_user_turn_ex(session_t *session,
                                  const char *user_turn_text,
                                  const char *turn_note) {
   (void)user_turn_text;
   composed_prompt_t cp = { .directives = strdup(s_directive) };
   prompt_sections_add(&cp, "persona", "your persona", s_persona);
   prompt_sections_add(&cp, "rules", "your operating rules", "R");
   cp.stable_prefix = prompt_sections_join(&cp);
   char ctx[64];
   snprintf(ctx, sizeof(ctx), "[system_time] %s\n", s_time);
   cp.context_head = strdup(ctx);
   /* As session_manager.c's dispatch_with_note: a history past its window is
    * compacted at the seam, before the turn's prompt is applied. */
   session_compaction_prepare(session, 0);
   session_prefix_apply_turn(session, &cp, turn_note);
   composed_prompt_free(&cp);
   return SUCCESS;
}

/* ---- the runtime the units call, stubbed ---- */

/* The thread's turn token, as session_manager.c keeps it. */
static __thread uint64_t tl_turn_token;
uint64_t session_turn_token(void) {
   return tl_turn_token;
}
void session_set_turn_token(uint64_t token) {
   tl_turn_token = token;
}
int session_effective_user_id(session_t *session) {
   (void)session;
   return s_user;
}
void session_touch(session_t *session) {
   (void)session;
}
void session_retain(session_t *session) {
   (void)session;
}
void session_release(session_t *session) {
   (void)session;
}
static session_t *s_command_context;
void session_set_command_context(session_t *session) {
   s_command_context = session;
}
session_t *session_get_command_context(void) {
   return s_command_context;
}
void session_get_llm_config(session_t *session, session_llm_config_t *config) {
   (void)session;
   memset(config, 0, sizeof(*config));
}
void session_manager_for_each_session_any(void (*fn)(session_t *session, void *ctx), void *ctx) {
   fn(s, ctx);
}
void job_manager_for_each_session(void (*fn)(session_t *session, void *ctx), void *ctx) {
   (void)fn, (void)ctx;
}

/* No focus handles, tool-result store, events or client frames here. */
int focus_handles_flush(struct session *session, int64_t conv_id, int user_id) {
   (void)session, (void)conv_id, (void)user_id;
   return SUCCESS;
}
void focus_handles_reset_locked(struct session *session) {
   (void)session;
}
int focus_handles_withdrawn_locked(const struct session *session,
                                   int64_t conv_id,
                                   const char *const *item_ids,
                                   int n_ids,
                                   int *out,
                                   int cap) {
   (void)session, (void)conv_id, (void)item_ids, (void)n_ids, (void)out, (void)cap;
   return 0;
}
int tool_result_store_bind_locked(struct session *session,
                                  int64_t conv_id,
                                  uint64_t turn_token,
                                  bool with_ended) {
   (void)session, (void)conv_id, (void)turn_token, (void)with_ended;
   return 0;
}
void tool_result_store_drop_trees(struct session *session) {
   (void)session;
}
void tool_result_store_reset_locked(struct session *session) {
   (void)session;
}
void tool_result_store_turn_ended_locked(struct session *session,
                                         uint64_t turn_token,
                                         bool to_live) {
   (void)session, (void)turn_token, (void)to_live;
}
void conv_event_emit(int64_t conv_id, int user_id, const char *kind, char *payload_owned) {
   (void)conv_id, (void)user_id, (void)kind;
   free(payload_owned);
}
char *event_payload_status(bool generating) {
   (void)generating;
   return NULL;
}
void webui_send_error(struct session *session, const char *code, const char *message) {
   (void)session, (void)code, (void)message;
}
void webui_send_metrics_update(struct session *session,
                               const char *state,
                               int ttft_ms,
                               float token_rate,
                               int context_percent) {
   (void)session, (void)state, (void)ttft_ms, (void)token_rate, (void)context_percent;
}
void webui_send_stream_delta(struct session *session, const char *text) {
   (void)session, (void)text;
}
void webui_send_stream_end(struct session *session, const char *reason) {
   (void)session, (void)reason;
}
void webui_send_transcript(struct session *session, const char *role, const char *text) {
   (void)session, (void)role, (void)text;
}
void webui_send_transcript_ex(struct session *session,
                              const char *role,
                              const char *text,
                              bool server_saved,
                              int64_t message_id) {
   (void)session, (void)role, (void)text, (void)server_saved, (void)message_id;
}
int text_to_speech(char *text) {
   (void)text;
   return 0;
}

/* The LLM layer around the call: one Claude model, no tools, no compaction. */
int llm_resolve_config(const session_llm_config_t *session_config,
                       llm_resolved_config_t *resolved) {
   (void)session_config;
   memset(resolved, 0, sizeof(*resolved));
   resolved->type = LLM_CLOUD;
   resolved->cloud_provider = CLOUD_PROVIDER_CLAUDE;
   resolved->model = MODEL;
   return 0;
}
void llm_turn_result_reset(void) {
}
bool llm_take_inline_tools_rejected(void) {
   return false;
}
int llm_last_error(void) {
   return LLM_ERR_NONE;
}
void llm_context_reset_turn_cache(uint32_t session_id) {
   (void)session_id;
}
void llm_set_cancel_flag(void *flag) {
   (void)flag;
}
void llm_set_cancel_flag_ex(void *flag, bool honor_global) {
   (void)flag, (void)honor_global;
}
void *llm_get_cancel_flag(void) {
   return NULL;
}
int llm_is_interrupt_requested(void) {
   return 0;
}
bool llm_tools_enabled(const llm_resolved_config_t *config) {
   (void)config;
   return false;
}
bool llm_tools_suppressed(void) {
   return true;
}
struct json_object *llm_tools_request_tools(struct json_object *history,
                                            bool is_remote,
                                            bool claude,
                                            bool inline_ok,
                                            const char **source_out) {
   (void)history, (void)is_remote, (void)claude, (void)inline_ok;
   if (source_out) {
      *source_out = NULL;
   }
   return NULL;
}
bool llm_cache_monitor_one_off_call(void) {
   return false;
}
void llm_cache_monitor_history_rewritten(uint32_t session_id) {
   (void)session_id;
}
const char *llm_get_default_claude_model(void) {
   return MODEL;
}
const char *llm_get_current_thinking_mode(void) {
   return "disabled";
}
const char *llm_get_current_reasoning_effort(void) {
   return "medium";
}
const char *llm_get_current_utility_effort(void) {
   return "medium";
}
int llm_budget_tokens_for_effort(const char *effort) {
   (void)effort;
   return 0;
}
local_provider_t llm_local_detect_provider(const char *endpoint) {
   (void)endpoint;
   return LOCAL_PROVIDER_UNKNOWN;
}
local_provider_t llm_local_get_provider(void) {
   return LOCAL_PROVIDER_UNKNOWN;
}
int is_vision_enabled_for_current_llm(void) {
   return 1;
}
int llm_context_estimate_tokens(struct json_object *history) {
   return llm_compaction_estimate_range(history, 0, (int)json_object_array_length(history));
}
/* A compaction: the history reaches its threshold, and the summary written. */
static bool s_over;
static const char *s_summary;

bool llm_context_over_threshold(uint32_t session_id,
                                struct json_object *history,
                                int extra_tokens,
                                llm_type_t type,
                                cloud_provider_t provider,
                                const char *model,
                                float threshold) {
   (void)session_id, (void)history, (void)extra_tokens, (void)type, (void)provider;
   (void)model, (void)threshold;
   return s_over;
}
void llm_context_note_compacted(uint32_t session_id, int tokens) {
   (void)session_id, (void)tokens;
}
char *llm_context_summarize(struct json_object *to_summarize,
                            int kept_tokens,
                            int window_tokens,
                            const llm_compaction_calibration_t *cal,
                            const char *tag,
                            const session_llm_config_t *config,
                            const atomic_bool *cancel,
                            llm_compaction_level_t *level_out) {
   (void)to_summarize, (void)kept_tokens, (void)window_tokens, (void)cal, (void)tag;
   (void)config, (void)cancel;
   *level_out = LLM_COMPACT_NORMAL;
   return s_summary ? strdup(s_summary) : NULL;
}
int llm_context_get_size(llm_type_t type, cloud_provider_t provider, const char *model) {
   (void)type, (void)provider, (void)model;
   return 200000;
}
void llm_context_calibration(uint32_t session_id,
                             llm_type_t type,
                             cloud_provider_t provider,
                             const char *model,
                             llm_compaction_calibration_t *out) {
   (void)session_id, (void)type, (void)provider, (void)model;
   memset(out, 0, sizeof(*out));
}
float llm_context_hard_threshold(void) {
   return 0.85f;
}

/* ---- a turn as the WebUI text worker runs it ---- */

static int64_t persist_tool_row(void *userdata, const session_tool_row_t *row) {
   (void)userdata;
   const conv_message_row_t db_row = { .role = row->role,
                                       .content = row->content ? row->content : "",
                                       .tool_calls = row->tool_calls,
                                       .tool_call_id = row->tool_call_id,
                                       .llm_blocks = row->llm_blocks,
                                       .kind = row->kind };
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(atomic_load(&s->stream_conversation_id),
                                                          s_user, &db_row, &id));
   return id;
}

/* The final answer saved as webui_persist_final_answer (webui_broadcasts.c)
 * saves it: its blocks with the row, a rendered visual taken from the session
 * and joined after the body (memory_history_join_visual), the row's id on the
 * history.  Its reasoning column, retention and fan-out are left out: a reload
 * reads none of them. */
static void persist_reply(int64_t conv, const char *body) {
   struct json_object *blocks = session_take_reply_blocks(s);
   char *stored = llm_turn_blocks_answer_stored(blocks);
   json_object_put(blocks);
   pthread_mutex_lock(&s->tools_mutex);
   char *visual = s->pending_visual;
   s->pending_visual = NULL;
   pthread_mutex_unlock(&s->tools_mutex);
   char *combined = (visual && visual[0]) ? memory_history_join_visual(body, visual) : NULL;
   const conv_message_row_t row = { .role = "assistant",
                                    .content = combined ? combined : body,
                                    .llm_blocks = stored };
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, s_user, &row, &id));
   free(combined);
   free(visual);
   free(stored);
   session_stamp_last_message_id(s, "assistant", id);
}

/* The question's kind (an envelope for a re-engagement), and whether a turn
 * with no reply takes its question back as a re-engagement does. */
static message_kind_t s_question_kind = MESSAGE_KIND_NONE;
static bool s_retract_unanswered;

static void run_turn(int64_t conv, const char *question, script_t script) {
   s_script = script;
   atomic_store(&s->cancel_requested, false);
   session_turn_begin(s, conv, s_user);
   s->tool_persist_cb = persist_tool_row;
   s->tool_persist_userdata = NULL;
   atomic_store(&s->will_persist_turn, true);
   const text_input_dispatch_opts_t opts = { .conversation_id = conv,
                                             .auth_user_id = s_user,
                                             .question_kind = s_question_kind };
   char *reply = core_text_input_dispatch(s, question, &opts);
   atomic_store(&s->will_persist_turn, false);
   s->tool_persist_cb = NULL;
   if (!reply) {
      /* A stop after the answer completed: the answer saved anyway. */
      reply = s->cancelled_final_response;
      s->cancelled_final_response = NULL;
   }
   if (reply && reply[0] != '\0') {
      persist_reply(conv, reply);
   } else if (s_retract_unanswered) {
      /* As job_reinvoke.c's retract_attempt: by the turn's question id.  An
       * envelope is request context, never the turn's question, so there is
       * none and nothing is taken back (a known gap of the retraction, kept
       * here as it is: live and stored both keep the envelope). */
      const int64_t envelope = session_turn_question_id(s);
      /* Pins today's behavior: the envelope is never the turn's question, so
       * there is no id to retract by.  Once a re-engagement can take back its
       * envelope, this expects that id and the comparisons below expect the
       * envelope gone from both. */
      TEST_ASSERT_EQUAL_INT64(0, envelope);
      if (envelope > 0 && conv_db_retract_envelope(conv, s_user, envelope) == AUTH_DB_SUCCESS) {
         (void)session_rollback_turn(s);
      }
   }
   free(reply);
   session_turn_end(s);
}

/* ---- live vs reload, as each provider is sent them ---- */

static char *claude_request(struct json_object *history) {
   struct json_object *req = convert_to_claude_format(history, NULL, MODEL, CARRIER, 0, false);
   TEST_ASSERT_NOT_NULL(req);
   char *out = strdup(json_object_to_json_string_ext(req, FLAGS));
   json_object_put(req);
   return out;
}

static char *responses_request(struct json_object *history) {
   char *instructions = llm_responses_extract_stable_instructions(history);
   char *volatile_ctx = llm_responses_extract_volatile_context(history);
   struct json_object *input = llm_responses_build_input(
       history, NULL, volatile_ctx, llm_responses_count_leading_system_run(history), false,
       "api.openai.com#0123456789abcdef", "gpt-5.5");
   TEST_ASSERT_NOT_NULL(input);
   const char *in = json_object_to_json_string_ext(input, FLAGS);
   size_t len = strlen(in) + (instructions ? strlen(instructions) : 0) + 2;
   char *out = malloc(len);
   snprintf(out, len, "%s\n%s", instructions ? instructions : "", in);
   json_object_put(input);
   free(instructions);
   free(volatile_ctx);
   return out;
}

static struct json_object *reload(int64_t conv) {
   conversation_t c = { 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_get(conv, s_user, &c));
   struct json_object *h = memory_history_request_context(conv, s_user, c.context_watermark_msg_id,
                                                          c.compaction_summary, NULL, NULL);
   conv_free(&c);
   TEST_ASSERT_NOT_NULL(h);
   return h;
}

/* Every provider is sent the same request from the live history as from a
 * reload, with the next question asked; the Claude request carries @p carries
 * (NULL: nothing to look for). */
static void assert_live_is_reload(int64_t conv, const char *carries) {
   struct json_object *reloaded = reload(conv);
   struct json_object *live = json_object_get(s->conversation_history);
   struct json_object *both[2] = { live, reloaded };
   char *claude[2], *responses[2];
   for (int i = 0; i < 2; i++) {
      struct json_object *next = json_object_new_object();
      json_object_object_add(next, "role", json_object_new_string("user"));
      json_object_object_add(next, "content", json_object_new_string("And then?"));
      /* A request is built from the history as it is held (blocks, kinds). */
      struct json_object *h = NULL;
      TEST_ASSERT_EQUAL_INT(0, json_object_deep_copy(both[i], &h, NULL));
      json_object_array_add(h, next);
      claude[i] = claude_request(h);
      responses[i] = responses_request(h);
      json_object_put(h);
   }
   TEST_ASSERT_EQUAL_STRING(claude[1], claude[0]);
   if (carries) {
      TEST_ASSERT_NOT_NULL(strstr(claude[0], carries));
   }
   TEST_ASSERT_EQUAL_STRING(responses[1], responses[0]);
   for (int i = 0; i < 2; i++) {
      free(claude[i]);
      free(responses[i]);
   }
   json_object_put(live);
   json_object_put(reloaded);
}

/* Whether the conversation's last assistant row is saved as @p content. */
static bool saved_reply_has(int64_t conv, const char *content) {
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK,
                         sqlite3_prepare_v2(s_db.db,
                                            "SELECT content FROM messages WHERE conversation_id = "
                                            "?1 AND role = 'assistant' ORDER BY id DESC LIMIT 1",
                                            -1, &st, NULL));
   sqlite3_bind_int64(st, 1, conv);
   const bool same = sqlite3_step(st) == SQLITE_ROW &&
                     strcmp((const char *)sqlite3_column_text(st, 0), content) == 0;
   sqlite3_finalize(st);
   return same;
}

/* ---- setup ---- */

static int64_t s_conv;

void setUp(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("alice", "h", true));
   auth_user_t u;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user("alice", &u));
   s_user = u.id;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "c", &s_conv));

   s = calloc(1, sizeof(*s));
   s->session_id = 7;
   s->type = SESSION_TYPE_WEBUI;
   s->metrics.user_id = (uint32_t)s_user;
   pthread_mutex_init(&s->history_mutex, NULL);
   pthread_mutex_init(&s->llm_config_mutex, NULL);
   pthread_mutex_init(&s->metrics_mutex, NULL);
   pthread_mutex_init(&s->tools_mutex, NULL);
   s->conversation_history = json_object_new_array();
   struct json_object *sys = json_object_new_object();
   json_object_object_add(sys, "role", json_object_new_string("system"));
   json_object_object_add(sys, "content", json_object_new_string("start"));
   json_object_array_add(s->conversation_history, sys);
   session_bind_history_conversation(s, s_conv);
   s_persona = "P1";
   s_directive = "D1";
   s_time = "09:00";
}

void tearDown(void) {
   free(s->pending_visual);
   free(s->cancelled_final_response);
   session_final_answer_clear(s);
   session_compaction_teardown(s);
   session_prefix_turn_free(s->prefix_turn);
   json_object_put(s->withdraw_pending);
   json_object_put(s->conversation_history);
   pthread_mutex_destroy(&s->history_mutex);
   pthread_mutex_destroy(&s->llm_config_mutex);
   pthread_mutex_destroy(&s->metrics_mutex);
   pthread_mutex_destroy(&s->tools_mutex);
   free(s);
   s = NULL;
   auth_db_shutdown();
}

/* ---- the turn kinds ---- */

#define SIGNED "\"signature\":\"SIG\""

static void test_plain_turns(void) {
   run_turn(s_conv, "Q1", (script_t){ .reply = "A1" });
   assert_live_is_reload(s_conv, "A1");
   s_time = "09:05";
   run_turn(s_conv, "Q2", (script_t){ .reply = "A2" });
   assert_live_is_reload(s_conv, "A2");
}

/* A reply with signed reasoning: replayed with its signature from both. */
static void test_a_reply_with_its_reasoning(void) {
   run_turn(s_conv, "Q1", (script_t){ .reply = "A1", .thinking = true });
   assert_live_is_reload(s_conv, SIGNED);
   run_turn(s_conv, "Q2", (script_t){ .reply = "A2", .thinking = true });
   assert_live_is_reload(s_conv, SIGNED);
}

/* A tool call and its result, as rows and as the turn's blocks. */
static void test_a_tool_exchange(void) {
   run_turn(s_conv, "Q1", (script_t){ .reply = "It's sunny.", .tool_call = "toolu_1" });
   assert_live_is_reload(s_conv, "\"signature\":\"TSIG\"");
   run_turn(s_conv, "Q2", (script_t){ .reply = "A2", .thinking = true, .tool_call = "toolu_2" });
   assert_live_is_reload(s_conv, "toolu_2");
}

/* Turn context every turn, a directive change and a persona change (an
 * instruction) mid-conversation: request-context rows and parts. */
static void test_context_and_changed_directions(void) {
   run_turn(s_conv, "Q1", (script_t){ .reply = "A1" });
   s_time = "09:05";
   s_directive = "D2";
   run_turn(s_conv, "Q2", (script_t){ .reply = "A2" });
   assert_live_is_reload(s_conv, "D2");
   s_time = "09:10";
   s_persona = "P2";
   run_turn(s_conv, "Q3", (script_t){ .reply = "A3" });
   assert_live_is_reload(s_conv, "P2");
}

/* A stop that lands once the answer is complete: the answer is saved, and the
 * next request carries it as a reload does. */
static void test_a_reply_stopped_after_it_completed(void) {
   run_turn(s_conv, "Q1", (script_t){ .reply = "A1" });
   run_turn(s_conv, "Q2", (script_t){ .reply = "A2", .cancel_after = true });
   assert_live_is_reload(s_conv, "A2");
   run_turn(s_conv, "Q3", (script_t){ .reply = "A3" });
   assert_live_is_reload(s_conv, "A3");
}

/* The same with its signed reasoning, after a tool exchange: the blocks the
 * row is saved with are the ones the history replays. */
static void test_a_reasoned_reply_stopped_after_it_completed(void) {
   run_turn(
       s_conv, "Q1",
       (script_t){ .reply = "A1", .thinking = true, .tool_call = "toolu_1", .cancel_after = true });
   assert_live_is_reload(s_conv, SIGNED);
   run_turn(s_conv, "Q2", (script_t){ .reply = "A2", .thinking = true });
   assert_live_is_reload(s_conv, SIGNED);
}

/* A stop mid-stream: no answer anywhere; the question and what the turn saved
 * before the stop (its tool exchange) in both. */
static void test_a_reply_stopped_mid_stream(void) {
   run_turn(s_conv, "Q1", (script_t){ .reply = "A1" });
   run_turn(s_conv, "Q2", (script_t){ .cancel_during = true });
   assert_live_is_reload(s_conv, "Q2");
   run_turn(s_conv, "Q3", (script_t){ .tool_call = "toolu_3", .cancel_during = true });
   assert_live_is_reload(s_conv, "toolu_3");
   run_turn(s_conv, "Q4", (script_t){ .reply = "A4" });
   assert_live_is_reload(s_conv, "A4");
}

/* A re-engagement: DAWN's envelope as the question (request context), its
 * reply; one that got no reply, and the retraction a re-engagement tries. */
static void test_a_re_engagement(void) {
   run_turn(s_conv, "Q1", (script_t){ .reply = "A1" });
   s_question_kind = MESSAGE_KIND_ENVELOPE;
   run_turn(s_conv, "A job finished: E1", (script_t){ .reply = "It did." });
   assert_live_is_reload(s_conv, "E1");
   s_retract_unanswered = true;
   run_turn(s_conv, "A job finished: E2", (script_t){ .reply = NULL });
   assert_live_is_reload(s_conv, "E2");
   s_question_kind = MESSAGE_KIND_NONE;
   s_retract_unanswered = false;
   run_turn(s_conv, "Q2", (script_t){ .reply = "A2" });
   assert_live_is_reload(s_conv, "A2");
}

/* A tool exchange on an OpenAI path: the call with its tool_calls and a
 * role:tool result in the history, as the reload rebuilds them. */
static void test_an_openai_tool_exchange(void) {
   run_turn(s_conv, "Q1",
            (script_t){ .reply = "It's sunny.", .tool_call = "call_1", .openai = true });
   bool tool_row = false;
   for (size_t i = 0; i < json_object_array_length(s->conversation_history); i++) {
      struct json_object *role = NULL;
      tool_row |= json_object_object_get_ex(json_object_array_get_idx(s->conversation_history, i),
                                            "role", &role) &&
                  strcmp(json_object_get_string(role), "tool") == 0;
   }
   TEST_ASSERT_TRUE(tool_row); /* the history holds it in OpenAI shape */
   assert_live_is_reload(s_conv, "call_1");
   run_turn(s_conv, "Q2", (script_t){ .reply = "A2", .tool_call = "call_2", .openai = true });
   assert_live_is_reload(s_conv, "call_2");
}

#define VISUAL "<dawn-visual title=\"Chart\"><svg></svg></dawn-visual>"

/* A reply with a rendered visual: saved with the visual after its body, kept
 * in the history without it; a reload takes the visual off again. */
static void test_a_reply_with_a_rendered_visual(void) {
   run_turn(s_conv, "Q1",
            (script_t){ .reply = "Here it is.", .tool_call = "toolu_v", .visual = VISUAL });
   assert_live_is_reload(s_conv, "Here it is.");
   struct json_object *reloaded = reload(s_conv);
   TEST_ASSERT_NULL(strstr(json_object_to_json_string(reloaded), "dawn-visual"));
   json_object_put(reloaded);
   TEST_ASSERT_TRUE(saved_reply_has(s_conv, "Here it is.\n" VISUAL "\n"));
   run_turn(s_conv, "Q2", (script_t){ .reply = "A2" });
   assert_live_is_reload(s_conv, "A2");
}

/* A compaction at the next turn's seam, after a tool exchange: the summary
 * leads the first kept question, live and on reload. */
static void test_a_compaction_at_the_seam(void) {
   run_turn(s_conv, "Q1", (script_t){ .reply = "A1" });
   run_turn(s_conv, "Q2", (script_t){ .reply = "A2", .thinking = true, .tool_call = "toolu_1" });
   run_turn(s_conv, "Q3", (script_t){ .reply = "A3" });
   run_turn(s_conv, "Q4", (script_t){ .reply = "A4" });
   run_turn(s_conv, "Q5", (script_t){ .reply = "A5" });
   s_over = true;
   s_summary = "They talked about Q1 and Q2.";
   run_turn(s_conv, "Q6", (script_t){ .reply = "A6" });
   s_over = false;
   s_summary = NULL;
   TEST_ASSERT_NULL(strstr(json_object_to_json_string(s->conversation_history), "\"Q1\""));
   assert_live_is_reload(s_conv, "They talked about Q1 and Q2.");
   run_turn(s_conv, "Q7", (script_t){ .reply = "A7" });
   assert_live_is_reload(s_conv, "A7");
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_plain_turns);
   RUN_TEST(test_a_reply_with_its_reasoning);
   RUN_TEST(test_a_tool_exchange);
   RUN_TEST(test_context_and_changed_directions);
   RUN_TEST(test_a_reply_stopped_after_it_completed);
   RUN_TEST(test_a_reasoned_reply_stopped_after_it_completed);
   RUN_TEST(test_a_reply_stopped_mid_stream);
   RUN_TEST(test_a_re_engagement);
   RUN_TEST(test_an_openai_tool_exchange);
   RUN_TEST(test_a_reply_with_a_rendered_visual);
   RUN_TEST(test_a_compaction_at_the_seam);
   return UNITY_END();
}
