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
#include "core/session_compaction.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "llm/llm_context.h"
#include "llm/llm_interface.h"
#include "llm/llm_selection.h"
#include "logging.h"
#include "tools/tool_registry.h"
#ifdef ENABLE_WEBUI
#include "messaging/messaging_engine.h"
#endif

/* ========== Forward Declarations ========== */

static char *switch_llm_tool_callback(const char *action, char *value, int *should_respond);

/* ========== Target Dispatch Table ========== */

typedef struct {
   const char *name;          /* Name to match (case-insensitive) */
   llm_type_t type;           /* LLM type to set */
   cloud_provider_t provider; /* Cloud provider (NONE for local) */
   const char *label;         /* Human-readable label for messages */
} llm_target_entry_t;

static const llm_target_entry_t llm_targets[] = {
   /* Local targets */
   { "local", LLM_LOCAL, CLOUD_PROVIDER_NONE, "local LLM" },
   { "llama", LLM_LOCAL, CLOUD_PROVIDER_NONE, "local LLM" },
   /* Cloud: the session's cloud provider, else the one dawn.toml names */
   { "cloud", LLM_CLOUD, CLOUD_PROVIDER_NONE, "cloud LLM" },
   /* OpenAI variants */
   { "openai", LLM_CLOUD, CLOUD_PROVIDER_OPENAI, "OpenAI" },
   { "gpt", LLM_CLOUD, CLOUD_PROVIDER_OPENAI, "OpenAI" },
   { "chatgpt", LLM_CLOUD, CLOUD_PROVIDER_OPENAI, "OpenAI" },
   { "chat gpt", LLM_CLOUD, CLOUD_PROVIDER_OPENAI, "OpenAI" },
   { "open ai", LLM_CLOUD, CLOUD_PROVIDER_OPENAI, "OpenAI" },
   /* Claude variants */
   { "claude", LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, "Claude" },
   { "clawed", LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, "Claude" },
   { "claud", LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, "Claude" },
   { "cloud ai", LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, "Claude" },
   /* Gemini variants */
   { "gemini", LLM_CLOUD, CLOUD_PROVIDER_GEMINI, "Gemini" },
   { "jiminy", LLM_CLOUD, CLOUD_PROVIDER_GEMINI, "Gemini" },
   /* OpenRouter */
   { "openrouter", LLM_CLOUD, CLOUD_PROVIDER_OPENROUTER, "OpenRouter" },
   { "open router", LLM_CLOUD, CLOUD_PROVIDER_OPENROUTER, "OpenRouter" },
};

/* ========== Parameter Definitions ========== */

static const treg_param_t switch_llm_params[] = {
   {
       .name = "target",
       .description = "The LLM target to switch to",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "local", "cloud", "openai", "claude", "gemini", "openrouter" },
       .enum_count = 6,
   },
};

/* ========== Tool Metadata ========== */

