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
 * The tool-call gate for tests that link llm_tools.c without the policy:
 * every call is allowed, as the action named (or "get").
 */

#include <stdio.h>
#include <string.h>

#include "core/tool_call_challenge.h"
#include "core/tool_call_policy.h"
#include "llm/llm_tools_internal.h"

#ifdef ENABLE_MULTI_CLIENT
tool_caller_t tool_call_policy_caller(void) {
   return TOOL_CALLER_USER;
}
#endif

const char *tool_call_policy_caller_name(tool_caller_t caller) {
   (void)caller;
   return "user";
}

tool_call_decision_t tool_call_policy_check(const tool_metadata_t *meta,
                                            const char *device,
                                            const char *named_action,
                                            const char *value,
                                            tool_caller_t caller,
                                            bool redeemed,
                                            tool_call_verdict_t *out) {
   (void)redeemed;
   (void)meta;
   (void)device;
   (void)value;
   memset(out, 0, sizeof(*out));
   out->caller = caller;
   out->kind = TOOL_KIND_READ;
   snprintf(out->action, sizeof(out->action), "%s",
            (named_action && named_action[0]) ? named_action : "get");
   return TOOL_CALL_ALLOW;
}

tool_call_scope_t tool_call_policy_enter(tool_action_kind_t kind,
                                         tool_caller_t caller,
                                         bool redeemed) {
   (void)kind;
   (void)caller;
   (void)redeemed;
   const tool_call_scope_t none = { 0 };
   return none;
}

void tool_call_policy_leave(tool_call_scope_t previous) {
   (void)previous;
}

tool_challenge_rc_t tool_call_challenge_create(const tool_challenge_t *c) {
   (void)c;
   return TOOL_CHALLENGE_FULL;
}

bool tool_call_challenge_live(int64_t channel_id) {
   (void)channel_id;
   return false;
}

bool llm_tools_drop_earlier_code(void) {
   return false;
}

void llm_tools_note_text_preview(const tool_metadata_t *meta,
                                 const char *action,
                                 bool dropped_earlier,
                                 tool_result_t *result) {
   (void)meta;
   (void)action;
   (void)dropped_earlier;
   (void)result;
}

int llm_tools_approved_binding(const tool_call_t *call,
                               const tool_metadata_t *meta,
                               const tool_call_verdict_t *verdict,
                               const char *device,
                               const char *value_buf,
                               char out[LLM_TOOLS_BINDING_HEX],
                               char *why,
                               size_t why_len) {
   (void)why;
   (void)why_len;
   (void)call;
   (void)meta;
   (void)verdict;
   (void)device;
   (void)value_buf;
   out[0] = '\0';
   return 1;
}

int llm_tools_hold_for_reply_code(const tool_call_t *call,
                                  const tool_metadata_t *meta,
                                  const tool_call_verdict_t *verdict,
                                  const char *device,
                                  const char *value_buf,
                                  tool_result_t *result) {
   (void)call;
   (void)meta;
   (void)verdict;
   (void)device;
   (void)value_buf;
   (void)result;
   return 1;
}
