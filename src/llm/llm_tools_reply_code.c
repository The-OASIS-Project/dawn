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
 * A call from a text that waits for the user's reply code
 * (core/tool_call_challenge.h): held with DAWN's description of it and a
 * fingerprint of the call as resolved, the description included; run once
 * when the user replies with the code, and only if it resolves to the same
 * call and the same description.
 */

#include <json-c/json.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/session_manager.h"
#include "core/tool_call_challenge.h"
#include "llm/llm_tools_internal.h"
#include "logging.h"
#include "utils/string_utils.h"

/* The fingerprint of a call as resolved, with what the user is shown of it:
 * each part with its NUL, so no two calls hash alike by shifting text between
 * parts. */
static void call_binding(const tool_metadata_t *meta,
                         const char *device,
                         const tool_call_verdict_t *verdict,
                         const char *value,
                         const char *description,
                         char out[LLM_TOOLS_BINDING_HEX]) {
   crypto_generichash_state st;
   crypto_generichash_init(&st, NULL, 0, crypto_generichash_BYTES);
   const char *parts[] = { meta->name,         device ? device : "",
                           verdict->action,    tool_action_kind_name(verdict->kind),
                           value ? value : "", description };
   for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
      crypto_generichash_update(&st, (const unsigned char *)parts[i], strlen(parts[i]) + 1);
   }
   unsigned char digest[crypto_generichash_BYTES];
   crypto_generichash_final(&st, digest, sizeof(digest));
   sodium_bin2hex(out, LLM_TOOLS_BINDING_HEX, digest, sizeof(digest));
}

/* A tool with no describe_call: its name, action and declared parameters, in
 * the order declared, required ones first.  Keys the tool doesn't declare
 * (which it ignores) are left out, so the model can't add words of its own. */
static int describe_from_params(const tool_call_t *call,
                                const tool_metadata_t *meta,
                                const tool_call_verdict_t *verdict,
                                char *out,
                                size_t out_len) {
   static const char cut_note[] = ", ... (more not shown)";
   struct json_object *args = json_tokener_parse(call->arguments);
   if (!args || !json_object_is_type(args, json_type_object) || out_len <= sizeof(cut_note)) {
      if (args) {
         json_object_put(args);
      }
      return FAILURE;
   }
   /* Whole parameters only, with room kept for the note that says some were
    * left out. */
   const size_t limit = out_len - sizeof(cut_note);
   int n = snprintf(out, limit, "%s %s", meta->name, verdict->action);
   if (n <= 0 || (size_t)n >= limit) {
      json_object_put(args);
      return FAILURE;
   }
   size_t used = (size_t)n;
   bool cut = false;
   const char *sep = ": ";
   for (int pass = 0; !cut && pass < 2; pass++) {
      for (int i = 0; !cut && i < meta->param_count; i++) {
         const treg_param_t *p = &meta->params[i];
         struct json_object *v = NULL;
         if (p->required != (pass == 0) || p->maps_to == TOOL_MAPS_TO_ACTION ||
             !json_object_object_get_ex(args, p->name, &v) || !v) {
            continue;
         }
         const char *text = json_object_is_type(v, json_type_string)
                                ? json_object_get_string(v)
                                : json_object_to_json_string_ext(v, JSON_C_TO_STRING_PLAIN);
         char shown[200];
         str_excerpt_line(text, 120, shown, sizeof(shown));
         n = snprintf(out + used, limit - used, "%s%s=\"%s\"", sep, p->name, shown);
         if (n <= 0 || (size_t)n >= limit - used) {
            out[used] = '\0';
            cut = true;
         } else {
            used += (size_t)n;
            sep = ", ";
         }
      }
   }
   json_object_put(args);
   if (cut) {
      snprintf(out + used, out_len - used, "%s", cut_note);
   }
   return SUCCESS;
}

/* What a held call does, for the text that asks for its code: the tool's own
 * words, or its name, action and declared parameters; one plain line either
 * way.  On FAILURE, @p out may say why (for the model). */
