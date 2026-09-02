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

#ifndef LLM_COMMAND_PARSER_H
#define LLM_COMMAND_PARSER_H

/* Header text for the TOOL DEFAULTS line emitted by get_localization_context()
 * and consumed by webui_auth_helpers.c::strip_tool_defaults() when User Context
 * supersedes it.  Producer and consumer share this single source of truth so a
 * future edit to the wording doesn't silently break the strip-on-authenticated
 * path. */
#define TOOL_DEFAULTS_HEADER_TEXT \
   "TOOL DEFAULTS (for tool calls only, do not mention in conversation):"

// Function to build local command prompt from commands_config_nuevo.json
// For local microphone interface - includes all commands (HUD, helmet, general)
const char *get_local_command_prompt(void);

// Function to build remote command prompt (excludes local-only topics: hud, helmet)
// For network satellite clients (DAP/DAP2) - includes general commands like date, time
const char *get_remote_command_prompt(void);

/**
 * @brief Builds dynamic system instructions based on enabled features
 *
 * Assembles core rules plus feature-specific rules based on config settings.
 * Only includes instructions for features that are actually enabled:
 * - Vision: Requires vision_enabled for current LLM type (cloud or local)
 * - Search: Requires SearXNG endpoint configured
 * - Weather/Calculator/URL: Always available
 *
 * The disabled-tool hint appended to the instructions is session-aware so
 * local and remote prompts correctly describe their available capabilities.
 *
 * @param is_remote true for a remote-session prompt, false for local
 * @return Pointer to static buffer containing assembled instructions
 */
const char *get_system_instructions(bool is_remote);

/**
 * @brief Get the current system-instructions version counter
 *
 * Monotonically increases each time invalidate_system_instructions() is called.
 * Consumers that build derived prompts (e.g., direct-mode prompt) can compare
 * against this to decide whether to rebuild.
 */
int get_system_instructions_version(void);

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

/* =============================================================================
 * Voice-session prompt directives (effective-value accessors)
 *
 * Each returns the configured directive text ([tts]/[asr] in dawn.toml) when
 * set, else the compile-time built-in default (dawn.h).  The prompt-build path
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
