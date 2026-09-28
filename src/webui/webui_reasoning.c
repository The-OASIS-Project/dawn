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
 * Reasoning settings for the clients.  See webui_reasoning.h.
 */

#include "webui/webui_reasoning.h"

#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

#include "config/dawn_config.h"
#include "llm/llm_capabilities.h"
#include "utils/string_utils.h"
#include "webui/webui_internal.h"

static void add_provider(json_object *map,
                         cloud_provider_t provider,
                         const char (*models)[LLM_CLOUD_MODEL_NAME_MAX],
                         int count) {
   json_object *by_model = json_object_new_object();
   for (int i = 0; i < count; i++) {
      if (!models[i][0]) {
         continue;
      }
      llm_thinking_caps_t caps;
      llm_thinking_caps(LLM_CLOUD, provider, models[i], &caps);
      json_object *entry = llm_thinking_caps_to_json(&caps);
      if (entry) {
         json_object_object_add(by_model, models[i], entry);
      }
   }
   json_object_object_add(map, cloud_provider_to_string(provider), by_model);
}

json_object *webui_reasoning_cloud_capabilities(void) {
   const llm_cloud_config_t *cloud = &g_config.llm.cloud;
   json_object *map = json_object_new_object();
   add_provider(map, CLOUD_PROVIDER_OPENAI, cloud->openai_models, cloud->openai_models_count);
   add_provider(map, CLOUD_PROVIDER_CLAUDE, cloud->claude_models, cloud->claude_models_count);
   add_provider(map, CLOUD_PROVIDER_GEMINI, cloud->gemini_models, cloud->gemini_models_count);
   add_provider(map, CLOUD_PROVIDER_OPENROUTER, cloud->openrouter_models,
                cloud->openrouter_models_count);
   return map;
}

void webui_reasoning_add_local(json_object *model_entry, const char *model) {
   llm_thinking_caps_t caps;
   llm_thinking_caps(LLM_LOCAL, CLOUD_PROVIDER_NONE, model, &caps);
   json_object *entry = llm_thinking_caps_to_json(&caps);
   if (entry) {
      json_object_object_add(model_entry, "reasoning_capabilities", entry);
   }
}

/* The config's effective model (session override, else the provider default),
 * its capabilities, and the pick resolved against them. */
static void resolve_config(const session_llm_config_t *cfg,
                           llm_resolved_config_t *resolved,
                           llm_thinking_caps_t *caps,
                           llm_thinking_resolved_t *out) {
   llm_resolve_config(cfg, resolved);
   llm_thinking_caps(resolved->type, resolved->cloud_provider, webui_effective_model_name(resolved),
                     caps);
   llm_thinking_resolve(caps, resolved->thinking_mode, resolved->reasoning_effort, false, out);
}

/* The effort to show and store: what's sent, or, while reasoning is off (or
 * takes no effort), the pick, kept for when it's turned on. */
static const char *shown_effort(const llm_thinking_resolved_t *r, const char *picked) {
   return r->effort[0] ? r->effort : picked;
}

void webui_reasoning_stamp(json_object *obj, const session_llm_config_t *cfg) {
   llm_resolved_config_t resolved = { 0 };
   llm_thinking_caps_t caps;
   llm_thinking_resolved_t r;
   resolve_config(cfg, &resolved, &caps, &r);
   /* The stored pick: what a client restores and locks with (only the user
    * changes it), beside what the model is actually sent. */
   json_object_object_add(obj, "thinking_mode_pick",
                          json_object_new_string(resolved.thinking_mode));
   json_object_object_add(obj, "reasoning_effort_pick",
                          json_object_new_string(resolved.reasoning_effort));
   json_object_object_add(obj, "thinking_mode",
                          json_object_new_string(llm_think_mode_name(r.mode)));
   json_object_object_add(obj, "reasoning_effort",
                          json_object_new_string(shown_effort(&r, resolved.reasoning_effort)));
   json_object *entry = llm_thinking_caps_to_json(&caps);
   if (entry) {
      json_object_object_add(obj, "reasoning_capabilities", entry);
   }
   json_object_object_add(obj, "reasoning_adjusted", json_object_new_boolean(r.clamped));
}

bool webui_reasoning_adjusted(const session_llm_config_t *cfg,
                              bool mode_changed,
                              bool effort_changed) {
   llm_resolved_config_t resolved = { 0 };
   llm_thinking_caps_t caps;
   llm_thinking_resolved_t r;
   resolve_config(cfg, &resolved, &caps, &r);
   return r.controllable &&
          ((mode_changed && r.mode_clamped) || (effort_changed && r.effort_clamped));
}
