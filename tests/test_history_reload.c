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
 * fake), saves a tool exchange's rows through the tool loop's own persist
 * (llm_tool_history_persist) and the job worker's hook and its reply as the
 * WebUI does, and the live history and
 * memory_history_request_context() are rendered by the Claude and OpenAI
 * Responses request builders and compared byte for byte.
 *
 * Two sessions on one conversation take turns of every kind, with posts,
 * compactions and boundaries between them: each, once caught up at its turn's
 * start, holds what a reload of the conversation builds.
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
#include "auth/auth_db_withdraw.h"
#include "blob_store.h"
#include "config/dawn_config.h"
#include "core/conv_post.h"
#include "core/conv_turn_lease.h"
#include "core/job_dispatch.h"
#include "core/llm_response_finalize.h"
#include "core/ocp_helpers.h"
#include "core/prompt_parts.h"
#include "core/session_compaction.h"
#include "core/session_manager.h"
#include "core/session_posts.h"
#include "core/session_prefix.h"
#include "core/session_stop.h"
#include "core/text_input_dispatch.h"
#include "core/turn_origin.h"
#include "core/turn_queue.h"
#include "dawn_error.h"
#include "image_store.h"
#include "llm/llm_claude_format.h"
#include "llm/llm_compaction.h"
#include "llm/llm_context.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_history_rows.h"
#include "llm/llm_interface.h"
#include "llm/llm_local_provider.h"
#include "llm/llm_openai_internal.h"
#include "llm/llm_openai_responses_input.h"
#include "llm/llm_selection.h"
#include "llm/llm_tool_defs.h"
#include "llm/llm_tool_history.h"
#include "llm/llm_tool_images.h"
#include "llm/llm_tools.h"
#include "llm/llm_turn_blocks.h"
#include "memory/memory_history_loader.h"
#include "prompts.h"
#include "unity.h"

dawn_config_t g_config;

#define CARRIER "api.anthropic.com#0123456789abcdef"
#define MODEL "claude-opus-5-5"
#define OPENAI_CARRIER "api.openai.com#0123456789abcdef"
#define OPENAI_MODEL "gpt-5.5"
#define CHAT_CARRIER "127.0.0.1:8080#0123456789abcdef"
#define CHAT_MODEL "local-model"
#define FLAGS (JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE)

/* ---- the model: one scripted response per call ---- */

typedef struct {
   const char *reply;     /* the final answer; NULL = the call fails */
   bool thinking;         /* the answer comes with signed reasoning */
   const char *tool_call; /* a tool exchange runs first: the call's id */
   bool openai;           /* ... on an OpenAI path (the history in OpenAI shape) */
   const char *visual;    /* a visual the turn rendered (render_visual's pending_visual) */
   bool tool_image;       /* the tool's result carries an image (a capture) */
   bool tool_reasoning;   /* on an OpenAI path, the call comes with encrypted reasoning */
   bool parallel;         /* a second call runs beside the first (its id + "b") */
   bool cancel_after;     /* the user's stop lands once the answer is complete */
   bool cancel_during;    /* the stop lands mid-stream: no answer */
   bool user_stop;        /* the user's Stop lands mid-stream (session_stop_turn_by_user) */
   const char *partial;   /* what had streamed when the stop cut the call (the tool loop's
                             final_answer.stopped_text) */
   bool capture;          /* the turn's confirm origin is read (s_origin), as a tool would */
   bool stop_after;       /* the user's Stop lands once the answer is complete (speech) */
   const char *post;      /* DAWN posts this into the conversation mid-turn */
   bool boundary;         /* the turn declares a boundary: reasoning so far left behind */
} script_t;

static script_t s_script;
static turn_origin_t s_origin;
static session_t *s; /* the session running the turn (one of s_sessions) */
static int s_user;
static int64_t s_conv;

/* The sessions on the conversation. */
#define SESSIONS_MAX 2
static session_t *s_sessions[SESSIONS_MAX];

static struct json_object *signed_blocks(const char *text) {
   struct json_object *blocks = llm_turn_blocks_new();
   llm_turn_blocks_add_reasoning(blocks, CARRIER, LLM_FORMAT_ANTHROPIC, MODEL,
                                 json_tokener_parse("{\"type\":\"thinking\",\"thinking\":"
                                                    "\"Let me see.\",\"signature\":\"SIG\"}"));
   llm_turn_blocks_add_text(blocks, text);
   return blocks;
}

/* A PNG's bytes (its signature, then a tag) as base64: what a camera tool
 * hands back. */
static char *png_base64(const char *tag) {
   unsigned char bytes[64] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
   const size_t n = 8 + strlen(tag);
   memcpy(bytes + 8, tag, n - 8);
   return ocp_base64_encode(bytes, n);
}

/* A tool exchange as the tool loop appends it (llm_tool_history.h): on a
 * Claude path the call's blocks rendered and one tool_result message, on an
 * OpenAI path the call with its tool_calls (and, with @p reasoning, the
 * encrypted reasoning it came with) and a role:tool message.  Saved as the
 * loop saves it (llm_tool_history_persist), through the turn's persist hook. */
static void run_tool_exchange(struct json_object *history, const script_t *sc) {
   const char *call_id = sc->tool_call;
   const int before = (int)json_object_array_length(history);
   llm_tool_response_t response;
   memset(&response, 0, sizeof(response));
   response.text = "Checking.";
   response.tool_calls.count = sc->parallel ? 2 : 1;
   for (int i = 0; i < response.tool_calls.count; i++) {
      tool_call_t *tc = &response.tool_calls.calls[i];
      snprintf(tc->id, sizeof(tc->id), "%s%s", call_id, i ? "b" : "");
      snprintf(tc->name, sizeof(tc->name), "weather");
      snprintf(tc->arguments, sizeof(tc->arguments), "{\"city\":\"%s\"}", i ? "Rome" : "Paris");
   }
   response.has_tool_calls = true;
   if (!sc->openai) {
      response.blocks = llm_turn_blocks_new();
      llm_turn_blocks_add_reasoning(response.blocks, CARRIER, LLM_FORMAT_ANTHROPIC, MODEL,
                                    json_tokener_parse("{\"type\":\"thinking\",\"thinking\":"
                                                       "\"A tool.\",\"signature\":\"TSIG\"}"));
      llm_turn_blocks_add_text(response.blocks, "Checking.");
      for (int i = 0; i < response.tool_calls.count; i++) {
         const tool_call_t *tc = &response.tool_calls.calls[i];
         llm_turn_blocks_add_tool_call(response.blocks, tc->id, tc->name, tc->arguments);
      }
   } else if (sc->tool_reasoning) {
      response.blocks = llm_turn_blocks_new();
      llm_turn_blocks_add_reasoning(response.blocks, OPENAI_CARRIER, LLM_FORMAT_OPENAI,
                                    OPENAI_MODEL,
                                    json_tokener_parse("{\"type\":\"reasoning\",\"id\":"
                                                       "\"rs_1\",\"encrypted_content\":"
                                                       "\"ENC\",\"summary\":[]}"));
      llm_turn_blocks_add_text(response.blocks, "Checking.");
      llm_turn_blocks_add_tool_call(response.blocks, call_id, "weather", "{\"city\":\"Paris\"}");
   }
   tool_result_list_t *results = calloc(1, sizeof(*results));
   TEST_ASSERT_NOT_NULL(results);
   results->count = response.tool_calls.count;
   for (int i = 0; i < results->count; i++) {
      tool_result_t *r = &results->results[i];
      snprintf(r->tool_call_id, sizeof(r->tool_call_id), "%s", response.tool_calls.calls[i].id);
      if (sc->tool_image) {
         TEST_ASSERT_TRUE(llm_tool_images_ingest(png_base64(r->tool_call_id), r));
         TEST_ASSERT_NOT_EQUAL('\0', r->vision_image_id[0]); /* stored, for the row to bind */
         snprintf(r->result, sizeof(r->result), "Image captured successfully.");
      } else {
         snprintf(r->result, sizeof(r->result), i ? "Rain, 15C" : "Sunny, 21C");
      }
      r->success = true;
   }
   if (sc->openai) {
      llm_tool_history_append_openai(history, &response, results);
   } else {
      llm_tool_history_append_claude(history, &response, results, CARRIER);
   }
   json_object_put(response.blocks);
   /* The display-only reasoning the loop saves with the call's row. */
   llm_tool_history_persist(s, history, before, "{\"text\":\"A tool.\"}", 1, results);
   for (int i = 0; i < results->count; i++) {
      free(results->results[i].vision_image);
   }
   free(results);
}

