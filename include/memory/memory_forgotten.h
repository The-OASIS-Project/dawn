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
 * What a user forgot, as an extraction of a batch said before the forget sees
 * it (memory_forgotten, written by memory_db_fact_forget).  Extraction runs as
 * usual; then what it returned is checked against the forgotten facts
 * (memory_forgotten_screen) and a fact that states one again is not stored
 * (memory_forgotten_is_repeat).  Checked after, not listed in the prompt: a
 * list there made the model drop what else the conversation said about the
 * same people and topics.
 */

#ifndef MEMORY_FORGOTTEN_H
#define MEMORY_FORGOTTEN_H

#include <stdbool.h>
#include <stdint.h>

#include "llm/llm_interface.h"
#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Cosine at or above which a returned fact is asked about against a forgotten
 * one (bge-small: a fact re-extracted from its own conversation scores 0.76 at
 * its 5th percentile).  Without embeddings every returned fact is asked about.
 */
#define MEMORY_FORGOTTEN_CANDIDATE 0.70f

/**
 * Cosine at or above which a live fact still says a forgotten one (a copy kept
 * while another was deleted), so it isn't checked.  High on purpose: a changed
 * value scores up to 0.911 against the old one ("favorite animal is a cat" /
 * "... dog", bge-small), a copy with an added clause 0.979.
 */
#define MEMORY_FORGOTTEN_STILL_KNOWN 0.96f

typedef struct {
   int64_t id;
   char text[MEMORY_FACT_TEXT_MAX];
   bool embedded;    /* tried (embedding NULL: couldn't be) */
   bool asked;       /* listed in this check's question */
   bool known;       /* a live fact still says it (another copy kept) */
   float *embedding; /* made on the first check that needs it */
   int dims;
   float norm;
} memory_forgotten_t;

/** Returned facts (folded) memory_forgotten_screen decided on, with what to
 *  store instead (to[i], NULL in the drop set).  Heap. */
typedef struct {
   char **from;
   char **to;
   int count;
   int cap;
} memory_forgotten_texts_t;

typedef struct {
   memory_forgotten_t *items; /* every one the batch is checked against (heap) */
   int user_id;               /* whose (memory_forgotten_load) */
   int count;
   memory_forgotten_texts_t drop;    /* not to store */
   memory_forgotten_texts_t rewrite; /* to store reworded */
} memory_forgotten_list_t;

/**
 * @brief The user's forgotten facts a batch starting at row id @p first_id
 *        was said before
 *
 * Those forgotten after the batch began: one forgotten before it is the user
 * saying it again.  @p first_id 0 (a history without row ids): the batch is
 * @p conversation_id's rows not extracted yet, or with no conversation (a
 * history never saved), those forgotten since @p since (epoch s; 0: none).
 *
 * @param out Filled (count 0 when none); release with memory_forgotten_free
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE (out empty)
 */
int memory_forgotten_load(int user_id,
                          int64_t conversation_id,
                          int64_t first_id,
                          int64_t since,
                          memory_forgotten_list_t *out);

/** Release what memory_forgotten_load and memory_forgotten_screen allocated. */
void memory_forgotten_free(memory_forgotten_list_t *list);

/**
 * @brief Decide which facts and corrections in the extraction model's
 *        @p response state a forgotten fact again (into @p list's drop set)
 *
 * A forgotten fact a live fact still says (another copy of it, kept: at the
 * MEMORY_FORGOTTEN_STILL_KNOWN) isn't checked.  Otherwise the same text (ignoring case
 * and spacing) is dropped outright; the facts
 * close to a forgotten one (MEMORY_FORGOTTEN_CANDIDATE) are asked about, all
 * in one call on @p cfg (prompt MEMORY_FORGOTTEN_CHECK_PROMPT, tried twice): the
 * same information reworded is dropped, a new value or another fact about the
 * same thing kept, reworded without a forgotten fact it still mentions
 * ("green, replacing blue" stored as "green": memory_forgotten_rewritten).  A
 * check that can't be made (the model fails, or answers something else, twice)
 * drops what it would have asked about; the rest is stored.  A restatement in
 * the same batch after the forget is dropped too (the remember tool brings it
 * back).  The sets are replaced on each call that checks anything.  No-op when
 * @p list is empty or @p response isn't JSON.
 *
 * @return SUCCESS (an allocation failure while asking counts as a check that
 *         can't be made); FAILURE when the returned facts can't be listed
 *         (memory: don't store it now)
 */
int memory_forgotten_screen(memory_forgotten_list_t *list,
                            const char *response,
                            const llm_resolved_config_t *cfg);

/**
 * @brief Whether @p text, returned by the extraction model, is in the drop set
 *        memory_forgotten_screen made.  Logs it (never the text).
 */
bool memory_forgotten_is_repeat(const memory_forgotten_list_t *list,
                                const char *text,
                                int64_t conversation_id);

/**
 * @brief @p text as memory_forgotten_screen reworded it (without a forgotten
 *        fact it mentioned), or NULL to store it as it is.  Owned by @p list.
 */
const char *memory_forgotten_rewritten(const memory_forgotten_list_t *list, const char *text);

/** Whether @p text is, word for word (case and spacing aside), one of @p list's
 *  forgotten facts. */
bool memory_forgotten_names(const memory_forgotten_list_t *list, const char *text);

/**
 * @brief A voice session's conversation @p conv_id was just saved: the user's
 *        forgets since it began (@p since, epoch s) were made after what it
 *        says, so they move past its rows and its extraction is checked
 *        against them (wherever the forget was made)
 */
void memory_forgotten_saved(int user_id, int64_t since, int64_t conv_id);

/** Age (seconds) before memory_forgotten_sweep may take a record: a voice
 *  session that saves later may still move it past its rows. */
#define MEMORY_FORGOTTEN_SWEEP_AGE_SEC 86400

/**
 * @brief Remove the records no extraction reads any more: made before
 *        @p older_than (epoch s), and none of their user's messages is from
 *        before the forget (those conversations deleted).  Run with memory
 *        recovery's pass (so not when recovery is off).
 * @return How many were removed
 */
int memory_forgotten_sweep(int64_t older_than);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_FORGOTTEN_H */
