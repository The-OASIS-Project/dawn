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
 * Request context on a history (message_kind.h), in memory.
 *
 * Two places carry a kind, both under MESSAGE_KIND_KEY:
 *  - a message: a directive, instruction, envelope, loop note or the frozen
 *    prefix is a message of its own;
 *  - a content part: a turn's context and the memory body are text parts at the
 *    front of the question's own user message (one message on the wire, one
 *    database row each).
 * Anything that counts, walks or shows messages uses llm_history_is_context();
 * nothing other than a model's request ever reads them.
 */

#ifndef LLM_HISTORY_KIND_H
#define LLM_HISTORY_KIND_H

#include <stdbool.h>
#include <stddef.h>

#include "core/message_kind.h"
#include "llm/llm_context_text.h"

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/** Key on a conversation's frozen prefix message holding the names of the tool
 *  set it advertises (a JSON array; never sent). */
#define LLM_HISTORY_TOOLS_KEY "_tools"

/** Key on a conversation's frozen prefix message holding the record of what is
 *  in force (prefix_in_force.h; never sent). */
#define LLM_HISTORY_IN_FORCE_KEY "_in_force"

/**
 * @name The conversation's tag
 *
 * A secret random tag per conversation marks what is DAWN's in a request: the
 * frozen system prompt declares it, and DAWN's own blocks carry it (TURN
 * CONTEXT, USER MEMORY, an operator's note sent as text).  Text that imitates
 * a block without the tag (in a retrieved item, a tool result, a background
 * job's output, the user's words) is data.  Web pages and job output never saw
 * the tag, so they can't forge it.
 * @{
 */
/** @p history's tag (its prefix's record), or NULL when it has none. */
const char *llm_history_tag(struct json_object *history);

/** @p history's first message when it is the frozen prefix (borrowed), else NULL. */
struct json_object *llm_history_prefix(struct json_object *history);

/**
 * The in-force record on @p prefix (borrowed; a JSON object), or NULL when it
 * has none.  With @p create, one is made when it has none (NULL only when out
 * of memory).
 */
struct json_object *llm_history_in_force(struct json_object *prefix, bool create);

/** @} */

/** Key on a loaded request-context message holding the row id of the question
 *  it was saved with (messages.context_of; never sent). */
#define LLM_HISTORY_CONTEXT_OF_KEY "_context_of"

/**
 * The tool set @p history's conversation advertises (borrowed; a JSON array of
 * names), or NULL when it has no frozen one (a request built without a prefix:
 * the current surface's tools then).
 */
struct json_object *llm_history_frozen_tools(struct json_object *history);

/** The kind on a message or content part; MESSAGE_KIND_NONE when absent. */
message_kind_t llm_history_kind_of(struct json_object *obj);

/** Mark a message or content part with @p kind (NONE removes the mark). */
void llm_history_set_kind(struct json_object *obj, message_kind_t kind);

/**
 * Whether @p msg is request context rather than a message: it has a kind, or
 * its content is nothing but kind parts.
 */
bool llm_history_is_context(struct json_object *msg);

/** Whether @p msg's role is @p role. */
bool llm_history_role_is(struct json_object *msg, const char *role);

/** @p msg's content when it is a string, else NULL (borrowed). */
const char *llm_history_text(struct json_object *msg);

/**
 * @brief Whether @p msg is a question a turn's context goes with: a user
 *        message, ordinary or an envelope, not one of tool results
 */
bool llm_history_is_question(struct json_object *msg);

/**
 * @brief The question's own words: its text, or its first text part that
 *        isn't request context (NULL when it has none; borrowed)
 */
const char *llm_history_question_text(struct json_object *msg);

/** Called for one context part: its message, the part, its kind and text. */
typedef void (*llm_history_part_fn)(struct json_object *msg,
                                    struct json_object *part,
                                    message_kind_t kind,
                                    const char *text,
                                    void *ctx);

