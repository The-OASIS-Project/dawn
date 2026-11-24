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
 * The tool loop's last call: an answer (tools off) by default, or a save.  Links
 * the real llm_tool_loop.c and tool_call_policy.c against a scripted provider and
 * a scripted batch runner that puts each call to the real policy check, so what a
 * save step refuses is what the policy refuses.  The daemon's own batch runner
 * (llm_tools.c) is not linked.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/llm_response_finalize.h"
#include "core/session_history.h"
#include "core/session_manager.h"
#include "core/tool_call_challenge.h"
#include "core/tool_call_policy.h"
#include "core/tool_result_store.h"
#include "llm/llm_cache_monitor.h"
#include "llm/llm_claude.h"
#include "llm/llm_claude_route.h"
#include "llm/llm_compaction.h"
#include "llm/llm_context.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_interface.h"
#include "llm/llm_key_tag.h"
#include "llm/llm_openai.h"
#include "llm/llm_rate_limit.h"
#include "llm/llm_tool_history.h"
#include "llm/llm_tool_images.h"
#include "llm/llm_tool_loop.h"
#include "llm/llm_tool_views.h"
#include "llm/llm_tool_views_apply.h"
#include "llm/llm_tools.h"
#include "llm/llm_turn_blocks.h"
#include "unity.h"

/* More calls than any turn may make, so an extra one shows. */
#define TEST_MAX_CALLS (LLM_TOOLS_MAX_ITERATIONS + 4)
#define TEST_NOTE "Last step: save what you found."
/* Input tokens each provider call adds to the session's total. */
#define TEST_TOKENS_PER_CALL 100

/* The calls a scripted reply can make. */
#define TOOL_LOOKUP "lookup" /* fetch kind */
#define TOOL_SAVE "save"     /* state kind */
#define TOOL_FINISH "finish" /* state kind, ends the turn (skip_followup) */

/* What the provider replies with. */
typedef enum {
   SCRIPT_LOOKUP_AND_SAVE, /* every reply: a lookup and a save */
   SCRIPT_REPEAT_LAST,     /* as above, but the last reply repeats the one before it */
   SCRIPT_TEXT_AT,         /* text (no tools) at call s_at */
   SCRIPT_FINISH_AT,       /* a save and a finish at call s_at */
} script_t;

static session_t s_session;
static script_t s_script;
static int s_at;
static int s_calls;
static int s_sent_iteration[TEST_MAX_CALLS];
static bool s_note_last[TEST_MAX_CALLS];    /* the history's last entry was the note */
static bool s_flag_in_call[TEST_MAX_CALLS]; /* the save-step flag during the call */
static int s_batches;
static int s_lookups_run[TEST_MAX_CALLS];
static int s_lookups_refused[TEST_MAX_CALLS];
static int s_saves_run[TEST_MAX_CALLS];
static bool s_flag_in_batch[TEST_MAX_CALLS];
static char s_prev_args[LLM_TOOLS_ARGS_LEN];  /* the provider's previous reply's first call */
static char s_batch_args[LLM_TOOLS_ARGS_LEN]; /* the last batch's first call */
static int s_last_error;
static bool s_quiet;
static int s_cache_iteration;

/* ── the scripted provider ───────────────────────────────────────────────── */

static void add_call(llm_tool_response_t *r, const char *name, const char *args) {
   tool_call_t *c = &r->tool_calls.calls[r->tool_calls.count];
   snprintf(c->id, sizeof(c->id), "call_%d_%d", s_calls, r->tool_calls.count);
   snprintf(c->name, sizeof(c->name), "%s", name);
   snprintf(c->arguments, sizeof(c->arguments), "%s", args);
   r->tool_calls.count++;
   r->has_tool_calls = true;
}

static bool history_ends_with_note(struct json_object *history) {
   const int n = (int)json_object_array_length(history);
   if (n == 0) {
      return false;
   }
   struct json_object *content = NULL;
   return json_object_object_get_ex(json_object_array_get_idx(history, n - 1), "content",
                                    &content) &&
          json_object_is_type(content, json_type_string) &&
          strcmp(json_object_get_string(content), TEST_NOTE) == 0;
}

