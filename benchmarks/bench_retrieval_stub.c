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
 * Stub providing globals for bench_retrieval standalone binary.
 * Allows document_db.c and embedding_engine.c to link without the
 * full daemon. Config fields are populated by main() from CLI args.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"

/* g_config and g_secrets are now defined by src/config/config_defaults.c
 * (linked into the bench for memory-pipeline mode). Pre-memory-pipeline the
 * bench used its own copies here.
 *
 * s_db is now defined by src/auth/auth_db_core.c. */

/* =============================================================================
 * Daemon-only stubs for memory-pipeline mode
 *
 * These symbols are referenced by memory_extraction.c, llm_*.c, and friends
 * when linked into the bench. The bench never drives the codepaths that need
 * real implementations (sessions, WebUI broadcasts, metrics) — these are
 * no-op shims so the link succeeds.
 *
 * If you're adding a new stub here and the function returns a session_t*
 * or buffer pointer, return NULL — callers null-check.
 * ============================================================================= */

/* session_manager.h has multiple inline definitions for these depending on
 * #ifdef gating; bench links against the non-inline-fallback path which
 * leaves the symbols undefined.  Define them as no-ops with sane returns. */
#include "core/session_manager.h"

session_t *session_get_command_context(void) {
   return NULL;
}
void session_set_command_context(session_t *session) {
   (void)session;
}
void session_get_llm_config(session_t *session, session_llm_config_t *config) {
   (void)session;
   if (config)
      memset(config, 0, sizeof(*config));
}

/* WebUI broadcasts — no-op when WebUI isn't built into the bench */
void webui_send_error(session_t *s, const char *code, const char *message) {
   (void)s;
   (void)code;
   (void)message;
}
void webui_send_thinking_start(session_t *s, const char *provider) {
   (void)s;
   (void)provider;
}
void webui_send_thinking_delta(session_t *s, const char *text) {
   (void)s;
   (void)text;
}
void webui_send_thinking_end(session_t *s, bool has_content) {
   (void)s;
   (void)has_content;
}
void webui_send_reasoning_summary(session_t *s, int reasoning_tokens) {
   (void)s;
   (void)reasoning_tokens;
}
void webui_broadcast_memory_notice(int user_id, const char *level, const char *message) {
   (void)user_id;
   (void)level;
   (void)message;
}
/* Added 2026-05-13 to unblock bench_retrieval after Phase 2 entity-merge
 * (commit 9bfda37) added these symbols to memory_extraction.c without
 * updating the bench stub list.  Pre-existing link breakage flagged by
 * the coding-review pass following the reranker workstream ship. */
void webui_broadcast_memory_proposals_changed(int user_id) {
   (void)user_id;
}

/* Metrics — recorded by daemon for observability; bench discards */
void metrics_record_llm_query(int duration_ms, bool is_cloud, bool success) {
   (void)duration_ms;
   (void)is_cloud;
   (void)success;
}
void metrics_record_llm_tokens(const char *provider,
                               int input_tokens,
                               int output_tokens,
                               int cached_tokens) {
   (void)provider;
   (void)input_tokens;
   (void)output_tokens;
   (void)cached_tokens;
}
void metrics_record_llm_ttft(int ttft_ms) {
   (void)ttft_ms;
}

/* TTS — bench has no audio output */
int text_to_speech(const char *text) {
   (void)text;
   return 0;
}

/* is_vision_enabled_for_current_llm is provided by linked llm_command_parser.c */

/* config_get / config_get_secrets are provided by config_defaults.c (linked) */

/* =============================================================================
 * Second batch — symbols pulled in by llm_*.c TUs that the bench links but
 * never executes (streaming / tool loop / context compaction paths).
 * ============================================================================= */

/* Session lifecycle — return NULL or no-op since the bench has no live sessions */
session_t *session_get(uint32_t session_id) {
   (void)session_id;
   return NULL;
}
void session_retain(session_t *session) {
   (void)session;
}
void session_release(session_t *session) {
   (void)session;
}
void session_record_query(session_t *session,
                          const char *provider,
                          uint64_t tokens_in,
                          uint64_t tokens_out,
                          uint64_t tokens_cached,
                          double llm_ttft_ms,
                          double llm_total_ms,
                          bool is_error) {
   (void)session;
   (void)provider;
   (void)tokens_in;
   (void)tokens_out;
   (void)tokens_cached;
   (void)llm_ttft_ms;
   (void)llm_total_ms;
   (void)is_error;
}

