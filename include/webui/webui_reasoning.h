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
 * Reasoning settings as the clients see them: every model's capabilities, a
 * conversation's effective mode and effort (what its model is actually sent),
 * and settling a picked setting against the model when it's saved.
 */

#ifndef WEBUI_REASONING_H
#define WEBUI_REASONING_H

#include <stdbool.h>

#include "llm/llm_interface.h" /* session_llm_config_t */

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;
struct session;

/**
 * @brief Capabilities of every configured cloud model
 *
 * { provider: { model: caps } } over [llm.cloud] openai/claude/gemini/openrouter
 * model lists, the provider keys as cloud_provider_to_string() spells them and
 * the model keys exactly as listed.  Caps: llm_thinking_caps_to_json().
 *
 * @return A new object (caller owns it)
 */
struct json_object *webui_reasoning_cloud_capabilities(void);

/**
 * @brief Add a model's capabilities to a local model entry
 *
 * The local provider decides them, the same for every local model.
 */
void webui_reasoning_add_local(struct json_object *model_entry, const char *model);

/**
 * @brief Stamp a frame with a session config's effective reasoning
 *
 * Sets "thinking_mode" and "reasoning_effort" to what the model is actually
 * sent (the picked effort is kept while reasoning is off, ready for when it's
 * turned on), "thinking_mode_pick" / "reasoning_effort_pick" to the stored
 * pick (what a client restores and locks with), and adds
 * "reasoning_capabilities" (the model's caps) and "reasoning_adjusted" (the
 * pick differs from what's sent).
 */
void webui_reasoning_stamp(struct json_object *obj, const session_llm_config_t *cfg);

/**
 * @brief Whether a picked reasoning setting will be adjusted for its model
 *
 * The pick itself is what's stored (so switching models loses nothing); each
 * request resolves it against its model, and the frames show the result.
 * This only reports whether the part the user just changed differs from what
 * will be sent (@p mode_changed / @p effort_changed: which fields they sent),
 * for telling them.
 */
bool webui_reasoning_adjusted(const session_llm_config_t *cfg,
                              bool mode_changed,
                              bool effort_changed);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_REASONING_H */