char *llm_chat_completion_streaming_with_config(struct json_object *conversation_history,
                                                const char *input_text,
                                                llm_text_chunk_callback chunk_callback,
                                                void *callback_userdata,
                                                const llm_resolved_config_t *config) {
   (void)input_text, (void)chunk_callback, (void)callback_userdata, (void)config;
   if (s_script.post) {
      /* A job's report or a channel post while the turn runs: its row waits
       * for the turn's end. */
      TEST_ASSERT_EQUAL_INT(SUCCESS, conv_post_unprompted(s_conv, s_user, s_script.post));
   }
   if (s_script.boundary) {
      /* What a declared boundary does outside a turn record
       * (session_prefix_inline_tools_rejected): the reasoning so far replays no
       * more, live and stored. */
      pthread_mutex_lock(&s->history_mutex);
      (void)llm_history_drop_turn_blocks(conversation_history);
      pthread_mutex_unlock(&s->history_mutex);
      const conv_turn_save_t save = { .floor_at_rows = true };
      TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_save_turn(s_conv, s_user, &save, NULL));
   }
   if (s_script.tool_call) {
      run_tool_exchange(conversation_history, &s_script);
   }
   if (s_script.visual) {
      /* What the render_visual tool leaves for the final answer's save. */
      pthread_mutex_lock(&s->tools_mutex);
      free(s->pending_visual);
      s->pending_visual = strdup(s_script.visual);
      pthread_mutex_unlock(&s->tools_mutex);
   }
   if (s_script.capture) {
      TEST_ASSERT_TRUE(turn_origin_capture(&s_origin));
   }
   if (s_script.partial) {
      /* What the tool loop keeps of a call a stop cut. */
      s->final_answer.stopped_text = strdup(s_script.partial);
      if (s_script.thinking) {
         s->final_answer.blocks = signed_blocks(s_script.partial);
      }
   }
   if (s_script.user_stop) {
      session_stop_turn_by_user(s);
      return NULL;
   }
   if (s_script.cancel_during) {
      atomic_store(&s->cancel_requested, true);
      return NULL;
   }
   if (!s_script.reply) {
      return NULL;
   }
   if (s_script.thinking) {
      /* The tool loop's stash of the final answer's blocks and display-only reasoning. */
      s->final_answer.blocks = signed_blocks(s_script.reply);
      free(s->final_answer.reasoning_json);
      s->final_answer.reasoning_json = strdup("{\"text\":\"Let me see.\"}");
   }
   if (s_script.cancel_after) {
      atomic_store(&s->cancel_requested, true);
   }
   if (s_script.stop_after) {
      session_stop_turn_by_user(s);
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
/* The tools registered now (a definitions array), or NULL for tools off; a
 * model that takes a tool defined in a message. */
static const char *s_tool_defs;

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
   if (s_tool_defs) {
      cp.tool_defs = strdup(s_tool_defs);
      cp.inline_tools = true;
   }
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
   for (int i = 0; i < SESSIONS_MAX; i++) {
      if (s_sessions[i]) {
         fn(s_sessions[i], ctx);
      }
   }
}
void job_manager_for_each_session(void (*fn)(session_t *session, void *ctx), void *ctx) {
   (void)fn, (void)ctx;
}

/* A post's tabs are told by the WebUI: none here. */
void webui_broadcast_conversation_messages_appended(int user_id, int64_t conv_id) {
   (void)user_id, (void)conv_id;
}

/* No turn queue, and no posts from DAWN into the conversation. */
bool turn_queue_busy(uint32_t session_id) {
   (void)session_id;
   return false;
}
void session_posts_untell_locked(session_t *session) {
   (void)session;
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
void conv_event_tool_step_fanout(int64_t conv_id,
                                 int user_id,
                                 uint32_t origin_session_id,
                                 unsigned stream_id,
                                 const char *kind,
                                 char *payload_owned) {
   (void)conv_id, (void)user_id, (void)origin_session_id, (void)stream_id, (void)kind;
   free(payload_owned);
}
char *event_payload_status(bool generating) {
   (void)generating;
   return NULL;
}
char *event_payload_tool_call(const char *tool_name,
                              const char *args_json,
                              const char *tool_call_id,
                              int iteration) {
   (void)tool_name, (void)args_json, (void)tool_call_id, (void)iteration;
   return NULL;
}
char *event_payload_tool_result(const char *tool_name,
                                const char *result_text,
                                const char *tool_call_id,
                                int iteration,
                                bool is_error) {
   (void)tool_name, (void)result_text, (void)tool_call_id, (void)iteration, (void)is_error;
   return NULL;
}

/* The rest of job_dispatch.c, whose persist hook a turn here saves through. */
bool job_manager_claim_reaped(uint32_t session_id, bool *user_cancelled_out) {
   (void)session_id;
   if (user_cancelled_out) {
      *user_cancelled_out = false;
   }
   return false;
}
bool job_manager_is_shutting_down(void) {
   return false;
}
void llm_get_default_config(session_llm_config_t *config) {
   memset(config, 0, sizeof(*config));
}
/* The LLM selection: every turn here runs (none refused), on the default. */
void llm_selection_resolve(session_llm_config_t *cfg) {
   (void)cfg;
}
void llm_selection_from_row(const char *llm_type,
                            const char *provider,
                            const char *model,
                            const char *thinking_mode,
                            const char *reasoning_effort,
                            session_llm_config_t *out) {
   (void)llm_type, (void)provider, (void)model, (void)thinking_mode, (void)reasoning_effort;
   memset(out, 0, sizeof(*out));
   out->type = LLM_UNDEFINED;
}
const char *cloud_provider_to_string(cloud_provider_t provider) {
   (void)provider;
   return "none";
}
int session_turn_selection_problem(session_t *session, char *reason, size_t reason_len) {
   (void)session;
   if (reason && reason_len > 0) {
      reason[0] = '\0';
   }
   return 0;
}
void llm_set_last_error(int code) {
   (void)code;
}
bool llm_error_is_transient(int code) {
   (void)code;
   return false;
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
   return s_tool_defs != NULL;
}
bool llm_tools_suppressed(void) {
   return true;
}
struct json_object *llm_tools_request_tools(struct json_object *history,
                                            bool is_remote,
                                            bool claude,
                                            bool inline_ok,
                                            const char **source_out) {
   (void)is_remote, (void)claude;
   /* The conversation's set, as llm_tools_filter.c gives it (unrendered). */
   struct json_object *defs = s_tool_defs ? llm_tool_defs_for_request(history, inline_ok) : NULL;
   if (source_out) {
      *source_out = defs ? "the conversation's set" : NULL;
   }
   return defs;
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

/* The final answer saved as webui_persist_final_answer (webui_broadcasts.c)
 * saves it: its reasoning and blocks with the row, a rendered visual taken
 * from the session and joined after the body (memory_history_join_visual),
 * the row's id on the history.  Its retention and fan-out are left out: a
 * reload reads neither. */
static void persist_reply(int64_t conv, const char *body) {
   char *reasoning = s->final_answer.reasoning_json;
   s->final_answer.reasoning_json = NULL;
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
                                    .reasoning = reasoning,
                                    .llm_blocks = stored,
                                    .stopped = session_turn_reply_stopped(s) };
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, s_user, &row, &id));
   free(combined);
   free(visual);
   free(stored);
   free(reasoning);
   session_stamp_last_message_id(s, "assistant", id);
}

