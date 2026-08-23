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
 * Switch LLM Tool - Switch between local and cloud LLM providers
 */

#include "tools/switch_llm_tool.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "auth/auth_db.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "llm/llm_context.h"
#include "llm/llm_interface.h"
#include "logging.h"
#include "tools/tool_registry.h"

/* ========== Forward Declarations ========== */

static char *switch_llm_tool_callback(const char *action, char *value, int *should_respond);

/* ========== Target Dispatch Table ========== */

typedef struct {
   const char *name;          /* Name to match (case-insensitive) */
   llm_type_t type;           /* LLM type to set */
   cloud_provider_t provider; /* Cloud provider (NONE for local) */
   const char *label;         /* Human-readable label for messages */
   const char *fail_hint;     /* Extra hint on failure (NULL for none) */
} llm_target_entry_t;

static const llm_target_entry_t llm_targets[] = {
   /* Local targets */
   { "local", LLM_LOCAL, CLOUD_PROVIDER_NONE, "local LLM", NULL },
   { "llama", LLM_LOCAL, CLOUD_PROVIDER_NONE, "local LLM", NULL },
   /* Cloud (keep current provider) */
   { "cloud", LLM_CLOUD, CLOUD_PROVIDER_NONE, "cloud LLM", "API key not configured." },
   /* OpenAI variants */
   { "openai", LLM_CLOUD, CLOUD_PROVIDER_OPENAI, "OpenAI", "API key not configured." },
   { "gpt", LLM_CLOUD, CLOUD_PROVIDER_OPENAI, "OpenAI", "API key not configured." },
   { "chatgpt", LLM_CLOUD, CLOUD_PROVIDER_OPENAI, "OpenAI", "API key not configured." },
   { "chat gpt", LLM_CLOUD, CLOUD_PROVIDER_OPENAI, "OpenAI", "API key not configured." },
   { "open ai", LLM_CLOUD, CLOUD_PROVIDER_OPENAI, "OpenAI", "API key not configured." },
   /* Claude variants */
   { "claude", LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, "Claude", "API key not configured." },
   { "clawed", LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, "Claude", "API key not configured." },
   { "claud", LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, "Claude", "API key not configured." },
   { "cloud ai", LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, "Claude", "API key not configured." },
   /* Gemini variants */
   { "gemini", LLM_CLOUD, CLOUD_PROVIDER_GEMINI, "Gemini", "API key not configured." },
   { "jiminy", LLM_CLOUD, CLOUD_PROVIDER_GEMINI, "Gemini", "API key not configured." },
   /* OpenRouter */
   { "openrouter", LLM_CLOUD, CLOUD_PROVIDER_OPENROUTER, "OpenRouter", "API key not configured." },
   { "open router", LLM_CLOUD, CLOUD_PROVIDER_OPENROUTER, "OpenRouter", "API key not configured." },
};

/* ========== Parameter Definitions ========== */

static const treg_param_t switch_llm_params[] = {
   {
       .name = "target",
       .description = "The LLM target to switch to",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "local", "cloud", "openai", "claude", "gemini" },
       .enum_count = 5,
   },
};

/* ========== Tool Metadata ========== */

static const tool_metadata_t switch_llm_metadata = {
   .name = "switch_llm",
   .device_string = "switch_llm",
   .topic = "dawn",
   .aliases = { "llm", "ai", "model", "provider", "cloud provider", "local llm", "cloud llm" },
   .alias_count = 7,

   .description =
       "Switch between local and cloud LLM, or change the cloud provider. Use 'local' to "
       "use the local LLM server, 'cloud' to use cloud AI, or specify a provider name like "
       "'openai', 'claude', or 'gemini'.",
   .params = switch_llm_params,
   .param_count = 1,

   .device_type = TOOL_DEVICE_TYPE_ANALOG,
   .capabilities = TOOL_CAP_NONE,
   .skip_followup = false,
   .default_remote = true,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = NULL,
   .callback = switch_llm_tool_callback,
};

/* ========== Callback Implementation ========== */

static const llm_target_entry_t *find_target(const char *name) {
   size_t count = sizeof(llm_targets) / sizeof(llm_targets[0]);
   for (size_t i = 0; i < count; i++) {
      if (strcasecmp(name, llm_targets[i].name) == 0) {
         return &llm_targets[i];
      }
   }
   return NULL;
}

/**
 * @brief Compact conversation history before switching to a provider with smaller context
 *
 * Uses the current (larger-context) provider to summarize before switching.
 */
