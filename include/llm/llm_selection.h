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
 * Which LLM a chat runs on, and whether it can.  A cloud API key is used only for
 * the provider the user selected: DAWN never substitutes another vendor or guesses
 * one, and when a selection can't run, the user is told why.
 */

#ifndef LLM_SELECTION_H
#define LLM_SELECTION_H

#include <stdbool.h>
#include <stddef.h>

#include "llm/llm_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Room for llm_selection_problem()'s sentence (the longest is under 100 bytes). */
#define LLM_SELECTION_REASON_MAX 128

/** The LLM a chat runs on. */
typedef struct {
   llm_type_t type;           /**< LLM_LOCAL or LLM_CLOUD */
   cloud_provider_t provider; /**< The cloud provider; NONE when local */
   /** The provider's default model, or for local the configured `[llm.local] model`
    *  ("" = whatever the local server loaded) */
   char model[LLM_MODEL_NAME_MAX];
} llm_selection_t;

/**
 * @brief The default selection, from dawn.toml.
 *
 * `[llm] type = "local"` → local.  `"cloud"` with a provider named in
 * `[llm.cloud] provider` → that provider and its default model.  `"cloud"` with no
 * provider selected (empty) → local: no key is used until the user picks a
 * provider.  Never looks at keys.  Reads type, provider and model as one snapshot
 * under the config read lock, so a settings save can't be seen half applied; must
 * not be called with g_config_rwlock held.
 *
 * @param[out] out The default selection
 */
void llm_default_selection(llm_selection_t *out);

/**
 * @brief Point a (resolved) config at a type and provider someone asked for
 *
 * Cloud with no provider named is the config's own cloud provider, else the one
 * `[llm.cloud] provider` names (NONE when it names none, which then refuses).
 * Local drops the provider.  A changed type or provider clears the model (the new
 * provider's default), so one vendor's model id never goes to another.  Must not
 * be called with g_config_rwlock held.
 */
void llm_selection_set_target(session_llm_config_t *cfg,
                              llm_type_t type,
                              cloud_provider_t provider);

/**
 * @brief Put a selection into a session config.
 *
 * Sets type, provider and model; leaves reasoning, endpoint and the rest as they are.
 */
void llm_selection_apply(const llm_selection_t *sel, session_llm_config_t *cfg);

/**
 * @brief Make a config concrete: LLM_UNDEFINED becomes the default selection.
 *
 * Type, provider and model come from llm_default_selection(); reasoning, endpoint
 * and the rest are kept.  A config with a type is left as it is.  Must not be
 * called with g_config_rwlock held.
 */
void llm_selection_resolve(session_llm_config_t *cfg);

/**
 * @brief A conversation's stored LLM columns as a session config.
 *
 * No type stored (or cloud with no provider it names, nor a model naming one) is
 * no selection of its own: LLM_UNDEFINED, which follows the default.  The
 * conversation's reasoning is kept either way, else the configured default's.
 * Arguments may be NULL or empty.  Takes g_config_rwlock (read).
 */
void llm_selection_from_row(const char *llm_type,
                            const char *provider,
                            const char *model,
                            const char *thinking_mode,
                            const char *reasoning_effort,
                            session_llm_config_t *out);

/**
 * @brief Whether a config can run, and if not, why.
 *
 * Local runs (whether the local server answers is found out when it's called).
 * LLM_UNDEFINED is the default selection, read at the time of the call: a caller
 * that will act on the answer should pass the config it resolved, so the check and
 * the request see the same selection.  A cloud provider needs its own API key; cloud
 * with no provider, or a provider value DAWN doesn't know, is refused.  Must not be
 * called with g_config_rwlock held.
 *
 * @param cfg        The config to check
 * @param[out] reason A sentence for the user (names the provider, never a key);
 *                    may be NULL
 * @param reason_len Size of @p reason
 * @return 0 when it can run, 1 when it can't
 */
int llm_selection_problem(const session_llm_config_t *cfg, char *reason, size_t reason_len);

/**
 * @brief The cloud provider whose dawn.toml model list names @p model
 *
 * Decides from configuration only (never the local server's list, which may not
 * have been fetched): NONE for a model no cloud list names, such as a local
 * server's gpt-oss.  Must not be called with g_config_rwlock held.
 */
cloud_provider_t llm_configured_model_provider(const char *model);

/**
 * @brief Whether @p provider has an API key in secrets.toml (or the environment).
 *
 * For showing which providers can be picked; never for choosing one.
 */
bool llm_provider_has_key(cloud_provider_t provider);

/**
 * @brief The URL @p provider's requests go to.
 *
 * `[llm.cloud] endpoint` when it belongs to @p provider (it's set and
 * `[llm.cloud] provider` names @p provider), else the provider's own URL; "" for
 * NONE.  So one provider's key never goes to a host set up for another.  The two
 * settings are read as one snapshot under the config read lock; must not be
 * called with g_config_rwlock held.
 *
 * @return Static storage (a built-in URL, or the configured endpoint in g_config,
 *         as the local endpoint is used): the pointer outlives any caller; a
 *         settings save can still rewrite the endpoint's text under it
 */
const char *llm_cloud_url(cloud_provider_t provider);

/** @brief @p provider's own URL, ignoring [llm.cloud] endpoint ("" for none). */
const char *llm_builtin_url(cloud_provider_t provider);

/**
 * @brief The provider's name for the user ("OpenAI", "Claude", "Gemini",
 *        "OpenRouter"; "None" otherwise).
 */
const char *cloud_provider_display_name(cloud_provider_t provider);

#ifdef __cplusplus
}
#endif

#endif /* LLM_SELECTION_H */