/* The newest id of a request-context row of @p kind. */
static int64_t newest_of_kind(const char *kind) {
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(s_db.db,
                                                       "SELECT MAX(id) FROM messages WHERE "
                                                       "conversation_id = ?1 AND kind = ?2",
                                                       -1, &st, NULL));
   sqlite3_bind_int64(st, 1, s_conv);
   sqlite3_bind_text(st, 2, kind, -1, SQLITE_STATIC);
   const int64_t id = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
   sqlite3_finalize(st);
   return id;
}

/* The question's kind (an envelope for a re-engagement), and whether a turn
 * with no reply takes its question back as a re-engagement does: by the turn's
 * question id, or by the envelope's row (once a re-engagement knows it). */
static message_kind_t s_question_kind = MESSAGE_KIND_NONE;
static bool s_retract_unanswered;
static bool s_retract_envelope_row;
/* As a resumed job that produced nothing: its own envelope taken back by row
 * (session_turn_envelope_id); the result is kept for the test. */
static bool s_retract_turn_envelope;
static int s_retract_rc;
/* The turn starts before its conversation is known (a voice turn), and binds
 * it in dispatch. */
static bool s_begin_unbound;
/* A new chat's first message: its conversation is created once the turn's
 * first tool call is saved, and its question written then. */
static bool s_new_chat;

/* As the WebUI's tool persist hook with a new chat's first message: the
 * conversation, created for the turn and claimed in its name, binds; the
 * question is written (persist_pending_user_row); then the row. */
static int64_t new_chat_persist_cb(void *userdata, const session_tool_row_t *row) {
   char *pending = NULL;
   if (session_turn_conversation(s) <= 0) {
      const uint64_t claim = session_turn_claim_token(s, true);
      TEST_ASSERT_TRUE(conv_turn_lease_claim_new(s_conv, s->session_id, claim));
      session_turn_set_conversation(s, s_conv, true);
      int64_t conv = 0;
      pending = session_turn_take_pending(s, "user", &conv);
      TEST_ASSERT_NOT_NULL(pending);
      int64_t id = 0;
      TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                            conv_db_add_message_ex(conv, s_user, "user", pending, &id));
      session_stamp_last_message_id(s, "user", id);
      session_prefix_question_saved(s, conv, s_user, id);
   }
   free(pending);
   return job_dispatch_tool_persist_cb(userdata, row);
}

static void run_turn(int64_t conv, const char *question, script_t script) {
   s_script = script;
   atomic_store(&s->cancel_requested, false);
   session_turn_begin(s, s_begin_unbound || s_new_chat ? 0 : conv, s_user);
   if (s_new_chat) {
      session_turn_await_conversation(s);
   }
   /* The job worker's hook: every field of a row into the conversation. */
   job_persist_ctx_t persist_ctx = { .conv_id = conv, .user_id = s_user };
   s->tool_persist_cb = s_new_chat ? new_chat_persist_cb : job_dispatch_tool_persist_cb;
   s->tool_persist_userdata = &persist_ctx;
   atomic_store(&s->will_persist_turn, true);
   const text_input_dispatch_opts_t opts = { .conversation_id = s_new_chat ? 0 : conv,
                                             .auth_user_id = s_user,
                                             .question_kind = s_question_kind,
                                             .await_conversation = s_new_chat };
   char *reply = core_text_input_dispatch(s, question, &opts);
   atomic_store(&s->will_persist_turn, false);
   s->tool_persist_cb = NULL;
   s->tool_persist_userdata = NULL;
   if (!reply) {
      /* A stop after the answer completed: the answer saved anyway. */
      reply = s->cancelled_final_response;
      s->cancelled_final_response = NULL;
   }
   if (reply && reply[0] != '\0') {
      persist_reply(conv, reply);
   } else if (!atomic_load(&s->cancel_requested)) {
      /* No reply, not stopped: as the WebUI text worker does. */
      (void)session_prefix_take_back_changes(s);
   }
   if ((!reply || reply[0] == '\0') && s_retract_unanswered) {
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
   } else if (s_retract_envelope_row) {
      TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                            conv_db_retract_envelope(conv, s_user, newest_of_kind("envelope")));
   } else if ((!reply || reply[0] == '\0') && s_retract_turn_envelope) {
      const int64_t envelope = session_turn_envelope_id(s);
      TEST_ASSERT_EQUAL_INT64(newest_of_kind("envelope"), envelope);
      s_retract_rc = conv_db_retract_envelope(conv, s_user, envelope);
   }
   free(reply);
   session_turn_end(s);
}

/* ---- live vs reload, as each provider is sent them ---- */

static char *claude_request(struct json_object *history) {
   struct json_object *req = convert_to_claude_format(history, NULL, MODEL, CARRIER, 0,
                                                      s_tool_defs != NULL, true);
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
       OPENAI_CARRIER, OPENAI_MODEL);
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