static const tool_metadata_t switch_llm_metadata = {
   .name = "switch_llm",
   .results = TOOL_RESULTS_OWN,
   /* Kind of action: act (the default): changes the model the conversation runs on. */
   .device_string = "switch_llm",
   .topic = "dawn",
   .aliases = { "llm", "ai", "model", "provider", "cloud provider", "local llm", "cloud llm" },
   .alias_count = 7,

   .description =
       "Switch between local and cloud LLM, or change the cloud provider. Use 'local' to "
       "use the local LLM server, 'cloud' to use cloud AI, or specify a provider name like "
       "'openai', 'claude', or 'gemini'.",
   .params = switch_llm_params,
   .param_count = TOOL_PARAM_COUNT(switch_llm_params),

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

static char *switch_llm_tool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1;

   /* For LLM tool calling: target comes in action parameter
    * For voice commands via device_types: action="set", target comes in value parameter */
   const char *target = action;
   if (action && strcasecmp(target, "set") == 0 && value && value[0]) {
      target = value;
   }

   if (!target || target[0] == '\0') {
      return strdup("No LLM target specified. Use 'local', 'cloud', 'openai', 'claude', "
                    "'gemini' or 'openrouter'.");
   }

   const llm_target_entry_t *entry = find_target(target);
   if (!entry) {
      char *result = malloc(128);
      if (result) {
         snprintf(result, 128,
                  TOOL_RESULT_ERROR_MARK
                  "Unknown LLM target '%.32s'. Use 'local', 'cloud', 'openai', 'claude', "
                  "'gemini' or 'openrouter'.",
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
      return strdup(TOOL_RESULT_ERROR_MARK "No active session available for LLM switch.");
   }

   /* Inside a turn, the switch lasts beyond it only when the user asked for it,
    * as a pick in the WebUI does: a background or job turn acting on untrusted
    * content (a fetched page, a job's result) changes this reply's model and
    * nothing else, and never to a cloud provider the conversation isn't already
    * on.  Outside a turn (an MQTT command) it is the session's setting, as
    * before: with no turn running to pin the history, the next turn's own
    * context management fits the history to the new model. */
   const bool in_turn = session_turn_is_caller(session);
   if (!in_turn && session_turn_active(session)) {
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Can't switch the AI while it is answering; try again in a moment.");
   }

   session_llm_config_t config;
   session_get_llm_config(session, &config);
   const llm_type_t type_before = config.type;

   int64_t conv_id = session->messaging_identity.conversation_id;
   if (conv_id <= 0) {
      conv_id = session_turn_conversation(session);
   }
   const bool user_asked = !in_turn || session_turn_user_originated(session);
   const bool lasting = user_asked;
   const int owner = session_effective_user_id(session); /* whose conversations */

   /* The selection asked for: "cloud" is the session's cloud provider, else the one
    * dawn.toml names.  Checked now: a provider without its key is refused with
    * the reason and nothing changes. */
   session_llm_config_t target_cfg = config;
   llm_selection_set_target(&target_cfg, entry->type, entry->provider);
   target_cfg.model[0] = '\0'; /* the provider's default model */
   if (!user_asked && target_cfg.type == LLM_CLOUD &&
       (type_before != LLM_CLOUD || target_cfg.cloud_provider != config.cloud_provider)) {
      /* Not even for this reply: the turn would send its whole history to a
       * cloud provider the user didn't pick, because of content the turn read. */
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Switching to another cloud provider needs the user to ask for it.");
   }
   char reason[LLM_SELECTION_REASON_MAX];
   if (llm_selection_problem(&target_cfg, reason, sizeof(reason)) != 0) {
      char *result = malloc(LLM_SELECTION_REASON_MAX + 32);
      if (result) {
         snprintf(result, LLM_SELECTION_REASON_MAX + 32, TOOL_RESULT_ERROR_MARK "%s", reason);
      }
      return result;
   }

   /* The switch takes effect with the next message: its seam fits the history
    * to the new model's window, summarizing with this one if it must (never
    * inside this tool call: mid tool round, the turn's request stands as it is). */
   if (in_turn && lasting) {
      session_compaction_note_switch(session, &config);
   }

   OLOG_INFO("Setting AI to %s via switch_llm tool%s.", entry->label,
             lasting ? "" : " (this reply only)");
   config = target_cfg;

   const int set_rc = lasting ? session_set_llm_config(session, &config)
                              : session_set_turn_llm_config(session, &config);
   if (set_rc != SUCCESS) {
      char *result = malloc(128);
      if (result) {
         snprintf(result, 128, TOOL_RESULT_ERROR_MARK "Failed to switch to %s.", entry->label);
      }
      return result;
   }

   /* A lasting switch inside a turn is saved to the conversation the turn
    * belongs to, so it holds whenever that conversation is used again (opened
    * in the WebUI, a later message on its channel, a session recreated after
    * idle or a restart), and for a turn on a conversation the user isn't
    * viewing.  Re-read the applied config and write the FULL config, because
    * conv_db_update_llm_settings overwrites all columns (no keep-current):
    * passing the current thinking_mode / reasoning_effort keeps the thinking
    * settings switch_llm doesn't touch. */
   bool saved = false;
   if (lasting && in_turn && conv_id > 0 && owner > 0) {
      session_llm_config_t applied;
      session_get_llm_config(session, &applied);
      const char *type_str = (applied.type == LLM_LOCAL) ? "local" : "cloud";
      const char *prov_str = (applied.type == LLM_CLOUD)
                                 ? cloud_provider_to_string(applied.cloud_provider)
                                 : "";
      /* tools_mode column is retired (dead) — pass empty. */
      int prc = conv_db_update_llm_settings(conv_id, owner, type_str, prov_str, applied.model, "",
                                            applied.thinking_mode, applied.reasoning_effort);
      if (prc == AUTH_DB_SUCCESS) {
         saved = true;
#ifdef ENABLE_WEBUI
         /* A messaging channel's conversation: its live session too (as saved). */
         messaging_engine_apply_conv_llm(conv_id, &applied);
#endif
      } else {
         /* This session keeps the switch, but the conversation reverts the next
          * time it is loaded.  Log it. */
         OLOG_WARNING("switch_llm: failed to save the LLM change to conversation %lld (rc=%d)",
                      (long long)conv_id, prc);
      }
   }

   char *result = malloc(128);
   if (result) {
      snprintf(result, 128, "AI switched to %s%s", entry->label,
               !lasting ? " (for this reply only; not saved to the conversation)"
               : (!saved && in_turn && conv_id > 0) ? " (not saved to the conversation)"
                                                    : "");
   }
   return result;
}

/* ========== Public API ========== */

int switch_llm_tool_register(void) {
   return tool_registry_register(&switch_llm_metadata);
}
