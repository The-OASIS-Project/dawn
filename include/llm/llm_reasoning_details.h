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
 * OpenRouter reasoning_details, gathered from a stream for replay.
 */

#ifndef LLM_REASONING_DETAILS_H
#define LLM_REASONING_DETAILS_H

#include <stdbool.h>
#include <stddef.h>

#include "core/strbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/** Largest total a turn's reasoning_details may reach and still be kept. */
#define LLM_REASONING_DETAILS_BYTES_MAX (256 * 1024)
/** Most entries a turn's reasoning_details may hold and still be kept. */
#define LLM_REASONING_DETAILS_ENTRIES_MAX 1024

/**
 * A stream's reasoning_details as they arrive.  OpenRouter streams an entry
 * in pieces sharing its type and index (a text entry's text in parts; a
 * Gemini signature as a separate "reasoning.encrypted" entry at the same
 * index as its text), and requires the whole sequence back exactly as
 * produced.  Pieces without an index (OpenRouter sends one; verified live
 * for Gemini, 2026-09-28) continue an entry of the same type.  Text and summary pieces are appended
 * in buffers (not rebuilt per piece); an entry is finished when a piece of another type or index
 * arrives.  Past either cap nothing is kept: a partial sequence is not the
 * sequence.
 */
typedef struct {
   struct json_object *entries; /**< finished entries */
   struct json_object *open;    /**< the entry being built, without text/summary */
   strbuf_t text;
   strbuf_t summary;
   bool has_text;
   bool has_summary;
   size_t bytes;
   int count;
   bool over;
} llm_reasoning_details_t;

void llm_reasoning_details_init(llm_reasoning_details_t *acc);

/**
 * @brief Add one streamed piece
 *
 * A piece continues the open entry when it has the same type and the same
 * index (or neither has one); otherwise it starts a new entry.  Text and
 * summary are appended; other fields are set as they arrive.
 */
void llm_reasoning_details_add(llm_reasoning_details_t *acc, struct json_object *piece);

/**
 * @brief The entries, in order (caller owns them)
 *
 * NULL when there are none or a cap was passed.  The accumulator is empty
 * afterwards.
 */
struct json_object *llm_reasoning_details_finish(llm_reasoning_details_t *acc);

void llm_reasoning_details_free(llm_reasoning_details_t *acc);

/**
 * @brief Whether a response came from the model asked for
 *
 * The same name, or a dated version of it ("gpt-5.5" served as
 * "gpt-5.5-2026-09-01"); an OpenRouter variant suffix on the asked name
 * (":free", ":thinking") is ignored.  A router that picks another model
 * ("openrouter/auto") is not, and neither is a different model sharing a
 * prefix ("gpt-5" is not "gpt-5.5").  Not said (empty) counts as yes.
 */
bool llm_served_as_asked(const char *served, const char *asked);

#ifdef __cplusplus
}
#endif

#endif /* LLM_REASONING_DETAILS_H */