static int describe(const tool_call_t *call,
                    const tool_metadata_t *meta,
                    const tool_call_verdict_t *verdict,
                    const char *value,
                    char *out,
                    size_t out_len,
                    int *valid_for_sec) {
   char raw[TOOL_CALL_CHALLENGE_DESC_MAX * 2];
   raw[0] = '\0';
   int rc = meta->describe_call
                ? meta->describe_call(verdict->action, value, raw, sizeof(raw), valid_for_sec)
                : TOOL_DESCRIBE_DEFAULT;
   if (rc == TOOL_DESCRIBE_DEFAULT) {
      raw[0] = '\0';
      rc = describe_from_params(call, meta, verdict, raw, sizeof(raw));
   }
   if (rc != SUCCESS) {
      str_excerpt_line(raw, out_len - 48, out, out_len);
      return FAILURE;
   }
   str_excerpt_line(raw, out_len - 48, out, out_len);
   return SUCCESS;
}

int llm_tools_approved_binding(const tool_call_t *call,
                               const tool_metadata_t *meta,
                               const tool_call_verdict_t *verdict,
                               const char *device,
                               const char *value_buf,
                               char out[LLM_TOOLS_BINDING_HEX],
                               char *why,
                               size_t why_len) {
   const char *value = (value_buf && value_buf[0]) ? value_buf : NULL;
   char description[TOOL_CALL_CHALLENGE_DESC_MAX];
   int valid_for = TOOL_CALL_CHALLENGE_TTL_SEC;
   out[0] = '\0';
   if (why && why_len) {
      why[0] = '\0';
   }
   if (describe(call, meta, verdict, value, description, sizeof(description), &valid_for) !=
       SUCCESS) {
      if (why && why_len) {
         str_excerpt_line(description, why_len > 48 ? why_len - 48 : 0, why, why_len);
      }
      return FAILURE;
   }
   call_binding(meta, device, verdict, value, description, out);
   return SUCCESS;
}

bool llm_tools_drop_earlier_code(void) {
   session_t *ctx = session_get_command_context();
   if (!ctx || ctx->messaging_identity.channel_id <= 0) {
      return false;
   }
   const bool dropped = tool_call_challenge_cancel_earlier(ctx->messaging_identity.channel_id,
                                                           session_turn_token());
   if (dropped) {
      OLOG_INFO("Dropped the action waiting for a reply code on channel %lld: a new request",
                (long long)ctx->messaging_identity.channel_id);
   }
   return dropped;
}

/* Append @p note to @p result's text (moved to result_extended when the fixed
 * buffer can't hold both). */
static void append_note(tool_result_t *result, const char *note, size_t n) {
   if (!result->result_extended) {
      const size_t len = strnlen(result->result, LLM_TOOLS_RESULT_LEN);
      if (len + n < LLM_TOOLS_RESULT_LEN) {
         memcpy(result->result + len, note, n + 1);
         return;
      }
      result->result_extended = strndup(result->result, LLM_TOOLS_RESULT_LEN);
      if (!result->result_extended) {
         return;
      }
   }
   const size_t len = strlen(result->result_extended);
   char *grown = realloc(result->result_extended, len + n + 1);
   if (grown) {
      memcpy(grown + len, note, n + 1);
      result->result_extended = grown;
   }
}

void llm_tools_note_text_preview(const tool_metadata_t *meta,
                                 const char *action,
                                 bool dropped_earlier,
                                 tool_result_t *result) {
   if (!result) {
      return;
   }
   if (dropped_earlier) {
      static const char dropped[] = "\n\nThe request from an earlier message that was waiting "
                                    "for the user's reply code was dropped for this one: its "
                                    "code no longer works. Say so.";
      append_note(result, dropped, sizeof(dropped) - 1);
   }
   const char *confirm = NULL;
   for (int i = 0; meta && action && i < meta->action_kind_count && !confirm; i++) {
      if (meta->action_kinds[i].confirm && strcmp(meta->action_kinds[i].action, action) == 0) {
         confirm = meta->action_kinds[i].confirm;
      }
   }
   if (!confirm || !result->success || result->is_error) {
      return;
   }
   char note[512];
   const int n = snprintf(note, sizeof(note),
                          "\n\nThis request came by text. If the above is a preview waiting "
                          "for the user's confirmation, don't ask them to reply yes: call %s "
                          "for it now. It won't run yet: DAWN texts the user exactly what it "
                          "does and a code, and their reply with that code is the "
                          "confirmation.",
                          confirm);
   if (n > 0 && (size_t)n < sizeof(note)) {
      append_note(result, note, (size_t)n);
   }
}

