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
 * WebUI text-input processing — async worker thread.
 *
 * Owns `webui_process_text_input_with_vision` and `webui_process_text_input`,
 * the two public entry points used by the JSON-message dispatcher when the
 * client sends text (with or without attached images), plus the
 * `text_worker_thread` that actually drives the session through the LLM
 * call on a detached thread.  Split out of webui_server.c so that file can
 * stay under the size limits in CLAUDE.md.
 *
 * The moved block uses only the public session_t API, public webui_*
 * helpers (queue_response, webui_sentence_audio_callback, etc., declared in
 * webui_internal.h), and public llm_/conv_db_ interfaces — no file-static
 * state from webui_server.c is touched here.
 */

#include <json-c/json.h>
#include <mosquitto.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/command_router.h"
#include "core/ocp_helpers.h"
#include "core/session_manager.h"
#include "core/text_input_dispatch.h"
#include "core/worker_pool.h"
#include "dawn.h"
#include "llm/llm_context.h"
#include "logging.h"
#include "webui/webui_internal.h"
#include "webui/webui_server.h"

/* =============================================================================
 * Text Processing (Async Worker Thread)
 *
 * For Phase 2, we use a simple detached thread for text processing.
 * Phase 4 may integrate with the worker pool for audio + text.
 * ============================================================================= */

/* Command processing constants */
#define MAX_TOOL_RESULTS 8
#define TOOL_RESULT_MSG_SIZE 1024
#define WEBUI_WORKER_ID 100 /* Virtual worker ID for command router */

typedef struct {
   session_t *session;
   char *text;
   unsigned int request_gen; /* Captured request_generation to detect superseded requests */
   bool input_was_voice;     /* True = this turn's input was ASR-transcribed (voice); applied
                              * to session->input_was_voice on the worker thread right before
                              * dispatch so the prompt builder gates the ASR hint per turn. */
   /* Vision support fields - supports multiple images per message */
   char *vision_images[WEBUI_MAX_VISION_IMAGES_CAP];       /* Base64 encoded images */
   size_t vision_image_sizes[WEBUI_MAX_VISION_IMAGES_CAP]; /* Size of each image */
   char vision_mimes[WEBUI_MAX_VISION_IMAGES_CAP][WEBUI_VISION_MIME_MAX]; /* MIME types */
   int vision_image_count;                                                /* Number of images */
} text_work_t;

/**
 * @brief Process commands in LLM response and make follow-up calls
 *
 * Searches for <command> tags in the response, executes them via MQTT,
 * and makes follow-up LLM calls with the results.
 *
 * @param llm_response The LLM response to process
 * @param session Session for follow-up LLM calls
 * @return Final response text (caller must free), or NULL if no commands
 */