/**
 * @brief Call @p fn for every text content part carrying a kind (a turn's
 *        context, a memory block, a summary), oldest first
 *
 * The one walk over what DAWN put in a history's messages: withdrawing what
 * the user forgot and reading which items a history already shows both go
 * through it, so the two can't see different parts.  @p fn may replace the
 * part's text; it must not add or remove messages or parts.
 */
void llm_history_for_each_context_part(struct json_object *history,
                                       llm_history_part_fn fn,
                                       void *ctx);

/** The USER MEMORY block @p history was last sent (borrowed), or NULL. */
const char *llm_history_memory_in_force(struct json_object *history);

/** Whether any of @p msg's content parts carries a kind. */
bool llm_history_has_context_parts(struct json_object *msg);

/**
 * A text content part carrying @p text marked with @p kind. NULL on failure.
 */
struct json_object *llm_history_context_part(const char *text, message_kind_t kind);

/**
 * @brief Insert @p msg (taken) into @p history at @p index
 * @return false on allocation failure (@p msg released, history unchanged)
 */
bool llm_history_insert(struct json_object *history, size_t index, struct json_object *msg);

/**
 * @brief Whether @p msg is a turn's context that was never saved: a user
 *        message of context parts alone, with no row id.  An envelope's
 *        earlier attempt leaves one just before it, which the next attempt
 *        replaces.
 */
bool llm_history_is_unsaved_context(struct json_object *msg);

/**
 * @brief A message of @p parts alone (taken): a turn's context in a user
 *        message of its own
 */
struct json_object *llm_history_context_message(struct json_object *parts);

/**
 * Fold loaded rows into the in-memory shape, from index @p from on.
 *
 * A row saved naming its question (LLM_HISTORY_CONTEXT_OF_KEY) goes with it,
 * whatever rows landed between them: turn context and memory become kind parts
 * at the front of the question (for an envelope, a message of their own just
 * before it: DAWN's context never shares a message with untrusted text), other
 * request context (a directive, an instruction change) follows it, each in row
 * order.  A named question not in
 * the load takes its rows with it.
 *
 * A row saved without one follows its position: turn-context and memory rows
 * after a turn's question (an ordinary or envelope user message) become parts
 * at its front; other request context stays where it is; a run with no
 * question since the last reply stays a message of its own, made only of kind
 * parts, carrying the id of its last row.
 *
 * @return 0, or 1 on allocation failure (the history is then left as loaded)
 */
int llm_history_fold_context(struct json_object *history, int from);

/**
 * Remove every piece of request context from @p history in place: context
 * messages (a message of nothing but kind parts is one), and kind parts from
 * the questions holding them; a question left with one text part is a string
 * again. For readers that aren't a model's request: memory extraction and
 * summaries. Callers pass a copy of their history.
 */
void llm_history_drop_context(struct json_object *history);

/**
 * @brief The request text of a compaction's summary: the one rendering, live
 *        and on reload, from values known when the history is compacted
 *
 * Framed as the conversation's CONVERSATION SUMMARY block, with @p tag when the
 * history has one.  No database ids: a conversation-less history has none, and
 * the same bytes must come back on reload.  @p summary is rendered verbatim
 * (its tag masked): it must be what was stored, neutralized once when it was
 * made, so a change of the neutralizer's rules never re-renders it.
 *
 * @return Heap text (caller frees), or NULL on allocation failure
 */
char *llm_history_summary_text(const char *summary, const char *tag);

/**
 * @brief Put @p summary in front of the first question at or after @p from,
 *        as a summary part (MESSAGE_KIND_SUMMARY), in place
 *
 * The question keeps its identity (a turn tracks its question by it); its text
 * becomes parts when it was a string.  With no question after @p from, the
 * summary is a user message of its own there, after the leading system
 * messages.  Live compaction and both loaders call this, so a reload renders
 * the request byte for byte as it was sent.
 *
 * @return The index of the message holding it, or -1 on failure (unchanged)
 */
int llm_history_attach_summary(struct json_object *history,
                               int from,
                               const char *summary,
                               const char *tag);

#ifdef __cplusplus
}
#endif

#endif /* LLM_HISTORY_KIND_H */
