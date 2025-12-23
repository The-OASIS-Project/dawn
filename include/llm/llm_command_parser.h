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

#include <mosquitto.h> /* Needed got struct mosquitto */
#include <stdbool.h>

#ifndef LLM_COMMAND_PARSER_H
#define LLM_COMMAND_PARSER_H

// Function to build local command prompt from commands_config_nuevo.json
// For local microphone interface - includes all commands (HUD, helmet, general)
const char *get_local_command_prompt(void);

// Function to build remote command prompt (excludes local-only topics: hud, helmet)
// For network satellite clients (DAP/DAP2) - includes general commands like date, time
const char *get_remote_command_prompt(void);

// Function to parse LLM responses for commands
int parse_llm_response_for_commands(const char *llm_response, struct mosquitto *mosq);

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
 * Call this when capabilities change at runtime (e.g., SmartThings
 * authenticates, devices are loaded, etc.) so the next call to
 * get_system_instructions() rebuilds the prompt with updated capabilities.
 */
void invalidate_system_instructions(void);

#endif  // LLM_COMMAND_PARSER_H