char *webui_process_commands(const char *llm_response, session_t *session) {
   if (!llm_response || !session) {
      return NULL;
   }

   /* Bail early if satellite/client already disconnected — avoid wasting LLM API calls */
   if (session->disconnected) {
      return NULL;
   }

   struct mosquitto *mosq = worker_pool_get_mosq();
   if (!mosq) {
      OLOG_WARNING("WebUI: No MQTT connection, cannot process commands");
      return NULL;
   }

   /* Collect all tool results */
   char *tool_results[MAX_TOOL_RESULTS] = { 0 };
   int num_results = 0;

   /* Search for <command> tags and process each one */
   const char *search_ptr = llm_response;
   const char *cmd_start;

   while ((cmd_start = strstr(search_ptr, "<command>")) != NULL && num_results < MAX_TOOL_RESULTS) {
      const char *cmd_end = strstr(cmd_start, "</command>");
      if (!cmd_end) {
         OLOG_WARNING("WebUI: Unclosed <command> tag");
         break;
      }

      /* Extract command JSON */
      const char *json_start = cmd_start + strlen("<command>");
      size_t json_len = cmd_end - json_start;

      char *cmd_json = malloc(json_len + 1);
      if (!cmd_json) {
         OLOG_ERROR("WebUI: Failed to allocate command JSON");
         break;
      }
      memcpy(cmd_json, json_start, json_len);
      cmd_json[json_len] = '\0';

      /* Send command to WebUI debug panel (wraps in tags for JS extraction)
       * Only send if we had streamed content - otherwise the "didn't stream" fallback
       * in session_manager.c already sends the full response with command tags */
      if (session->stream_had_content) {
         char *debug_cmd = malloc(json_len + 32);
         if (debug_cmd) {
            snprintf(debug_cmd, json_len + 32, "<command>%s</command>", cmd_json);
            webui_send_transcript(session, "assistant", debug_cmd);
            free(debug_cmd);
         }
      }

      OLOG_INFO("WebUI: Processing command: %s", cmd_json);

      /* Parse JSON to extract device/action */
      struct json_object *parsed_json = json_tokener_parse(cmd_json);
      if (!parsed_json) {
         OLOG_WARNING("WebUI: Invalid command JSON: %s", cmd_json);
         free(cmd_json);
         search_ptr = cmd_end + strlen("</command>");
         continue;
      }

      /* Get device and action - both required for valid command */
      struct json_object *device_obj = NULL;
      struct json_object *action_obj = NULL;

      if (!json_object_object_get_ex(parsed_json, "device", &device_obj) || !device_obj) {
         OLOG_WARNING("WebUI: Skipping malformed command - missing 'device' field: %s", cmd_json);
         json_object_put(parsed_json);
         free(cmd_json);
         search_ptr = cmd_end + strlen("</command>");
         continue;
      }

      const char *device_name = json_object_get_string(device_obj);
      if (!device_name || device_name[0] == '\0') {
         OLOG_WARNING("WebUI: Skipping malformed command - empty 'device' field: %s", cmd_json);
         json_object_put(parsed_json);
         free(cmd_json);
         search_ptr = cmd_end + strlen("</command>");
         continue;
      }

      /* Action is optional for some commands (e.g., triggers), default to "unknown" */
      const char *action_name = "unknown";
      if (json_object_object_get_ex(parsed_json, "action", &action_obj) && action_obj) {
         const char *action_str = json_object_get_string(action_obj);
         if (action_str && action_str[0] != '\0') {
            action_name = action_str;
         }
      }

      /* Register pending request */
      pending_request_t *req = command_router_register(WEBUI_WORKER_ID);
      if (!req) {
         OLOG_ERROR("WebUI: Failed to register pending request");
         json_object_put(parsed_json);
         free(cmd_json);
         search_ptr = cmd_end + strlen("</command>");
         continue;
      }

      const char *request_id = command_router_get_id(req);
      OLOG_INFO("WebUI: Registered request %s", request_id);

      /* Add request_id, session_id, and timestamp to command JSON (OCP v1.1) */
      json_object_object_add(parsed_json, "request_id", json_object_new_string(request_id));
      json_object_object_add(parsed_json, "session_id",
                             json_object_new_int((int32_t)session->session_id));
      json_object_object_add(parsed_json, "timestamp",
                             json_object_new_int64(ocp_get_timestamp_ms()));
      const char *cmd_with_id = json_object_to_json_string(parsed_json);

      /* Send tool call status to UI (works for both WebUI and satellites) */
      char tool_detail[128];
      snprintf(tool_detail, sizeof(tool_detail), "Calling %s...", device_name);
      webui_send_state_with_detail(session, "tool_call", tool_detail);

      /* Publish command via MQTT */
      int rc = mosquitto_publish(mosq, NULL, APPLICATION_NAME, strlen(cmd_with_id), cmd_with_id, 0,
                                 false);
      if (rc != MOSQ_ERR_SUCCESS) {
         OLOG_ERROR("WebUI: MQTT publish failed: %d", rc);
         command_router_cancel(req);
         json_object_put(parsed_json);
         free(cmd_json);
         search_ptr = cmd_end + strlen("</command>");
         continue;
      }
      OLOG_INFO("WebUI: Published command to %s", APPLICATION_NAME);

      /* Wait for result */
      char *callback_result = command_router_wait(req, COMMAND_RESULT_TIMEOUT_MS);

      /* Format result for LLM */
      tool_results[num_results] = malloc(TOOL_RESULT_MSG_SIZE);
      if (tool_results[num_results]) {
         if (callback_result && strlen(callback_result) > 0) {
            OLOG_INFO("WebUI: Received callback result: %.50s%s", callback_result,
                      strlen(callback_result) > 50 ? "..." : "");
            snprintf(tool_results[num_results], TOOL_RESULT_MSG_SIZE,
                     "[Tool Result: %s.%s returned: %s]", device_name, action_name,
                     callback_result);
         } else {
            OLOG_WARNING("WebUI: No callback result (timeout or empty)");
            snprintf(tool_results[num_results], TOOL_RESULT_MSG_SIZE,
                     "[Tool Result: %s.%s completed successfully]", device_name, action_name);
         }

         /* Send tool result to WebUI for debug display */
         webui_send_transcript(session, "assistant", tool_results[num_results]);

         num_results++;
      }

      if (callback_result) {
         free(callback_result);
      }

      json_object_put(parsed_json);
      free(cmd_json);
      search_ptr = cmd_end + strlen("</command>");
   }

   /* If no results collected, return NULL (no commands processed) */
   if (num_results == 0) {
      return NULL;
   }

   /* Build combined tool results message for LLM */
   size_t total_len = 1; /* For null terminator */
   for (int i = 0; i < num_results; i++) {
      if (tool_results[i]) {
         total_len += strlen(tool_results[i]) + 1; /* +1 for newline */
      }
   }

   char *combined_results = malloc(total_len);
   if (!combined_results) {
      OLOG_ERROR("WebUI: Failed to allocate combined results");
      for (int i = 0; i < num_results; i++) {
         free(tool_results[i]);
      }
      return NULL;
   }

   char *ptr = combined_results;
   for (int i = 0; i < num_results; i++) {
      if (tool_results[i]) {
         size_t len = strlen(tool_results[i]);
         memcpy(ptr, tool_results[i], len);
         ptr += len;
         if (i < num_results - 1) {
            *ptr++ = '\n';
         }
         free(tool_results[i]);
      }
   }
   *ptr = '\0';

   OLOG_INFO("WebUI: Sending tool results to LLM: %s", combined_results);

   /* Make follow-up LLM call with tool results */
   char *final_response = session_llm_call(session, combined_results);

   free(combined_results);

   if (!final_response) {
      OLOG_ERROR("WebUI: Follow-up LLM call failed");
      return NULL;
   }

   OLOG_INFO("WebUI: LLM final response: %.50s%s", final_response,
             strlen(final_response) > 50 ? "..." : "");

   return final_response;
}