static int scripted_provider(struct json_object *history,
                             const char *input_text,
                             const char *base_url,
                             const char *api_key,
                             const char *model,
                             void *chunk_callback,
                             void *callback_userdata,
                             int iteration,
                             llm_tool_response_t *result) {
   (void)input_text;
   (void)base_url;
   (void)api_key;
   (void)model;
   (void)chunk_callback;
   (void)callback_userdata;
   const int call = s_calls++;
   TEST_ASSERT_TRUE_MESSAGE(call < TEST_MAX_CALLS, "more provider calls than any turn makes");
   s_sent_iteration[call] = iteration;
   s_note_last[call] = history_ends_with_note(history);
   s_flag_in_call[call] = atomic_load(&s_session.turn_save_step);

   /* Tools off: a server that honors it answers in words. */
   if (iteration >= LLM_TOOLS_MAX_ITERATIONS) {
      result->text = strdup("the answer");
      return 0;
   }
   const int step = call + 1;
   if (s_script == SCRIPT_TEXT_AT && step == s_at) {
      result->text = strdup("Done");
      return 0;
   }
   char args[64];
   if (s_script == SCRIPT_REPEAT_LAST && step == LLM_TOOLS_MAX_ITERATIONS + 1) {
      add_call(result, TOOL_LOOKUP, s_prev_args);
   } else {
      snprintf(args, sizeof(args), "{\"q\":\"query %d\"}", step);
      add_call(result, TOOL_LOOKUP, args);
   }
   add_call(result, TOOL_SAVE, "{\"claim\":\"a finding\"}");
   if (s_script == SCRIPT_FINISH_AT && step == s_at) {
      result->tool_calls.count = 0;
      add_call(result, TOOL_SAVE, "{\"claim\":\"a finding\"}");
      add_call(result, TOOL_FINISH, "{}");
   }
   snprintf(s_prev_args, sizeof(s_prev_args), "%s", result->tool_calls.calls[0].arguments);
   return 0;
}

/* ── the batch: each call decided by the real policy ─────────────────────── */

static tool_action_kind_t kind_of(const char *name) {
   return strcmp(name, TOOL_LOOKUP) == 0 ? TOOL_KIND_FETCH : TOOL_KIND_STATE;
}

int llm_tools_execute_all(const tool_call_list_t *calls,
                          tool_result_list_t *results,
                          llm_tools_batch_finish_fn finish,
                          void *userdata) {
   const int b = s_batches++;
   s_flag_in_batch[b] = atomic_load(&s_session.turn_save_step);
   results->count = calls->count;
   for (int i = 0; i < calls->count; i++) {
      const tool_call_t *c = &calls->calls[i];
      tool_result_t *r = &results->results[i];
      snprintf(r->tool_call_id, sizeof(r->tool_call_id), "%s", c->id);
      const tool_action_kind_t kind = kind_of(c->name);
      const tool_metadata_t meta = { .name = c->name };
      tool_call_verdict_t verdict;
      const bool allowed = tool_call_policy_check(&meta, NULL, NULL, NULL,
                                                  tool_call_policy_caller(), false,
                                                  &verdict) == TOOL_CALL_ALLOW;
      r->success = allowed;
      r->is_error = !allowed;
      r->should_respond = true;
      snprintf(r->result, sizeof(r->result), "%s", allowed ? "ok" : "refused");
      if (allowed && strcmp(c->name, TOOL_FINISH) == 0) {
         r->skip_followup = true;
      }
      if (kind == TOOL_KIND_FETCH && allowed) {
         s_lookups_run[b]++;
      } else if (kind == TOOL_KIND_FETCH) {
         s_lookups_refused[b]++;
      } else if (allowed) {
         s_saves_run[b]++;
      }
   }
   if (calls->count > 0) {
      snprintf(s_batch_args, sizeof(s_batch_args), "%s", calls->calls[0].arguments);
   }
   if (finish) {
      finish(calls, results, userdata);
   }
   return 0;
}