/* Conversation-event ring + reconnect/metrics accessors gained by
 * llm_tool_loop.c in the background-jobs era.  The bench drives extraction
 * directly (no live session, no conversation stream), so these no-op — but
 * conv_event_emit takes ownership of payload_owned, so free it to stay
 * leak-clean. */
void conv_event_emit(int64_t conv_id, int user_id, const char *kind, char *payload_owned) {
   (void)conv_id;
   (void)user_id;
   (void)kind;
   free(payload_owned);
}
/* Ephemeral tool-step fan (§Phase-3); same ownership contract as conv_event_emit — frees
 * payload_owned on every path.  Added 2026-08-28 to unblock bench_retrieval: fa5e8bc added the
 * call to llm_tool_loop.c without updating this stub (latent — make dawn doesn't relink the
 * bench). */
void conv_event_tool_step_fanout(int64_t conv_id,
                                 int user_id,
                                 uint32_t origin_session_id,
                                 unsigned stream_id,
                                 const char *kind,
                                 char *payload_owned) {
   (void)conv_id;
   (void)user_id;
   (void)origin_session_id;
   (void)stream_id;
   (void)kind;
   free(payload_owned);
}
session_t *session_get_for_reconnect(uint32_t session_id) {
   (void)session_id;
   return NULL;
}
void session_metrics_totals(session_t *session, uint64_t *tokens_in_out, uint32_t *queries_out) {
   (void)session;
   if (tokens_in_out != NULL) {
      *tokens_in_out = 0;
   }
   if (queries_out != NULL) {
      *queries_out = 0;
   }
}

/* Memory-citation signal (Option B): memory_callback.c + recall_format.c record
 * tool-surfaced facts as citeable and gate a cite hint on whether the signal is
 * on.  The bench has no live session/citation stash, so citation is disabled and
 * recording is a no-op. */
bool memory_citation_enabled(void) {
   return false;
}
void memory_citation_record_tool_fact_current(int64_t fact_id) {
   (void)fact_id;
}

/* conversation_events tool-call/result payload builders (background-jobs era).
 * llm_tool_loop.c emits these into conv_event_emit, whose bench stub frees the
 * returned string — so NULL is safe (free(NULL) is a no-op) and the bench never
 * drives the job-event path. */
char *event_payload_tool_call(const char *tool_name,
                              const char *args_json,
                              const char *tool_call_id) {
   (void)tool_name;
   (void)args_json;
   (void)tool_call_id;
   return NULL;
}
char *event_payload_tool_result(const char *tool_name,
                                const char *result_text,
                                const char *tool_call_id) {
   (void)tool_name;
   (void)result_text;
   (void)tool_call_id;
   return NULL;
}

/* Mirror of ws_error_severity_t (webui/webui_server.h) — replicated here rather
 * than including that header, which drags in the libwebsockets-dependent server
 * surface the headless bench can't (and needn't) link. */
typedef enum {
   WS_SEVERITY_ERROR = 0,
   WS_SEVERITY_WARNING,
   WS_SEVERITY_INFO,
} ws_error_severity_t;
void webui_send_error_ex(session_t *session,
                         const char *code,
                         const char *message,
                         ws_error_severity_t severity) {
   (void)session;
   (void)code;
   (void)message;
   (void)severity;
}

/* WebUI server entry points the daemon uses; bench is headless */
void webui_broadcast_conversation_renamed(int user_id, int64_t conv_id, const char *title) {
   (void)user_id;
   (void)conv_id;
   (void)title;
}
/* Signature corrected 2026-05-13 to match canonical declaration in
 * include/webui/webui_server.h:566 — the prior `int user_id` form was
 * an ABI mismatch (UB per C11 §6.5.2.2; worked only by aarch64 ABI
 * coincidence).  Mirrors tests/test_prompt_builder_stub.c. */
int64_t webui_get_active_conversation_id(session_t *s) {
   (void)s;
   return 0;
}
void webui_send_compaction_complete(session_t *s) {
   (void)s;
}
void webui_send_metrics_update(session_t *s) {
   (void)s;
}
void webui_send_session_json(session_t *s, const char *json) {
   (void)s;
   (void)json;
}
void webui_send_state_with_detail(session_t *s, const char *state, const char *detail) {
   (void)s;
   (void)state;
   (void)detail;
}

/* Daemon metrics — recorded for observability, bench discards */
void metrics_log_activity(const char *event, const char *detail) {
   (void)event;
   (void)detail;
}
void metrics_record_fallback(void) {
}
void metrics_record_llm_total_time(int duration_ms, bool is_cloud) {
   (void)duration_ms;
   (void)is_cloud;
}
void metrics_update_llm_config(const char *provider, const char *model) {
   (void)provider;
   (void)model;
}

