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
 * Model vendor routing for the per-model models.toml tables.
 */

#include "llm/llm_model_family.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Anthropic's own ids spell versions with dashes (claude-opus-5-5); OpenRouter
 * with dots (anthropic/claude-opus-5.5).  Copy @p model the Anthropic way. */
static void anthropic_spelling(const char *model, char *out, size_t len) {
   size_t n = 0;
   for (; model[n] && n + 1 < len; n++) {
      const bool version_dot = model[n] == '.' && n > 0 && model[n - 1] >= '0' &&
                               model[n - 1] <= '9' && model[n + 1] >= '0' && model[n + 1] <= '9';
      out[n] = version_dot ? '-' : model[n];
   }
   out[n] = '\0';
}

llm_model_family_t llm_model_route(llm_type_t type,
                                   cloud_provider_t provider,
                                   const char *model,
                                   char *id_out,
                                   size_t id_len) {
   if (id_len == 0) {
      return LLM_FAMILY_OTHER;
   }
   id_out[0] = '\0';
   const char *id = model ? model : "";
   if (type == LLM_LOCAL) {
      snprintf(id_out, id_len, "%s", id);
      return LLM_FAMILY_LOCAL;
   }
   llm_model_family_t family = LLM_FAMILY_OTHER;
   switch (provider) {
      case CLOUD_PROVIDER_CLAUDE:
         family = LLM_FAMILY_ANTHROPIC;
         break;
      case CLOUD_PROVIDER_OPENAI:
         family = LLM_FAMILY_OPENAI;
         break;
      case CLOUD_PROVIDER_GEMINI:
         family = LLM_FAMILY_GEMINI;
         break;
      case CLOUD_PROVIDER_OPENROUTER: {
         const char *slash = strchr(id, '/');
         const size_t vlen = slash ? (size_t)(slash - id) : 0;
         if (vlen == 9 && strncmp(id, "anthropic", 9) == 0) {
            family = LLM_FAMILY_ANTHROPIC;
         } else if (vlen == 6 && strncmp(id, "openai", 6) == 0) {
            family = LLM_FAMILY_OPENAI;
         } else if (vlen == 6 && strncmp(id, "google", 6) == 0) {
            family = LLM_FAMILY_GEMINI;
         }
         if (family != LLM_FAMILY_OTHER) {
            id = slash + 1;
         }
         break;
      }
      default:
         break;
   }
   if (family == LLM_FAMILY_ANTHROPIC) {
      anthropic_spelling(id, id_out, id_len);
   } else {
      snprintf(id_out, id_len, "%s", id);
   }
   return family;
}

cloud_provider_t llm_model_anthropic_id(const char *model, char *id_out, size_t id_len) {
   const cloud_provider_t provider = model && strchr(model, '/') ? CLOUD_PROVIDER_OPENROUTER
                                                                 : CLOUD_PROVIDER_CLAUDE;
   llm_model_route(LLM_CLOUD, provider, model, id_out, id_len);
   return provider;
}