bool llm_tools_is_duplicate_call(struct json_object *history,
                                 const char *tool_name,
                                 const char *tool_args,
                                 llm_history_format_t format) {
   (void)history;
   (void)tool_name;
   (void)format;
   /* A repeat of the call the last batch ran (the scripts' arguments are
    * otherwise all different). */
   return s_batches > 0 && strcmp(tool_args, s_batch_args) == 0;
}

void llm_tools_prepare_followup(const tool_result_list_t *results, tool_followup_context_t *ctx) {
   memset(ctx, 0, sizeof(*ctx));
   for (int i = 0; i < results->count; i++) {
      ctx->skip_followup |= results->results[i].skip_followup;
   }
   if (ctx->skip_followup) {
      ctx->direct_response = strdup("concluded");
   }
}

void llm_tool_response_free(llm_tool_response_t *response) {
   free(response->text);
   response->text = NULL;
   free(response->thinking_content);
   response->thinking_content = NULL;
   json_object_put(response->blocks);
   response->blocks = NULL;
   free(response->response_id);
   response->response_id = NULL;
}

/* ── history ─────────────────────────────────────────────────────────────── */

void session_history_append(struct json_object *history, struct json_object *msg) {
   json_object_array_add(history, msg);
}

void llm_history_set_kind(struct json_object *obj, message_kind_t kind) {
   (void)obj;
   (void)kind;
}

static void append_turn(struct json_object *history, const llm_tool_response_t *response) {
   struct json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string("assistant"));
   json_object_object_add(m, "tool_calls",
                          json_object_new_int(response ? response->tool_calls.count : 0));
   json_object_array_add(history, m);
   struct json_object *t = json_object_new_object();
   json_object_object_add(t, "role", json_object_new_string("tool"));
   json_object_array_add(history, t);
}

void llm_tool_history_append_openai(struct json_object *history,
                                    const llm_tool_response_t *response,
                                    const tool_result_list_t *results) {
   (void)results;
   append_turn(history, response);
}

void llm_tool_history_append_claude(struct json_object *history,
                                    const llm_tool_response_t *response,
                                    const tool_result_list_t *results,
                                    const char *carrier) {
   (void)results;
   (void)carrier;
   append_turn(history, response);
}

void llm_tool_history_persist(struct session *s,
                              struct json_object *history,
                              int before_len,
                              const char *reasoning_json,
                              int iteration,
                              const tool_result_list_t *results) {
   (void)s;
   (void)history;
   (void)before_len;
   (void)reasoning_json;
   (void)iteration;
   (void)results;
}

struct json_object *llm_tool_history_blocks_as_run(const llm_tool_response_t *response,
                                                   bool calls_ran) {
   (void)response;
   (void)calls_ran;
   return NULL;
}

/* ── the session, as the loop and the policy see it ──────────────────────── */

session_t *session_get(uint32_t session_id) {
   (void)session_id;
   return &s_session;
}

session_t *session_get_for_reconnect(uint32_t session_id) {
   (void)session_id;
   return &s_session;
}

void session_release(session_t *session) {
   (void)session;
}

void session_metrics_totals(session_t *session, uint64_t *tokens_in_out, uint32_t *queries_out) {
   (void)session;
   if (tokens_in_out) {
      *tokens_in_out = (uint64_t)s_calls * TEST_TOKENS_PER_CALL;
   }
   if (queries_out) {
      *queries_out = (uint32_t)s_calls;
   }
}

session_t *session_get_command_context(void) {
   return &s_session;
}

bool session_turn_user_originated(session_t *session) {
   (void)session;
   return false;
}

bool session_turn_is_background(session_t *session) {
   (void)session;
   return false;
}

bool session_turn_carries_third_party(session_t *session) {
   (void)session;
   return false;
}

uint64_t session_turn_token(void) {
   return 1;
}

bool session_call_code_redeemed(void) {
   return false;
}

