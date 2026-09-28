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
 * Per-model reasoning capabilities (models.toml [thinking.<provider>]): which
 * reasoning modes a model accepts and its effort levels, and the one resolver
 * every request builder and client-facing frame uses to turn a requested
 * mode/effort into one the model can honor.
 */

#ifndef LLM_CAPABILITIES_H
#define LLM_CAPABILITIES_H

#include <stdbool.h>

#include "llm/llm_interface.h" /* llm_type_t, cloud_provider_t */

#ifdef __cplusplus
extern "C" {
#endif

struct toml_table_t;

/** A reasoning mode. */
typedef enum {
   LLM_THINK_DISABLED = 0, /**< No reasoning (Claude "disabled"; OpenAI effort "none") */
   LLM_THINK_ADAPTIVE,     /**< Claude adaptive thinking, sized by effort */
   LLM_THINK_ENABLED,      /**< Claude/local: fixed budget; OpenAI/Gemini: reasoning at an effort */
} llm_think_mode_t;

#define LLM_THINK_MODES_MAX 3
#define LLM_EFFORTS_MAX 7
#define LLM_EFFORT_NAME_MAX 8

/** One mode a model accepts, with the effort levels that apply in it. */
typedef struct {
   llm_think_mode_t mode;
   int effort_count;                                   /**< 0: no effort choice in this mode */
   char efforts[LLM_EFFORTS_MAX][LLM_EFFORT_NAME_MAX]; /**< Lowest first */
   bool budget; /**< The efforts pick one of DAWN's [llm.thinking] budget_* sizes */
} llm_think_mode_caps_t;

/** Where a model's capabilities come from. */
typedef enum {
   LLM_CAPS_ROW,              /**< Its models.toml [thinking.<provider>] row */
   LLM_CAPS_PROVIDER_DEFAULT, /**< No row: its provider's conservative default */
   LLM_CAPS_LOCAL,            /**< A local model: the local provider's rules */
} llm_caps_source_t;

/** A model's reasoning capabilities. */
typedef struct {
   llm_caps_source_t source;
   int mode_count;                                   /**< 0: the model has no reasoning control */
   llm_think_mode_caps_t modes[LLM_THINK_MODES_MAX]; /**< In display order */
} llm_thinking_caps_t;

/** What a request actually sends. */
typedef struct {
   bool controllable; /**< The model has reasoning control (else send nothing) */
   llm_think_mode_t mode;
   char effort[LLM_EFFORT_NAME_MAX]; /**< "" when the mode takes none */
   bool budget;                      /**< effort names a budget size (Claude/local enabled) */
   bool clamped;                     /**< The request asked for something the model can't do */
} llm_thinking_resolved_t;

/**
 * @brief Load the [thinking.<provider>] tables from models.toml
 *
 * Called once at startup with the parsed file (llm_context owns the parse); a
 * missing table leaves that provider on its defaults.  Read-only afterwards.
 */
void llm_capabilities_load_registry(struct toml_table_t *root);

/** Free what llm_capabilities_load_registry() loaded. */
void llm_capabilities_free_registry(void);

/**
 * @brief A model's reasoning capabilities
 *
 * Its longest-prefix models.toml row, else its provider's default: for Claude,
 * adaptive at low/medium/high (no "disabled"); for OpenAI and Gemini, no
 * reasoning control (only listed models get reasoning parameters).  An
 * OpenRouter model ("vendor/model") uses its vendor's table; another vendor's
 * gets "disabled" + "enabled".  A local model's capabilities come from the
 * local provider in use: llama.cpp off or a budget, Ollama think on or off.
 */
void llm_thinking_caps(llm_type_t type,
                       cloud_provider_t provider,
                       const char *model,
                       llm_thinking_caps_t *out);

/**
 * @brief Resolve a requested mode and effort against a model's capabilities
 *
 * "disabled" becomes the model's off switch, or, on a model that can't turn
 * reasoning off, its first reasoning mode at its lowest effort.  Any other
 * mode ("adaptive", "enabled", or the legacy "auto") becomes that mode if the
 * model has it, else the model's first reasoning mode.  An effort the mode
 * doesn't offer snaps to the nearest one it does.  A utility call (compaction,
 * summaries: DAWN's own bookkeeping) gets the cheapest legal setting.
 *
 * @param caps The model's capabilities
 * @param mode Requested mode (NULL or "" = "disabled")
 * @param effort Requested effort (NULL or "" = "medium")
 * @param utility A utility call: the cheapest legal setting, never a clamp
 * @param out The setting to send
 */
void llm_thinking_resolve(const llm_thinking_caps_t *caps,
                          const char *mode,
                          const char *effort,
                          bool utility,
                          llm_thinking_resolved_t *out);

/**
 * @brief Resolve the current call's reasoning setting for a model
 *
 * The session's thinking mode and effort (llm_get_current_thinking_mode /
 * llm_get_current_reasoning_effort) against the model's capabilities, as a
 * utility call when tools are suppressed for it.  What every request builder
 * sends.
 */
void llm_thinking_resolve_current(llm_type_t type,
                                  cloud_provider_t provider,
                                  const char *model,
                                  llm_thinking_resolved_t *out);

/**
 * @brief The thinking budget a budget level names
 *
 * "low" | "medium" | "high" | "xhigh": the [llm.thinking] budget_* sizes (the
 * levels of a Claude or llama.cpp "enabled" mode).  Anything else is medium.
 */
int llm_thinking_budget_size(const char *level);

/** The wire name of a mode ("disabled", "adaptive", "enabled"). */
const char *llm_think_mode_name(llm_think_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* LLM_CAPABILITIES_H */