/* A Chat Completions request's messages (llama.cpp, Ollama, OpenRouter). */
static char *chat_request(struct json_object *history) {
   struct json_object *msgs = llm_openai_prepare_chat_history(history, CHAT_CARRIER, CHAT_MODEL);
   TEST_ASSERT_NOT_NULL(msgs);
   char *out = strdup(json_object_to_json_string_ext(msgs, FLAGS));
   json_object_put(msgs);
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
   char *claude[2], *responses[2], *chat[2];
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
      chat[i] = chat_request(h);
      json_object_put(h);
   }
   TEST_ASSERT_EQUAL_STRING(claude[1], claude[0]);
   if (carries) {
      TEST_ASSERT_NOT_NULL(strstr(claude[0], carries));
   }
   TEST_ASSERT_EQUAL_STRING(responses[1], responses[0]);
   TEST_ASSERT_EQUAL_STRING(chat[1], chat[0]);
   for (int i = 0; i < 2; i++) {
      free(claude[i]);
      free(responses[i]);
      free(chat[i]);
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

/* The turn loader the WebUI registers (webui_turn_history_loader): the
 * conversation's request as a reload builds it. */
static int s_reloads; /* histories the turn loader built */

static struct json_object *reload_loader(int user_id,
                                         int64_t conv_id,
                                         session_llm_config_t *cfg_out) {
   memset(cfg_out, 0, sizeof(*cfg_out));
   cfg_out->type = LLM_UNDEFINED; /* none of its own: follows the default */
   s_reloads++;
   conversation_t c = { 0 };
   struct json_object *h = NULL;
   if (conv_db_get(conv_id, user_id, &c) == AUTH_DB_SUCCESS) {
      h = memory_history_request_context(conv_id, user_id, c.context_watermark_msg_id,
                                         c.compaction_summary, NULL, NULL);
   }
   conv_free(&c);
   return h;
}

/* A WebUI session holding the conversation, opened before it had a turn. */
static session_t *new_session(uint32_t id) {
   session_t *x = calloc(1, sizeof(*x));
   x->session_id = id;
   x->type = SESSION_TYPE_WEBUI;
   x->metrics.user_id = (uint32_t)s_user;
   pthread_mutex_init(&x->history_mutex, NULL);
   pthread_mutex_init(&x->llm_config_mutex, NULL);
   pthread_mutex_init(&x->metrics_mutex, NULL);
   pthread_mutex_init(&x->tools_mutex, NULL);
   x->conversation_history = json_object_new_array();
   struct json_object *sys = json_object_new_object();
   json_object_object_add(sys, "role", json_object_new_string("system"));
   json_object_object_add(sys, "content", json_object_new_string("start"));
   json_object_array_add(x->conversation_history, sys);
   session_bind_history_conversation(x, s_conv);
   return x;
}

static void free_session(session_t *x) {
   free(x->pending_visual);
   free(x->cancelled_final_response);
   session_final_answer_clear(x);
   session_compaction_teardown(x);
   session_prefix_turn_free(x->prefix_turn);
   json_object_put(x->withdraw_pending);
   json_object_put(x->conversation_history);
   pthread_mutex_destroy(&x->history_mutex);
   pthread_mutex_destroy(&x->llm_config_mutex);
   pthread_mutex_destroy(&x->metrics_mutex);
   pthread_mutex_destroy(&x->tools_mutex);
   free(x);
}

void setUp(void) {
   s_over = false;
   s_summary = NULL;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("alice", "h", true));
   auth_user_t u;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user("alice", &u));
   s_user = u.id;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "c", &s_conv));
   /* The loaders the WebUI registers: every turn here catches up first. */
   session_set_history_loader(reload_loader);
   session_set_history_tail_loader(memory_history_request_tail);
   conv_post_init();
   s_sessions[0] = new_session(7);
   s = s_sessions[0];
   s_persona = "P1";
   s_directive = "D1";
   s_time = "09:00";
   s_tool_defs = NULL;
}

void tearDown(void) {
   for (int i = 0; i < SESSIONS_MAX; i++) {
      if (s_sessions[i]) {
         free_session(s_sessions[i]);
         s_sessions[i] = NULL;
      }
   }
   s = NULL;
   session_set_history_loader(NULL);
   session_set_history_tail_loader(NULL);
   conv_turn_lease_set_writer(NULL, NULL);
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

/* Two calls answered in one batch, on each path: on a Claude path one message
 * holds both results, each saved as its own row. */
static void test_parallel_tool_calls(void) {
   run_turn(s_conv, "Q1",
            (script_t){ .reply = "Sunny and rainy.", .tool_call = "toolu_p", .parallel = true });
   assert_live_is_reload(s_conv, "toolu_pb");
   run_turn(
       s_conv, "Q2",
       (script_t){ .reply = "Still.", .tool_call = "call_p", .openai = true, .parallel = true });
   assert_live_is_reload(s_conv, "call_pb");
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

/* An OpenAI call that came with encrypted reasoning: the reasoning goes back
 * with the call on the Responses path, from the live history and a reload. */
static void test_an_openai_tool_exchange_with_reasoning(void) {
   run_turn(s_conv, "Q1",
            (script_t){ .reply = "It's sunny.",
                        .tool_call = "call_r",
                        .openai = true,
                        .tool_reasoning = true });
   assert_live_is_reload(s_conv, "call_r");
   struct json_object *reloaded = reload(s_conv);
   char *responses = responses_request(reloaded);
   TEST_ASSERT_NOT_NULL(strstr(responses, "\"encrypted_content\":\"ENC\""));
   free(responses);
   json_object_put(reloaded);
   run_turn(s_conv, "Q2", (script_t){ .reply = "A2" });
   assert_live_is_reload(s_conv, "A2");
}

/* The start of a PNG's bytes in base64: the image itself is in the request. */
#define PNG_BASE64_HEAD "iVBORw0KGgp"

/* A tool result carrying an image (a capture): stored, named by its row, and
 * rebuilt from the store on reload as the live turn built it, on each path. */
static void test_a_tool_result_with_an_image(void) {
   char dir[] = "/tmp/dawn_reload_img_XXXXXX";
   TEST_ASSERT_NOT_NULL(mkdtemp(dir));
   image_store_config_t cfg = { .max_size = 4 * 1024 * 1024, .max_per_user = 100, .data_dir = dir };
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_init(&cfg));
   g_config.vision.max_image_size_kb = 4096;
   run_turn(s_conv, "Q1",
            (script_t){ .reply = "A cat.", .tool_call = "toolu_img", .tool_image = true });
   assert_live_is_reload(s_conv, PNG_BASE64_HEAD);
   run_turn(s_conv, "Q2",
            (script_t){ .reply = "A dog.",
                        .tool_call = "call_img",
                        .openai = true,
                        .tool_image = true });
   assert_live_is_reload(s_conv, PNG_BASE64_HEAD);
   g_config.vision.max_image_size_kb = 0;
   image_store_shutdown();
   blob_store_shutdown();
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

/* ---- two sessions on one conversation ---- */

/* Session @p x runs the turns from here. */
static void as(session_t *x) {
   s = x;
}

/* What is in force, on the live history's frozen prompt and on a reload's. */
static void assert_same_in_force(void) {
   struct json_object *reloaded = reload(s_conv);
   struct json_object *want = llm_history_in_force(llm_history_prefix(reloaded), false);
   pthread_mutex_lock(&s->history_mutex);
   struct json_object *have = llm_history_in_force(llm_history_prefix(s->conversation_history),
                                                   false);
   char *live = have ? strdup(json_object_to_json_string_ext(have, FLAGS)) : NULL;
   pthread_mutex_unlock(&s->history_mutex);
   TEST_ASSERT_EQUAL_STRING(want ? json_object_to_json_string_ext(want, FLAGS) : "(none)",
                            live ? live : "(none)");
   free(live);
   json_object_put(reloaded);
}

/* @p x's next turn starts: caught up, it holds what a reload builds (with
 * @p carries in its Claude request).  No one holds the conversation between
 * turns. */
static void caught_up(session_t *x, const char *carries) {
   as(x);
   TEST_ASSERT_FALSE(conv_turn_lease_holder(s_conv, NULL, NULL, NULL));
   session_turn_begin(x, s_conv, s_user);
   uint32_t holder = 0;
   TEST_ASSERT_TRUE(conv_turn_lease_holder(s_conv, &holder, NULL, NULL));
   TEST_ASSERT_EQUAL_UINT32(x->session_id, holder);
   assert_live_is_reload(s_conv, carries);
   assert_same_in_force();
   session_turn_end(x);
   TEST_ASSERT_FALSE(conv_turn_lease_holder(s_conv, NULL, NULL, NULL));
}

/* A turn on @p x, whose surface has @p directive, then its history checked. */
static void turn_on(session_t *x, const char *directive, const char *q, script_t sc) {
   as(x);
   s_directive = directive;
   run_turn(s_conv, q, sc);
   assert_live_is_reload(s_conv, NULL);
   TEST_ASSERT_FALSE(conv_turn_lease_holder(s_conv, NULL, NULL, NULL));
}

static session_t *second_session(void) {
   s_sessions[1] = new_session(8);
   return s_sessions[1];
}

/* Alternating turns of every kind, each surface with its own directions:
 * every catch-up appends the other's rows (none builds the history again). */
static void test_two_sessions_alternate_turns_of_every_kind(void) {
   session_t *a = s_sessions[0];
   session_t *b = second_session();
   turn_on(a, "DA", "Q1", (script_t){ .reply = "A1" });
   caught_up(b, "A1"); /* opened before any turn: built from the conversation */
   const int reloads = s_reloads;
   turn_on(b, "DB", "Q2", (script_t){ .reply = "A2", .thinking = true, .tool_call = "toolu_b1" });
   caught_up(a, "toolu_b1");
   turn_on(
       a, "DA", "Q3",
       (script_t){ .reply = "A3", .tool_call = "call_a1", .openai = true, .tool_reasoning = true });
   caught_up(b, "call_a1");
   turn_on(b, "DB", "Q4", (script_t){ .reply = "A4", .tool_call = "toolu_p", .parallel = true });
   caught_up(a, "toolu_pb");
   turn_on(a, "DA", "Q5",
           (script_t){ .reply = "A5", .tool_call = "call_p", .openai = true, .parallel = true });
   caught_up(b, "call_pb");
   turn_on(b, "DB", "Q6", (script_t){ .reply = "A6", .thinking = true });
   caught_up(a, SIGNED);
   turn_on(a, "DA", "Q7", (script_t){ .reply = "A7", .cancel_after = true });
   caught_up(b, "A7");
   turn_on(b, "DB", "Q8", (script_t){ .cancel_during = true });
   caught_up(a, "Q8");
   turn_on(a, "DA", "Q9", (script_t){ .tool_call = "toolu_s", .cancel_during = true });
   caught_up(b, "toolu_s");
   s_question_kind = MESSAGE_KIND_ENVELOPE;
   turn_on(b, "DB", "A job finished: E1", (script_t){ .reply = "It did." });
   s_question_kind = MESSAGE_KIND_NONE;
   caught_up(a, "E1");
   turn_on(a, "DA", "Q10",
           (script_t){ .reply = "Here it is.", .tool_call = "toolu_v", .visual = VISUAL });
   caught_up(b, "Here it is.");
   /* One that learns its conversation in dispatch catches up there. */
   turn_on(b, "DB", "Q11", (script_t){ .reply = "A11" });
   s_begin_unbound = true;
   turn_on(a, "DA", "Q12", (script_t){ .reply = "A12", .thinking = true });
   s_begin_unbound = false;
   TEST_ASSERT_NOT_NULL(strstr(json_object_to_json_string(a->conversation_history), "A11"));
   caught_up(b, "A12");
   TEST_ASSERT_EQUAL_INT(reloads, s_reloads);
}

/* Whether the conversation's last assistant row is marked as a stopped reply. */
static bool last_reply_stopped(int64_t conv) {
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK,
                         sqlite3_prepare_v2(s_db.db,
                                            "SELECT EXISTS (SELECT 1 FROM message_stopped s "
                                            "WHERE s.message_id = (SELECT MAX(id) FROM messages "
                                            "WHERE conversation_id = ?1 AND role = 'assistant'))",
                                            -1, &st, NULL));
   sqlite3_bind_int64(st, 1, conv);
   const bool stopped = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) != 0;
   sqlite3_finalize(st);
   return stopped;
}