void session_set_call_code_redeemed(bool redeemed) {
   (void)redeemed;
}

bool tool_call_challenge_live_in_turn(int64_t channel_id, uint64_t turn_token) {
   (void)channel_id;
   (void)turn_token;
   return false;
}

/* The registry's action helpers, for the policy's check: a tool's kind is
 * its name's (kind_of). */
bool tool_action_canonical(const tool_metadata_t *meta,
                           const char *action,
                           char *out,
                           size_t out_len) {
   (void)meta;
   snprintf(out, out_len, "%s", action ? action : "");
   return true;
}

void tool_action_list(const tool_metadata_t *meta, char *out, size_t out_len) {
   (void)meta;
   if (out_len > 0) {
      out[0] = '\0';
   }
}

const char *tool_effective_action(const tool_metadata_t *meta, const char *action) {
   (void)meta;
   return action ? action : "";
}

tool_action_kind_t tool_action_kind(const tool_metadata_t *meta,
                                    const char *device,
                                    const char *action,
                                    const char *value) {
   (void)device;
   (void)action;
   (void)value;
   return kind_of(meta->name);
}

void tool_result_store_set_read_budget(struct session *session, size_t chars) {
   (void)session;
   (void)chars;
}

char *llm_response_clean(session_t *session, const char *raw) {
   (void)session;
   return strdup(raw);
}

/* ── the provider layer and the request budget, held still ───────────────── */

int llm_last_error(void) {
   return s_last_error;
}

void llm_set_last_error(int code) {
   s_last_error = code;
}

bool llm_error_is_transient(int code) {
   return code == LLM_ERR_TRANSIENT_NETWORK || code == LLM_ERR_UNREACHABLE;
}

bool llm_provider_errors_quiet(void) {
   return s_quiet;
}

void llm_set_provider_errors_quiet(bool quiet) {
   s_quiet = quiet;
}

int llm_get_current_resolved_config(llm_resolved_config_t *config_out) {
   (void)config_out;
   return FAILURE;
}

bool llm_uses_anthropic_messages(llm_type_t type,
                                 cloud_provider_t provider,
                                 const char *model,
                                 const char *base_url) {
   (void)type;
   (void)provider;
   (void)model;
   (void)base_url;
   return false;
}

int llm_claude_streaming_single_shot(struct json_object *conversation_history,
                                     const char *input_text,
                                     const char *base_url,
                                     const char *api_key,
                                     const char *model,
                                     llm_claude_text_chunk_callback chunk_callback,
                                     void *callback_userdata,
                                     int iteration,
                                     struct llm_tool_response *result) {
   return scripted_provider(conversation_history, input_text, base_url, api_key, model,
                            (void *)chunk_callback, callback_userdata, iteration, result);
}

int llm_openai_streaming_single_shot(struct json_object *conversation_history,
                                     const char *input_text,
                                     const char *base_url,
                                     const char *api_key,
                                     const char *model,
                                     llm_openai_text_chunk_callback chunk_callback,
                                     void *callback_userdata,
                                     int iteration,
                                     struct llm_tool_response *result) {
   return scripted_provider(conversation_history, input_text, base_url, api_key, model,
                            (void *)chunk_callback, callback_userdata, iteration, result);
}

void llm_request_carrier(const char *base_url, const char *api_key, char *out, size_t out_len) {
   (void)base_url;
   (void)api_key;
   if (out_len > 0) {
      out[0] = '\0';
   }
}

bool llm_interrupt_ctx_triggered(const llm_interrupt_ctx_t *ctx) {
   return ctx && ctx->session_flag && atomic_load(ctx->session_flag);
}

int llm_rate_limit_wait_ctx(const llm_interrupt_ctx_t *ctx) {
   (void)ctx;
   return 0;
}

int llm_sleep_with_interrupt_check_ctx(int total_ms, const llm_interrupt_ctx_t *ctx) {
   (void)total_ms;
   (void)ctx;
   return 0;
}

void llm_cache_monitor_set_iteration(int iteration) {
   s_cache_iteration = iteration;
}

