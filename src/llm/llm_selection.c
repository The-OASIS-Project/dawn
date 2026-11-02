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
 * Which LLM a chat runs on, and whether it can (see llm_selection.h), plus the
 * provider names and default models that decision is made from.  Reads only
 * g_config and g_secrets, so it builds and tests without the provider stack.
 */

#include "llm/llm_selection.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "config/dawn_config.h"
#include "dawn_error.h"
#include "utils/string_utils.h"

/* ---- provider names (declared in llm_interface.h) ---- */

const char *cloud_provider_to_string(cloud_provider_t provider) {
   switch (provider) {
      case CLOUD_PROVIDER_OPENAI:
         return "openai";
      case CLOUD_PROVIDER_CLAUDE:
         return "claude";
      case CLOUD_PROVIDER_GEMINI:
         return "gemini";
      case CLOUD_PROVIDER_OPENROUTER:
         return "openrouter";
      case CLOUD_PROVIDER_NONE:
      default:
         return "none";
   }
}

int cloud_provider_from_string(const char *str,
                               llm_type_t *type_out,
                               cloud_provider_t *provider_out) {
   if (str == NULL || type_out == NULL || provider_out == NULL) {
      return FAILURE;
   }

   /* "local"/"ollama" -> local server; the rest are cloud providers.  Accepts the
    * "anthropic" alias for Claude (kept for parity with resolve_silent_observe_config). */
   if (strcmp(str, "local") == 0 || strcmp(str, "ollama") == 0) {
      *type_out = LLM_LOCAL;
      *provider_out = CLOUD_PROVIDER_NONE;
   } else if (strcmp(str, "openai") == 0) {
      *type_out = LLM_CLOUD;
      *provider_out = CLOUD_PROVIDER_OPENAI;
   } else if (strcmp(str, "claude") == 0 || strcmp(str, "anthropic") == 0) {
      *type_out = LLM_CLOUD;
      *provider_out = CLOUD_PROVIDER_CLAUDE;
   } else if (strcmp(str, "gemini") == 0) {
      *type_out = LLM_CLOUD;
      *provider_out = CLOUD_PROVIDER_GEMINI;
   } else if (strcmp(str, "openrouter") == 0) {
      *type_out = LLM_CLOUD;
      *provider_out = CLOUD_PROVIDER_OPENROUTER;
   } else {
      return FAILURE; /* unknown -> leave outputs untouched */
   }

   return SUCCESS;
}

const char *cloud_provider_display_name(cloud_provider_t provider) {
   switch (provider) {
      case CLOUD_PROVIDER_OPENAI:
         return "OpenAI";
      case CLOUD_PROVIDER_CLAUDE:
         return "Claude";
      case CLOUD_PROVIDER_GEMINI:
         return "Gemini";
      case CLOUD_PROVIDER_OPENROUTER:
         return "OpenRouter";
      case CLOUD_PROVIDER_NONE:
      default:
         return "None";
   }
}

/* ---- default models (declared in llm_interface.h) ---- */

/* A provider's configured default model: its default index into its model list,
 * the first model when the index is out of range, the built-in model when the
 * list is empty.  Takes no lock; may be called with g_config_rwlock held for read
 * (llm_default_selection does). */
static const char *default_model(char models[][LLM_CLOUD_MODEL_NAME_MAX],
                                 int count,
                                 int idx,
                                 const char *builtin) {
   if (count <= 0) {
      return builtin;
   }
   if (idx < 0 || idx >= count) {
      idx = 0;
   }
   return models[idx];
}

const char *llm_get_default_openai_model(void) {
   return default_model(g_config.llm.cloud.openai_models, g_config.llm.cloud.openai_models_count,
                        g_config.llm.cloud.openai_default_model_idx, LLM_DEFAULT_OPENAI_MODEL);
}

const char *llm_get_default_claude_model(void) {
   return default_model(g_config.llm.cloud.claude_models, g_config.llm.cloud.claude_models_count,
                        g_config.llm.cloud.claude_default_model_idx, LLM_DEFAULT_CLAUDE_MODEL);
}

const char *llm_get_default_gemini_model(void) {
   return default_model(g_config.llm.cloud.gemini_models, g_config.llm.cloud.gemini_models_count,
                        g_config.llm.cloud.gemini_default_model_idx, LLM_DEFAULT_GEMINI_MODEL);
}

const char *llm_get_default_openrouter_model(void) {
   return default_model(g_config.llm.cloud.openrouter_models,
                        g_config.llm.cloud.openrouter_models_count,
                        g_config.llm.cloud.openrouter_default_model_idx,
                        LLM_DEFAULT_OPENROUTER_MODEL);
}

static const char *provider_default_model(cloud_provider_t provider) {
   switch (provider) {
      case CLOUD_PROVIDER_OPENAI:
         return llm_get_default_openai_model();
      case CLOUD_PROVIDER_CLAUDE:
         return llm_get_default_claude_model();
      case CLOUD_PROVIDER_GEMINI:
         return llm_get_default_gemini_model();
      case CLOUD_PROVIDER_OPENROUTER:
         return llm_get_default_openrouter_model();
      case CLOUD_PROVIDER_NONE:
      default:
         return "";
   }
}

