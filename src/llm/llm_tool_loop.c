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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Central tool iteration loop for LLM streaming with tool calling.
 *
 * This module extracts the tool call -> execute -> re-call loop from the
 * individual providers (OpenAI, Claude) into a single central loop that:
 * - Runs auto-compaction between iterations (THE KEY FIX)
 * - Provides duplicate tool call detection for all providers
 * - Handles provider switching mid-loop (switch_llm tool)
 * - Enforces uniform iteration limits
 */

#include "llm/llm_tool_loop.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/llm_response_finalize.h"
#include "core/session_manager.h"
#include "core/session_stop.h"
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
#include "llm/llm_openai_internal.h"
#include "llm/llm_rate_limit.h"
#include "llm/llm_tool_history.h"
#include "llm/llm_tool_images.h"
#include "llm/llm_tool_views.h"
#include "llm/llm_tool_views_apply.h"
#include "llm/llm_tools.h"
#include "llm/llm_turn_blocks.h"
#include "logging.h"
#include "prompts.h"
#include "utils/string_utils.h"
#include "webui/webui_server.h"

/* Transient-network retry policy.  Triggered when the provider returns rc != 0
 * with LLM_ERR_TRANSIENT_NETWORK (HTTP 429/5xx, or sent with nothing back) or
 * LLM_ERR_UNREACHABLE (see the loop for when that one isn't retried).  Backoff is
 * exponential from BASE: 1s, 2s, 4s. */
#define LLM_TRANSIENT_RETRY_MAX 3
#define LLM_TRANSIENT_BACKOFF_BASE_MS 1000

/* Thread-local flag: set when the tool loop skips follow-up */
static _Thread_local bool s_did_skip_followup = false;

bool llm_tool_loop_did_skip_followup(void) {
   return s_did_skip_followup;
}

/* =============================================================================
 * History Format Helpers
 *
 * Format assistant messages with tool calls in the provider's native format.
 * Extracted from llm_openai.c and llm_claude.c recursion blocks.
 * ============================================================================= */

/* Map the active provider to a short label stored as METADATA in the reasoning JSON.
 * The WebUI does NOT display this — the "AI thought" panel is labelled with the
 * assistant's name (DawnFormat.assistantName) — but it's kept for debugging/provenance
 * (which provider produced a stored reasoning blob). */
static const char *reasoning_provider_label(llm_type_t llm_type, cloud_provider_t cloud_provider) {
   if (llm_type == LLM_LOCAL) {
      return "local";
   }
   switch (cloud_provider) {
      case CLOUD_PROVIDER_CLAUDE:
         return "claude";
      case CLOUD_PROVIDER_GEMINI:
         return "gemini";
      case CLOUD_PROVIDER_OPENROUTER:
         return "openrouter";
      case CLOUD_PROVIDER_OPENAI:
      default:
         return "openai";
   }
}

/* Build the display-only reasoning JSON ({provider, content?, tokens?}) for an iteration's
 * assistant message, or NULL if there's nothing to show.  Duration is intentionally omitted
 * (the daemon doesn't track per-iteration thinking wall-clock; the panel degrades to
 * "(N tokens)").  Caller frees with free(). */
static char *build_reasoning_json(const llm_tool_response_t *result, const char *provider_label) {
   bool has_content = result->thinking_content && result->thinking_content[0] != '\0';
   if (!has_content && result->reasoning_tokens <= 0) {
      return NULL;
   }
   struct json_object *obj = json_object_new_object();
   if (!obj) {
      return NULL;
   }
   json_object_object_add(obj, "provider", json_object_new_string(provider_label));
   if (has_content) {
      json_object_object_add(obj, "content", json_object_new_string(result->thinking_content));
   }
   if (result->reasoning_tokens > 0) {
      json_object_object_add(obj, "tokens", json_object_new_int(result->reasoning_tokens));
   }
   /* PLAIN: no inter-token whitespace — trims the persisted blob (reasoning content can
    * be multi-KB). Re-parsed as JSON on reload, so the formatting is immaterial. */
   char *out = strdup(json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN));
   json_object_put(obj);
   if (!out) {
      OLOG_WARNING("build_reasoning_json: strdup OOM — reasoning will not persist for this turn");
   }
   return out;
}

/* A request of this call's session is being sent: its size estimate, to
 * calibrate its model's density when its usage comes back (llm_compaction.h).
 * Counted as the reply clamp counts it: a question already in the history
 * (a session's turn appends it before the loop) counts once. */
static void note_request(const llm_tool_loop_params_t *params, const char *input) {
   if (!params->has_session) {
      return;
   }
   const int estimate = llm_compaction_request_estimate(params->conversation_history, input);
   llm_context_note_request(params->session_id, estimate, params->llm_type, params->cloud_provider,
                            params->model);
}

/* The session this call is for (a ref, or NULL): none for a call on no
 * session's behalf.  @p even_detached finds one whose client has gone (its
 * turn still runs and saves). */
