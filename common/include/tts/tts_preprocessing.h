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

/**
 * @file tts_preprocessing.h
 * @brief Text preprocessing utilities for TTS (Text-to-Speech)
 *
 * This module provides text transformations to improve TTS speech quality:
 * - Emoji removal for clean speech output
 * - Character filtering (asterisks, etc.)
 * - Em-dash to comma replacement for proper pauses
 * - Temperature unit expansion (°F -> "degrees Fahrenheit")
 * - US state abbreviation expansion (CA -> California)
 * - Day of week abbreviation expansion (Mon -> Monday)
 * - Month abbreviation expansion (Jan -> January)
 *
 * The module provides both C-compatible functions for in-place string
 * manipulation and C++ std::string functions for more complex transformations.
 *
 * @note C code should use preprocess_text_for_tts_c() for full preprocessing.
 * @note C++ code can use either the C wrapper or preprocess_text_for_tts().
 */

#ifndef TTS_PREPROCESSING_H
#define TTS_PREPROCESSING_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Removes all occurrences of specified characters from a string.
 *
 * This function modifies the input string in place by removing any characters
 * that are present in the chars_to_remove string.
 *
 * @param str The string to be modified (must be mutable)
 * @param chars_to_remove Characters to remove from str
 */
void remove_chars(char *str, const char *chars_to_remove);

/**
 * @brief Checks if a Unicode code point represents an emoji character.
 *
 * Covers emoticons, symbols, pictographs, variation selectors, zero-width
 * joiners, regional indicators (flags), and extended emoji ranges.
 *
 * @param codepoint The Unicode code point to check
 * @return true if the code point is an emoji or emoji-related character
 */
bool is_emoji(unsigned int codepoint);

/**
 * @brief Removes emoji characters from a UTF-8 string in place.
 *
 * Properly decodes UTF-8 sequences and removes emoji characters while
 * preserving other multi-byte characters. Includes validation to handle
 * malformed/truncated UTF-8 sequences safely.
 *
 * @param str The null-terminated UTF-8 string to process (modified in place)
 */
void remove_emojis(char *str);

/**
 * @brief Full TTS preprocessing pipeline (C-compatible wrapper)
 *
 * Performs all text transformations needed for clean TTS output:
 * 1. Removes asterisks (markdown bold markers)
 * 2. Removes emoji characters
 * 3. Replaces em-dashes with commas for proper pauses
 * 4. Expands temperature units to spoken words
 * 5. Expands US state abbreviations to full names
 * 6. Expands day and month abbreviations to full names
 *
 * @param input The null-terminated input string
 * @param output Buffer to receive preprocessed text (must be large enough)
 * @param output_size Size of the output buffer
 * @return Number of bytes written (excluding null terminator), or -1 on error
 *
 * @note Output buffer should be at least 2x input size to accommodate expansions
 */
int preprocess_text_for_tts_c(const char *input, char *output, size_t output_size);

#ifdef __cplusplus
}

// C++ only: std::string-based preprocessing functions
#include <string>

/**
 * @brief Preprocess text for TTS to improve speech quality
 *
 * Performs the following transformations in a single optimized pass:
 * - Removes asterisks (markdown bold markers)
 * - Removes emoji characters
 * - Replaces em-dashes (—) with commas for proper pauses
 * - Converts temperature units (°F, °C, °K) to spoken words
 * - Expands US state abbreviations to full names
 * - Expands day of week abbreviations (Mon, Tue, etc.)
 * - Expands month abbreviations (Jan, Feb, etc.)
 *
 * Uses a two-pass architecture: Pass 1 calculates exact output size,
 * Pass 2 generates output into a single preallocated buffer.
 *
 * @param input The text to preprocess
 * @return Preprocessed text optimized for TTS
 */
std::string preprocess_text_for_tts(const std::string &input);

#endif  // __cplusplus

#endif  // TTS_PREPROCESSING_H