/* The user's Stop mid-stream: the reply as far as it streamed is saved and
 * kept, marked, and the next question carries the note, live and reloaded. */
static void test_a_partial_reply_the_user_stopped(void) {
   run_turn(s_conv, "Q1", (script_t){ .reply = "A1" });
   run_turn(s_conv, "Q2",
            (script_t){ .user_stop = true, .partial = "Half an ans", .capture = true });
   const turn_origin_t stopped = s_origin;
   TEST_ASSERT_TRUE(saved_reply_has(s_conv, "Half an ans"));
   TEST_ASSERT_TRUE(last_reply_stopped(s_conv));
   assert_live_is_reload(s_conv, STOPPED_REPLY_USER_NOTE);
   /* What the stopped turn prepared isn't confirmed by the next one. */
   run_turn(s_conv, "Q3", (script_t){ .reply = "A3", .capture = true });
   TEST_ASSERT_EQUAL_UINT64(stopped.turn_token, s_origin.stopped_tokens[0]);
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_STOPPED, turn_origin_check(&stopped, &s_origin));
   TEST_ASSERT_FALSE(last_reply_stopped(s_conv));
   assert_live_is_reload(s_conv, STOPPED_REPLY_USER_NOTE);
}

/* A Stop once the whole reply arrived (cutting its speech) leaves no stopped
 * turn: what it staged is confirmed by the next.  Two Stops mid-stream in a
 * row: the first turn's item is still refused after the second. */
static void test_which_stops_discard_what_a_turn_staged(void) {
   run_turn(s_conv, "Q1", (script_t){ .reply = "A1", .stop_after = true, .capture = true });
   const turn_origin_t whole = s_origin;
   TEST_ASSERT_TRUE(saved_reply_has(s_conv, "A1"));
   TEST_ASSERT_FALSE(last_reply_stopped(s_conv));
   run_turn(s_conv, "Q2", (script_t){ .user_stop = true, .partial = "Half", .capture = true });
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_OK, turn_origin_check(&whole, &s_origin));
   const turn_origin_t first = s_origin;
   run_turn(s_conv, "Q3", (script_t){ .user_stop = true, .partial = "Again", .capture = true });
   run_turn(s_conv, "Q4", (script_t){ .reply = "A4", .capture = true });
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_STOPPED, turn_origin_check(&first, &s_origin));
   assert_live_is_reload(s_conv, STOPPED_REPLY_USER_NOTE);
}

/* The same with its signed reasoning, and after a tool exchange. */
static void test_a_stopped_partial_keeps_its_reasoning_and_tools(void) {
   run_turn(s_conv, "Q1", (script_t){ .user_stop = true, .partial = "Half", .thinking = true });
   TEST_ASSERT_TRUE(saved_reply_has(s_conv, "Half"));
   assert_live_is_reload(s_conv, SIGNED);
   run_turn(s_conv, "Q2",
            (script_t){ .user_stop = true, .partial = "Sunny, so", .tool_call = "toolu_k" });
   TEST_ASSERT_TRUE(saved_reply_has(s_conv, "Sunny, so"));
   assert_live_is_reload(s_conv, "toolu_k");
   assert_live_is_reload(s_conv, STOPPED_REPLY_USER_NOTE);
}

/* A partial with nothing left of it, and a cancel that isn't the user's Stop
 * (a superseding turn): nothing saved, nothing in the history. */
