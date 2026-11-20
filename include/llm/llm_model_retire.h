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
 * Retired models: models.toml's [retired.<provider>] tables, each a model the
 * vendor shuts down and the model of the same vendor that replaces it, so a
 * conversation, a channel or a dawn.toml list naming a retired model keeps
 * working.  Loaded once at startup, read-only after.
 */

#ifndef LLM_MODEL_RETIRE_H
#define LLM_MODEL_RETIRE_H

#include <time.h>

#include "llm/llm_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

struct toml_table_t;

/** Room for a retired or replacement model id (what a request can name). */
#define LLM_RETIRE_MODEL_MAX LLM_MODEL_NAME_MAX

/**
 * @brief Load the [retired] tables from a models.toml root (NULL: none)
 *
 * Each row: `"<model>" = { by = "<model>", on = "YYYY-MM-DD" }` under
 * [retired.anthropic], [retired.openai], [retired.gemini] or [retired.openrouter].
 * Without `on` the model is retired now.  A row whose replacement names another
 * vendor (an OpenRouter "vendor/model" id) is ignored with a warning: DAWN never
 * moves a conversation to a vendor nobody chose.  Not thread-safe: call once at
 * startup, before any request.
 *
 * @param root A parsed models.toml (the root table), or NULL to clear the rows
 */
void llm_model_retire_load(struct toml_table_t *root);

/**
 * @brief The model that replaces @p model when it is retired at @p now
 *
 * A row matches the model id itself or a snapshot of it: the id, '-', then only
 * digits and dashes starting with at least three digits ("claude-haiku-4-5-20251001",
 * "gpt-4-0613"), so "claude-opus-4" never matches "claude-opus-4-8".  A
 * replacement that is itself retired is followed (a few steps at most).
 *
 * @param provider The provider the model is called on
 * @param model    The model id the request would name
 * @param now      The time to judge retirement dates against
 * @return The replacement (static storage), or NULL when @p model isn't retired
 */
const char *llm_model_replacement(cloud_provider_t provider, const char *model, time_t now);

#ifdef __cplusplus
}
#endif

#endif /* LLM_MODEL_RETIRE_H */
