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
 * Filters for the tags a model writes into its reply text.
 */

#ifndef TEXT_FILTER_H
#define TEXT_FILTER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Output callback for the streaming tag filter
 *
 * @param text Filtered text chunk
 * @param len Length of text chunk
 * @param ctx User context
 */
typedef void (*text_filter_output_fn)(const char *text, size_t len, void *ctx);

/* =============================================================================
 * Memory-Citation Tag Constants
 *
 * The `<cited>M1,M5,M11</cited>` bookkeeping tag the model appends when the
 * memory-citation signal is on.  It is stripped from the COMPLETED response by
 * llm_response_finalize, but WebUI/DAP2/JOB stream token-by-token, so the tag
 * would reach the browser live (DOMPurify drops the unknown element but keeps
 * its inner text, leaking `M1,M5,M11`) before finalization runs.  This filter is
 * the live-stream strip seam — the streaming twin of the finalizer's strip.
 *
 * A tag is the opener, a run of CITED_TAG_CONTENT_CHARS (the citation tokens
 * `M<n>` / `ID:<n>`, the brackets the model sees them in, and the separators the
 * capture parser reads), and the closer, which may carry a few spaces
 * (`</cited >`, `</ cited>`, `< /cited>`).  The model is told to END its reply
 * with the tag, so a tag counts only there: one followed by at most
 * CITED_TAG_TRAIL_WS_MAX whitespace bytes and then the end of the text or another
 * opener (a second trailing tag) is removed, with that whitespace, and its
 * citations captured.  Anywhere else it is text, every byte kept and nothing
 * captured: an example quoted mid-reply, or a mention ("my `<cited>` tags").  A
 * tag the text ends inside (a truncated stream) is dropped.  One exception, for
 * the stream's sake: a run longer than CITED_TAG_HOLD_MAX is more than the stream
 * can hold to give back, so it is tag content wherever it sits and however it
 * ends: the opener, the run and its closer (when one ends it) are removed and
 * captured, and the text goes on from the byte after.  "The text" is what one
 * call is given: a model message for the finalizer, a sentence for TTS, a
 * model message's stream between flushes.  One byte step reads this grammar for
 * the whole-string strip, the streaming filter and text_filter_cited_find() (the
 * capture parser's way in, memory_citation_parse.c), so they agree on every
 * input.
 * ============================================================================= */

#define CITED_TAG_OPEN "<cited>"
#define CITED_TAG_CLOSE "</cited>"
#define CITED_TAG_OPEN_LEN 7
#define CITED_TAG_CLOSE_LEN 8

/* The bytes a tag's content may hold: digits, the `M` and `ID:` prefixes in
 * either case, the brackets `[M1]` / `[ID:6432]` are shown in, and the
 * separators (comma, space, tab, newline).  No '<': it starts the closer. */
#define CITED_TAG_CONTENT_CHARS "0123456789MmIiDd:[], \t\n"

/* How much of a tag's content the streaming filter keeps to give back word for
 * word if it proves to be text: longer than any run of tag characters prose puts
 * after the opener.  Past it the run is tag content and is dropped as it comes. */
#define CITED_TAG_HOLD_MAX 320

/* Spaces or tabs a malformed closer may carry in all. */
#define CITED_TAG_CLOSE_PAD_MAX 4

/* Whitespace a tag may be followed by and still end the text.  More, and it is
 * text: the stream would have to hold it all to decide, and a tag left showing
 * loses nothing, where one dropped could be an example mid-reply. */
#define CITED_TAG_TRAIL_WS_MAX 32

/* The streaming filter's held bytes: at most a whole tag within the hold (opener,
 * run, padded closer), its trailing whitespace and all but the last byte of the
 * opener after it. */
#define CITED_TAG_BUF_SIZE                                                                    \
   (CITED_TAG_OPEN_LEN + CITED_TAG_HOLD_MAX + CITED_TAG_CLOSE_LEN + CITED_TAG_CLOSE_PAD_MAX + \
    CITED_TAG_TRAIL_WS_MAX + CITED_TAG_OPEN_LEN)

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

/* The Gemma chat template's turn end.  A local server whose template doesn't
 * stop on it passes it through as text, with the model's next turn after it, so
 * on a local model the reply ends there (llm_response_end_of_turn_strip). */
#define END_OF_TURN_TAG "<end_of_turn>"

/** @brief How far a candidate tag has been read (its byte step's state). */
typedef struct {
   size_t len;     /**< Bytes read, from its '<' (0 = no candidate) */
   size_t content; /**< Of them, content bytes */
   size_t close;   /**< Closer characters matched (its padding not counted) */
   bool over;      /**< The content ran past CITED_TAG_HOLD_MAX: a tag however it ends */
   bool trail;     /**< A whole tag within the hold was read: now what follows it */
   size_t ws;      /**< Whitespace read after it */
   size_t next;    /**< Bytes of a following opener matched */
} cited_tag_scan_t;

