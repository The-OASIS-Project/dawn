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
 * Who may make a tool call (see core/tool_call_policy.h).
 */


#include "core/tool_call_policy.h"

#include <stdio.h>
#include <string.h>

#include "core/session_manager.h"
#include "core/tool_call_challenge.h"
#include "utils/string_utils.h" /* utf8_trim_incomplete */

#define A TOOL_CALL_ALLOW
#define R TOOL_CALL_REFUSE
#define C TOOL_CALL_CHALLENGE

/* [caller][kind]: the header's table.  Kinds in tool_action_kind_t order:
 * act, read, fetch, state, device, prepare. */
static const tool_call_decision_t s_policy[4][6] = {
   /*                          act read fetch state device prepare */
   [TOOL_CALLER_USER] = { A, A, A, A, A, A },
   [TOOL_CALLER_UNVERIFIED] = { C, A, C, A, C, A },
   [TOOL_CALLER_JOB] = { R, A, A, A, R, R },
   [TOOL_CALLER_UNATTENDED] = { R, A, R, A, R, R },
};

#undef A
#undef R
#undef C

/* The call running on this thread, as decided. */
static __thread tool_call_scope_t s_scope;

tool_caller_t tool_call_policy_caller(void) {
   session_t *ctx = session_get_command_context();
   if (!ctx) {
      return TOOL_CALLER_UNATTENDED;
   }
   /* A background turn first: a job's follow-up runs on a job's session when
    * nobody views its conversation, and on the viewer's when someone does;
    * either way it is the same unattended turn. */
   if (session_turn_is_background(ctx)) {
      return TOOL_CALLER_UNATTENDED;
   }
   if (ctx->type == SESSION_TYPE_JOB) {
      return TOOL_CALLER_JOB;
   }
   if (!session_turn_user_originated(ctx)) {
      return TOOL_CALLER_UNATTENDED;
   }
   /* Set when the session is created, before it's published: never changes. */
   return ctx->messaging_identity.sender_unverified ? TOOL_CALLER_UNVERIFIED : TOOL_CALLER_USER;
}

/* Whether this turn already holds an action for its code on the calling
 * session's channel.  (One an earlier turn held is dropped when a new request
 * prepares something: llm_tools_drop_earlier_code.) */
static bool channel_code_waiting(void) {
   session_t *ctx = session_get_command_context();
   return ctx && ctx->messaging_identity.channel_id > 0 &&
          tool_call_challenge_live_in_turn(ctx->messaging_identity.channel_id,
                                           session_turn_token());
}

const char *tool_call_policy_caller_name(tool_caller_t caller) {
   switch (caller) {
      case TOOL_CALLER_USER:
         return "user";
      case TOOL_CALLER_UNVERIFIED:
         return "unverified sender";
      case TOOL_CALLER_JOB:
         return "background job";
      case TOOL_CALLER_UNATTENDED:
         break;
   }
   return "unattended turn";
}

tool_call_decision_t tool_call_policy_decide(tool_caller_t caller, tool_action_kind_t kind) {
   if ((unsigned)caller > (unsigned)TOOL_CALLER_UNATTENDED ||
       (unsigned)kind > (unsigned)TOOL_KIND_PREPARE) {
      return TOOL_CALL_REFUSE;
   }
   return s_policy[caller][kind];
}

/* How much a decision holds back: allow < wait for a code < refuse. */
static int strictness(tool_call_decision_t d) {
   return d == TOOL_CALL_ALLOW ? 0 : d == TOOL_CALL_CHALLENGE ? 1 : 2;
}

/* Why @p caller may not make a call of @p kind, for the model. */
static const char *refusal_text(tool_caller_t caller, tool_action_kind_t kind) {
   switch (caller) {
      case TOOL_CALLER_UNVERIFIED:
         return "Not done: this request came by text message, and a text can claim any "
                "sender, so an action from a text needs the user's reply code, and this one "
                "can't take one (a plan, or a step inside one). Ask for actions one at a "
                "time, or from the app or by voice.";
      case TOOL_CALLER_JOB:
         return "Not done: background work can read and look things up, but not act (send, "
                "save, change, play or start anything). Report what you would do, and the "
                "user can ask for it.";
      case TOOL_CALLER_UNATTENDED:
         return kind == TOOL_KIND_FETCH
                    ? "Not done: this turn wasn't started by the user, so it can't reach out "
                      "to the web. Report what you found so far, and the user can ask for more."
                    : "Not done: this turn wasn't started by the user, so it can't act (send, "
                      "save, change, play or start anything). Report what you would do, and the "
                      "user can ask for it.";
      case TOOL_CALLER_USER:
         break;
   }
   return "Not done.";
}

