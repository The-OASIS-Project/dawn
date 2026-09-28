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
 */

#include "llm/llm_claude_betas.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "core/session_manager.h"
#include "llm/llm_cache_monitor.h"
#include "logging.h"

#define BETA_CACHE_DIAGNOSIS "cache-diagnosis-2026-04-07"
#define BETA_BINDING_CONTROLS "thinking-binding-controls-2026-08-01"
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
} rejected_model_t;
static rejected_model_t s_rejected[REJECTED_MODELS_MAX];
static int s_rejected_count;
static bool s_all_diagnostics_off;
static bool s_all_binding_off;
static pthread_mutex_t s_rejected_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Which betas are off for @p model. */
static void rejected_for(const char *model, bool *diagnostics_off, bool *binding_off) {
   pthread_mutex_lock(&s_rejected_mutex);
   *diagnostics_off = s_all_diagnostics_off;
   *binding_off = s_all_binding_off;
   for (int i = 0; i < s_rejected_count; i++) {
      if (strcmp(s_rejected[i].model, model) == 0) {
         *diagnostics_off = *diagnostics_off || s_rejected[i].diagnostics;
         *binding_off = *binding_off || s_rejected[i].binding;
         break;
      }
   }
   pthread_mutex_unlock(&s_rejected_mutex);
}

/* Turn a beta off for @p model.  Returns true the first time (to log once). */
static bool reject_for(const char *model, bool diagnostics, bool binding) {
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
      first = (diagnostics && !entry->diagnostics) || (binding && !entry->binding);
      entry->diagnostics = entry->diagnostics || diagnostics;
      entry->binding = entry->binding || binding;
   } else {
      first = (diagnostics && !s_all_diagnostics_off) || (binding && !s_all_binding_off);
      s_all_diagnostics_off = s_all_diagnostics_off || diagnostics;
      s_all_binding_off = s_all_binding_off || binding;
   }
   pthread_mutex_unlock(&s_rejected_mutex);
   return first;
}
/* The calling thread's request was rejected for a beta: send it again. */
static __thread bool t_retry = false;

/* The Anthropic API itself (exact host), where the betas exist; a gateway or
 * proxy in front of it may reject the unknown fields. */
static bool is_first_party_endpoint(const char *base_url) {
   if (!base_url) {
      return false;
   }
   CURLU *url = curl_url();
   char *host = NULL;
   bool first_party = false;
   if (url && curl_url_set(url, CURLUPART_URL, base_url, 0) == CURLUE_OK &&
       curl_url_get(url, CURLUPART_HOST, &host, 0) == CURLUE_OK && host) {
      first_party = strcasecmp(host, "api.anthropic.com") == 0;
   }
   curl_free(host);
   curl_url_cleanup(url);
   return first_party;
}

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

void claude_betas_add(struct json_object *request, const char *base_url, claude_betas_t *sent) {
   sent->diagnostics = false;
   sent->binding = false;
   sent->model[0] = '\0';
   if (!request || !is_first_party_endpoint(base_url)) {
      return;
   }
   struct json_object *model = NULL;
   if (json_object_object_get_ex(request, "model", &model)) {
      snprintf(sent->model, sizeof(sent->model), "%s", json_object_get_string(model));
   }
   bool diagnostics_off = false;
   bool binding_off = false;
   rejected_for(sent->model, &diagnostics_off, &binding_off);
   if (!diagnostics_off) {
      add_cache_diagnostics(request);
      sent->diagnostics = true;
   }
   if (!binding_off) {
      add_binding_controls(request); /* the header goes on every request, thinking or not */
      sent->binding = true;
   }
}

struct curl_slist *claude_betas_header(struct curl_slist *headers, const claude_betas_t *sent) {
   if (!sent->diagnostics && !sent->binding) {
      return headers;
   }
   char header[160];
   snprintf(header, sizeof(header), "anthropic-beta: %s%s%s",
            sent->diagnostics ? BETA_CACHE_DIAGNOSIS : "",
            sent->diagnostics && sent->binding ? "," : "",
            sent->binding ? BETA_BINDING_CONTROLS : "");
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

bool claude_betas_rejected(long http_code, const char *body, const claude_betas_t *sent) {
   if (http_code != 400 || !body || !sent || (!sent->diagnostics && !sent->binding)) {
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
         if (reject_for(sent->model, true, false)) {
            OLOG_WARNING("Claude API rejected the cache diagnostics for %s (%.200s); continuing "
                         "without them",
                         sent->model, message);
         }
         rejected = true;
      }
      if (sent->binding && refuses(message, BETA_BINDING_CONTROLS, "block_binding")) {
         if (reject_for(sent->model, false, true)) {
            /* Correctness, not telemetry: without the controls an account created
             * on or after 2026-08-31 gets a 400 on any history mismatch. */
            OLOG_ERROR("Claude API rejected the thinking binding controls for %s (%.200s); "
                       "continuing without them, so a history mismatch on %s now fails the "
                       "request",
                       sent->model, message, sent->model);
         }
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