/**
 * @brief Memory-citation tag filter state.
 *
 * Tracks streaming state for stripping <cited>...</cited>. Must be zero-initialized
 * before first use.  While a candidate is read the filter holds its bytes, which
 * go out as text the moment a byte proves it isn't a trailing tag; past the hold
 * the content is dropped as it comes and only a closer being matched is held.
 */
typedef struct {
   char buffer[CITED_TAG_BUF_SIZE]; /**< The held bytes */
   size_t held;                     /**< How many */
   cited_tag_scan_t scan;           /**< The candidate being read */
} cited_tag_filter_state_t;

/**
 * @brief Strip <cited>...</cited> tags from streaming text.
 *
 * Character-by-character state machine tolerant of a tag split across chunk
 * boundaries (e.g. "<cit" then "ed>").  Non-tag text is emitted via callback.
 * From a '<' the bytes are held while they can still be a trailing tag (see the
 * grammar above): a whole tag stays held with the whitespace after it until the
 * next byte decides it (another opener drops it, anything else releases it as
 * text, in order), a byte that can't be part of a tag releases what is held as
 * text (past the hold the run is dropped instead, and text resumes at that byte),
 * and text_filter_cited_flush_to_buffer, at the end of the model's message,
 * resolves what is still held.  Output per call is at most the bytes held before
 * it (under CITED_TAG_BUF_SIZE) plus the chunk's own.  For already-assembled
 * text, use the whole-string text_filter_cited_strip() instead.
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
 * Call at the end of each model message (the end of its text).  A partial
 * <cited> OPENER that never completed is real text and is emitted.  A whole tag
 * with only whitespace after it ended the text: it is DROPPED with that
 * whitespace; followed by a partial opener it is text, emitted.  A complete
 * opener with tag content after it and no closer (truncated stream) is DROPPED
 * (citation bookkeeping, not user text), as is a run past the hold.  Resets
 * @p state.
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
 * @brief Strip the trailing <cited>…</cited> tag(s) from an already-assembled string, in place.
 *
 * The whole-string counterpart to the streaming text_filter_cited_tags: for text
 * that is complete (a finalized response, one TTS sentence), not arriving as
 * deltas, with the same result as the stream for any input (the grammar above):
 * trailing tags go, with the whitespace after them; a tag anywhere else stays.
 * A tag the string ends inside (truncated stream) drops from the opener to
 * end-of-string so a partial tag can never surface; an opener in prose stays. The single shared
 * strip for every non-streaming surface — response finalizer, local-voice TTS, WebUI-audio TTS — so
 * the citation tag is never spoken or persisted.
 *
 * Opener-anchored, matching the streaming filter: a lone `</cited>` closer with no
 * preceding opener is left as literal text (it carries no citation content).
 *
 * @param text NUL-terminated, mutable C string (compacted in place). NULL is
 *             tolerated (no-op).
 */
void text_filter_cited_strip(char *text);

/**
 * @brief Find the next citation tag in @p text (the grammar above).
 *
 * Only a tag the grammar removes is found (a trailing tag, or a run past the
 * hold); an opener in prose, or a whole tag mid-text, is passed over.
 *
 * @param text        NUL-terminated string to search
 * @param content_len Receives the tag's content bytes (to the closer, the byte
 *                    that ended a run past the hold, or the end of @p text)
 * @param tag_len     Receives the bytes to remove: the tag, closer included, and
 *                    for a trailing tag the whitespace after it; 0 when @p text
 *                    ends inside the tag
 * @return The tag's opener, or NULL when @p text has no tag (or is NULL)
 */
const char *text_filter_cited_find(const char *text, size_t *content_len, size_t *tag_len);

/**
 * @brief Canonicalize whitespace-malformed citation tags in place.
 *
 * Some models emit the citation tag with stray interior whitespace —
 * `< cited>`, `<cited >`, `</ cited >` — which the exact-`strstr` grammar used by
 * the strip AND the capture parser both miss, so the tag both leaks (into the
 * persisted body / rendered text) AND loses its citation (never recorded).
 * Rewrite any `< [ws] [/] [ws] cited [ws] >` to the canonical `<cited>` / `</cited>`
 * so the downstream capture + strip see a well-formed tag.  Idempotent; the
 * canonical form is never longer than the input, so it compacts in place.
 *
 * Call on a complete buffer BEFORE memory_citation_capture + text_filter_cited_strip
 * (e.g. in llm_response_finalize).  Does NOT cover the streaming filter (a
 * char-by-char state machine) — the live origin stream may still flash a malformed
 * tag until reload.
 *
 * @param text NUL-terminated mutable C string; NULL tolerated (no-op).
 */
void text_filter_cited_normalize(char *text);

/**
 * @brief Truncate at the first <end_of_turn>, in place.
 *
 * For a reply from a model that writes the marker; whether one does is the
 * session's call (llm_response_end_of_turn_strip), shared by the response
 * finalizer, local-voice TTS and WebUI-audio TTS.
 *
 * @param text NUL-terminated, mutable C string. NULL is tolerated (no-op).
 */