tool_call_decision_t tool_call_policy_check(const tool_metadata_t *meta,
                                            const char *device,
                                            const char *named_action,
                                            const char *value,
                                            tool_caller_t caller,
                                            bool redeemed,
                                            tool_call_verdict_t *out) {
   memset(out, 0, sizeof(*out));
   out->caller = caller;
   out->kind = TOOL_KIND_ACT;
   if (!meta) {
      out->refusal = "Not done: no such tool.";
      snprintf(out->message, sizeof(out->message), "%s", out->refusal);
      return TOOL_CALL_REFUSE;
   }

   /* The action the call names, in the tool's own spelling. */
   char named[sizeof(out->action)] = "";
   if (named_action && named_action[0] &&
       !tool_action_canonical(meta, named_action, named, sizeof(named))) {
      char actions[160];
      tool_action_list(meta, actions, sizeof(actions));
      out->refusal = "unknown action";
      char shown[49];
      snprintf(shown, sizeof(shown), "%s", named_action);
      utf8_trim_incomplete(shown); /* never half a character into a result */
      snprintf(out->message, sizeof(out->message), "Error: '%s' has no action '%s'%s%s%s",
               meta->name, shown, actions[0] ? " (it has: " : ".", actions, actions[0] ? ")." : "");
      return TOOL_CALL_REFUSE;
   }
   snprintf(out->action, sizeof(out->action), "%s", tool_effective_action(meta, named));
   out->kind = tool_action_kind(meta, device, out->action, value);

   tool_call_decision_t decision = tool_call_policy_decide(caller, out->kind);
   /* Inside another call (a plan's step): no freer than that call's caller. */
   if (s_scope.active && s_scope.caller != caller) {
      const tool_call_decision_t outer = tool_call_policy_decide(s_scope.caller, out->kind);
      if (strictness(outer) > strictness(decision)) {
         decision = outer;
         out->caller = s_scope.caller;
         caller = s_scope.caller;
      }
   }
   /* A text may prepare things, but not after this turn held an action for
    * its code: the code's text describes things as they were. */
   if (decision == TOOL_CALL_ALLOW && caller == TOOL_CALLER_UNVERIFIED &&
       out->kind == TOOL_KIND_PREPARE && !redeemed && channel_code_waiting()) {
      out->refusal = "an action is waiting for its code";
      snprintf(out->message, sizeof(out->message),
               "Not done: this message already holds an action for the user's reply code. "
               "Prepare anything else in a later message.");
      return TOOL_CALL_REFUSE;
   }
   if (decision == TOOL_CALL_CHALLENGE) {
      if (redeemed && !s_scope.active) {
         return TOOL_CALL_ALLOW; /* the user approved this one call by code */
      }
      /* A code approves one call the user is shown: never a call inside
       * another (a plan's step), nor a tool that can't be described. */
      if (s_scope.active || meta->no_reply_code) {
         decision = TOOL_CALL_REFUSE;
      }
   }
   if (decision == TOOL_CALL_REFUSE) {
      out->refusal = refusal_text(caller, out->kind);
      snprintf(out->message, sizeof(out->message), "%s", out->refusal);
   }
   return decision;
}

tool_call_scope_t tool_call_policy_enter(tool_action_kind_t kind,
                                         tool_caller_t caller,
                                         bool redeemed) {
   tool_call_scope_t previous = s_scope;
   previous.code_redeemed = session_call_code_redeemed();
   s_scope.active = true;
   s_scope.kind = kind;
   s_scope.caller = caller;
   /* A code approves this one call; a call inside it is never redeemed. */
   session_set_call_code_redeemed(redeemed);
   return previous;
}

void tool_call_policy_leave(tool_call_scope_t previous) {
   session_set_call_code_redeemed(previous.code_redeemed);
   previous.code_redeemed = false;
   s_scope = previous;
}

bool tool_call_policy_decided(tool_action_kind_t *kind) {
   if (kind) {
      *kind = s_scope.active ? s_scope.kind : TOOL_KIND_ACT;
   }
   return s_scope.active;
}
