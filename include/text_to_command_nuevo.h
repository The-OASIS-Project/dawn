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
 *
 * Text-to-command processing using tool_registry pattern matching.
 * The legacy JSON-based command configuration has been removed;
 * all patterns are now defined in compile-time tool metadata.
 */

#ifndef TEXT_TO_COMMAND_H
#define TEXT_TO_COMMAND_H

#include <stdbool.h>
#include <stddef.h> /* For size_t */

/* Buffer size constants used for command processing */
#define MAX_WORD_LENGTH 256
#define MAX_COMMAND_LENGTH 512

/**
 * @brief Normalizes text for command matching.
 *
 * Converts input to lowercase and strips leading/trailing punctuation and whitespace.
 * This handles Whisper ASR output which includes capitalization and punctuation
 * that would otherwise prevent matching against lowercase command patterns.
 *
 * Example: "Turn on the lights." -> "turn on the lights"
 *
 * @param input  The input string to normalize (e.g., Whisper transcription).
 * @param output Buffer to store the normalized string.
 * @param size   Size of the output buffer.
 *
 * @note Output buffer must be at least as large as input to avoid truncation.
 */
void normalize_for_matching(const char *input, char *output, size_t size);

/**
 * @brief Try to match input against tool_registry tools using device_types patterns
 *
 * Iterates through all registered tools and tries to match the input against
 * their device type patterns. This allows direct command matching using
 * compile-time defined patterns instead of JSON.
 *
 * @param input       Normalized input text (lowercase, trimmed)
 * @param out_command Buffer to receive the command JSON on match
 * @param command_size Size of out_command buffer
 * @param out_topic   Buffer to receive the MQTT topic on match (can be NULL)
 * @param topic_size  Size of out_topic buffer
 * @param out_secret  Optional: set when the matched value is a passphrase, which
 *                    the caller keeps out of the history
 * @return 1 if matched, 0 if no match
 */
int try_tool_registry_match(const char *input,
                            char *out_command,
                            size_t command_size,
                            char *out_topic,
                            size_t topic_size,
                            bool *out_secret);

#endif /* TEXT_TO_COMMAND_H */
