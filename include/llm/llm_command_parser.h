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
 */

#include <stdbool.h>
#include <stddef.h>

#ifndef LLM_COMMAND_PARSER_H
#define LLM_COMMAND_PARSER_H

/* Header text for the TOOL DEFAULTS line (llm_command_parser.c):
 * the configured location / units / timezone, for a caller with no user
 * settings of its own. */
#define TOOL_DEFAULTS_HEADER_TEXT \
   "TOOL DEFAULTS (for tool calls only, do not mention in conversation):"

/** The command prompt's pieces, each a private copy. */
typedef struct {
   char *persona;       /**< who the assistant is */
   char *rules;         /**< system instructions (get_system_instructions) */
   char *tool_defaults; /**< the TOOL DEFAULTS line, "" when nothing is configured */
} command_prompt_parts_t;

/**
 * @brief The pieces the command prompt joins, for a builder that sends them
 *        as sections of their own
 *
 * @return 0, or 1 on allocation failure (@p out then empty)
 */
int get_command_prompt_parts(command_prompt_parts_t *out);

/** Free a command_prompt_parts_t's copies and zero it.  NULL-safe. */
void command_prompt_parts_free(command_prompt_parts_t *parts);

/**
 * @brief The command prompt every surface starts from: persona, system
 *        instructions, localization (a private copy: caller frees; NULL on OOM)
 *
 * Surface-neutral: what differs by surface (voice output, ASR hints, a room,
 * a channel) reaches the model as standing directions, so a conversation
 * keeps one system prompt wherever it continues.  The persona and
 * localization are built on each call from the config; the rules are cached
 * until invalidate_system_instructions().
 */
char *get_command_prompt_dup(void);

/**
 * @brief Builds dynamic system instructions based on enabled features
 *
 * Assembles core rules plus feature-specific rules based on config settings.
 * Only includes instructions for features that are actually enabled:
 * - Vision: Requires vision_enabled for current LLM type (cloud or local)
 * - Search: Requires SearXNG endpoint configured
 * - Weather/Calculator/URL: Always available
 *
 * @return Pointer to static buffer containing assembled instructions
 */
const char *get_system_instructions(void);


/**
 * @brief Checks if vision is enabled for the current LLM type
 *
 * Vision availability is controlled by the vision_enabled setting for the
 * current LLM type (cloud or local). Use this at command execution time
 * to check if vision can be processed.
 *
 * @return 1 if vision is available for current LLM, 0 otherwise
 */
int is_vision_enabled_for_current_llm(void);

/**
 * @brief Invalidates cached system instructions, forcing rebuild on next call
 *
 * Call this when capabilities change at runtime (e.g., a tool's auth
 * status changes, devices are loaded, etc.) so the next call to
 * get_system_instructions() rebuilds the prompt with updated capabilities.
 */
void invalidate_system_instructions(void);

/**
 * @brief Write the default persona (AI_PERSONA_TEMPLATE, prompts.h) into @p out
 *
 * Uses general.ai_name (AI_NAME when unset), first letter capitalized.  Not the
 * effective persona: a [persona] description, when set, replaces this.  Shown by
 * the settings panels; the prompt build uses the same text.
 *
 * @param out  Buffer to write into (always NUL-terminated)
 * @param size Size of @p out
 */
void llm_persona_default(char *out, size_t size);

/**
 * @brief Write the persona every prompt opens with into @p out
 *
 * The [persona] description when set, else llm_persona_default().  Built on
 * each call, never cached, so a new description reaches the next prompt.
 *
 * @param out  Buffer to write into, CONFIG_DESCRIPTION_MAX for no truncation
 * @param size Size of @p out
 */
void llm_persona_effective(char *out, size_t size);

/* =============================================================================
 * Voice-session prompt directives (effective-value accessors)
 *
 * Each returns the configured directive text ([tts]/[asr] in dawn.toml) when
 * set, else the compile-time built-in default (prompts.h).  The prompt-build path
 * injects these only on the relevant voice surfaces:
 *   - voice_directive:            satellites (DAP2/DAP) + local mic (spoken out)
 *   - voice_directive_webui:      WebUI voice turns (softer; screen leeway)
 *   - asr_disambiguation_hint:    any voice-input turn (speech-transcribed input)
 * Returned pointer is valid until the next config change; treat as read-only.
 * ============================================================================= */
const char *voice_directive_effective(void);
const char *voice_directive_webui_effective(void);
const char *asr_disambiguation_hint_effective(void);

#endif  // LLM_COMMAND_PARSER_H
