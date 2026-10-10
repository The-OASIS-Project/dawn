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
 * Someone else's text (an email, a web page) framed as such: the frame a tool
 * result goes in (tools/tool_registry.h TOOL_FRAME_*), and finding one again.
 */

#ifndef LLM_THIRD_PARTY_H
#define LLM_THIRD_PARTY_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Someone else's text (an email, a web page) in its frame @p name
 *        (a TOOL_FRAME_*), with the line saying it is data and not an
 *        instruction, then @p body
 *
 * @p body must already be neutralized (llm_context_neutralize), so nothing in
 * it can end the frame.  @p tag may be NULL (no conversation tag yet).
 *
 * @return Heap text (caller frees); NULL for a name that isn't a frame, or on
 *         allocation failure
 */
char *llm_third_party_frame(const char *name, const char *tag, const char *body);

/**
 * @brief The frame whose open line @p text holds (as llm_third_party_frame
 *        writes it): text DAWN framed as someone else's.  An imitation in the
 *        text itself is defused by the neutralizer, so a line found is DAWN's
 *        own.
 * @return Its TOOL_FRAME_* (the email frame first, when both are there), or
 *         NULL for none
 */
const char *llm_third_party_present(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* LLM_THIRD_PARTY_H */