static void test_a_stop_that_keeps_no_partial(void) {
   run_turn(s_conv, "Q1", (script_t){ .reply = "A1" });
   run_turn(s_conv, "Q2", (script_t){ .user_stop = true, .partial = "" });
   TEST_ASSERT_TRUE(saved_reply_has(s_conv, "A1"));
   assert_live_is_reload(s_conv, "Q2");
   run_turn(s_conv, "Q3", (script_t){ .cancel_during = true, .partial = "Not kept" });
   TEST_ASSERT_TRUE(saved_reply_has(s_conv, "A1"));
   assert_live_is_reload(s_conv, "Q3");
   struct json_object *h = s->conversation_history;
   TEST_ASSERT_NULL(strstr(json_object_to_json_string(h), "Not kept"));
   TEST_ASSERT_NULL(strstr(json_object_to_json_string(h), "_stopped"));
}

/* A new chat's first exchange stopped after its conversation was created. */
static void test_a_new_chats_stopped_reply(void) {
   session_clear_history(s);
   s_new_chat = true;
   run_turn(s_conv, "Q1",
            (script_t){ .user_stop = true, .partial = "Half", .tool_call = "toolu_c" });
   s_new_chat = false;
   TEST_ASSERT_TRUE(last_reply_stopped(s_conv));
   assert_live_is_reload(s_conv, STOPPED_REPLY_USER_NOTE);
}

/* The other session catches up to a stopped reply and notes it the same way. */
static void test_a_stopped_reply_seen_by_the_other_session(void) {
   session_t *a = s_sessions[0];
   session_t *b = second_session();
   turn_on(a, "DA", "Q1", (script_t){ .reply = "A1" });
   caught_up(b, "A1");
   turn_on(a, "DA", "Q2", (script_t){ .user_stop = true, .partial = "Half", .thinking = true });
   caught_up(b, STOPPED_REPLY_USER_NOTE);
   turn_on(b, "DB", "Q3", (script_t){ .reply = "A3" });
   caught_up(a, STOPPED_REPLY_USER_NOTE);
}

/* A tool result's image, rebuilt from the store on either side. */
static void test_two_sessions_share_a_tool_result_image(void) {
   char dir[] = "/tmp/dawn_shared_img_XXXXXX";
   TEST_ASSERT_NOT_NULL(mkdtemp(dir));
   image_store_config_t cfg = { .max_size = 4 * 1024 * 1024, .max_per_user = 100, .data_dir = dir };
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_init(&cfg));
   g_config.vision.max_image_size_kb = 4096;
   session_t *a = s_sessions[0];
   session_t *b = second_session();
   turn_on(a, "DA", "Q1",
           (script_t){ .reply = "A cat.", .tool_call = "toolu_i", .tool_image = true });
   caught_up(b, PNG_BASE64_HEAD);
   const int reloads = s_reloads;
   turn_on(
       b, "DB", "Q2",
       (script_t){ .reply = "A dog.", .tool_call = "call_i", .openai = true, .tool_image = true });
   caught_up(a, PNG_BASE64_HEAD);
   TEST_ASSERT_EQUAL_INT(reloads, s_reloads);
   g_config.vision.max_image_size_kb = 0;
   image_store_shutdown();
   blob_store_shutdown();
}

/* The number of rows DAWN posted into the conversation. */
static int posted_rows(void) {
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK,
                         sqlite3_prepare_v2(s_db.db,
                                            "SELECT COUNT(*) FROM message_unprompted p JOIN "
                                            "messages m ON m.id = p.message_id WHERE "
                                            "m.conversation_id = ?1",
                                            -1, &st, NULL));
   sqlite3_bind_int64(st, 1, s_conv);
   const int n = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
   sqlite3_finalize(st);
   return n;
}

#define TDEF(name, desc)                                                 \
   "{\"name\":\"" name "\",\"description\":\"" desc "\",\"parameters\":" \
   "{\"type\":\"object\",\"properties\":{}}}"

/* The conversation's rows of @p kind (NULL: its questions). */
static int64_t saved_rows(int64_t conv, const char *kind) {
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK,
                         sqlite3_prepare_v2(s_db.db,
                                            "SELECT COUNT(*) FROM messages WHERE conversation_id "
                                            "= ?1 AND (kind = ?2 OR (?2 IS NULL AND kind IS NULL "
                                            "AND role = 'user'))",
                                            -1, &st, NULL));
   sqlite3_bind_int64(st, 1, conv);
   if (kind) {
      sqlite3_bind_text(st, 2, kind, -1, SQLITE_STATIC);
   }
   const int64_t n = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
   sqlite3_finalize(st);
   return n;
}

/* A post made while a turn runs is written at its end, after its rows; both
 * sessions, the one whose turn it was too, take it in at their next turn. */
