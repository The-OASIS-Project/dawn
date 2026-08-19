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
 * LLM Status Tool - Get and set LLM configuration
 */

#include "tools/llm_status_tool.h"

#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "dawn.h"
#include "llm/llm_interface.h"
#include "llm/llm_local_provider.h"
#include "logging.h"
#include "tools/tool_registry.h"
#include "tts/text_to_speech.h"

/* ========== Forward Declarations ========== */

static char *llm_status_tool_callback(const char *action, char *value, int *should_respond);

/* ========== Tool Metadata ========== */

static const tool_metadata_t llm_status_metadata = {
   .name = "llm_status",
   .device_string = "llm",
   .topic = "dawn",
   .aliases = { "ai", "ai status", "llm mode", "ai mode" },
   .alias_count = 4,

   .description = "Get information about the current LLM configuration. Returns whether using "
                  "local or cloud LLM and the model name.",
   .params = NULL,
   .param_count = 0,

   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_NONE,
   .skip_followup = false,
   .default_remote = true,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = NULL,
   .callback = llm_status_tool_callback,
};

/* ========== Callback Implementation ========== */

static char *llm_status_tool_callback(const char *action, char *value, int *should_respond) {
   (void)action; /* Unused - this is a pure getter */
   (void)value;

   char *result = NULL;
   *should_respond = 1;

   /* Get session from command context, fall back to local session for external MQTT */
   session_t *session = session_get_command_context();
   if (!session) {
      session = session_get_local();
   }

   /* Return current LLM status */
   session_llm_config_t session_config;
   llm_resolved_config_t resolved;
   session_get_llm_config(session, &session_config);
   llm_resolve_config(&session_config, &resolved);

   llm_type_t current = resolved.type;
   const char *provider = resolved.cloud_provider == CLOUD_PROVIDER_OPENAI       ? "OpenAI"
                          : resolved.cloud_provider == CLOUD_PROVIDER_CLAUDE     ? "Claude"
                          : resolved.cloud_provider == CLOUD_PROVIDER_GEMINI     ? "Gemini"
                          : resolved.cloud_provider == CLOUD_PROVIDER_OPENROUTER ? "OpenRouter"
                                                                                 : "None";

   /* Get model name - fall back to provider defaults if not set in session */
   const char *model = resolved.model;
   static char local_model_buf[LLM_LOCAL_MODEL_NAME_MAX]; /* Static buffer for local model name */

   if (!model || model[0] == '\0') {
      if (current == LLM_LOCAL) {
         /* Query the local LLM server for the actual loaded model */
         llm_local_model_t models[8];
         size_t count = 0;
         const char *endpoint = resolved.endpoint ? resolved.endpoint : g_config.llm.local.endpoint;

         if (llm_local_list_models(endpoint, models, 8, &count) == 0 && count > 0) {
            /* For llama.cpp: first model is the loaded one
             * For Ollama: use config model or first available */
            local_provider_t provider = llm_local_detect_provider(endpoint);
            if (provider == LOCAL_PROVIDER_LLAMA_CPP) {
               strncpy(local_model_buf, models[0].name, sizeof(local_model_buf) - 1);
               local_model_buf[sizeof(local_model_buf) - 1] = '\0';
               model = local_model_buf;
            } else if (g_config.llm.local.model[0] != '\0') {
               model = g_config.llm.local.model;
            } else {
               strncpy(local_model_buf, models[0].name, sizeof(local_model_buf) - 1);
               local_model_buf[sizeof(local_model_buf) - 1] = '\0';
               model = local_model_buf;
            }
         } else {
            model = g_config.llm.local.model[0] ? g_config.llm.local.model : "local";
         }
      } else if (resolved.cloud_provider == CLOUD_PROVIDER_OPENAI) {
         model = llm_get_default_openai_model();
      } else if (resolved.cloud_provider == CLOUD_PROVIDER_CLAUDE) {
         model = llm_get_default_claude_model();
      } else if (resolved.cloud_provider == CLOUD_PROVIDER_GEMINI) {
         model = llm_get_default_gemini_model();
      } else if (resolved.cloud_provider == CLOUD_PROVIDER_OPENROUTER) {
         model = llm_get_default_openrouter_model();
      }
   }
   const char *type_str = (current == LLM_LOCAL) ? "local" : "cloud";

   if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
      /* Direct mode: use text-to-speech */
      result = malloc(256);
      if (result) {
         if (current == LLM_CLOUD) {
            snprintf(result, 256, "Currently using %s LLM with %s, model %s.", type_str, provider,
                     model ? model : "unknown");
         } else {
            snprintf(result, 256, "Currently using %s LLM, model %s.", type_str,
                     model ? model : "unknown");
         }
         text_to_speech(result);
         free(result);
      }
      *should_respond = 0;
      return NULL;
   } else {
      /* AI modes: return the raw data for AI to process */
      result = malloc(256);
      if (result) {
         if (current == LLM_CLOUD) {
            snprintf(result, 256, "Currently using %s LLM (%s, model: %s)", type_str, provider,
                     model ? model : "unknown");
         } else {
            snprintf(result, 256, "Currently using %s LLM (model: %s)", type_str,
                     model ? model : "unknown");
         }
      }
      return result;
   }
}

/* ========== Public API ========== */

int llm_status_tool_register(void) {
   return tool_registry_register(&llm_status_metadata);
}
