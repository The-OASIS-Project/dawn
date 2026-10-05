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
 * Which vendor's rules a model follows, and its id in that vendor's own
 * spelling: the key every per-model models.toml table (pricing, reasoning) is
 * looked up by.  OpenRouter's "vendor/model" slugs route to the vendor.
 */

#ifndef LLM_MODEL_FAMILY_H
#define LLM_MODEL_FAMILY_H

#include <stddef.h>

#include "llm/llm_interface.h" /* llm_type_t, cloud_provider_t */

#ifdef __cplusplus
extern "C" {
#endif

/** A model's vendor family. */
typedef enum {
   LLM_FAMILY_LOCAL,     /**< A local model (llama.cpp, Ollama) */
   LLM_FAMILY_ANTHROPIC, /**< Claude, directly or through OpenRouter */
   LLM_FAMILY_OPENAI,    /**< OpenAI (and OpenAI-compatible endpoints) */
   LLM_FAMILY_GEMINI,    /**< Gemini, directly or through OpenRouter (google/) */
   LLM_FAMILY_OTHER,     /**< Another OpenRouter vendor */
} llm_model_family_t;

/**
 * @brief A model's vendor family and its id as that vendor spells it
 *
 * OpenRouter "anthropic/claude-opus-5.5" is ANTHROPIC "claude-opus-5-5":
 * the vendor prefix is dropped, and Anthropic's version dots (OpenRouter's
 * spelling) become dashes (Anthropic's).
 *
 * @param type LLM_LOCAL or LLM_CLOUD
 * @param provider The cloud provider serving it
 * @param model The model as configured (may be NULL)
 * @param id_out Receives the vendor id ("" for NULL)
 * @param id_len Size of id_out
 * @return The family
 */
llm_model_family_t llm_model_route(llm_type_t type,
                                   cloud_provider_t provider,
                                   const char *model,
                                   char *id_out,
                                   size_t id_len);

/**
 * @brief A model a Messages request names, as Anthropic spells it
 * An OpenRouter slug ("anthropic/claude-opus-5.5") becomes "claude-opus-5-5";
 * a first-party id is unchanged.  Anthropic's own ids never hold a '/'.
 * @return The provider the id was read under (OPENROUTER for a slug)
 */
cloud_provider_t llm_model_anthropic_id(const char *model, char *id_out, size_t id_len);

#ifdef __cplusplus
}
#endif

#endif /* LLM_MODEL_FAMILY_H */