static void test_a_post_deferred_while_a_turn_runs(void) {
   session_t *a = s_sessions[0];
   session_t *b = second_session();
   turn_on(a, "DA", "Q1", (script_t){ .reply = "A1" });
   caught_up(b, "A1");
   const int reloads = s_reloads;
   turn_on(b, "DB", "Q2", (script_t){ .reply = "A2", .post = "Your report is ready." });
   TEST_ASSERT_EQUAL_INT(1, posted_rows()); /* written at the turn's end */
   caught_up(a, "A2");
   caught_up(b, "A2");
   /* One posted while no turn runs: written at once. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, conv_post_unprompted(s_conv, s_user, "A briefing."));
   TEST_ASSERT_EQUAL_INT(2, posted_rows());
   caught_up(a, NULL);
   turn_on(a, "DA", "Q3", (script_t){ .reply = "A3" });
   caught_up(b, "A3");
   TEST_ASSERT_EQUAL_INT(reloads, s_reloads);
}

/* A compaction the other session applied rewrites what a reload builds: the
 * history is built again, with its summary. */
static void test_a_compaction_by_the_other_session(void) {
   session_t *a = s_sessions[0];
   session_t *b = second_session();
   turn_on(a, "DA", "Q1", (script_t){ .reply = "A1" });
   caught_up(b, "A1");
   turn_on(b, "DB", "Q2", (script_t){ .reply = "A2", .thinking = true, .tool_call = "toolu_1" });
   caught_up(a, "toolu_1");
   turn_on(a, "DA", "Q3", (script_t){ .reply = "A3" });
   caught_up(b, "A3");
   turn_on(b, "DB", "Q4", (script_t){ .reply = "A4" });
   caught_up(a, "A4");
   turn_on(a, "DA", "Q5", (script_t){ .reply = "A5" });
   caught_up(b, "A5");
   s_over = true;
   s_summary = "They talked about Q1 and Q2.";
   turn_on(b, "DB", "Q6", (script_t){ .reply = "A6" });
   s_over = false;
   s_summary = NULL;
   const int reloads = s_reloads;
   caught_up(a, "They talked about Q1 and Q2.");
   TEST_ASSERT_EQUAL_INT(reloads + 1, s_reloads);
   turn_on(a, "DA", "Q7", (script_t){ .reply = "A7" });
   caught_up(b, "A7");
   TEST_ASSERT_EQUAL_INT(reloads + 1, s_reloads);
}

/* A boundary the other session declared leaves the reasoning before it
 * behind: the history is built again without it. */
static void test_a_boundary_by_the_other_session(void) {
   session_t *a = s_sessions[0];
   session_t *b = second_session();
   turn_on(a, "DA", "Q1", (script_t){ .reply = "A1", .thinking = true });
   caught_up(b, SIGNED);
   turn_on(b, "DB", "Q2", (script_t){ .reply = "A2", .boundary = true });
   const int reloads = s_reloads;
   caught_up(a, "A2");
   TEST_ASSERT_EQUAL_INT(reloads + 1, s_reloads);
   struct json_object *reloaded = reload(s_conv);
   char *claude = claude_request(reloaded);
   TEST_ASSERT_NULL(strstr(claude, SIGNED)); /* A1's reasoning stays behind */
   free(claude);
   json_object_put(reloaded);
   turn_on(a, "DA", "Q3", (script_t){ .reply = "A3", .thinking = true });
   caught_up(b, SIGNED);
}

/* The job worker's next resume: the session's history built again from the
 * conversation, as memory_history_load_for_llm hands it over. */
static void resume_from_storage(int64_t conv) {
   session_replace_history_at(s, reload(conv), conv, s_user, NULL);
   assert_live_is_reload(conv, NULL);
}

/* Resumed jobs: one that produces nothing takes its directive and the context
 * sent with it back, so failed resumes leave no copies; one whose attempt saved
 * a tool exchange keeps its directive (the restart point of that work). */
static void test_failed_resumes_leave_one_directive(void) {
   run_turn(s_conv, "The goal", (script_t){ .reply = "Halfway.", .tool_call = "toolu_1" });
   const int64_t contexts = saved_rows(s_conv, "turn_context");
   s_question_kind = MESSAGE_KIND_ENVELOPE;
   s_retract_turn_envelope = true;
   for (int attempt = 0; attempt < 2; attempt++) {
      resume_from_storage(s_conv);
      run_turn(s_conv, "Your previous attempt did not finish.", (script_t){ .reply = NULL });
      TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, s_retract_rc);
      TEST_ASSERT_EQUAL_INT64(0, saved_rows(s_conv, "envelope"));
      TEST_ASSERT_EQUAL_INT64(contexts, saved_rows(s_conv, "turn_context"));
   }
   /* A resume whose attempt did work before failing keeps its directive. */
   resume_from_storage(s_conv);
   run_turn(s_conv, "Your previous attempt did not finish.",
            (script_t){ .reply = NULL, .tool_call = "toolu_2" });
   TEST_ASSERT_EQUAL_INT(AUTH_DB_DUPLICATE, s_retract_rc);
   TEST_ASSERT_EQUAL_INT64(1, saved_rows(s_conv, "envelope"));
   /* The resume after it adds one more: a real restart point. */
   s_retract_turn_envelope = false;
   resume_from_storage(s_conv);
   run_turn(s_conv, "Your previous attempt did not finish.", (script_t){ .reply = "Done." });
   TEST_ASSERT_EQUAL_INT64(2, saved_rows(s_conv, "envelope"));
   TEST_ASSERT_TRUE(saved_rows(s_conv, "turn_context") > contexts); /* each resume has one */
   s_question_kind = MESSAGE_KIND_NONE;
   resume_from_storage(s_conv);
}

/* A re-engagement with no reply, its take-back tried as the code does it, and
 * one whose envelope's row is taken back inside its own turn (as a
 * re-engagement does once it knows the row): the other session never holds
 * that envelope, and appends what the turn left. */
static void test_a_retraction_by_the_other_session(void) {
   session_t *a = s_sessions[0];
   session_t *b = second_session();
   turn_on(a, "DA", "Q1", (script_t){ .reply = "A1" });
   caught_up(b, "A1");
   const int reloads = s_reloads;
   s_question_kind = MESSAGE_KIND_ENVELOPE;
   s_retract_unanswered = true;
   turn_on(b, "DB", "A job finished: E1", (script_t){ .reply = NULL });
   s_retract_unanswered = false;
   caught_up(a, "E1");
   as(b);
   s_retract_envelope_row = true;
   run_turn(s_conv, "A job finished: E2", (script_t){ .reply = NULL });
   s_retract_envelope_row = false;
   s_question_kind = MESSAGE_KIND_NONE;
   caught_up(a, NULL);
   TEST_ASSERT_NULL(strstr(json_object_to_json_string(a->conversation_history), "E2"));
   TEST_ASSERT_EQUAL_INT(reloads, s_reloads);
}

/* A new chat's first message, its conversation created after its first
 * Claude tool call: the question itself gets its row's id (not the tool
 * results that came after it), and the history is what a reload builds. */
static void test_a_new_chats_first_question_saved_after_a_tool_call(void) {
   session_clear_history(s);
   s_new_chat = true;
   run_turn(s_conv, "Q1", (script_t){ .reply = "A1", .tool_call = "toolu_n" });
   s_new_chat = false;
   pthread_mutex_lock(&s->history_mutex);
   struct json_object *question = NULL;
   for (size_t i = 0; i < json_object_array_length(s->conversation_history); i++) {
      struct json_object *m = json_object_array_get_idx(s->conversation_history, i);
      if (llm_history_is_question(m)) {
         question = m;
      }
   }
   const bool stamped = question && json_object_object_get_ex(question, "id", NULL);
   pthread_mutex_unlock(&s->history_mutex);
   TEST_ASSERT_TRUE(stamped);
   assert_live_is_reload(s_conv, "toolu_n");
}

/* A row as another surface's turn stored it. */
static int64_t add_stored(const char *role, const char *content, const char *kind, int64_t of) {
   const conv_message_row_t row = { .role = role,
                                    .content = content,
                                    .kind = kind,
                                    .context_of = of };
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(s_conv, s_user, &row, &id));
   return id;
}

/* A withdrawal (a forgotten memory) made from the other session rewrites
 * stored context and leaves the reasoning behind: the history is built
 * again, without what was forgotten. */
static void test_a_withdrawal_by_the_other_session(void) {
   session_t *a = s_sessions[0];
   session_t *b = second_session();
   const int64_t q = add_stored("user", "Q0", NULL, 0);
   add_stored("user", "--- USER MEMORY (t) ---\n- food: tacos\n--- END USER MEMORY (t) ---\n",
              "memory", q);
   add_stored("assistant", "A0", NULL, 0);
   caught_up(a, "tacos");
   caught_up(b, "tacos");
   turn_on(a, "DA", "Q1", (script_t){ .reply = "A1", .thinking = true });
   caught_up(b, SIGNED);
   const int reloads = s_reloads;
   conv_withdrawn_t w;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_withdraw(s_user, true, &w));
   TEST_ASSERT_EQUAL_INT(1, w.n_convs);
   conv_withdrawn_free(&w);
   caught_up(a, "A1");
   TEST_ASSERT_EQUAL_INT(reloads + 1, s_reloads);
   TEST_ASSERT_NULL(strstr(json_object_to_json_string(a->conversation_history), "tacos"));
   caught_up(b, "A1");
   TEST_ASSERT_EQUAL_INT(reloads + 2, s_reloads);
}

/* Rows the other surface saved with a tool change among them (its tool set
 * changed for its turn): appended as a reload has them, no rebuild. */