/* Command execution — only used by tool loop / direct mode; bench never gets there */
void cmd_exec_result_free(void *result) {
   (void)result;
}
char *command_execute(const char *json, int *should_respond) {
   (void)json;
   if (should_respond)
      *should_respond = 0;
   return NULL;
}
char *command_execute_json(const char *json, int *should_respond) {
   (void)json;
   if (should_respond)
      *should_respond = 0;
   return NULL;
}
void command_execute_mqtt_direct(const char *json) {
   (void)json;
}
char *command_execute_sync(const char *json) {
   (void)json;
   return NULL;
}

/* TOML helpers are provided by linked src/tools/toml.c */
/* config_get is provided by linked config_defaults.c */

struct mosquitto;
struct mosquitto *worker_pool_get_mosq(void) {
   return NULL;
}
bool component_status_is_hud_online(void) {
   return false;
}
void session_manager_refresh_all_prompts(void) {
}

/* Tool device-type lookup — registry has empty table in bench */
struct device_type_def;
const struct device_type_def *device_type_get_def(const char *name) {
   (void)name;
   return NULL;
}

/* llm_silent_observe.c brings full SSE + audit-log dependency cone.  The
 * bench never invokes silent observation, but config_validate.c calls
 * llm_silent_observe_provider_is_valid() when validating a freshly-loaded
 * dawn.toml.  Stub returns true so config_load passes; the bench's loaded
 * config never executes silent_observe paths. */
bool llm_silent_observe_provider_is_valid(const char *provider) {
   (void)provider;
   return true;
}

/* =============================================================================
 * Phase 2 entity-merge stubs — added 2026-05-13 to unblock bench_retrieval
 * after the entity-merge ship (commit 9bfda37, 2026-05-12) wired
 * memory_db_entity_consider_auto_merge / _get_user_self_id /
 * _maybe_auto_promote_user_self into memory_extraction.c without
 * updating the bench stub list.  The bench never drives entity merging,
 * so these no-ops return MEMORY_DB_NOT_FOUND / sane defaults that keep
 * memory_extraction.c's branches falling through to the non-merge path.
 *
 * Signatures mirror include/memory/memory_db_aliases.h:304/595/597.
 * ============================================================================= */
#include "memory/memory_db_aliases.h"
#include "memory/memory_types.h"

int memory_db_entity_consider_auto_merge(int user_id,
                                         int64_t entity_id,
                                         memory_alias_evaluate_t *out_eval) {
   (void)user_id;
   (void)entity_id;
   if (out_eval != NULL) {
      memset(out_eval, 0, sizeof(*out_eval));
   }
   return MEMORY_DB_NOT_FOUND;
}

int memory_db_entity_get_user_self_id(int user_id, int64_t *out_id) {
   (void)user_id;
   if (out_id != NULL) {
      *out_id = 0;
   }
   return MEMORY_DB_SUCCESS;
}

int memory_db_entity_maybe_auto_promote_user_self(int user_id,
                                                  int64_t entity_id,
                                                  const char *canonical_name,
                                                  bool *out_promoted) {
   (void)user_id;
   (void)entity_id;
   (void)canonical_name;
   if (out_promoted != NULL) {
      *out_promoted = false;
   }
   return MEMORY_DB_NOT_FOUND;
}

/* memory_extraction.c aliases an abstract "user" entity to the user_self
 * anchor during extraction; the bench has no user_self anchor so this is a
 * no-op, but the symbol must resolve. */
int memory_db_entity_maybe_alias_user_to_self(int user_id,
                                              int64_t entity_id,
                                              const char *canonical_name,
                                              bool *out_aliased) {
   (void)user_id;
   (void)entity_id;
   (void)canonical_name;
   if (out_aliased != NULL) {
      *out_aliased = false;
   }
   return MEMORY_DB_SUCCESS;
}

/* memory_callback.c's `merge_entities` tool action calls this; the bench
 * never reaches that path but the symbol must resolve. */
int memory_db_entity_alias_link(int user_id,
                                int64_t source_id,
                                int64_t target_id,
                                const char *link_kind,
                                const char *reason,
                                float composite_score,
                                const char *evidence_json,
                                int64_t *out_link_id) {
   (void)user_id;
   (void)source_id;
   (void)target_id;
   (void)link_kind;
   (void)reason;
   (void)composite_score;
   (void)evidence_json;
   if (out_link_id != NULL) {
      *out_link_id = 0;
   }
   return MEMORY_DB_NOT_FOUND;
}