static session_t *call_session(const llm_tool_loop_params_t *params, bool even_detached) {
   if (!params->has_session) {
      return NULL;
   }
   return even_detached ? session_get_for_reconnect(params->session_id)
                        : session_get(params->session_id);
}

static char *clean_block_text(const char *text, void *session) {
   return llm_response_clean((session_t *)session, text);
}

/* What the model said before its tool calls is stored and sent back on every
 * later request, like a reply: it gets the reply's cleanup (tags out, text
 * imitating DAWN's framing defused).  Its text and its blocks, which every
 * provider renders and stores the turn from.  Text that can't be cleaned (a
 * failed allocation) is dropped, never kept as the model wrote it. */
static void clean_narration(const llm_tool_loop_params_t *params, llm_tool_response_t *result) {
   session_t *s = call_session(params, true);
   if (result->text && result->text[0] != '\0') {
      char *clean = llm_response_clean(s, result->text);
      free(result->text);
      result->text = clean; /* NULL on failure: no text, as with none written */
   }
   llm_turn_blocks_clean_text(result->blocks, clean_block_text, s);
   if (s) {
      session_release(s);
   }
}

/* Persist the tool messages just appended to history in [before_len, end)
 * (llm_tool_history_persist), on the session the call is for.
 *
 * for_reconnect (not session_get): a turn that survives a client disconnect
 * must STILL persist its tool-iteration rows server-side.  session_get() skips
 * disconnected sessions and would return NULL here, silently dropping the
 * tool_calls + role:tool rows so a reload shows the answer but no tool use.
 * The session is not torn down mid-turn (turn_in_flight guards the idle sweep;
 * the worker holds a ref). */
static void persist_appended_tool_turn(llm_tool_loop_params_t *params,
                                       int before_len,
                                       const char *reasoning_json,
                                       int iteration,
                                       const tool_result_list_t *results) {
   session_t *s = call_session(params, true);
   if (!s) {
      return;
   }
   llm_tool_history_persist(s, params->conversation_history, before_len, reasoning_json, iteration,
                            results);
   session_release(s);
}

/* Stash BOTH final-turn signals on the session in ONE lookup:
 *   - the finish/stop reason, so a background-job worker can tell a cut-off answer
 *     ("max_tokens"/"length") from a clean finish; and
 *   - the FINAL answer's display-only reasoning (E3 "AI thought" panel), so the
 *     post-dispatch server persist (webui_persist_final_answer) writes it to the
 *     messages.reasoning column + fans it out — the browser client-save carried this
 *     today, and retiring that save drops it server-side otherwise (SERVER_AUTHORITATIVE
 *     §6c-G1).
 * Both are additive + best-effort: other callers never read last_finish_reason, a lookup
 * miss is a no-op (the worker then reads an empty reason = "not truncated"), and a turn
 * with no thinking content leaves final_answer.reasoning_json NULL (build_reasoning_json returns
 * NULL; the turn-start clear in llm_call_prepare guarantees no stale inheritance).  Folded
 * from two helpers into one so a final-answer return does a single session_get_for_reconnect
 * rather than two.  TAKES ownership of the built reasoning JSON into the session (the
 * consuming persist frees it). */
static void tool_loop_stash_final(const llm_tool_loop_params_t *params,
                                  const llm_tool_response_t *result,
                                  const char *provider_label) {
   const char *reason = result != NULL ? result->finish_reason : NULL;
   bool have_reason = (reason != NULL && reason[0] != '\0');
   char *json = build_reasoning_json(result, provider_label);
   /* A final answer ran no calls: its blocks keep none. */
   struct json_object *blocks = llm_tool_history_blocks_as_run(result, false);
   if (!have_reason && json == NULL && blocks == NULL) {
      return; /* nothing to write — skip the lookup entirely */
   }
   session_t *s = call_session(params, true);
   if (s == NULL) {
      free(json);
      json_object_put(blocks);
      return;
   }
   if (have_reason) {
      snprintf(s->last_finish_reason, sizeof(s->last_finish_reason), "%s", reason);
   }
   if (json != NULL) {
      free(s->final_answer.reasoning_json);
      s->final_answer.reasoning_json = json; /* take ownership */
   }
   if (blocks != NULL) {
      json_object_put(s->final_answer.blocks);
      s->final_answer.blocks = blocks; /* the answer's blocks (take ownership) */
   }
   session_release(s);
}

/* A call a stop cut (@p result failed, holding what had streamed): its text
 * is kept with its finished blocks and reasoning, for the turn's finalizer to
 * save, when the user's Stop cut this turn (session_stop.h; no other cancel)
 * and the turn's caller saves its reply.  The loop still returns NULL:
 * nothing here reads as a finished reply. */
static void keep_stopped_reply(const llm_tool_loop_params_t *params,
                               const llm_tool_response_t *result) {
   if (!result->stopped || !result->text || !*result->text) {
      return;
   }
   session_t *s = call_session(params, true);
   if (!s) {
      return;
   }
   if (atomic_load(&s->will_persist_turn) && session_turn_stopped_by_user(s)) {
      tool_loop_stash_final(params, result,
                            reasoning_provider_label(params->llm_type, params->cloud_provider));
      free(s->final_answer.stopped_text);
      s->final_answer.stopped_text = strdup(result->text);
   }
   session_release(s);
}