int llm_tools_hold_for_reply_code(const tool_call_t *call,
                                  const tool_metadata_t *meta,
                                  const tool_call_verdict_t *verdict,
                                  const char *device,
                                  const char *value_buf,
                                  tool_result_t *result) {
   session_t *ctx = session_get_command_context();
   const char *value = (value_buf && value_buf[0]) ? value_buf : NULL;
   char description[TOOL_CALL_CHALLENGE_DESC_MAX];
   int valid_for = TOOL_CALL_CHALLENGE_TTL_SEC;
   const char *message;
   char why[TOOL_CALL_CHALLENGE_DESC_MAX + 16];
   tool_challenge_rc_t rc = TOOL_CHALLENGE_FULL;
   if (!ctx || ctx->messaging_identity.channel_id <= 0) {
      message = "Not done: this request can't be confirmed by a reply code here. Ask from the "
                "app or by voice.";
   } else if (describe(call, meta, verdict, value, description, sizeof(description), &valid_for) !=
              SUCCESS) {
      if (description[0]) {
         snprintf(why, sizeof(why), "Not done: %s.", description);
         message = why;
      } else {
         message = "Not done: couldn't say what this would do (it may have expired). Prepare "
                   "it again.";
      }
   } else if (valid_for < TOOL_CALL_CHALLENGE_MIN_SEC) {
      message = "Not done: what this confirms expires too soon for a reply code. Prepare it "
                "again.";
   } else {
      char binding[LLM_TOOLS_BINDING_HEX];
      call_binding(meta, device, verdict, value, description, binding);
      const tool_challenge_t held = {
         .channel_id = ctx->messaging_identity.channel_id,
         .user_id = session_effective_user_id(ctx),
         .turn_token = session_turn_token(),
         .valid_for_sec = valid_for,
         .tool = meta->name,
         .args = call->arguments,
         .binding = binding,
         .description = description,
      };
      rc = tool_call_challenge_create(&held);
      switch (rc) {
         case TOOL_CHALLENGE_OK:
            message = "Not done yet: this request came by text, so it waits for the user's "
                      "reply code. DAWN is texting them what was asked and a 6-digit code; they "
                      "reply with that code to go ahead, or STOP. DAWN checks codes itself and "
                      "answers them directly: if you see a number in a message, DAWN did not "
                      "accept it as a code. Don't make up or repeat a code.";
            break;
         case TOOL_CHALLENGE_SAME:
            message = "Not done yet: this exact request already waits for the user's reply code, "
                      "and DAWN has texted it (or will at the end of this turn). Don't ask for it "
                      "again: they reply with that code to go ahead, or STOP. If they didn't get "
                      "the code, they reply STOP and ask again.";
            break;
         case TOOL_CHALLENGE_BUSY:
            message = "Not done: one action from this message already waits for the user's "
                      "reply code. Ask for the next one after it.";
            break;
         case TOOL_CHALLENGE_LIMIT:
            message = "Not done: too many confirmation codes were sent to this number lately. "
                      "Ask from the app or by voice.";
            break;
         default:
            message = "Not done: confirmations are busy right now. Try again in a few minutes.";
            break;
      }
   }
   safe_strncpy(result->result, message, LLM_TOOLS_RESULT_LEN);
   /* Held is not a failure: it waits. */
   const bool held = rc == TOOL_CHALLENGE_OK || rc == TOOL_CHALLENGE_SAME;
   result->success = held;
   result->is_error = !held;
   result->should_respond = true;
   OLOG_INFO("Tool '%s' action '%s' (%s) needs a reply code: %s", call->name, verdict->action,
             tool_action_kind_name(verdict->kind),
             rc == TOOL_CHALLENGE_OK     ? "held"
             : rc == TOOL_CHALLENGE_SAME ? "already held"
                                         : message);
   llm_tools_notify_execution(call->name, call->arguments, result->result, false);
   return held ? 0 : 1;
}

int llm_tools_execute_stored(const char *tool,
                             const char *args,
                             const char *binding,
                             tool_result_t *result) {
   if (!tool || !binding || !result) {
      return 1;
   }
   tool_call_t *call = calloc(1, sizeof(*call));
   if (!call) {
      memset(result, 0, sizeof(*result));
      snprintf(result->result, LLM_TOOLS_RESULT_LEN, "Error: out of memory");
      return 1;
   }
   safe_strncpy(call->id, "code_approved", sizeof(call->id));
   safe_strncpy(call->name, tool, sizeof(call->name));
   safe_strncpy(call->arguments, args ? args : "", sizeof(call->arguments));
   const int rc = llm_tools_execute_approved(call, result, binding);
   free(call);
   return rc;
}
