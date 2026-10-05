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
 * The Anthropic betas on first-party Claude requests.
 *
 * Cache diagnostics: each response says why its prompt cache missed, compared
 * with the request it names in diagnostics.previous_message_id.  A request's
 * fingerprint is kept only when it carries the header AND a diagnostics object
 * (empty when there is no previous one), so every request carries both.
 *
 * Thinking binding controls: a replayed thinking block is bound to the exact
 * prefix (system, tools, earlier messages) it was produced under.  Accounts
 * created on or after 2026-08-31 get a 400 on any mismatch; with the controls,
 * the API drops the mismatched blocks instead and names each drop in the
 * response's input_transformations.  DAWN always asks for "drop_block": a
 * prefix edit is a harness bug, and a degraded turn beats a failed one.  The
 * field is only accepted beside thinking type "adaptive" or "enabled" (under
 * "disabled" it's a 400); with thinking omitted, the header alone selects the
 * beta's default, which is also drop_block.
 *
 * Inline tools: a conversation's later tool changes are sent in place, a
 * `tool_addition` whose tool is a `tool_definition` in a mid-conversation
 * system message, so `tools` stays what the conversation started with.  The
 * header goes on every request whose body carries one (llm_claude_tools.c
 * decides where they go).
 */

#include "llm/llm_claude_betas.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "core/session_manager.h"
#include "llm/llm_cache_monitor.h"
#include "llm/llm_capabilities.h"
#include "llm/llm_claude_route.h"
#include "logging.h"

#define BETA_CACHE_DIAGNOSIS "cache-diagnosis-2026-04-07"
#define BETA_BINDING_CONTROLS "thinking-binding-controls-2026-08-01"
#define BETA_INLINE_TOOLS "inline-tools-2026-09-15"
#define PREFIX_MISMATCH_BEHAVIOR "drop_block"

/* The models Anthropic rejected a beta for: the rest of this process sends that
 * model none of it, rather than failing every request.  Keyed by model because
 * acceptance can differ by model, and one old model turning the binding controls
 * off would leave an enforced model's history mismatches failing.  The table is
 * small (models in use); a full table turns the beta off for every model, which
 * is safe, just coarser. */
#define REJECTED_MODELS_MAX 32
typedef struct {
   char model[64];
   bool diagnostics;
   bool binding;
   bool inline_tools;
} rejected_model_t;
static rejected_model_t s_rejected[REJECTED_MODELS_MAX];
static int s_rejected_count;
static bool s_all_diagnostics_off;
static bool s_all_binding_off;
static bool s_all_inline_off;
static pthread_mutex_t s_rejected_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Which betas are off for @p model. */
typedef struct {
   bool diagnostics;
   bool binding;
   bool inline_tools;
} betas_off_t;

static betas_off_t rejected_for(const char *model) {
   pthread_mutex_lock(&s_rejected_mutex);
   betas_off_t off = { s_all_diagnostics_off, s_all_binding_off, s_all_inline_off };
   for (int i = 0; i < s_rejected_count; i++) {
      if (strcmp(s_rejected[i].model, model) == 0) {
         off.diagnostics = off.diagnostics || s_rejected[i].diagnostics;
         off.binding = off.binding || s_rejected[i].binding;
         off.inline_tools = off.inline_tools || s_rejected[i].inline_tools;
         break;
      }
   }
   pthread_mutex_unlock(&s_rejected_mutex);
   return off;
}

/* Turn the betas @p off names off for @p model.  Returns true the first time
 * (to log once). */
static bool reject_for(const char *model, betas_off_t off) {
   bool first = false;
   pthread_mutex_lock(&s_rejected_mutex);
   rejected_model_t *entry = NULL;
   for (int i = 0; i < s_rejected_count; i++) {
      if (strcmp(s_rejected[i].model, model) == 0) {
         entry = &s_rejected[i];
         break;
      }
   }
   if (!entry && s_rejected_count < REJECTED_MODELS_MAX) {
      entry = &s_rejected[s_rejected_count++];
      memset(entry, 0, sizeof(*entry));
      snprintf(entry->model, sizeof(entry->model), "%s", model);
   }
   if (entry) {
      first = (off.diagnostics && !entry->diagnostics) || (off.binding && !entry->binding) ||
              (off.inline_tools && !entry->inline_tools);
      entry->diagnostics = entry->diagnostics || off.diagnostics;
      entry->binding = entry->binding || off.binding;
      entry->inline_tools = entry->inline_tools || off.inline_tools;
   } else {
      first = (off.diagnostics && !s_all_diagnostics_off) || (off.binding && !s_all_binding_off) ||
              (off.inline_tools && !s_all_inline_off);
      s_all_diagnostics_off = s_all_diagnostics_off || off.diagnostics;
      s_all_binding_off = s_all_binding_off || off.binding;
      s_all_inline_off = s_all_inline_off || off.inline_tools;
   }
   pthread_mutex_unlock(&s_rejected_mutex);
   return first;
}
/* The calling thread's request was rejected for a beta: send it again. */
static __thread bool t_retry = false;
/* The calling thread's turn had tools defined in a message rejected: the rest
 * of its requests fold them, and the session records it on the conversation
 * once the turn's call returns (claude_betas_take_inline_rejected). */
static __thread bool t_inline_rejected = false;

/* The diagnostics object: the previous response on this conversation, if any. */
static void add_cache_diagnostics(struct json_object *request) {
   struct json_object *diagnostics = json_object_new_object();
   if (!diagnostics) {
      return;
   }
   /* A side call (a compaction on the turn's session, ...) isn't the
    * conversation's next request: nothing to compare it with. */
   session_t *session = llm_cache_monitor_in_side_call() ? NULL : session_get_command_context();
   char previous[64];
   if (session &&
       llm_cache_monitor_previous_message_id(session->session_id,
                                             atomic_load(&session->stream_conversation_id),
                                             previous, sizeof(previous))) {
      json_object_object_add(diagnostics, "previous_message_id", json_object_new_string(previous));
   }
   json_object_object_add(request, "diagnostics", diagnostics);
}

/* The explicit mismatch behavior, on the thinking types that accept it. */
static void add_binding_controls(struct json_object *request) {
   struct json_object *thinking = NULL;
   struct json_object *type = NULL;
   if (!json_object_object_get_ex(request, "thinking", &thinking) ||
       !json_object_object_get_ex(thinking, "type", &type)) {
      return;
   }
   const char *t = json_object_get_string(type);
   if (!t || (strcmp(t, "adaptive") != 0 && strcmp(t, "enabled") != 0)) {
      return;
   }
   struct json_object *binding = json_object_new_object();
   if (!binding) {
      return;
   }
   json_object_object_add(binding, "prefix_mismatch_behavior",
                          json_object_new_string(PREFIX_MISMATCH_BEHAVIOR));
   json_object_object_add(thinking, "block_binding", binding);
}

bool claude_betas_inline_tools_ok(const char *base_url, const char *model) {
   return model && llm_claude_route(base_url).first_party && llm_model_inline_tools(model) &&
          !rejected_for(model).inline_tools;
}

bool claude_betas_render_inline(const char *base_url) {
   return !t_inline_rejected && llm_claude_route(base_url).first_party;
}

bool claude_betas_take_inline_rejected(void) {
   const bool rejected = t_inline_rejected;
   t_inline_rejected = false;
   return rejected;
}

/* Whether @p request defines a tool in a message (a tool_addition block in a
 * mid-conversation system message). */
static bool carries_inline_tools(struct json_object *request) {
   struct json_object *messages = NULL;
   if (!json_object_object_get_ex(request, "messages", &messages)) {
      return false;
   }
   const size_t n = json_object_array_length(messages);
   for (size_t i = 0; i < n; i++) {
      struct json_object *msg = json_object_array_get_idx(messages, i);
      struct json_object *role = NULL;
      struct json_object *content = NULL;
      if (!json_object_object_get_ex(msg, "role", &role) ||
          strcmp(json_object_get_string(role), "system") != 0 ||
          !json_object_object_get_ex(msg, "content", &content) ||
          !json_object_is_type(content, json_type_array)) {
         continue;
      }
      const size_t nb = json_object_array_length(content);
      for (size_t b = 0; b < nb; b++) {
         struct json_object *type = NULL;
         if (json_object_object_get_ex(json_object_array_get_idx(content, b), "type", &type) &&
             strcmp(json_object_get_string(type), "tool_addition") == 0) {
            return true;
         }
      }
   }
   return false;
}

void claude_betas_add(struct json_object *request, const char *base_url, claude_betas_t *sent) {
   sent->diagnostics = false;
   sent->binding = false;
   sent->inline_tools = false;
   sent->inline_rejected = false;
   sent->model[0] = '\0';
   /* The Anthropic API takes every beta; OpenRouter's Messages endpoint passes
    * the binding controls through, drops the diagnostics (none come back) and
    * rejects a tool defined in a message.  Anything else (a proxy) gets none. */
   const llm_claude_route_t route = llm_claude_route(base_url);
   if (!request || (!route.first_party && route.provider != CLOUD_PROVIDER_OPENROUTER)) {
      return;
   }
   struct json_object *model = NULL;
   if (json_object_object_get_ex(request, "model", &model)) {
      snprintf(sent->model, sizeof(sent->model), "%s", json_object_get_string(model));
   }
   const betas_off_t off = rejected_for(sent->model);
   if (route.first_party && !off.diagnostics) {
      add_cache_diagnostics(request);
      sent->diagnostics = true;
   }
   if (!off.binding) {
      add_binding_controls(request); /* the header goes on every request, thinking or not */
      sent->binding = true;
   }
   /* The body decided (claude_betas_inline_tools_ok when it was built). */
   sent->inline_tools = carries_inline_tools(request);
}

struct curl_slist *claude_betas_header(struct curl_slist *headers, const claude_betas_t *sent) {
   const char *on[3];
   int n = 0;
   if (sent->diagnostics) {
      on[n++] = BETA_CACHE_DIAGNOSIS;
   }
   if (sent->binding) {
      on[n++] = BETA_BINDING_CONTROLS;
   }
   if (sent->inline_tools) {
      on[n++] = BETA_INLINE_TOOLS;
   }
   if (n == 0) {
      return headers;
   }
   char header[192];
   int len = snprintf(header, sizeof(header), "anthropic-beta: ");
   for (int i = 0; i < n && len > 0 && (size_t)len < sizeof(header); i++) {
      len += snprintf(header + len, sizeof(header) - (size_t)len, "%s%s", i ? "," : "", on[i]);
   }
   return curl_slist_append(headers, header);
}

/* The API's two ways of refusing a beta: an unknown header value ("Unexpected
 * value(s) `<beta>` for the `anthropic-beta` header"), or an unknown request
 * field ("<path>.<field>: Extra inputs are not permitted").  Nothing else counts:
 * a binding mismatch's own 400 names block_binding too, and turning the controls
 * off then would remove the one thing protecting enforced accounts. */
static bool refuses(const char *message, const char *beta, const char *field) {
   if (strstr(message, "Unexpected value") && strstr(message, "anthropic-beta") &&
       strstr(message, beta)) {
      return true;
   }
   return strstr(message, "Extra inputs are not permitted") && strstr(message, field);
}

bool claude_betas_rejected(long http_code, const char *body, claude_betas_t *sent) {
   if (http_code != 400 || !body || !sent ||
       (!sent->diagnostics && !sent->binding && !sent->inline_tools)) {
      return false;
   }
   json_object *parsed = json_tokener_parse(body);
   json_object *error = NULL;
   json_object *v = NULL;
   const char *type = NULL;
   const char *message = NULL;
   if (parsed && json_object_object_get_ex(parsed, "error", &error)) {
      if (json_object_object_get_ex(error, "type", &v)) {
         type = json_object_get_string(v);
      }
      if (json_object_object_get_ex(error, "message", &v)) {
         message = json_object_get_string(v);
      }
   }
   bool rejected = false;
   if (type && message && strcmp(type, "invalid_request_error") == 0) {
      if (sent->diagnostics && refuses(message, BETA_CACHE_DIAGNOSIS, "diagnostics")) {
         if (reject_for(sent->model, (betas_off_t){ .diagnostics = true })) {
            OLOG_WARNING("Claude API rejected the cache diagnostics for %s (%.200s); continuing "
                         "without them",
                         sent->model, message);
         }
         rejected = true;
      }
      if (sent->binding && refuses(message, BETA_BINDING_CONTROLS, "block_binding")) {
         if (reject_for(sent->model, (betas_off_t){ .binding = true })) {
            /* Correctness, not telemetry: without the controls an account created
             * on or after 2026-08-31 gets a 400 on any history mismatch. */
            OLOG_ERROR("Claude API rejected the thinking binding controls for %s (%.200s); "
                       "continuing without them, so a history mismatch on %s now fails the "
                       "request",
                       sent->model, message, sent->model);
         }
         rejected = true;
      }
      /* Strictly an error naming the beta: one about a tool's definition
       * itself (a bad schema) must fail as it is, not fold. */
      if (sent->inline_tools && strstr(message, BETA_INLINE_TOOLS)) {
         if (reject_for(sent->model, (betas_off_t){ .inline_tools = true })) {
            OLOG_WARNING("Claude API rejected tools defined in a message for %s (%.200s); "
                         "the conversation's tool changes fold into its tools from now on",
                         sent->model, message);
         }
         /* This turn folds them from its resend on; the session records it on
          * the conversation (its later requests, after a restart). */
         sent->inline_rejected = true;
         t_inline_rejected = true;
         rejected = true;
      }
   }
   if (rejected) {
      t_retry = true;
   }
   json_object_put(parsed);
   return rejected;
}

bool claude_betas_take_retry(void) {
   const bool retry = t_retry;
   t_retry = false;
   return retry;
}