int llm_cache_monitor_get_iteration(void) {
   return s_cache_iteration;
}

int llm_compaction_request_estimate(struct json_object *history, const char *input_text) {
   (void)history;
   (void)input_text;
   return 0;
}

int llm_context_estimate_tokens(struct json_object *history) {
   (void)history;
   return 0;
}

int llm_context_get_size(llm_type_t type, cloud_provider_t provider, const char *model) {
   (void)type;
   (void)provider;
   (void)model;
   return 0; /* window unknown: the room checks stand aside */
}

float llm_context_hard_threshold(void) {
   return 1.0f;
}

void llm_context_note_request(uint32_t session_id,
                              int estimate,
                              llm_type_t type,
                              cloud_provider_t provider,
                              const char *model) {
   (void)session_id;
   (void)estimate;
   (void)type;
   (void)provider;
   (void)model;
}

int llm_context_request_tokens(uint32_t session_id,
                               int estimate,
                               llm_type_t type,
                               cloud_provider_t provider,
                               const char *model) {
   (void)session_id;
   (void)type;
   (void)provider;
   (void)model;
   return estimate;
}

bool llm_tool_images_request_limit(llm_type_t type,
                                   cloud_provider_t provider,
                                   const char *model,
                                   llm_image_limit_t *out) {
   (void)type;
   (void)provider;
   (void)model;
   memset(out, 0, sizeof(*out));
   return false;
}

int llm_tool_images_cap_batch(struct json_object *history,
                              tool_result_list_t *results,
                              const llm_image_limit_t *limit) {
   (void)history;
   (void)results;
   (void)limit;
   return 0;
}

void llm_tool_views_budget_batch(const llm_tool_loop_params_t *params,
                                 const tool_call_list_t *calls,
                                 const char *assistant_text,
                                 llm_tool_views_budget_t *out) {
   (void)params;
   (void)calls;
   (void)assistant_text;
   memset(out, 0, sizeof(*out));
}

void llm_tool_views_finish_batch(const tool_call_list_t *calls,
                                 tool_result_list_t *results,
                                 void *batch) {
   (void)calls;
   (void)results;
   (void)batch;
}

size_t llm_tool_views_read_chars(size_t batch_chars, int calls) {
   (void)batch_chars;
   (void)calls;
   return 0;
}

int llm_turn_blocks_clean_text(struct json_object *blocks,
                               llm_turn_text_clean_fn clean,
                               void *ctx) {
   (void)blocks;
   (void)clean;
   (void)ctx;
   return SUCCESS;
}

/* ── the tests ───────────────────────────────────────────────────────────── */

static struct json_object *s_history;

void setUp(void) {
   memset(&s_session, 0, sizeof(s_session));
   s_session.type = SESSION_TYPE_JOB;
   s_script = SCRIPT_LOOKUP_AND_SAVE;
   s_at = 0;
   s_calls = 0;
   s_batches = 0;
   memset(s_sent_iteration, 0, sizeof(s_sent_iteration));
   memset(s_note_last, 0, sizeof(s_note_last));
   memset(s_flag_in_call, 0, sizeof(s_flag_in_call));
   memset(s_lookups_run, 0, sizeof(s_lookups_run));
   memset(s_lookups_refused, 0, sizeof(s_lookups_refused));
   memset(s_saves_run, 0, sizeof(s_saves_run));
   memset(s_flag_in_batch, 0, sizeof(s_flag_in_batch));
   s_prev_args[0] = '\0';
   s_batch_args[0] = '\0';
   s_history = json_object_new_array();
}

void tearDown(void) {
   json_object_put(s_history);
   s_history = NULL;
}

