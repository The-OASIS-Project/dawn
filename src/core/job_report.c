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
 * A finished background job reporting back: the no-tools take and the post
 * into the conversation the job came from.
 */

#include "core/job_report.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "core/session_manager.h"
#include "core/text_input_dispatch.h"
#include "dawn_error.h"
#include "llm/llm_cache_monitor.h"
#include "llm/llm_command_parser.h"
#include "logging.h"
#include "utils/string_utils.h"

void job_report_persona(char *out, size_t out_len) {
   llm_persona_effective(out, out_len);
   size_t len = strlen(out);
   while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == ' ')) {
      out[--len] = '\0';
   }
}

char *job_report_take(struct session *s, const char *system_prompt, const char *ask, int user_id) {
   if (s == NULL || system_prompt == NULL || ask == NULL) {
      return NULL;
   }
   session_init_system_prompt(s, system_prompt);
   session_set_tools_suppressed(s, true); /* words only */
   text_input_dispatch_opts_t opts = {
      .conversation_id = 0,
      .auth_user_id = user_id,
      .skip_prompt_rebuild = true,
   };
   /* A side call: recorded, but not counted as a conversation's turn. */
   const int kind_prev = llm_cache_monitor_push_kind(LLM_CALL_SYNTHESIS);
   char *take = core_text_input_dispatch(s, ask, &opts);
   llm_cache_monitor_pop_kind(kind_prev);
   session_set_tools_suppressed(s, false);
   if (take != NULL && (take[0] == '\0' || atomic_load(&s->cancel_requested))) {
      free(take); /* empty, or cut off: the caller's fixed line instead */
      take = NULL;
   }
   if (take != NULL) {
      sanitize_utf8_for_json(take);
   }
   return take;
}

int job_report_post(int64_t parent_conv, int user_id, const char *text) {
   return job_report_post_ex(parent_conv, user_id, text, NULL);
}

int job_report_post_ex(int64_t parent_conv,
                       int user_id,
                       const char *text,
                       conv_post_written_fn on_written) {
   /* DAWN's report on a finished job (a take written in a turn with no tools,
    * or a fixed line), not a turn to replay. */
   return conv_post_unprompted_ex(parent_conv, user_id, text, on_written);
}