static void compact_before_switch(session_t *session,
                                  const session_llm_config_t *current,
                                  const llm_target_entry_t *entry) {
   extern struct json_object *conversation_history;
   if (!conversation_history) {
      return;
   }

   /* Determine target provider: for "cloud" with no explicit provider, keep current */
   cloud_provider_t target_provider = entry->provider;
   if (entry->type == LLM_CLOUD && target_provider == CLOUD_PROVIDER_NONE) {
      target_provider = current->cloud_provider;
   }

   if (!llm_context_needs_compaction_for_switch(session->session_id, conversation_history,
                                                entry->type, target_provider, NULL)) {
      return;
   }

   llm_compaction_result_t compact_result = { 0 };
   int rc = llm_context_compact_for_switch(session->session_id, conversation_history, current->type,
                                           current->cloud_provider, current->model, entry->type,
                                           target_provider, NULL, &compact_result);
   if (rc == 0 && compact_result.performed) {
      OLOG_INFO("Pre-switch compaction: %d messages summarized, %d -> %d tokens",
                compact_result.messages_summarized, compact_result.tokens_before,
                compact_result.tokens_after);
   }
   llm_compaction_result_free(&compact_result);
}

static char *switch_llm_tool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1;

   /* For LLM tool calling: target comes in action parameter
    * For voice commands via device_types: action="set", target comes in value parameter */
   const char *target = action;
   if (action && strcasecmp(target, "set") == 0 && value && value[0]) {
      target = value;
   }

   if (!target || target[0] == '\0') {
      return strdup("No LLM target specified. Use 'local', 'cloud', 'openai', 'claude', or "
                    "'gemini'.");
   }

   const llm_target_entry_t *entry = find_target(target);
   if (!entry) {
      char *result = malloc(128);
      if (result) {
         snprintf(result, 128,
                  "Unknown LLM target '%.32s'. Use 'local', 'cloud', 'openai', 'claude', "
                  "or 'gemini'.",
                  target);
      }
      return result;
   }

   /* Get session from command context, fall back to local session for external MQTT */
   session_t *session = session_get_command_context();
   if (!session) {
      session = session_get_local();
   }

   if (!session) {
      return strdup("No active session available for LLM switch.");
   }

   session_llm_config_t config;
   session_get_llm_config(session, &config);

   /* Compact conversation if switching to a provider with smaller context window */
   compact_before_switch(session, &config, entry);

   OLOG_INFO("Setting AI to %s via switch_llm tool.", entry->label);
   config.type = entry->type;
   if (entry->provider != CLOUD_PROVIDER_NONE) {
      config.cloud_provider = entry->provider;
   }
   config.model[0] = '\0'; /* Clear model to use provider default */

   if (session_set_llm_config(session, &config) != SUCCESS) {
      char *result = malloc(128);
      if (result) {
         snprintf(result, 128, "Failed to switch to %s.%s%s", entry->label,
                  entry->fail_hint ? " " : "", entry->fail_hint ? entry->fail_hint : "");
      }
      return result;
   }

   /* Persist the change to the conversation row so it survives session
    * recreation (idle eviction / daemon restart).  Only messaging sessions
    * carry a conversation_id; WebUI/local sessions (conv_id 0) keep their
    * existing live-only behavior — the WebUI has its own per-conversation
    * lock UI.  Re-read the applied config first (session_set_llm_config may
    * fall back on a missing key) and write the FULL config, because
    * conv_db_update_llm_settings overwrites all columns (no keep-current) —
    * passing the current thinking_mode/reasoning_effort preserves the
    * conversation's thinking-on seed that switch_llm itself doesn't touch. */
   if (session->messaging_identity.conversation_id > 0 && session->metrics.user_id > 0) {
      session_llm_config_t applied;
      session_get_llm_config(session, &applied);
      const char *type_str = (applied.type == LLM_LOCAL) ? "local" : "cloud";
      const char *prov_str = (applied.type == LLM_CLOUD)
                                 ? cloud_provider_to_string(applied.cloud_provider)
                                 : "";
      /* tools_mode column is retired (dead) — pass empty. */
      int prc = conv_db_update_llm_settings(session->messaging_identity.conversation_id,
                                            session->metrics.user_id, type_str, prov_str,
                                            applied.model, "", applied.thinking_mode,
                                            applied.reasoning_effort);
      if (prc != AUTH_DB_SUCCESS) {
         /* Best-effort: the live session changed but the row didn't, so the
          * change reverts on next session recreation.  Log so it's diagnosable. */
         OLOG_WARNING("switch_llm: failed to persist LLM change to conversation %lld (rc=%d); "
                      "live session updated but it will not survive restart",
                      (long long)session->messaging_identity.conversation_id, prc);
      }
   }

   char *result = malloc(64);
   if (result) {
      snprintf(result, 64, "AI switched to %s", entry->label);
   }
   return result;
}

/* ========== Public API ========== */

int switch_llm_tool_register(void) {
   return tool_registry_register(&switch_llm_metadata);
}