/* A background turn, as research rounds run. */
static char *run_loop(const char *save_note, int64_t ceiling) {
   llm_tool_loop_params_t p = {
      .conversation_history = s_history,
      .input_text = "the question",
      .model = "test-model",
      .provider_fn = scripted_provider,
      .history_format = LLM_HISTORY_OPENAI,
      .session_id = 1,
      .has_session = true,
      .llm_type = LLM_LOCAL,
      .cancel_flag = &s_session.cancel_requested,
      .is_background = true,
      .cumulative_input_token_ceiling = ceiling,
      .pinned = true,
      .save_note = save_note,
   };
   return llm_tool_iteration_loop(&p);
}

static int history_notes(void) {
   int notes = 0;
   for (size_t i = 0; i < json_object_array_length(s_history); i++) {
      struct json_object *content = NULL;
      if (json_object_object_get_ex(json_object_array_get_idx(s_history, i), "content", &content) &&
          strcmp(json_object_get_string(content), TEST_NOTE) == 0) {
         notes++;
      }
   }
   return notes;
}

/* Without a save note the turn runs as it always has: eight steps with tools,
 * then one answer-only call, whose words are the reply. */
static void test_default_turn_ends_on_an_answer(void) {
   char *reply = run_loop(NULL, 0);
   TEST_ASSERT_EQUAL_STRING("the answer", reply);
   free(reply);
   TEST_ASSERT_EQUAL_INT(LLM_TOOLS_MAX_ITERATIONS + 1, s_calls);
   for (int i = 0; i < LLM_TOOLS_MAX_ITERATIONS; i++) {
      TEST_ASSERT_EQUAL_INT(i, s_sent_iteration[i]);
      TEST_ASSERT_EQUAL_INT(1, s_lookups_run[i]);
      TEST_ASSERT_FALSE(s_flag_in_batch[i]);
   }
   TEST_ASSERT_EQUAL_INT(LLM_TOOLS_MAX_ITERATIONS, s_sent_iteration[LLM_TOOLS_MAX_ITERATIONS]);
   TEST_ASSERT_EQUAL_INT(LLM_TOOLS_MAX_ITERATIONS, s_batches);
   TEST_ASSERT_EQUAL_INT(0, history_notes());
}

/* A save turn: nine calls, none answer-only, so every request has the same
 * tools; the note is the last thing before the ninth and nowhere before it;
 * lookups run in eight batches and the ninth runs only saves; the turn ends
 * with an empty reply and no tenth call. */
static void test_save_turn_ends_after_its_save_step(void) {
   char *reply = run_loop(TEST_NOTE, 0);
   TEST_ASSERT_NOT_NULL(reply);
   TEST_ASSERT_EQUAL_STRING("", reply);
   free(reply);
   TEST_ASSERT_EQUAL_INT(LLM_TOOLS_MAX_ITERATIONS + 1, s_calls);
   TEST_ASSERT_EQUAL_INT(LLM_TOOLS_MAX_ITERATIONS + 1, s_batches);
   for (int i = 0; i <= LLM_TOOLS_MAX_ITERATIONS; i++) {
      TEST_ASSERT_LESS_THAN_INT(LLM_TOOLS_MAX_ITERATIONS, s_sent_iteration[i]);
      TEST_ASSERT_EQUAL(i == LLM_TOOLS_MAX_ITERATIONS, s_note_last[i]);
      TEST_ASSERT_FALSE(s_flag_in_call[i]);
      TEST_ASSERT_EQUAL(i == LLM_TOOLS_MAX_ITERATIONS, s_flag_in_batch[i]);
      TEST_ASSERT_EQUAL_INT(1, s_saves_run[i]);
   }
   int lookup_batches = 0;
   for (int i = 0; i < s_batches; i++) {
      lookup_batches += s_lookups_run[i] > 0;
   }
   TEST_ASSERT_EQUAL_INT(LLM_TOOLS_MAX_ITERATIONS, lookup_batches);
   TEST_ASSERT_EQUAL_INT(1, s_lookups_refused[LLM_TOOLS_MAX_ITERATIONS]);
   TEST_ASSERT_EQUAL_INT(1, history_notes());
   TEST_ASSERT_FALSE(atomic_load(&s_session.turn_save_step));
}