/* strip_command_tags() is shared across webui modules — see webui_satellite.c */

/* REQUEST_SUPERSEDED macro now defined in webui_internal.h */

/* Context + callback for the LLM tool loop's structured tool-turn persistence (E2).
 * The daemon owns the structured tool data (the browser only saves the final visible
 * assistant text); this writes the assistant tool_calls + role:tool rows to conv_db.
 *
 * Holds the retained `session` (NOT a raw `conn`): the hook fires seconds into the turn
 * when tools resolve, and a client disconnect on the lws thread frees `conn` after
 * LWS_CALLBACK_CLOSED — so caching `conn` here would be a use-after-free.  The worker
 * already holds a session reference for the whole turn, so the session is lifetime-safe.
 * The conv_id is read live via webui_get_active_conversation_id() (which goes through
 * session->client_data, NULLed on disconnect) because a brand-new chat's conversation is
 * created a moment AFTER the turn starts (the browser's new_conversation message), still
 * well before the LLM emits tool calls; a setup-time snapshot would be 0.  auth_user_id is
 * stable for the connection, so it is snapshotted at setup. */
typedef struct {
   session_t *session;
   int auth_user_id;
} webui_tool_persist_ctx_t;

static void webui_tool_persist_cb(void *userdata,
                                  const char *role,
                                  const char *content,
                                  const char *tool_calls_json,
                                  const char *tool_call_id,
                                  const char *reasoning_json) {
   webui_tool_persist_ctx_t *ctx = (webui_tool_persist_ctx_t *)userdata;
   if (!ctx || !ctx->session || !role) {
      return;
   }
   /* Persist to the conversation THIS TURN belongs to (captured at dispatch /
    * back-filled at conversation creation), NOT the live view — otherwise
    * switching conversations mid-tool-loop writes this turn's iteration rows into
    * whatever conversation is on screen.  No live-view fallback: an untagged turn
    * (conv_id 0) is skipped rather than mis-attributed. */
   int64_t conv_id = ctx->session->stream_conversation_id;
   if (conv_id <= 0) {
      return; /* turn not tagged with a conversation — skip rather than guess */
   }
   if (conv_db_add_message_with_tools(conv_id, ctx->auth_user_id, role, content ? content : "",
                                      tool_calls_json, tool_call_id, reasoning_json,
                                      NULL) != AUTH_DB_SUCCESS) {
      OLOG_WARNING("WebUI: failed to persist tool-turn %s row to conv %lld (may orphan on reload)",
                   role, (long long)conv_id);
   }
}

