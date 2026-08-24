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
 * Text filtering utilities for command tag stripping.
 */

#ifndef TEXT_FILTER_H
#define TEXT_FILTER_H

#include <stdbool.h>
#include <stddef.h>

/* =============================================================================
 * Command Tag Constants
 *
 * Shared between text_filter and webui_server to ensure consistency.
 * ============================================================================= */

#define CMD_TAG_OPEN "<command>"
#define CMD_TAG_CLOSE "</command>"
#define CMD_TAG_OPEN_LEN 9
#define CMD_TAG_CLOSE_LEN 10
#define CMD_TAG_BUF_SIZE 16     /* Enough for "</command>" (10) + margin */
#define CMD_TAG_MAX_NESTING 100 /* Max nesting depth to prevent overflow */

/**
 * @brief Command tag filter state
 *
 * Tracks state for filtering <command>...</command> tags from streaming text.
 * Must be initialized to zero before first use. Supports nested tags.
 */
typedef struct {
   char buffer[CMD_TAG_BUF_SIZE]; /**< Buffer for partial tag detection */
   unsigned char len;             /**< Current length of partial tag buffer */
   int nesting_depth;             /**< Nesting depth (0 = outside tags, >0 = inside) */
} cmd_tag_filter_state_t;

/**
 * @brief Output callback type for command tag filter
 *
 * @param text Filtered text chunk
 * @param len Length of text chunk
 * @param ctx User context
 */
typedef void (*text_filter_output_fn)(const char *text, size_t len, void *ctx);

/**
 * @brief Filter <command>...</command> tags from streaming text
 *
 * Uses a character-by-character state machine that handles partial tags
 * spanning chunk boundaries. Filtered text is emitted via callback.
 *
 * @param state Filter state (must be zeroed before first chunk)
 * @param text Input text to filter
 * @param output_fn Callback to emit filtered text
 * @param ctx User context passed to output_fn
 *
 * @note If stream ends with a partial tag buffer, content is silently dropped.
 */
void text_filter_command_tags(cmd_tag_filter_state_t *state,
                              const char *text,
                              text_filter_output_fn output_fn,
                              void *ctx);

/**
 * @brief Filter command tags into a buffer
 *
 * Convenience wrapper that filters text into a fixed-size buffer.
 *
 * @param state Filter state (must be zeroed before first chunk)
 * @param text Input text to filter
 * @param out_buf Output buffer
 * @param out_size Size of output buffer
 * @return Length of filtered text written (excluding null terminator)
 */
int text_filter_command_tags_to_buffer(cmd_tag_filter_state_t *state,
                                       const char *text,
                                       char *out_buf,
                                       size_t out_size);

/**
 * @brief Reset filter state
 *
 * Call this when starting a new stream or to clear partial tag state.
 *
 * @param state Filter state to reset
 */
void text_filter_reset(cmd_tag_filter_state_t *state);

/* =============================================================================
 * Memory-Citation Tag Constants
 *
 * The `<cited>M1,M5,M11</cited>` bookkeeping tag the model appends when the
 * memory-citation signal is on.  It is stripped from the COMPLETED response by
 * llm_response_finalize, but WebUI/DAP2/JOB stream token-by-token, so the tag
 * would reach the browser live (DOMPurify drops the unknown element but keeps
 * its inner text, leaking `M1,M5,M11`) before finalization runs.  This filter is
 * the live-stream strip seam — the streaming twin of the finalizer's strip.
 * Unlike command tags there is no nesting: everything between the open and close
 * is suppressed.
 * ============================================================================= */

#define CITED_TAG_OPEN "<cited>"
#define CITED_TAG_CLOSE "</cited>"
#define CITED_TAG_OPEN_LEN 7
#define CITED_TAG_CLOSE_LEN 8
#define CITED_TAG_BUF_SIZE 16 /* Enough for "</cited>" (8) + margin */

/* Canonical spoken/written example of the citation tag for PROMPT text.  Single-
 * sourced from the same opener/closer the filter strips, so the prompt-builders
 * (k_citation_footer, the focus-block reminder) and the strip/parse paths can
 * never drift on grammar.  The ordinals are illustrative. */
#define CITED_TAG_EXAMPLE CITED_TAG_OPEN "M1,M4" CITED_TAG_CLOSE

/* Marker a memory TOOL result prints before each surfaced fact, e.g. "[ID:6432]".
 * Single-sourced so both renderers (memory_callback.c search/recall and
 * recall_format.c) print ONE marker and the model learns ONE grammar; the capture
 * tokenizer keys on the same "ID:" prefix.  printf-format: one %lld (the fact id). */
#define SURFACED_ID_FMT "[ID:%lld]"

/* Literal `[ID:x]` for PROMPT prose (SURFACED_ID_FMT carries a %lld and can't be
 * used as plain text). */
#define SURFACED_ID_HINT "[ID:x]"