/* The WebUI's (webui_server.c); without the WebUI there is no stream to end. */
__attribute__((weak)) void webui_stream_message_end(struct session *session) {
   (void)session;
}

/* The model's message before its tool calls has ended: its stream resolves a
 * citation tag at its end (webui_stream_message_end). */
static void end_message_stream(llm_tool_loop_params_t *params) {
   session_t *s = call_session(params, false);
   if (s) {
      webui_stream_message_end(s);
      session_release(s);
   }
}

/* Fire the per-session iteration-boundary hook (if installed).  Called when an
 * iteration produced tool calls, just before the tools execute, so the WebUI can
 * close the current streaming bubble — the next iteration's text then opens a fresh
 * bubble below the tool entries.  No-op for satellite / local-mic turns (hook NULL). */
static void fire_tool_iteration_boundary(llm_tool_loop_params_t *params) {
   session_t *s = call_session(params, false);
   if (!s) {
      return;
   }
   if (s->tool_iteration_cb) {
      s->tool_iteration_cb(s, s->tool_iteration_userdata);
   }
   session_release(s);
}

/* Append a note to the model (a user message of the loop's own), marked as
 * request context. */
static void append_loop_note(struct json_object *history, const char *text) {
   json_object *note = json_object_new_object();
   if (!note) {
      return;
   }
   json_object_object_add(note, "role", json_object_new_string("user"));
   json_object_object_add(note, "content", json_object_new_string(text));
   llm_history_set_kind(note, MESSAGE_KIND_LOOP_NOTE);
   session_history_append(history, note);
}

/* One provider call: this attempt's outcome alone (the error reset), and while the
 * local LLM can still take the turn, a single short attempt with its transient error
 * frames held back (llm_provider_errors_quiet).  A provider turns tools off for an
 * iteration at the cap, so an @p answer_only call is sent as one and every other as
 * an iteration below it: a save turn's last call keeps its tools, and its request
 * reads as every one before it. */
static int call_provider(const llm_tool_loop_params_t *params,
                         const char *input_text,
                         int iteration,
                         bool answer_only,
                         llm_tool_response_t *result) {
   const int sent = answer_only                                ? LLM_TOOLS_MAX_ITERATIONS
                    : iteration < LLM_TOOLS_MAX_ITERATIONS - 1 ? iteration
                                                               : LLM_TOOLS_MAX_ITERATIONS - 1;
   const bool was_quiet = llm_provider_errors_quiet();
   llm_set_provider_errors_quiet(params->fallback_available && !params->tools_ran);
   llm_set_last_error(LLM_ERR_NONE);
   const int rc = params->provider_fn(params->conversation_history, input_text, params->base_url,
                                      params->api_key, params->model, params->chunk_callback,
                                      params->callback_userdata, sent, result);
   llm_set_provider_errors_quiet(was_quiet);
   return rc;
}

/* The turn's last word: @p note appended (and saved) after the tool results,
 * then one call with tools disabled.  Returns the answer (caller frees), or
 * NULL when the call fails or brings no text. */
static char *final_answer_without_tools(llm_tool_loop_params_t *params,
                                        const char *note,
                                        int iteration) {
   const int note_at = json_object_array_length(params->conversation_history);
   append_loop_note(params->conversation_history, note);
   persist_appended_tool_turn(params, note_at, NULL, iteration, NULL);

   OLOG_INFO("Tool loop: Making final call without tools to present gathered results");
   llm_tool_response_t result;
   memset(&result, 0, sizeof(result));
   note_request(params, "");
   const int rc = call_provider(params, "", iteration, true, &result);
   char *text = NULL;
   if (rc == 0 && result.text) {
      text = strdup(result.text);
      tool_loop_stash_final(params, &result,
                            reasoning_provider_label(params->llm_type, params->cloud_provider));
   } else if (rc != 0) {
      keep_stopped_reply(params, &result);
   }
   llm_tool_response_free(&result);
   return text;
}

/**
 * @brief Add closing assistant message to complete tool call history
 *
 * When skip_followup is set, we need to add a synthetic assistant message
 * to prevent HTTP 400 on subsequent requests due to incomplete history.
 */
static void append_closing_message(struct json_object *history,
                                   const char *text,
                                   llm_history_format_t format) {
   if (!text) {
      return;
   }

   json_object *closing_msg = json_object_new_object();
   json_object_object_add(closing_msg, "role", json_object_new_string("assistant"));

   if (format == LLM_HISTORY_CLAUDE) {
      json_object *content_array = json_object_new_array();
      json_object *text_block = json_object_new_object();
      json_object_object_add(text_block, "type", json_object_new_string("text"));
      json_object_object_add(text_block, "text", json_object_new_string(text));
      json_object_array_add(content_array, text_block);
      json_object_object_add(closing_msg, "content", content_array);
   } else {
      json_object_object_add(closing_msg, "content", json_object_new_string(text));
   }
   llm_history_set_kind(closing_msg, MESSAGE_KIND_LOOP_NOTE);

   session_history_append(history, closing_msg);
   OLOG_INFO("Tool loop: Added closing assistant message to complete history");
}

