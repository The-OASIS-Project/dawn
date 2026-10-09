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
 * Scheduler briefing summarization prompt assembly.  Extracted from
 * scheduler.c so the prompt-injection defenses (fence neutralization and the
 * PREFIX -> instructions -> SECURITY -> data ordering) are independently
 * unit-testable without dragging in the full scheduler engine.
 */

#ifndef BRIEFING_PROMPT_H
#define BRIEFING_PROMPT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Neutralize forged fence tags in an untrusted string so a crafted payload
 * cannot close the <briefing_data> block or open a fake <briefing_instructions>
 * one.  Replaces the '<' of any "<briefing_data", "</briefing_data", or
 * "<briefing_instructions" (case-insensitive) with '[' IN PLACE.  Tolerates a
 * leading '/' and ASCII whitespace between '<' and the tag name.  NULL-safe.
 */
void neutralize_briefing_fences(char *s);

/**
 * Build the briefing summarization system message.  Layered so precedence is
 * explicit and tamper-evident:
 *   1. PREFIX          — default shape/voice/formatting (overridable).
 *   2. <briefing_instructions> — the owner's per-briefing steering, if any.
 *   3. SECURITY        — absolute, non-overridable data-not-instructions rule,
 *      emitted LAST so it is the final directive before the data (a security
 *      invariant: it must appear after the instructions block and before the
 *      data block).
 *   4. <briefing_data> — the (fence-neutralized) tool output to summarize.
 *
 * Both `instructions` and `cleaned_data` are fence-neutralized (instructions
 * into a bounded local copy; `cleaned_data` in place — see the .c note on the
 * aliasing fallback).  Returns a malloc'd string the caller owns, or NULL on
 * allocation failure.
 *
 * @param briefing_name  Display name (NULL/empty -> "scheduled").
 * @param instructions   Optional per-briefing steering (NULL/empty -> omitted).
 * @param cleaned_data   Tool output to summarize (mutated in place); NULL ->
 *                       "(no data)".
 */
char *build_briefing_system_message(const char *briefing_name,
                                    const char *instructions,
                                    char *cleaned_data);

#ifdef __cplusplus
}
#endif

#endif /* BRIEFING_PROMPT_H */