/* Prompt example of citing a tool-surfaced fact by id (see SURFACED_ID_FMT). */
#define CITED_TAG_ID_EXAMPLE CITED_TAG_OPEN "ID:6432" CITED_TAG_CLOSE

/* Command / end-of-turn tag grammar — the legacy `<command>…</command>` transport
 * and the `<end_of_turn>` marker some local models emit.  Shared by
 * text_filter_command_strip so the finalizer + every TTS path key off one source. */
#define COMMAND_TAG_OPEN "<command>"
#define COMMAND_TAG_CLOSE "</command>"
#define COMMAND_TAG_CLOSE_LEN 10
#define END_OF_TURN_TAG "<end_of_turn>"

/**
 * @brief Memory-citation tag filter state.
 *
 * Tracks streaming state for stripping <cited>...</cited>. Must be zero-initialized
 * before first use. No nesting; `in_tag` marks "open seen, close pending."
 */
typedef struct {
   char buffer[CITED_TAG_BUF_SIZE]; /**< Partial-tag holdback buffer */
   unsigned char len;               /**< Current partial-tag buffer length */
   bool in_tag;                     /**< true = between <cited> and </cited> (suppressing) */
} cited_tag_filter_state_t;

/**
 * @brief Strip <cited>...</cited> tags from streaming text.
 *
 * Character-by-character state machine tolerant of a tag split across chunk
 * boundaries (e.g. "<cit" then "ed>").  Non-tag text is emitted via callback;
 * held-back partial-opener bytes persist in @p state and surface on a later
 * chunk (if they prove not to be a tag) or via text_filter_cited_flush_to_buffer.
 * For already-assembled text (no cross-chunk splitting), use the whole-string
 * text_filter_cited_strip() instead.
 *
 * @param state    Filter state (zeroed before first chunk)
 * @param text     Input chunk
 * @param output_fn Callback to emit stripped text
 * @param ctx      User context passed to output_fn
 */
void text_filter_cited_tags(cited_tag_filter_state_t *state,
                            const char *text,
                            text_filter_output_fn output_fn,
                            void *ctx);

/**
 * @brief Strip <cited> tags into a fixed-size buffer.
 *
 * @return Length written (excluding NUL).  0 when the whole chunk was tag or was
 *         held back pending more input.
 */
int text_filter_cited_tags_to_buffer(cited_tag_filter_state_t *state,
                                     const char *text,
                                     char *out_buf,
                                     size_t out_size);

/**
 * @brief Flush held-back bytes at stream end into a buffer.
 *
 * A partial <cited> OPENER that never completed is real text and is emitted.
 * If mid-tag (open seen, close never arrived — truncated stream), the suppressed
 * remainder is DROPPED (citation bookkeeping, not user text).  Resets @p state.
 *
 * @return Length written (excluding NUL); 0 if nothing to flush.
 */
int text_filter_cited_flush_to_buffer(cited_tag_filter_state_t *state,
                                      char *out_buf,
                                      size_t out_size);

/**
 * @brief Reset citation-tag filter state (new stream / clear partial state).
 */
void text_filter_cited_reset(cited_tag_filter_state_t *state);

/**
 * @brief Strip every <cited>…</cited> tag from an already-assembled string, in place.
 *
 * The whole-string counterpart to the streaming text_filter_cited_tags: for text
 * that is complete (a finalized response, one TTS sentence), not arriving as
 * deltas. An orphan opener with no closer (truncated stream) drops from the opener
 * to end-of-string so a partial tag can never surface. The single shared strip for
 * every non-streaming surface — response finalizer, local-voice TTS, WebUI-audio
 * TTS — so the citation tag is never spoken or persisted.
 *
 * Opener-anchored, matching the streaming filter: a lone `</cited>` closer with no
 * preceding opener is left as literal text (it carries no citation content).
 *
 * @param text NUL-terminated, mutable C string (the body uses strstr/strlen and
 *             compacts in place). NULL is tolerated (no-op).
 */
void text_filter_cited_strip(char *text);

/**
 * @brief Strip <command>…</command> pairs and truncate at <end_of_turn>, in place.
 *
 * The whole-string residual-tag scrub for the legacy command transport plus the
 * end-of-turn marker some local models emit. The single shared strip for the
 * response finalizer, local-voice TTS, and WebUI-audio TTS.
 *
 * @param text NUL-terminated, mutable C string. NULL is tolerated (no-op).
 * @param truncate_orphan How to handle an orphan `<command>` opener (no closer):
 *   - true  (per-sentence TTS): drop from the opener to end-of-string, so a
 *           sentence that split mid-tag can't speak a partial `<command>`.
 *   - false (a complete response): leave the remainder as literal text — in an
 *           assembled response an unclosed `<command>` is more likely real prose
 *           than a truncated tag, so it is not destroyed.
 */
void text_filter_command_strip(char *text, bool truncate_orphan);

#endif /* TEXT_FILTER_H */
