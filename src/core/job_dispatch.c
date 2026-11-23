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
 * Shared background-job dispatch helpers.  See job_dispatch.h.
 */

#include "core/job_dispatch.h"

#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_messages.h"
#include "core/session_manager.h"
#include "llm/llm_interface.h"
#include "llm/llm_selection.h"
#include "logging.h"

void job_selection_of_spawner(session_llm_config_t *out) {
   session_t *ctx = session_get_command_context();
   if (ctx != NULL) {
      session_get_llm_config(ctx, out);
      return;
   }
   llm_get_default_config(out);
   llm_selection_resolve(out);
}

void job_selection_columns(const session_llm_config_t *cfg, conv_llm_columns_t *out) {
   const bool local = (cfg->type == LLM_LOCAL);
   out->llm_type = local ? "local" : "cloud";
   out->cloud_provider = local ? "" : cloud_provider_to_string(cfg->cloud_provider);
   out->model = cfg->model;
   out->thinking_mode = cfg->thinking_mode;
   out->reasoning_effort = cfg->reasoning_effort;
}

int job_selection_load(int64_t conv_id, int user_id, session_llm_config_t *out) {
   conversation_t conv;
   int rc = conv_db_get(conv_id, user_id, &conv);
   if (rc != AUTH_DB_SUCCESS) {
      return rc;
   }
   llm_selection_from_row(conv.llm_type, conv.cloud_provider, conv.model, conv.thinking_mode,
                          conv.reasoning_effort, out);
   conv_free(&conv);
   return AUTH_DB_SUCCESS;
}

job_provider_class_t job_provider_for(const session_llm_config_t *cfg) {
   session_llm_config_t resolved = *cfg;
   llm_selection_resolve(&resolved);
   return (resolved.type == LLM_LOCAL) ? JOB_PROVIDER_LOCAL : JOB_PROVIDER_CLOUD;
}

void job_disposition_signals(session_t *s,
                             bool *user_cancelled,
                             bool *reaped,
                             bool *shutdown_stop) {
   bool uc = false;
   bool rp = s != NULL && job_manager_claim_reaped(s->session_id, &uc);
   if (user_cancelled != NULL) {
      *user_cancelled = uc;
   }
   if (reaped != NULL) {
      *reaped = rp;
   }
   if (shutdown_stop != NULL) {
      *shutdown_stop = !rp && !uc && job_manager_is_shutting_down();
   }
}

const char *job_spawn_origin_string(void) {
   const session_t *ctx = session_get_command_context();
   if (ctx == NULL) {
      return "job";
   }
   switch (ctx->type) {
      case SESSION_TYPE_LOCAL:
         return "voice";
      case SESSION_TYPE_WEBUI:
         return "webui";
      case SESSION_TYPE_MESSAGING:
         return "messaging";
      case SESSION_TYPE_DAP:
      case SESSION_TYPE_DAP2:
         return "satellite"; /* voice, but not the local speaker — notify-only in v1 */
      default:
         return "job"; /* no session, or a job spawning a job (blocked in P0) */
   }
}

int64_t job_dispatch_tool_persist_cb(void *userdata, const session_tool_row_t *row) {
   job_persist_ctx_t *ctx = (job_persist_ctx_t *)userdata;
   if (ctx == NULL || row == NULL || row->role == NULL) {
      return 0;
   }
   const conv_message_row_t db_row = { .role = row->role,
                                       .content = row->content ? row->content : "",
                                       .tool_calls = row->tool_calls,
                                       .tool_call_id = row->tool_call_id,
                                       .reasoning = row->reasoning,
                                       .llm_blocks = row->llm_blocks,
                                       .kind = row->kind,
                                       .is_error = row->is_error,
                                       .images = row->images };
   int64_t id = 0;
   if (conv_db_add_row(ctx->conv_id, ctx->user_id, &db_row, &id) != AUTH_DB_SUCCESS) {
      OLOG_WARNING("job_dispatch: failed to persist tool-turn %s row to conv %lld", row->role,
                   (long long)ctx->conv_id);
      return 0;
   }
   return id;
}