/* A repeated call on the save step runs as a save step: no extra call. */
static void test_save_step_repeat_makes_no_extra_call(void) {
   s_script = SCRIPT_REPEAT_LAST;
   char *reply = run_loop(TEST_NOTE, 0);
   TEST_ASSERT_EQUAL_STRING("", reply);
   free(reply);
   TEST_ASSERT_EQUAL_INT(LLM_TOOLS_MAX_ITERATIONS + 1, s_calls);
   TEST_ASSERT_EQUAL_INT(LLM_TOOLS_MAX_ITERATIONS + 1, s_batches);
   TEST_ASSERT_EQUAL_INT(1, s_saves_run[LLM_TOOLS_MAX_ITERATIONS]);
}

/* A turn that ends before its last step is never told about it. */
static void test_early_end_appends_no_note(void) {
   s_script = SCRIPT_TEXT_AT;
   s_at = 3;
   char *reply = run_loop(TEST_NOTE, 0);
   TEST_ASSERT_EQUAL_STRING("Done", reply);
   free(reply);
   TEST_ASSERT_EQUAL_INT(3, s_calls);
   TEST_ASSERT_EQUAL_INT(0, history_notes());
}

/* A reply on the save step with nothing left to save is the turn's reply. */
static void test_save_step_without_calls_returns_its_words(void) {
   s_script = SCRIPT_TEXT_AT;
   s_at = LLM_TOOLS_MAX_ITERATIONS + 1;
   char *reply = run_loop(TEST_NOTE, 0);
   TEST_ASSERT_EQUAL_STRING("Done", reply);
   free(reply);
   TEST_ASSERT_EQUAL_INT(LLM_TOOLS_MAX_ITERATIONS + 1, s_calls);
   TEST_ASSERT_TRUE(s_note_last[LLM_TOOLS_MAX_ITERATIONS]);
}

/* A call that ends the turn (skip_followup) at step k: its batch's saves run,
 * and no call k+1 follows.  A save turn has no reader, so its tools' results
 * are not its reply. */
static void test_finish_ends_the_turn_at_its_step(void) {
   s_script = SCRIPT_FINISH_AT;
   s_at = 4;
   char *reply = run_loop(TEST_NOTE, 0);
   TEST_ASSERT_EQUAL_STRING("", reply);
   free(reply);
   TEST_ASSERT_EQUAL_INT(4, s_calls);
   TEST_ASSERT_EQUAL_INT(2, s_saves_run[3]);
}

/* The same call in a turn that ends on an answer: the tool's result is the reply. */
static void test_finish_reply_is_kept_in_an_answer_turn(void) {
   s_script = SCRIPT_FINISH_AT;
   s_at = 4;
   char *reply = run_loop(NULL, 0);
   TEST_ASSERT_EQUAL_STRING("concluded", reply);
   free(reply);
   TEST_ASSERT_EQUAL_INT(4, s_calls);
}

/* The input-token ceiling reached at the last step stops the turn before the
 * note or the call. */
static void test_ceiling_at_last_step_makes_no_call(void) {
   char *reply = run_loop(TEST_NOTE, (int64_t)LLM_TOOLS_MAX_ITERATIONS * TEST_TOKENS_PER_CALL);
   TEST_ASSERT_EQUAL_STRING("", reply);
   free(reply);
   TEST_ASSERT_EQUAL_INT(LLM_TOOLS_MAX_ITERATIONS, s_calls);
   TEST_ASSERT_EQUAL_INT(0, history_notes());
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_default_turn_ends_on_an_answer);
   RUN_TEST(test_save_turn_ends_after_its_save_step);
   RUN_TEST(test_save_step_repeat_makes_no_extra_call);
   RUN_TEST(test_early_end_appends_no_note);
   RUN_TEST(test_save_step_without_calls_returns_its_words);
   RUN_TEST(test_finish_ends_the_turn_at_its_step);
   RUN_TEST(test_finish_reply_is_kept_in_an_answer_turn);
   RUN_TEST(test_ceiling_at_last_step_makes_no_call);
   return UNITY_END();
}
