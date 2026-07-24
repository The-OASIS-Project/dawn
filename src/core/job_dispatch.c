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
#include "llm/llm_interface.h"
#include "logging.h"

job_provider_class_t job_provider_from_default(void) {
   session_llm_config_t defcfg;
   memset(&defcfg, 0, sizeof(defcfg));
   llm_get_default_config(&defcfg);
   return (defcfg.type == LLM_LOCAL) ? JOB_PROVIDER_LOCAL : JOB_PROVIDER_CLOUD;
}

void job_dispatch_tool_persist_cb(void *userdata,
                                  const char *role,
                                  const char *content,
                                  const char *tool_calls_json,
                                  const char *tool_call_id,
                                  const char *reasoning_json) {
   job_persist_ctx_t *ctx = (job_persist_ctx_t *)userdata;
   if (ctx == NULL || role == NULL) {
      return;
   }
   if (conv_db_add_message_with_tools(ctx->conv_id, ctx->user_id, role, content ? content : "",
                                      tool_calls_json, tool_call_id, reasoning_json,
                                      NULL) != AUTH_DB_SUCCESS) {
      OLOG_WARNING("job_dispatch: failed to persist tool-turn %s row to conv %lld", role,
                   (long long)ctx->conv_id);
   }
}
