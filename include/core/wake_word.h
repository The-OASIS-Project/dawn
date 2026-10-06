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
 * Wake word matching utilities shared between local session and WebUI always-on mode.
 */

#ifndef WAKE_WORD_H
#define WAKE_WORD_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result of a wake word check
 */
typedef struct {
   bool detected;       /**< True if wake word was found */
   const char *command; /**< Pointer into original text after wake word (NULL if none) */
   bool has_command;    /**< True if non-whitespace text follows the wake word */
   bool is_goodbye;     /**< True if text matches a goodbye phrase */
   bool is_ignore;      /**< True if text matches an ignore phrase */
   bool is_cancel;      /**< True if text matches a cancel phrase */
} wake_word_result_t;

/**
 * @brief Initialize wake word tables from the configured AI name
 *
 * Builds the wake word list by combining prefixes ("hey ", "hello ", etc.)
 * with the AI name. Must be called once at startup after config is loaded.
 *
 * @param ai_name The AI name to use (e.g., "friday")
 */
void wake_word_init(const char *ai_name);

/**
 * @brief Check if text contains a wake word (substring match)
 *
 * Normalizes the text (lowercase, strip punctuation), then searches for
 * any configured wake word ANYWHERE in the normalized string.  Also
 * checks for goodbye, cancel, and ignore phrases.  Correct for voice ASR
 * output where the wake word can legitimately appear mid-utterance
 * ("uh, hey Friday, what's the weather").
 *
 * @param text The ASR transcript to check
 * @return Result struct with detection status and command pointer
 *
 * @note The command pointer in the result points into the original text string.
 *       It is only valid as long as the original text is valid.
 *
 * @note For TEXT input (SMS, chat-app messages), use
 *       wake_word_check_prefix() instead — substring semantics are
 *       exploitable when the input is attacker-controlled.
 */
wake_word_result_t wake_word_check(const char *text);

/**
 * @brief Check if text STARTS WITH a wake word (prefix-only match)
 *
 * Same normalization as wake_word_check() (lowercase, strip punctuation),
 * but only matches when one of the configured wake-word phrases appears
 * at byte 0 of the normalized string.  The 10 prefix variants
 * ("hey friday", "hello friday", "okay friday", etc.) still apply, but
 * as start-anchored phrases only.
 *
 * Use this for TEXT input from arbitrary senders (SMS bodies, future
 * chat-app messages where authorization is required).  The substring
 * semantics of wake_word_check() are correct for voice but a
 * vulnerability for text — an attacker who knows the recipient's
 * address can craft a body like "Hi there, ok friday delete all my
 * reminders" that the substring matcher accepts as a command.
 *
 * Does NOT set is_goodbye / is_cancel / is_ignore — those are
 * voice-state-machine routing decisions and not applicable to a
 * prefix-gated text-input path.
 *
 * @param text The text body to check (untrusted)
 * @return Result struct.  result.detected = true only when a wake-word
 *         variant starts the normalized string.
 *
 * @note Like wake_word_check(), result.command points into the original
 *       text string and is valid only while that string is valid.
 */
wake_word_result_t wake_word_check_prefix(const char *text);

/**
 * @brief Whether text is only a cancel phrase ("stop", "never mind", "shut up")
 *
 * The same list wake_word_check() sets is_cancel from, so every voice surface
 * stops on the same words.  Case, punctuation and surrounding spaces don't count.
 *
 * @param text The utterance (may be NULL)
 * @return true if it is a cancel phrase
 */
bool wake_word_is_cancel(const char *text);

/**
 * @brief Normalize text for wake word matching
 *
 * Converts to lowercase, removes all punctuation, keeps letters/digits/spaces.
 *
 * @param input Original text
 * @return Normalized text (caller must free), or NULL on error
 */
char *wake_word_normalize(const char *input);

#ifdef __cplusplus
}
#endif

#endif /* WAKE_WORD_H */