/**
 * @brief Free heap-allocated data from tool results (vision images, extended results)
 */
static void free_tool_result_resources(tool_result_list_t *results) {
   if (!results) {
      return;
   }
   for (int i = 0; i < results->count; i++) {
      if (results->results[i].vision_image) {
         free(results->results[i].vision_image);
         results->results[i].vision_image = NULL;
      }
      if (results->results[i].result_extended) {
         free(results->results[i].result_extended);
         results->results[i].result_extended = NULL;
      }
   }
}

/**
 * @brief Resolve current provider configuration for follow-up calls
 *
 * Checks if the provider was switched (e.g., switch_llm tool) and updates
 * the loop parameters accordingly.
 *
 * @return true if provider changed, false if same provider
 */
static bool resolve_provider_switch(llm_tool_loop_params_t *params) {
   llm_resolved_config_t current_config;

   if (llm_get_current_resolved_config(&current_config) != 0) {
      return false; /* Can't resolve config, stay on current provider */
   }

   /* The selection's model, empty included (its provider's default, such as a
    * local server's own), never the previous provider's name.  Copied: the
    * resolved pointer may dangle after return. */
   safe_strscpy(params->model_storage, current_config.model ? current_config.model : "");
   params->model = params->model_storage;

   /* Determine new provider type and format */
   llm_single_shot_fn new_fn = params->provider_fn;
   llm_history_format_t new_format = params->history_format;
   bool switched = false;

   /* The wire format follows the route, not the provider: OpenRouter's
    * anthropic/ models take the Messages format too. */
   const char *endpoint = current_config.endpoint ? current_config.endpoint : params->base_url;
   const bool messages = llm_uses_anthropic_messages(current_config.type,
                                                     current_config.cloud_provider, params->model,
                                                     endpoint);
   if (!messages && params->history_format == LLM_HISTORY_CLAUDE) {
      new_fn = (llm_single_shot_fn)llm_openai_streaming_single_shot;
      new_format = LLM_HISTORY_OPENAI;
      switched = true;
      OLOG_INFO("Tool loop: Provider switched to the OpenAI-compatible API");
   } else if (messages && params->history_format == LLM_HISTORY_OPENAI) {
      new_fn = (llm_single_shot_fn)llm_claude_streaming_single_shot;
      new_format = LLM_HISTORY_CLAUDE;
      switched = true;
      OLOG_INFO("Tool loop: Provider switched to the Anthropic Messages API");
   }

   /* Always update credentials (even if provider didn't change,
    * config may have changed model/endpoint) */
   params->base_url = endpoint;
   params->api_key = current_config.api_key;
   params->llm_type = current_config.type;
   params->cloud_provider = current_config.cloud_provider;

   if (switched) {
      params->provider_fn = new_fn;
      params->history_format = new_format;
   }

   return switched;
}

/* =============================================================================
 * Central Tool Iteration Loop
 * ============================================================================= */

/* A batch's finish: its images held to what the next request may carry (a
 * loop has no seam to compact at), then the view stage. */
static void finish_batch(const tool_call_list_t *calls, tool_result_list_t *results, void *batch) {
   const llm_tool_views_batch_t *b = batch;
   llm_image_limit_t limit;
   (void)llm_tool_images_request_limit(b->params->llm_type, b->params->cloud_provider,
                                       b->params->model, &limit);
   (void)llm_tool_images_cap_batch(b->params->conversation_history, results, &limit);
   llm_tool_views_finish_batch(calls, results, batch);
}