/* Iteration-boundary hook: close the current streaming bubble (if one is open) so this
 * iteration's tool entries render after its text and the next iteration opens a fresh
 * bubble.  reason="tool_iteration" tells the browser to seal the bubble WITHOUT going
 * idle or saving it (only the final stream_end does that).  Must stay in sync with the
 * matching reason string in www/js/ui/streaming.js. */
static void webui_tool_iteration_cb(session_t *session, void *userdata) {
   (void)userdata;
   if (session && atomic_load(&session->llm_streaming_active)) {
      webui_send_stream_end(session, "tool_iteration");
   }
}

/* Callback fired by core_text_input_dispatch after the user message is
 * added to history + (optionally) persisted to conv_db, but before
 * focus injection and the LLM call.  Preserves the WebUI's "transcript
 * echoes immediately after the user types" UX while keeping the
 * add/persist/focus/LLM sequence inside the Layer 2 helper. */
static void webui_text_dispatch_on_user_msg(void *ctx, const char *text, bool persisted_to_db) {
   session_t *session = (session_t *)ctx;
   if (!session || !text) {
      return;
   }
   webui_send_transcript_ex(session, "user", text, persisted_to_db);
}

static void text_worker_cleanup(text_work_t *work, session_t *session, char *text) {
   if (session) {
      session_release(session);
   }
   if (text) {
      free(text);
   }
   if (work) {
      /* Free all vision images if present */
      for (int i = 0; i < work->vision_image_count; i++) {
         if (work->vision_images[i]) {
            free(work->vision_images[i]);
            work->vision_images[i] = NULL;
         }
      }
      work->vision_image_count = 0;
      free(work);
   }
}

