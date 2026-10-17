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
 * Memory Context API
 *
 * Builds memory context blocks to inject into LLM system prompts.
 */

#ifndef MEMORY_CONTEXT_H
#define MEMORY_CONTEXT_H

#include <stddef.h>

#include "core/tz.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build the USER MEMORY block for the cached stable prefix.
 *
 * Constructs a formatted string containing the user's preferences and
 * recent conversation summaries — both session-stable (preferences
 * change only on explicit edits; summaries are fixed once written and
 * the [today]/[yesterday]/[this week] label is constant within a
 * session).  Designed to be appended into the stable_prefix segment so
 * the Anthropic prompt cache attaches.
 *
 * Sections:
 * - USER PREFERENCES: category: value (from memory_preferences, up to
 *   MAX_CONTEXT_PREFS entries)
 * - RECENT CONVERSATIONS: summaries with topics (from memory_summaries,
 *   <= 30 days old, up to MAX_CONTEXT_SUMMARIES entries)
 *
 * Output is bounded by per-item caps internal to the implementation;
 * no per-character budget truncation (the cached prefix has no
 * per-turn token cost).
 *
 * @param user_id User ID to build context for.
 * @param zone    The user's zone: summaries are dated in it.
 * @param token_budget Deprecated — kept for API stability only.  Ignored
 *                     internally; size is bounded by the per-item caps.
 * @return Heap-allocated NUL-terminated string on success (caller must
 *         `free()`).  Returns NULL when there are no memories to surface
 *         or on allocation failure.  Empty user → NULL (callers can
 *         skip the prompt-augmentation branch cheaply).
 */
char *memory_build_context(int user_id, const tz_t *zone, int token_budget);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_CONTEXT_H */
