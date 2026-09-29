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
 * The text of what DAWN adds to a request: the conversation's tag filled in,
 * an operator's note label, and DAWN's markers defused in text it didn't
 * write.  Pure string functions (libc only), for any module that renders
 * such text.  The tag itself: llm_history_kind.h.
 */

#ifndef LLM_CONTEXT_TEXT_H
#define LLM_CONTEXT_TEXT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Where the system prompt declares the tag; filled in when frozen. */
#define LLM_CONTEXT_TAG_PLACEHOLDER "{{dawn_tag}}"
/** Longest tag, with its NUL. */
#define LLM_CONTEXT_TAG_MAX 24

/** @p text with every LLM_CONTEXT_TAG_PLACEHOLDER replaced by @p tag ("" when
 *  NULL).  Caller frees; NULL on allocation failure. */
char *llm_context_with_tag(const char *text, const char *tag);

/** The label of an operator's note sent as text: "[Operator note <tag>] ",
 *  or "[Operator note] " with no tag. */
void llm_operator_note_label(const char *tag, char *buf, size_t size);

/**
 * @brief Untrusted text with DAWN's markers defused
 *
 * Each place the text imitates a block DAWN frames (TURN CONTEXT, USER
 * MEMORY, an operator's note, an instruction change) is rewritten so it no
 * longer reads as one, whatever separates its words (whitespace, newlines,
 * "-_.:*"), whatever opens it ("---", "===", "**"; "[", "(", "{", "<" and
 * their CJK forms), and whatever letters spell it (Cyrillic, Greek or
 * mathematical lookalikes match, characters that render as nothing are
 * seen through).  A tag-shaped string ("dawn-ctx-" and 8 hex digits, its
 * hyphens in any spelling) becomes "dawn_ctx_(withheld)".  Only those spans
 * change: every other byte of the text is kept as it was.  Linear in the
 * text's length.  For any text DAWN puts in front of the model but didn't
 * write (retrieved items, remembered facts, tool results, background-job
 * output, device event details).  Caller frees; NULL on allocation failure.
 */
char *llm_context_neutralize(const char *text);

/** llm_context_neutralize() of @p text, which it takes (frees): for a tool
 *  callback's result, wherever one is called (llm_tools_execute, the
 *  scheduler's briefings).  NULL when @p text is, or on allocation failure. */
char *llm_context_neutralize_owned(char *text);

/** llm_context_neutralize() of @p text made one line (line breaks, U+2028,
 *  U+2029 and U+0085 included, become spaces).  Caller frees; NULL on
 *  allocation failure. */
char *llm_context_neutralize_line(const char *text);

/** The secret part of @p tag (its 8 hex digits, lowercase) in @p hex.
 *  Returns false when @p tag isn't a conversation tag. */
bool llm_context_tag_secret(const char *tag, char hex[9]);

/**
 * @brief @p text with every copy of a conversation's secret @p hex (read as
 *        llm_context_carries_secret reads it: split, spaced or encoded) made
 *        "x"s, the rest kept as it was.  For what reaches the model or a
 *        conversation from outside (a tool result, the model's own reply),
 *        so the secret never sits in it in any form.  Caller frees; NULL on
 *        allocation failure; a copy when @p hex is NULL.
 */
char *llm_context_mask_secret(const char *text, const char *hex);

/** @p text (taken) with every copy of conversation tag @p tag's secret
 *  masked (llm_context_mask_secret).  Returns @p text itself when there is
 *  none (or @p tag is NULL); NULL on allocation failure. */
char *llm_context_mask_tag(char *text, const char *tag);

/**
 * @brief Whether @p text carries a conversation's secret @p hex: read as its
 *        letters and digits only, each in any spelling (fullwidth, circled,
 *        superscript, Cyrillic or Greek lookalikes) and escapes decoded
 *        ("%XX", "&#N;", "&#xN;", "\\uXXXX"), so a split, spaced or encoded copy counts.  A
 * tripwire for what a tool call sends out, not the control (llm_context_neutralize on everything
 * that comes in is).  True on allocation failure.
 */
bool llm_context_carries_secret(const char *text, const char *hex);

/**
 * @brief A turn context's item lines withdrawn: each line naming one of
 *        @p handles ("[M7 ...") keeps its "[M7 " and says the item was
 *        withdrawn, for items the user forgot or deleted
 * @param out Set to the new text (caller frees), or NULL when nothing changed
 * @return 0, or 1 on allocation failure
 */
int llm_context_withdraw_items(const char *text, const int *handles, int count, char **out);

/**
 * @brief A USER MEMORY block withdrawn: its framing lines kept, its body
 *        replaced by a line saying it changed or was forgotten since
 * @param changed Set when it wasn't withdrawn already
 * @return New text (caller frees), or NULL on allocation failure
 */
char *llm_context_withdraw_body(const char *text, bool *changed);

#ifdef __cplusplus
}
#endif

#endif /* LLM_CONTEXT_TEXT_H */