static void *text_worker_thread(void *arg) {
   text_work_t *work = (text_work_t *)arg;
   session_t *session = work->session;
   char *text = work->text;
   unsigned int expected_gen = work->request_gen;

   /* Check if session is still valid or if this request was superseded */
   if (!session || REQUEST_SUPERSEDED(session, expected_gen)) {
      OLOG_INFO("WebUI: Session disconnected or request superseded, aborting text processing");
      text_worker_cleanup(work, session, text);
      return NULL;
   }

   if (work->vision_image_count > 0) {
      size_t total_bytes = 0;
      for (int i = 0; i < work->vision_image_count; i++) {
         total_bytes += work->vision_image_sizes[i];
      }
      OLOG_INFO("WebUI: Processing text+vision for session %u: %s (%d images, %zu total bytes)",
                session->session_id, text, work->vision_image_count, total_bytes);
   } else {
      OLOG_INFO("WebUI: Processing text input for session %u: %s", session->session_id, text);
   }

   /* Clear stale pending_visual from a previous turn that may not have
    * been consumed (e.g., LLM response interrupted or error). Prevents
    * visuals from attaching to the wrong assistant message. */
   pthread_mutex_lock(&session->tools_mutex);
   free(session->pending_visual);
   session->pending_visual = NULL;
   pthread_mutex_unlock(&session->tools_mutex);

   /* Send "thinking" state */
   if (work->vision_image_count > 0) {
      if (work->vision_image_count == 1) {
         webui_send_state_with_detail(session, "thinking", "Analyzing image...");
      } else {
         webui_send_state_with_detail(session, "thinking", "Analyzing images...");
      }
   } else {
      webui_send_state_with_detail(session, "thinking", "Processing request...");
   }

   ws_connection_t *conn = (ws_connection_t *)session->client_data;
   bool tts_enabled = conn && conn->tts_enabled;

   /* Stamp this turn's input modality (voice vs typed) onto the session right
    * before dispatch, on THIS worker thread — same pattern as tts_enabled above.
    * Setting it at the entry point (LWS/always-on thread) instead left a window
    * where a concurrent always-on voice turn could clobber a typed turn's reset;
    * stamping it here, adjacent to the synchronous build, closes that window. The
    * prompt builder reads session->input_was_voice to gate the ASR hint. */
   session->input_was_voice = work->input_was_voice;

   /* Dispatch the turn through the provider-agnostic Layer 2 helper:
    * add user msg → persist to conv_db → transcript echo (via callback)
    * → focus injection → LLM call.  Vision buffers are owned by `work`
    * and freed below after the LLM call returns. */
   /* Use the turn's CAPTURED conversation (set at dispatch, back-filled at
    * creation) rather than a live re-read of conn->active_conversation_id, which
    * this worker could observe AFTER a mid-turn view switch — that would persist
    * the user message / inject focus for the wrong conversation. */
   int64_t turn_conv = session->stream_conversation_id;
   if (turn_conv <= 0 && conn && conn->active_conversation_id > 0) {
      turn_conv = conn->active_conversation_id;
   }
   text_input_dispatch_opts_t dispatch_opts = {
      .conversation_id = turn_conv,
      .auth_user_id = conn ? conn->auth_user_id : 0,
      .sentence_cb = tts_enabled ? webui_sentence_audio_callback : NULL,
      .sentence_userdata = tts_enabled ? session : NULL,
      .on_user_msg_added = webui_text_dispatch_on_user_msg,
      .user_msg_added_ctx = session,
   };

   /* Install the tool-turn persist hook for the duration of the (synchronous) LLM
    * call so the tool loop can durably persist structured assistant tool_calls +
    * role:tool rows.  persist_ctx is stack-scoped and the hook fires on THIS thread
    * during core_text_input_dispatch, so it's valid throughout; cleared right after.
    * Installed whenever we have a connection — the callback reads the conversation id
    * live (it may still be 0 here for a brand-new chat, but is set before tools fire). */
   webui_tool_persist_ctx_t persist_ctx = { .session = session,
                                            .auth_user_id = conn ? conn->auth_user_id : 0 };
   if (conn) {
      session_set_tool_persist_hook(session, webui_tool_persist_cb, &persist_ctx);
      /* Live-transcript reorder: segment the streaming bubble per tool-loop iteration.
       * Stateless (acts on the session), so no userdata needed. */
      session_set_tool_iteration_hook(session, webui_tool_iteration_cb, NULL);
   }

   char *response = core_text_input_dispatch(
       session, text, (const char **)work->vision_images, work->vision_image_sizes,
       (const char(*)[WEBUI_VISION_MIME_MAX])work->vision_mimes, work->vision_image_count,
       &dispatch_opts);

   session_set_tool_persist_hook(session, NULL, NULL); /* persist_ctx goes out of scope below */
   session_set_tool_iteration_hook(session, NULL, NULL);

   /* Free vision data after LLM call (it's been sent over HTTP, no longer needed) */
   for (int i = 0; i < work->vision_image_count; i++) {
      if (work->vision_images[i]) {
         free(work->vision_images[i]);
         work->vision_images[i] = NULL;
      }
   }
   work->vision_image_count = 0;

   /* Check if request was superseded during LLM call */
   if (REQUEST_SUPERSEDED(session, expected_gen)) {
      /* Request superseded - don't try to send response */
      OLOG_INFO("WebUI: Session %u request superseded during LLM call", session->session_id);
      session_release(session);
      free(response);
      free(text);
      free(work);
      return NULL;
   }

   if (!response) {
      /* LLM call failed */
      webui_send_error(session, "LLM_ERROR", "Failed to get response from AI");
      webui_send_state(session, "idle");
      session_release(session);
      free(text);
      free(work);
      return NULL;
   }

   /* Check for command tags and process them */
   char *final_response = response;
   if (strstr(response, "<command>")) {
      OLOG_INFO("WebUI: Response contains commands, processing...");

      /* Note: Don't send intermediate response here - streaming already delivered it */

      /* Process commands and get follow-up response */
      char *processed = webui_process_commands(response, session);
      if (processed) {
         /* Check if request was superseded after command processing */
         if (REQUEST_SUPERSEDED(session, expected_gen)) {
            OLOG_INFO("WebUI: Session %u request superseded during command processing",
                      session->session_id);
            session_release(session);
            free(response);
            free(processed);
            free(text);
            free(work);
            return NULL;
         }

         /* Recursively process if the follow-up also contains commands.
          * Limit iterations to prevent infinite loops from confused LLMs. */
         int follow_up_iterations = 0;
         const int MAX_FOLLOW_UP_ITERATIONS = 5;

         while (strstr(processed, "<command>") && !REQUEST_SUPERSEDED(session, expected_gen)) {
            follow_up_iterations++;
            if (follow_up_iterations > MAX_FOLLOW_UP_ITERATIONS) {
               OLOG_WARNING("WebUI: Command loop limit reached (%d iterations), breaking",
                            MAX_FOLLOW_UP_ITERATIONS);
               break;
            }

            OLOG_INFO(
                "WebUI: Follow-up response contains more commands, processing... (iter %d/%d)",
                follow_up_iterations, MAX_FOLLOW_UP_ITERATIONS);
            /* Note: Don't send transcript - streaming already delivered it */

            char *next_processed = webui_process_commands(processed, session);
            free(processed);
            if (!next_processed) {
               processed = NULL;
               break;
            }
            processed = next_processed;
         }

         if (processed) {
            free(response);
            final_response = processed;

            /* Generate TTS for the command result (follow-up response) */
            if (tts_enabled && strlen(processed) > 0) {
               OLOG_INFO("WebUI: Generating TTS for command result: %.60s%s", processed,
                         strlen(processed) > 60 ? "..." : "");
               webui_sentence_audio_callback(processed, session);
            }
         } else {
            /* Command processing failed, use original response */
            final_response = response;
         }
      }
   }

   /* Strip any remaining command tags from final response */
   strip_command_tags(final_response);

   /* Send audio end marker if TTS was enabled */
   if (tts_enabled) {
      bool use_opus = conn && conn->use_opus;
      webui_send_audio_end(session, use_opus);
   }

   /* Note: Don't send transcript here - streaming already delivered the content.
    * The LLM call uses webui_send_stream_start/delta/end for real-time delivery.
    * Assistant response is already added to history inside the LLM call. */

   /* Persist the final assistant answer for a BACKGROUNDED turn — one that
    * finished while the client was viewing a DIFFERENT conversation.  The final
    * answer is normally saved by the client (finalizeStream), but the client only
    * saves the conversation it is looking at, so a turn that completes in the
    * background would otherwise lose its answer on reload.  A foreground turn
    * (turn conv == the on-screen conv) is saved by the client — skip it here to
    * avoid a duplicate row.  Tool-turn rows are already persisted server-side by
    * webui_tool_persist_cb to the same (captured) conversation. */
   if (conn && final_response && final_response[0] != '\0') {
      int64_t turn_conv = session->stream_conversation_id;
      if (turn_conv > 0 && turn_conv != webui_get_active_conversation_id(session)) {
         if (conv_db_add_message_with_tools(turn_conv, conn->auth_user_id, "assistant",
                                            final_response, NULL, NULL, NULL,
                                            NULL) != AUTH_DB_SUCCESS) {
            OLOG_WARNING("WebUI: failed to persist backgrounded assistant answer to conv %lld",
                         (long long)turn_conv);
         } else {
            OLOG_INFO("WebUI: persisted backgrounded assistant answer to conv %lld",
                      (long long)turn_conv);
         }
      }
   }

   /* Free the final response (either original response or processed copy) */
   free(final_response);

   /* Send context usage update to WebUI */
   {
      int current_tokens, max_tokens;
      float threshold;
      llm_context_get_last_usage(&current_tokens, &max_tokens, &threshold);
      if (max_tokens > 0) {
         webui_send_context(session, current_tokens, max_tokens, threshold);
      }
   }

   /* Return to idle state */
   webui_send_state(session, "idle");

   /* Mark interaction complete for conversation idle timeout tracking */
   session_update_interaction_complete(session);

   /* Release session reference (acquired in webui_process_text_input) */
   session_release(session);

   free(text);
   free(work);
   return NULL;
}