static char *tool_iteration_loop_body(llm_tool_loop_params_t *params) {
   s_did_skip_followup = false; /* this run's alone, even one refused below */
   if (!params || !params->provider_fn || !params->conversation_history) {
      OLOG_ERROR("Tool loop: Invalid parameters");
      return NULL;
   }

   char *final_response = NULL;

   /* Cancellation policy for this turn (loop-invariant).  A background turn
    * (job / deep research) breaks only on its own session cancel flag; a
    * foreground turn additionally honors the global wake-word / Ctrl+C
    * interrupt.  A zero-valued params (cancel_flag=NULL, is_background=false)
    * yields {NULL, honor_global=true} — the legacy global-only behavior. */
   const llm_interrupt_ctx_t ictx = { .session_flag = params->cancel_flag,
                                      .honor_global = !params->is_background };

   /* The turn's last call: an answer (tools off), or with a save note, a save. */
   const bool saves = params->save_note != NULL;

   for (int iteration = 0; iteration <= LLM_TOOLS_MAX_ITERATIONS; iteration++) {
      llm_cache_monitor_set_iteration(iteration); /* tags this iteration's provider call */
      const bool last = iteration >= LLM_TOOLS_MAX_ITERATIONS;
      const bool save_step = last && saves;
      /* Step 0: cumulative-session input-token ceiling (opt-in; 0 = unlimited).
       * Input tokens are recorded per provider response (session_record_query), so
       * at the top of this iteration the session total reflects spend through the
       * previous iteration.  Stopping HERE bounds overshoot to a single
       * iteration's tokens rather than a whole multi-tool turn's (the deep-
       * research per-round guard).  This check is only ever reached BETWEEN
       * iterations (a completed iteration with no tool calls already returned its
       * text at step 4), so there is no partial answer to hand back — return an
       * empty string (never NULL barring OOM) so the caller doesn't misread a
       * budget stop as a provider failure. */
      if (params->cumulative_input_token_ceiling > 0) {
         session_t *loop_session = call_session(params, true);
         if (loop_session) {
            uint64_t tok = 0;
            session_metrics_totals(loop_session, &tok, NULL);
            session_release(loop_session);
            if (tok >= (uint64_t)params->cumulative_input_token_ceiling) {
               OLOG_INFO("Tool loop: session input tokens %llu reached ceiling %lld at "
                         "iteration %d — stopping turn",
                         (unsigned long long)tok, (long long)params->cumulative_input_token_ceiling,
                         iteration);
               char *empty = malloc(1);
               if (empty != NULL) {
                  empty[0] = '\0';
               }
               return empty;
            }
         }
      }

      /* Step 1: the request this iteration sends must fit.  A history is
       * compacted only at a turn seam (session_compaction.h), never mid tool round
       * (its reasoning is bound to the request as it stands): past the hard
       * threshold after tools ran, the turn closes here.  A background turn goes
       * on in a continuation turn, compacted at its seam; a foreground one answers
       * with what it has, or, when even that wouldn't fit, says so. */
      {
         /* The request's real size: the estimate, calibrated by what this
          * session's requests measured (llm_compaction.h). */
         const int history_estimate = llm_context_estimate_tokens(params->conversation_history);
         const int est_tokens = params->has_session
                                    ? llm_context_request_tokens(params->session_id,
                                                                 history_estimate, params->llm_type,
                                                                 params->cloud_provider,
                                                                 params->model)
                                    : history_estimate;
         const int window = llm_context_get_size(params->llm_type, params->cloud_provider,
                                                 params->model);
         const float hard = llm_context_hard_threshold();
         if (window > 0 && iteration > 0 && est_tokens >= (int)((float)window * hard)) {
            OLOG_WARNING("Tool loop: ~%d tokens reach the hard threshold of the %d window at "
                         "iteration %d; closing the turn",
                         est_tokens, window, iteration);
            if (params->is_background) {
               session_t *loop_session = call_session(params, true);
               if (loop_session) {
                  atomic_store(&loop_session->turn_overflowed, true);
                  session_release(loop_session);
               }
               char *empty = malloc(1);
               if (empty != NULL) {
                  empty[0] = '\0';
               }
               return empty;
            }
            if (est_tokens < window) {
               return final_answer_without_tools(params, TOOL_LOOP_DIRECTIVE_CONTEXT_FULL,
                                                 iteration);
            }
            return strdup("I've run out of room in this conversation partway through this. Send "
                          "another message and I'll pick it up from a summary of what we have.");
         }
         if (window > 0 && est_tokens >= window) {
            OLOG_ERROR("Tool loop: estimated request ~%d tokens exceeds the model window %d "
                       "(iteration %d): a single message is larger than the window",
                       est_tokens, window, iteration);
         }
      }

      /* Step 2: Gate cloud calls through rate limiter */
      if (params->llm_type != LLM_LOCAL) {
         if (llm_rate_limit_wait_ctx(&ictx)) {
            OLOG_WARNING("Tool loop: rate limit wait interrupted at iteration %d", iteration);
            return NULL;
         }
      }

      /* Step 2a: a save step is told what it is for.  After the stops above, so a
       * turn that ends there appends nothing. */
      if (save_step) {
         const int note_at = json_object_array_length(params->conversation_history);
         append_loop_note(params->conversation_history, params->save_note);
         persist_appended_tool_turn(params, note_at, NULL, iteration, NULL);
      }

      /* Step 3: Call provider single-shot */
      llm_tool_response_t result;
      memset(&result, 0, sizeof(result));

      note_request(params, params->input_text);
      int rc = call_provider(params, params->input_text, iteration, last && !saves, &result);

      /* Retry transient network failures (unreachable, HTTP 429, HTTP 5xx) with
       * exponential backoff before bubbling up.  Each retry is
       * a fresh provider call within the same iteration — failed attempts
       * don't burn an iteration slot.  Hard errors (auth, bad request,
       * parse failure) bypass this loop.
       *
       * Retries re-enter llm_rate_limit_wait() on cloud paths: a server-driven
       * 429 means we're locally overcounted, so we must consult the local
       * budget again rather than racing past it.  Log noise stays bounded:
       * intermediate retries log at INFO, final give-up logs at ERROR. */
      /* An unreachable cloud with a local answer still possible goes straight back
       * to the caller; once a tool ran, the turn can't move, so it retries. */
      const bool retry_unreachable = !(params->fallback_available && !params->tools_ran);
      for (int retry = 0; rc != 0 &&
                          (llm_last_error() == LLM_ERR_TRANSIENT_NETWORK ||
                           (llm_last_error() == LLM_ERR_UNREACHABLE && retry_unreachable)) &&
                          retry < LLM_TRANSIENT_RETRY_MAX;
           retry++) {
         int backoff_ms = LLM_TRANSIENT_BACKOFF_BASE_MS << retry; /* 1s, 2s, 4s */
         OLOG_INFO("Tool loop: transient network error at iteration %d, "
                   "retry %d/%d in %dms",
                   iteration, retry + 1, LLM_TRANSIENT_RETRY_MAX, backoff_ms);
         if (llm_sleep_with_interrupt_check_ctx(backoff_ms, &ictx)) {
            OLOG_INFO("Tool loop: retry backoff interrupted at iteration %d", iteration);
            llm_tool_response_free(&result);
            return NULL;
         }
         /* Re-enter the rate-limit gate on cloud retries (mirrors the
          * top-of-iteration gate at the start of this loop).  Local LLM
          * is exempt — no shared per-minute budget there. */
         if (params->llm_type != LLM_LOCAL) {
            if (llm_rate_limit_wait_ctx(&ictx)) {
               OLOG_WARNING("Tool loop: rate limit wait interrupted during retry at "
                            "iteration %d",
                            iteration);
               llm_tool_response_free(&result);
               return NULL;
            }
         }
         llm_tool_response_free(&result);
         memset(&result, 0, sizeof(result));
         note_request(params, params->input_text);
         rc = call_provider(params, params->input_text, iteration, last && !saves, &result);
      }

      if (rc != 0) {
         if (llm_last_error() == LLM_ERR_UNREACHABLE && !retry_unreachable) {
            OLOG_INFO("Tool loop: cloud unreachable at iteration %d; back to the caller for the "
                      "local LLM",
                      iteration);
         } else if (llm_error_is_transient(llm_last_error())) {
            OLOG_ERROR("Tool loop: transient network error at iteration %d after %d retries, "
                       "giving up",
                       iteration, LLM_TRANSIENT_RETRY_MAX);
         } else if (llm_interrupt_ctx_triggered(&ictx)) {
            /* The user interrupted (wake word, Stop): not a failure. */
            OLOG_INFO("Tool loop: provider call interrupted at iteration %d", iteration);
         } else {
            OLOG_ERROR("Tool loop: Provider call failed at iteration %d", iteration);
         }
         keep_stopped_reply(params, &result);
         llm_tool_response_free(&result);
         return NULL;
      }

      /* Step 4: If no tool calls, return text response.
       *
       * A clean provider call (rc == 0) with no tool_calls and NULL text ends
       * the turn without a word.  After tools ran in a turn someone is waiting
       * on, the model is asked once for the outcome (tools off).  Otherwise, or
       * when that brings nothing, an empty malloc'd string is returned rather
       * than NULL so callers (session_manager.c) do not classify it as an LLM
       * call failure.  malloc(1) so the caller owns a heap pointer it can
       * free() identically to the populated path. */
      if (!result.has_tool_calls) {
         if (result.text == NULL && iteration > 0 && !params->is_background &&
             !llm_interrupt_ctx_triggered(&ictx)) {
            /* Tools ran this turn and the model ended without a word: the user
             * would see nothing, even when a tool failed.  Ask once for the
             * outcome, tools off.  (A background turn has no one waiting; its
             * empty ending is judged by its job.) */
            OLOG_INFO("Tool loop: empty content at iteration %d after tool calls; asking for "
                      "the outcome",
                      iteration);
            llm_tool_response_free(&result);
            char *answer = final_answer_without_tools(params, TOOL_LOOP_DIRECTIVE_REPORT_OUTCOME,
                                                      iteration);
            /* Stopped during that call: no reply, never an empty finished one. */
            if (answer || llm_interrupt_ctx_triggered(&ictx)) {
               return answer;
            }
            memset(&result, 0, sizeof(result)); /* freed above: nothing left to free */
         }
         if (result.text == NULL) {
            OLOG_INFO("Tool loop: provider returned empty content at iteration %d "
                      "(clean end_turn, no response needed)",
                      iteration);
            llm_tool_response_free(&result);
            char *empty = malloc(1);
            if (!empty) {
               OLOG_ERROR("Tool loop: malloc(1) failed on empty-content path");
               return NULL;
            }
            empty[0] = '\0';
            return empty;
         }
         tool_loop_stash_final(params, &result,
                               reasoning_provider_label(params->llm_type, params->cloud_provider));
         final_response = result.text;
         result.text = NULL; /* Transfer ownership to caller */
         llm_tool_response_free(&result);
         return final_response;
      }

      /* Flush any streamed text from this iteration before executing tools.
       * The LLM may have streamed text before its tool_use blocks (e.g. "Let me check
       * that for you."). Without this, the sentence buffer holds "you." waiting for more
       * text, and the next iteration's response concatenates directly ("you.Alright")
       * without a sentence break. The paragraph break triggers the sentence buffer to
       * emit the pending sentence for TTS before the tool execution pause. */
      end_message_stream(params); /* before the break: a tag ending it is dropped */
      if (params->chunk_callback) {
         typedef void (*text_chunk_cb)(const char *, void *);
         ((text_chunk_cb)params->chunk_callback)("\n\n", params->callback_userdata);
      }

      /* Close the current streaming bubble at the iteration boundary so this
       * iteration's tool call/result entries render after its text, and the next
       * iteration opens a fresh bubble below them (live order matches reload order). */
      fire_tool_iteration_boundary(params);

      OLOG_INFO("Tool loop: %d tool call(s) at iteration %d/%d", result.tool_calls.count, iteration,
                LLM_TOOLS_MAX_ITERATIONS);

      for (int i = 0; i < result.tool_calls.count; i++) {
         OLOG_INFO("  Tool call [%d]: id=%s name=%s args=%s", i, result.tool_calls.calls[i].id,
                   result.tool_calls.calls[i].name, result.tool_calls.calls[i].arguments);
      }

      /* Step 5: Check for duplicate tool calls (prevents infinite loops).  Not on a
       * save step: nothing follows it to loop, and the check would cost a call. */
      if (!save_step && result.tool_calls.count > 0 &&
          llm_tools_is_duplicate_call(params->conversation_history, result.tool_calls.calls[0].name,
                                      result.tool_calls.calls[0].arguments,
                                      params->history_format)) {
         OLOG_WARNING("Tool loop: Duplicate tool call detected, forcing text response");

         /* Add a hint to use existing results.  Phrased as a plain instruction
          * (no "[System:]" prefix) so a reasoning model doesn't mistake this
          * daemon control message for an injected directive and flag it; tool-
          * agnostic wording since this fires for any repeated tool, not search. */
         const int note_at = json_object_array_length(params->conversation_history);
         append_loop_note(params->conversation_history, TOOL_LOOP_NOTE_DUPLICATE_CALL);
         persist_appended_tool_turn(params, note_at, NULL, iteration, NULL);

         llm_tool_response_free(&result);

         /* Make one more call with tools disabled (iteration = MAX forces no tools) */
         OLOG_INFO("Tool loop: Making final call without tools to force text response");
         memset(&result, 0, sizeof(result));
         note_request(params, "");
         rc = call_provider(params, "", iteration, true, &result);

         if (rc != 0) {
            keep_stopped_reply(params, &result);
            llm_tool_response_free(&result);
            return NULL;
         }

         tool_loop_stash_final(params, &result,
                               reasoning_provider_label(params->llm_type, params->cloud_provider));
         final_response = result.text;
         result.text = NULL;
         llm_tool_response_free(&result);
         return final_response;
      }

      /* Step 6: Execute tools */
      tool_result_list_t *results = calloc(1, sizeof(tool_result_list_t));
      if (!results) {
         OLOG_ERROR("Tool loop: Failed to allocate tool results");
         llm_tool_response_free(&result);
         return NULL;
      }
      /* The batch's budget, planned before it runs: a result_read answer in
       * it is built to its fair share.  After it runs, the view stage keeps
       * what is over its share whole and shows a view, then finishes every
       * result (neutralized last; the WebUI told what the model sees) before
       * anything reads them. */
      llm_tool_views_budget_t view_budget;
      llm_tool_views_budget_batch(params, &result.tool_calls, result.text, &view_budget);
      session_t *view_session = call_session(params, true);
      tool_result_store_set_read_budget(
          view_session, llm_tool_views_read_chars(view_budget.chars, result.tool_calls.count));
      const llm_tool_views_batch_t view_batch = {
         .params = params,
         .session = view_session,
         .budget = &view_budget,
      };
      /* A save step's batch runs only what saves (the policy's save-step caller):
       * a lookup now would bring a result no call is left to read. */
      if (view_session && save_step) {
         atomic_store(&view_session->turn_save_step, true);
      }
      llm_tools_execute_all(&result.tool_calls, results, finish_batch, (void *)&view_batch);
      params->tools_ran = true;
      if (view_session) {
         atomic_store(&view_session->turn_save_step, false);
         tool_result_store_set_read_budget(view_session, 0);
         session_release(view_session);
      }

      /* Log tool results */
      for (int i = 0; i < results->count; i++) {
         const char *content = tool_result_content(&results->results[i]);
         OLOG_INFO("  Tool result [%d] id=%s result=%.200s%s", i, results->results[i].tool_call_id,
                   content, strlen(content) > 200 ? "..." : "");
      }

      /* Step 7: Check follow-up context BEFORE appending to history.
       * Tools like reset_conversation invalidate the conversation history pointer,
       * so we must not append to it after they run. */
      tool_followup_context_t followup;
      llm_tools_prepare_followup(results, &followup);

      if (followup.skip_followup) {
         OLOG_INFO("Tool loop: Skipping follow-up (tool requested no follow-up)");
         s_did_skip_followup = true;

         /* A save turn has no reader: its tools' results are not a reply. */
         if (saves) {
            free(followup.direct_response);
            followup.direct_response = strdup("");
         }
         /* Send through chunk callback so TTS receives it */
         if (followup.direct_response && followup.direct_response[0] && params->chunk_callback) {
            void (*cb)(const char *, void *) = params->chunk_callback;
            cb(followup.direct_response, params->callback_userdata);
         }

         free_tool_result_resources(results);
         free(results);
         llm_tool_response_free(&result);
         /* No final-reasoning stash: direct_response is a tool's own canned text, not a
          * model turn, so it carries no E3 reasoning.  The turn-start clear leaves the
          * stash NULL here — do NOT "fix" this by stashing result's reasoning. */
         return followup.direct_response; /* Caller must free */
      }

      /* Step 8: Append assistant message + tool results to history */
      clean_narration(params, &result);
      int hist_before = json_object_array_length(params->conversation_history);
      if (params->history_format == LLM_HISTORY_CLAUDE) {
         char carrier[LLM_CARRIER_MAX];
         llm_request_carrier(params->base_url, params->api_key, carrier, sizeof(carrier));
         llm_tool_history_append_claude(params->conversation_history, &result, results, carrier);
      } else {
         llm_tool_history_append_openai(params->conversation_history, &result, results);
      }
      /* Step 8a: Persist the just-appended structured tool messages (E2) + this iteration's
       * display-only reasoning (E3) attached to its assistant row. */
      char *reasoning_json = build_reasoning_json(
          &result, reasoning_provider_label(params->llm_type, params->cloud_provider));
      persist_appended_tool_turn(params, hist_before, reasoning_json, iteration, results);
      free(reasoning_json);
      reasoning_json = NULL;

      /* Step 8b: All-silent check — tools handled their own output */
      if (followup.all_silent) {
         OLOG_INFO("Tool loop: All tools silent (should_respond=false), skipping follow-up");
         const int note_at = json_object_array_length(params->conversation_history);
         append_closing_message(params->conversation_history,
                                "[Tool execution completed without follow-up response]",
                                params->history_format);
         persist_appended_tool_turn(params, note_at, NULL, iteration, NULL);
         free_tool_result_resources(results);
         free(results);
         llm_tool_response_free(&result);
         return strdup(""); /* Empty = no TTS output */
      }

      /* Step 9: Check iteration limit.  A save turn ends here, its findings saved;
       * an answer turn gets a final text response with what it has. */
      if (save_step) {
         OLOG_INFO("Tool loop: save step done at iteration %d; turn ends without a reply",
                   iteration);
         free_tool_result_resources(results);
         free(results);
         llm_tool_response_free(&result);
         return strdup("");
      }
      if (last) {
         OLOG_WARNING("Tool loop: Max iterations (%d) reached, forcing text response",
                      LLM_TOOLS_MAX_ITERATIONS);

         free_tool_result_resources(results);
         free(results);
         llm_tool_response_free(&result);
         /* Plain wording, as the duplicate-call note: a "[System:]" prefix reads
          * to a reasoning model as an injected directive. */
         return final_answer_without_tools(params, TOOL_LOOP_DIRECTIVE_ITERATION_CAP, iteration);
      }

      /* Step 11: Check interrupt.  Background turns (job / research) break only
       * on their own session cancel flag; foreground turns also honor the global
       * wake-word / Ctrl+C interrupt.  See llm_interrupt_ctx_t. */
      if (llm_interrupt_ctx_triggered(&ictx)) {
         OLOG_INFO("Tool loop: Interrupted by user");
         free_tool_result_resources(results);
         free(results);
         llm_tool_response_free(&result);
         return NULL;
      }

      /* Step 12: Handle provider switching (switch_llm tool) */
      if (!params->pinned) {
         resolve_provider_switch(params);
      }

      /* Clear input text for follow-up calls (history already contains the context) */
      params->input_text = "";

      /* Cleanup for next iteration */
      free_tool_result_resources(results);
      free(results);
      llm_tool_response_free(&result);
   }

   /* Should not reach here (loop exits via returns) */
   OLOG_ERROR("Tool loop: Fell through iteration loop unexpectedly");
   return NULL;
}

char *llm_tool_iteration_loop(llm_tool_loop_params_t *params) {
   /* A side call's own loop (a compaction or summarizer run from inside a turn's
    * iteration) hands the iteration back to the loop it interrupted. */
   const int outer_iteration = llm_cache_monitor_get_iteration();
   char *response = tool_iteration_loop_body(params);
   llm_cache_monitor_set_iteration(outer_iteration);
   return response;
}