void text_filter_end_of_turn_strip(char *text);

/* =============================================================================
 * Expression Tags
 *
 * `[emote:curious]` at the start of a sentence, written by the model on the
 * local voice surface while an expressive body is present (the
 * EXPRESSION_DIRECTIVE).  The tag is taken out of each spoken sentence (its
 * name goes to TTS, which publishes it as the sentence starts playing) and out
 * of the finished reply, so it is never spoken, shown or stored.
 * ============================================================================= */

#define EMOTE_TAG_OPEN "[emote:"
#define EMOTE_TAG_OPEN_LEN 7
#define EMOTE_NAME_MAX 24 /* Longest name accepted inside a tag, plus margin */
#define EMOTE_TAG_SPAN 48 /* A "]" this far past the opener closes a malformed tag */

/* The vocabulary: the prompt lists it and the strip accepts only these.  Feelings
 * are how FRIDAY feels, spread over what a viewer can read on a low-DoF body
 * (positive or negative, calm or excited); acts are what she is doing in the
 * sentence.  The robot bridge maps each name to moves in its own config
 * (services/reachy-bridge/reachy-bridge.toml); tests/check_reachy_vocab_mirror.sh
 * keeps the two in step. */
#define EMOTE_FEELINGS(X) \
   X(happy)               \
   X(excited)             \
   X(calm)                \
   X(curious)             \
   X(surprised)           \
   X(confused)            \
   X(concerned)           \
   X(sad)                 \
   X(frustrated)
#define EMOTE_ACTS(X) \
   X(nod)             \
   X(no)              \
   X(unsure)          \
   X(done)            \
   X(thinking)        \
   X(greeting)        \
   X(grateful)        \
   X(laugh)           \
   X(oops)
#define EMOTE_NAMES(X) EMOTE_FEELINGS(X) EMOTE_ACTS(X)
#define EMOTE_NAME_AS_TEXT(name) " " #name
#define EMOTE_FEELINGS_TEXT EMOTE_FEELINGS(EMOTE_NAME_AS_TEXT) /* " happy excited ..." */
#define EMOTE_ACTS_TEXT EMOTE_ACTS(EMOTE_NAME_AS_TEXT)         /* " nod no ..." */

/**
 * @brief Strip every `[emote:<name>]` tag from complete text, in place.
 *
 * A tag is the opener (any case), a name of letters, `_` or `-` (optionally
 * spaced, any case), and `]`; one space after it goes too.  What the model gets
 * slightly wrong ("[emote: happy, curious]", an unknown name) is removed as
 * well, with no name, so it is never spoken: anything the opener starts that a
 * `]` closes on the same line within EMOTE_TAG_SPAN.  Otherwise it is text.
 *
 * Strip these BEFORE <cited>: removing one can join the text around it, and
 * the later strip must see the result.
 *
 * @param text      NUL-terminated, mutable C string. NULL is tolerated (no-op).
 * @param name_out  Receives the first vocabulary name found, lower-case, or ""
 *                  (may be NULL when the name isn't wanted)
 * @param name_size Size of @p name_out (at least EMOTE_NAME_MAX)
 * @return true when a vocabulary name was found
 */
bool text_filter_emote_strip(char *text, char *name_out, size_t name_size);

/* =============================================================================
 * Pieces of a stream chunk
 * ============================================================================= */

#define TEXT_PIECE_MAX 1023 /* Longest piece text_split_pieces hands out */
#define TEXT_PIECE_MIN 4    /* Shortest limit: the longest UTF-8 character */

/** Receives one NUL-terminated piece of text_split_pieces' input. */
typedef void (*text_piece_fn)(const char *piece, void *ctx);

/**
 * @brief Hand @p text to @p fn in order, in pieces of at most @p max_bytes
 *
 * For a caller that filters a stream chunk into a fixed buffer: a provider's
 * chunk has no size limit.  Text no longer than @p max_bytes is passed through
 * as is (the same pointer), in one call; longer text is copied out piece by
 * piece.  Each cut is moved back over the continuation bytes (10xxxxxx) after
 * it to the start of a character, so with valid UTF-8 every piece is whole
 * characters.  Invalid UTF-8 is passed on byte for byte, nothing dropped or
 * changed: a cut is placed by the same rule, and a run of continuation bytes
 * reaching back to the piece's start is cut at @p max_bytes.  The tag filters
 * keep their state across calls, so a tag cut between two pieces is handled
 * as one cut between chunks.
 *
 * @param text      NUL-terminated text; NULL or "" calls nothing
 * @param max_bytes Longest piece: 0 or over TEXT_PIECE_MAX means TEXT_PIECE_MAX,
 *                  under TEXT_PIECE_MIN means TEXT_PIECE_MIN
 * @param fn        Called once per piece
 * @param ctx       Passed to @p fn
 */
void text_split_pieces(const char *text, size_t max_bytes, text_piece_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* TEXT_FILTER_H */