/**
 * @brief Process text input with optional vision images (internal)
 *
 * @param session Session context
 * @param text User text input
 * @param vision_images Array of base64 encoded image data (NULL for text-only)
 * @param vision_image_sizes Array of image sizes
 * @param vision_mimes Array of MIME type strings
 * @param vision_image_count Number of images (0 for text-only)
 * @return 0 on success, non-zero on failure
 */
int webui_process_text_input_with_vision(session_t *session,
                                         const char *text,
                                         const char **vision_images,
                                         const size_t *vision_image_sizes,
                                         const char **vision_mimes,
                                         int vision_image_count,
                                         bool input_was_voice) {
   if (!session || !text || strlen(text) == 0) {
      return 1;
   }

   /* Validate image count */
   const int max_vision_images = g_config.vision.max_images;
   if (vision_image_count > max_vision_images) {
      OLOG_WARNING("WebUI: Too many images (%d), limiting to %d", vision_image_count,
                   max_vision_images);
      vision_image_count = max_vision_images;
   }

   /* Increment request generation FIRST to invalidate any pending requests,
    * then reset disconnected flag. This prevents race conditions where an
    * old worker sees disconnected=false after a new message is sent. */
   unsigned int new_gen = atomic_fetch_add(&session->request_generation, 1) + 1;
   session->disconnected = false;

   /* Bind this turn to the conversation being viewed NOW (dispatch time), before
    * any thinking/stream frame — so every frame of the turn stays tagged with it
    * even if the client switches views mid-turn (background-jobs delta routing).
    * Captured here (not at stream_start) so THINKING frames, which precede the
    * answer stream, carry the correct conversation too. */
   session->stream_conversation_id = webui_get_active_conversation_id(session);

   /* Create work item */
   text_work_t *work = calloc(1, sizeof(text_work_t));
   if (!work) {
      OLOG_ERROR("WebUI: Failed to allocate text work item");
      return 1;
   }

   work->session = session;
   work->text = strdup(text);
   work->request_gen = new_gen; /* Capture generation for this request */
   work->input_was_voice = input_was_voice;
   if (!work->text) {
      OLOG_ERROR("WebUI: Failed to allocate text copy");
      free(work);
      return 1;
   }

   /* Copy vision images if present */
   if (vision_images && vision_image_count > 0) {
      for (int i = 0; i < vision_image_count; i++) {
         if (!vision_images[i] || vision_image_sizes[i] == 0) {
            continue;
         }

         work->vision_images[work->vision_image_count] = strdup(vision_images[i]);
         if (!work->vision_images[work->vision_image_count]) {
            OLOG_ERROR("WebUI: Failed to allocate vision image %d copy", i);
            /* Free previously allocated images */
            for (int j = 0; j < work->vision_image_count; j++) {
               free(work->vision_images[j]);
            }
            free(work->text);
            free(work);
            return 1;
         }
         work->vision_image_sizes[work->vision_image_count] = vision_image_sizes[i];
         if (vision_mimes && vision_mimes[i]) {
            strncpy(work->vision_mimes[work->vision_image_count], vision_mimes[i],
                    WEBUI_VISION_MIME_MAX - 1);
            work->vision_mimes[work->vision_image_count][WEBUI_VISION_MIME_MAX - 1] = '\0';
         }
         work->vision_image_count++;
      }
   }

   /* Retain session for worker thread (worker will release when done) */
   session_retain(session);

   /* Spawn detached worker thread */
   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

   int ret = pthread_create(&thread, &attr, text_worker_thread, work);
   pthread_attr_destroy(&attr);

   if (ret != 0) {
      OLOG_ERROR("WebUI: Failed to create text worker thread");
      session_release(session);
      free(work->text);
      for (int i = 0; i < work->vision_image_count; i++) {
         free(work->vision_images[i]);
      }
      free(work);
      return 1;
   }

   return 0;
}

int webui_process_text_input(session_t *session, const char *text, bool input_was_voice) {
   return webui_process_text_input_with_vision(session, text, NULL, NULL, NULL, 0, input_was_voice);
}