static void test_a_tail_carrying_a_tool_change(void) {
   session_t *a = s_sessions[0];
   session_t *b = second_session();
   turn_on(a, "DA", "Q1", (script_t){ .reply = "A1" });
   caught_up(b, "A1");
   const int reloads = s_reloads;
   const int64_t q = add_stored("user", "Q2", NULL, 0);
   add_stored("system",
              "{\"rendered\":\"inline\",\"tools\":[{\"name\":\"weather\",\"description\":"
              "\"The weather.\",\"input_schema\":{\"type\":\"object\"}}]}",
              "tool_change", q);
   add_stored("assistant", "A2", NULL, 0);
   caught_up(a, "A2");
   caught_up(b, "A2");
   TEST_ASSERT_EQUAL_INT(reloads, s_reloads);
}

/* The rows of a conversation that was empty when the history was built are
 * all its tail. */
static void test_a_tail_from_an_empty_conversation(void) {
   bool rebuild = true;
   struct json_object *tail = memory_history_request_tail(s_conv, s_user, 0, &rebuild);
   TEST_ASSERT_NOT_NULL(tail);
   TEST_ASSERT_FALSE(rebuild);
   TEST_ASSERT_EQUAL_INT(0, (int)json_object_array_length(tail));
   json_object_put(tail);
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_message_ex(s_conv, s_user, "user", "Q", &id));
   tail = memory_history_request_tail(s_conv, s_user, 0, &rebuild);
   TEST_ASSERT_NOT_NULL(tail);
   TEST_ASSERT_EQUAL_INT(1, (int)json_object_array_length(tail));
   json_object_put(tail);
}

static int changes_in(struct json_object *hist) {
   int n = 0;
   for (size_t i = 0; i < json_object_array_length(hist); i++) {
      n += llm_history_kind_of(json_object_array_get_idx(hist, i)) == MESSAGE_KIND_TOOL_CHANGE;
   }
   return n;
}

/* The changes @p hist's record counted: the conversation's, and DAWN's own
 * tools' this hour. */
static void assert_counted(struct json_object *hist, int want) {
   struct json_object *rec = llm_history_in_force(llm_history_prefix(hist), false);
   struct json_object *v = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(rec, "tool_changes", &v));
   TEST_ASSERT_EQUAL_INT(want, json_object_get_int(v));
   struct json_object *servers = NULL;
   struct json_object *e = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(rec, "tool_change_servers", &servers));
   TEST_ASSERT_TRUE(json_object_object_get_ex(servers, "", &e));
   TEST_ASSERT_TRUE(json_object_object_get_ex(e, "n", &v));
   TEST_ASSERT_EQUAL_INT(want, json_object_get_int(v));
}

/* A tool changed after the conversation froze, and the turn that appended the
 * change in place failed (no reply): its question stays, the change goes,
 * stored and live, with what it counted.  The next question gets the change
 * in place after it, so the request's tools stay the frozen set and the
 * earlier signed reasoning still replays. */
static void test_a_failed_turn_takes_its_tool_change_back(void) {
   s_tool_defs = "[" TDEF("a", "A") "]";
   run_turn(s_conv, "Q1", (script_t){ .reply = "A1", .thinking = true });
   char *frozen = strdup(
       json_object_to_json_string_ext(llm_history_frozen_tools(s->conversation_history), FLAGS));

   s_tool_defs = "[" TDEF("a", "A") "," TDEF("b", "B") "]";
   s_time = "09:05";
   run_turn(s_conv, "Q2", (script_t){ .reply = NULL });
   TEST_ASSERT_EQUAL_INT(0, changes_in(s->conversation_history));
   TEST_ASSERT_EQUAL_INT64(0, saved_rows(s_conv, "tool_change"));
   TEST_ASSERT_NOT_NULL(strstr(json_object_to_json_string(s->conversation_history), "\"Q2\""));
   TEST_ASSERT_EQUAL_INT64(2, saved_rows(s_conv, NULL));
   assert_live_is_reload(s_conv, SIGNED);

   s_time = "09:10";
   run_turn(s_conv, "Q3", (script_t){ .reply = "A3", .thinking = true });
   TEST_ASSERT_EQUAL_INT(1, changes_in(s->conversation_history));
   TEST_ASSERT_EQUAL_INT64(1, saved_rows(s_conv, "tool_change"));
   assert_counted(s->conversation_history, 1);
   struct json_object *reloaded = reload(s_conv);
   assert_counted(reloaded, 1);
   json_object_put(reloaded);
   struct json_object *defs = llm_tool_defs_for_request(s->conversation_history, true);
   TEST_ASSERT_EQUAL_STRING(frozen, json_object_to_json_string_ext(defs, FLAGS));
   json_object_put(defs);
   free(frozen);
   char *req = claude_request(s->conversation_history);
   const char *q3 = strstr(req, "Q3");
   TEST_ASSERT_NOT_NULL(q3);
   TEST_ASSERT_NOT_NULL(strstr(q3, "tool_addition"));
   TEST_ASSERT_NOT_NULL(strstr(req, SIGNED));
   free(req);
   assert_live_is_reload(s_conv, SIGNED);
}

/* The window's room (llm_context.c, not linked): the ceiling as given. */
int llm_context_fit_reply(int max_tokens,
                          struct json_object *history,
                          const char *input_text,
                          struct json_object *tools,
                          llm_type_t type,
                          cloud_provider_t provider,
                          const char *model) {
   (void)history;
   (void)input_text;
   (void)tools;
   (void)type;
   (void)provider;
   (void)model;
   return max_tokens;
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_plain_turns);
   RUN_TEST(test_a_reply_with_its_reasoning);
   RUN_TEST(test_a_tool_exchange);
   RUN_TEST(test_parallel_tool_calls);
   RUN_TEST(test_context_and_changed_directions);
   RUN_TEST(test_a_reply_stopped_after_it_completed);
   RUN_TEST(test_a_reasoned_reply_stopped_after_it_completed);
   RUN_TEST(test_a_reply_stopped_mid_stream);
   RUN_TEST(test_a_re_engagement);
   RUN_TEST(test_failed_resumes_leave_one_directive);
   RUN_TEST(test_an_openai_tool_exchange);
   RUN_TEST(test_an_openai_tool_exchange_with_reasoning);
   RUN_TEST(test_a_tool_result_with_an_image);
   RUN_TEST(test_a_reply_with_a_rendered_visual);
   RUN_TEST(test_a_compaction_at_the_seam);
   RUN_TEST(test_two_sessions_alternate_turns_of_every_kind);
   RUN_TEST(test_two_sessions_share_a_tool_result_image);
   RUN_TEST(test_a_post_deferred_while_a_turn_runs);
   RUN_TEST(test_a_compaction_by_the_other_session);
   RUN_TEST(test_a_boundary_by_the_other_session);
   RUN_TEST(test_a_retraction_by_the_other_session);
   RUN_TEST(test_a_new_chats_first_question_saved_after_a_tool_call);
   RUN_TEST(test_a_withdrawal_by_the_other_session);
   RUN_TEST(test_a_tail_carrying_a_tool_change);
   RUN_TEST(test_a_tail_from_an_empty_conversation);
   RUN_TEST(test_a_failed_turn_takes_its_tool_change_back);
   RUN_TEST(test_a_partial_reply_the_user_stopped);
   RUN_TEST(test_which_stops_discard_what_a_turn_staged);
   RUN_TEST(test_a_stopped_partial_keeps_its_reasoning_and_tools);
   RUN_TEST(test_a_stop_that_keeps_no_partial);
   RUN_TEST(test_a_new_chats_stopped_reply);
   RUN_TEST(test_a_stopped_reply_seen_by_the_other_session);
   return UNITY_END();
}
