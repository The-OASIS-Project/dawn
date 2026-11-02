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
 * The consolidator ([memory] consolidator = "agent"): a batch's extraction
 * reply, continued in the same request history.  Code shows the model what
 * it extracted (C), the stored facts, relations and entities near each one
 * (S, R, E), what the user forgot (F) and what an earlier batch set aside
 * (D); the model answers with one JSON plan giving each C a disposition (and
 * at most one repair round when part of it can't apply).  Code checks every
 * operation against fixed guards, applies those that pass, and records each
 * with what it changed (memory_db_consolidation.h).
 *
 * Runs on the extraction thread, under the user's extraction slot.
 */

#ifndef MEMORY_CONSOLIDATE_H
#define MEMORY_CONSOLIDATE_H

#include <stdbool.h>
#include <stdint.h>

#include "llm/llm_interface.h"
#include "memory/memory_forgotten.h"
#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/** Input tokens a batch's consolidation rounds may use together (cached ones
 *  count; the extraction samples are extraction's cost, not counted here). */
#define MEMORY_CONSOLIDATE_INPUT_BUDGET 65536
/** Largest request one round may send, in tokens: a conversation too long for
 *  it gets no round. */
#define MEMORY_CONSOLIDATE_REQUEST_MAX 150000
/** Output tokens one consolidation reply may use. */
#define MEMORY_CONSOLIDATE_OUTPUT_BUDGET 4096

/**
 * @brief One model call: @p history (OpenAI-shaped messages) on @p cfg
 * @param prompt_tokens Receives the call's whole input, cached included
 * @param output_tokens Receives its output
 * @return The reply (caller frees), or NULL on failure
 */
typedef char *(*memory_consolidate_model_fn)(struct json_object *history,
                                             const llm_resolved_config_t *cfg,
                                             int *prompt_tokens,
                                             int *output_tokens);

/** Tests only: script the model's replies (NULL restores the real call). */
void memory_consolidate_set_model_fn(memory_consolidate_model_fn fn);

/** Extraction samples a batch takes: the extraction's own reply.  A second
 *  sample's recall gain was within run-to-run noise and it doubled the
 *  near-duplicates stored. */
#define MEMORY_CONSOLIDATE_SAMPLES 1

/** Benchmarks only: take @p n samples (1 or 2; anything else is
 *  MEMORY_CONSOLIDATE_SAMPLES).  The daemon never calls this, so a second
 *  sample is taken only for a benchmark. */
void memory_consolidate_set_samples(int n);

/** memory_consolidate_batch(): the conversation may no longer be learned from
 *  (made private, deleted): nothing was stored. */
#define MEMORY_CONSOLIDATE_REVOKED 2

/** Why a batch's consolidation ended (memory_consolidation_batches.stop). */
typedef enum {
   MEMORY_CONSOLIDATE_GO_ON = 0,     /* not ended: the next round runs */
   MEMORY_CONSOLIDATE_FINISHED,      /* every C disposed, nothing rejected */
   MEMORY_CONSOLIDATE_TRIAGE,        /* nothing needed the model's judgment */
   MEMORY_CONSOLIDATE_REPAIR_FAILED, /* the repair left something rejected or undisposed */
   MEMORY_CONSOLIDATE_CANCELLED,     /* the daemon is stopping */
   MEMORY_CONSOLIDATE_TOKENS,        /* over the input or output budget */
} memory_consolidate_stop_t;

/** The stop's name as recorded ("finished", "triage", ...). */
const char *memory_consolidate_stop_name(memory_consolidate_stop_t stop);

/** Where a batch's consolidation stands, for memory_consolidate_should_stop. */
typedef struct {
   bool complete;    /* the plan so far disposes every C, nothing rejected */
   bool repaired;    /* the repair round was used */
   bool cancelled;   /* the daemon is stopping */
   int input_tokens; /* the consolidation rounds' input so far */
   int next_request; /* the next round's request, estimated (0: none) */
   int request_max;  /* the largest request a round may send (0: MEMORY_CONSOLIDATE_REQUEST_MAX) */
   int max_output;   /* the largest consolidation reply's output */
} memory_consolidate_progress_t;

/**
 * @brief Whether to end, and why, in order: finished, repair_failed,
 *        cancelled, tokens (the rounds' budget, or a next request over
 *        MEMORY_CONSOLIDATE_REQUEST_MAX); MEMORY_CONSOLIDATE_GO_ON to run the
 *        next round
 */
memory_consolidate_stop_t memory_consolidate_should_stop(const memory_consolidate_progress_t *p);

/** A batch to consolidate: its extraction (round 0) already made. */
typedef struct {
   int user_id;
   int64_t conversation_id;
   const char *session_id;
   int message_count; /* new messages in the batch (the summary's) */
   int duration_seconds;
   const memory_provenance_t *prov; /* its source range and outside text */
   int64_t first_msg_id;            /* the first saved row it read (0: none) */
   memory_forgotten_list_t *forgotten;
   const llm_resolved_config_t *cfg; /* the model that answered round 0 */
   struct json_object *history;      /* round 0's request: the extraction prompt */
   const char *reply;                /* round 0's reply */
   int input_tokens;                 /* round 0's input */
   /* Each extraction reply as the model returned it (the bench's candidates
    * dump); NULL for none. */
   void (*on_reply)(int64_t conversation_id, const char *reply, int sample);
   /* Whether the conversation may still be learned from, asked again just
    * before anything is stored (NULL: always). */
   bool (*allowed)(int user_id, int64_t conversation_id);
} memory_consolidate_input_t;

/** What a batch's consolidation did. */
typedef struct {
   memory_consolidate_stop_t stop;
   bool partial;     /* only what can't lose a stored item was applied */
   int64_t batch_id; /* the record's (0: not recorded) */
   int rounds;       /* model replies after the extraction */
   int calls;        /* model calls, the extractions included */
   int input_tokens;
   int candidates; /* after the samples' union (same text once) */
   int to_model;   /* sent to the model (triage left them) */
   int triaged_reinforced, triaged_forgotten, triaged_new;
   int added, reinforced, merged, replaced, closed, linked, proposed, deferred, dropped;
   int suggested_forget, refused, rejected;
} memory_consolidate_result_t;

/**
 * @brief Consolidate a batch: the extraction's candidates, triage, the step
 *        that decides each candidate, the guards, then storing and recording
 *        what passed (and the batch's preferences, summary and title)
 *
 * @param out May be NULL
 * @return SUCCESS when the batch was stored (whatever its stop),
 *         MEMORY_CONSOLIDATE_REVOKED when @p in's allowed() said no (nothing
 *         stored: the entities the batch created are removed), FAILURE when
 *         nothing could be (the reply unreadable, out of memory)
 */
int memory_consolidate_batch(const memory_consolidate_input_t *in,
                             memory_consolidate_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_CONSOLIDATE_H */