static const char *builtin_url(cloud_provider_t provider) {
   switch (provider) {
      case CLOUD_PROVIDER_OPENAI:
         return CLOUDAI_URL;
      case CLOUD_PROVIDER_CLAUDE:
         return CLAUDE_URL;
      case CLOUD_PROVIDER_GEMINI:
         return GEMINI_URL;
      case CLOUD_PROVIDER_OPENROUTER:
         return OPENROUTER_URL;
      case CLOUD_PROVIDER_NONE:
      default:
         return "";
   }
}

const char *llm_cloud_url(cloud_provider_t provider) {
   const char *url = builtin_url(provider);
   pthread_rwlock_rdlock(&g_config_rwlock);
   llm_type_t type;
   cloud_provider_t configured;
   if (url[0] != '\0' && g_config.llm.cloud.endpoint[0] != '\0' &&
       cloud_provider_from_string(g_config.llm.cloud.provider, &type, &configured) == SUCCESS &&
       type == LLM_CLOUD && configured == provider) {
      url = g_config.llm.cloud.endpoint;
   }
   pthread_rwlock_unlock(&g_config_rwlock);
   return url;
}

/* ---- selection ---- */

bool llm_provider_has_key(cloud_provider_t provider) {
   switch (provider) {
      case CLOUD_PROVIDER_OPENAI:
         return g_secrets.openai_api_key[0] != '\0';
      case CLOUD_PROVIDER_CLAUDE:
         return g_secrets.claude_api_key[0] != '\0';
      case CLOUD_PROVIDER_GEMINI:
         return g_secrets.gemini_api_key[0] != '\0';
      case CLOUD_PROVIDER_OPENROUTER:
         return g_secrets.openrouter_api_key[0] != '\0';
      case CLOUD_PROVIDER_NONE:
      default:
         return false;
   }
}

void llm_default_selection(llm_selection_t *out) {
   memset(out, 0, sizeof(*out));
   pthread_rwlock_rdlock(&g_config_rwlock);
   llm_type_t named_type = LLM_LOCAL;
   cloud_provider_t provider = CLOUD_PROVIDER_NONE;
   /* Empty (not selected) or unknown names no provider; config_validate refuses
    * unknown names at load. */
   if (cloud_provider_from_string(g_config.llm.cloud.provider, &named_type, &provider) != SUCCESS ||
       named_type != LLM_CLOUD) {
      provider = CLOUD_PROVIDER_NONE;
   }
   if (strcmp(g_config.llm.type, "cloud") == 0 && provider != CLOUD_PROVIDER_NONE) {
      out->type = LLM_CLOUD;
      out->provider = provider;
      safe_strscpy(out->model, provider_default_model(provider));
   } else {
      out->type = LLM_LOCAL;
      out->provider = CLOUD_PROVIDER_NONE;
      safe_strscpy(out->model, g_config.llm.local.model);
   }
   pthread_rwlock_unlock(&g_config_rwlock);
}

void llm_selection_apply(const llm_selection_t *sel, session_llm_config_t *cfg) {
   cfg->type = sel->type;
   cfg->cloud_provider = sel->provider;
   safe_strscpy(cfg->model, sel->model);
}

void llm_selection_resolve(session_llm_config_t *cfg) {
   if (cfg->type != LLM_UNDEFINED) {
      return;
   }
   llm_selection_t sel;
   llm_default_selection(&sel);
   llm_selection_apply(&sel, cfg);
}

static int refuse(char *reason, size_t reason_len, const char *text) {
   if (reason && reason_len > 0) {
      snprintf(reason, reason_len, "%s", text);
   }
   return 1;
}

int llm_selection_problem(const session_llm_config_t *cfg, char *reason, size_t reason_len) {
   if (reason && reason_len > 0) {
      reason[0] = '\0';
   }
   if (!cfg) {
      return refuse(reason, reason_len, "No language model is configured.");
   }

   llm_type_t type = cfg->type;
   cloud_provider_t provider = cfg->cloud_provider;
   if (type == LLM_UNDEFINED) {
      llm_selection_t sel;
      llm_default_selection(&sel);
      type = sel.type;
      provider = sel.provider;
   }

   if (type == LLM_LOCAL) {
      return 0;
   }
   switch (provider) {
      case CLOUD_PROVIDER_OPENAI:
      case CLOUD_PROVIDER_CLAUDE:
      case CLOUD_PROVIDER_GEMINI:
      case CLOUD_PROVIDER_OPENROUTER:
         break;
      case CLOUD_PROVIDER_NONE:
      default: /* NONE, or a value from a corrupt stored row */
         return refuse(reason, reason_len,
                       "No cloud provider is selected. Choose one in Settings, under Language "
                       "Model.");
   }
   if (!llm_provider_has_key(provider)) {
      if (reason && reason_len > 0) {
         snprintf(reason, reason_len,
                  "%s is selected, but it has no API key. Add one in Settings, or pick "
                  "another provider.",
                  cloud_provider_display_name(provider));
      }
      return 1;
   }
   return 0;
}
