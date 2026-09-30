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
 * The compaction core: how big a history is, how small a compaction must make
 * it, and a summary of the part it drops, escalating from an LLM summary to a
 * shorter one to a mechanical one.  Pure: the summarizer's provider call comes
 * in as a function, so it holds no session, database or provider state.
 */

#ifndef LLM_COMPACTION_H
#define LLM_COMPACTION_H

#include <stdatomic.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

#define LLM_CONTEXT_SUMMARY_TARGET_L3 150 /* Hard budget for L3 mechanical truncation */

/* An image's share of the estimate: vision providers bill an image by its
 * pixel tiles, not its encoded size, so a flat, generous charge per image
 * (about 2000 tokens) beats scaling with base64 length. */
#define LLM_COMPACTION_IMAGE_ESTIMATE_CHARS 8000
#define LLM_COMPACTION_KEEP_EXCHANGES 2 /* Exchanges a compaction keeps */

/**
 * @brief Compaction escalation levels: each is tried until one fits, and the
 *        last always does
 */
typedef enum {
   LLM_COMPACT_NORMAL = 0,       /* An LLM summary, in prose (about 100 words) */
   LLM_COMPACT_AGGRESSIVE = 1,   /* An LLM summary, at most five bullets */
   LLM_COMPACT_DETERMINISTIC = 2 /* Mechanical: one short line per message, no LLM call */
} llm_compaction_level_t;

#define LLM_COMPACT_MAX_LEVEL LLM_COMPACT_DETERMINISTIC

/**
 * @brief The tokens a compaction aims for: well under the hard threshold, so
 *        the history doesn't cross it again at once
 * @param context_size The model's context window, in tokens
 * @param threshold The hard threshold (fraction of the window)
 */
int llm_compaction_target_tokens(int context_size, float threshold);

/**
 * @brief Estimated tokens of @p history's messages [@p start_idx, @p end_idx):
 *        text, tool payloads, replayed reasoning, a flat charge per image
 */
int llm_compaction_estimate_range(struct json_object *history, int start_idx, int end_idx);

/**
 * @brief A mechanical summary of @p to_summarize (one short line per message)
 *        within about @p token_budget tokens; heap, caller frees
 */
char *llm_compaction_deterministic(struct json_object *to_summarize, int token_budget);

/**
 * @brief An LLM summary of @p to_summarize at @p level (normal or aggressive);
 *        heap (caller frees), or NULL when the call fails.
 */
typedef char *(*llm_compaction_summarize_fn)(struct json_object *to_summarize,
                                             llm_compaction_level_t level,
                                             void *ctx);

/*
 * Calibration: the estimate (characters / 4) counts only a history's text, and
 * a model's tokenizer reads text at its own density.  A request's real size is
 * a fixed part (tool schemas, the system prompt, framing: not in the estimate)
 * plus the history at the model's density:
 *
 *    tokens ~= fixed + factor * estimate
 *
 * The factor is learned per model from the growth between two requests of one
 * conversation (the fixed part cancels out), the fixed part per conversation
 * from its last request.
 */
#define LLM_COMPACTION_FACTOR_MIN 0.5f
#define LLM_COMPACTION_FACTOR_MAX 4.0f
/* Growth (estimated tokens) a sample needs: less is noise. */
#define LLM_COMPACTION_FACTOR_MIN_GROWTH 1000
/* How far one sample moves the factor. */
#define LLM_COMPACTION_FACTOR_WEIGHT 0.3f

/**
 * @brief @p factor after one more sample: a request @p d_prompt real tokens
 *        and @p d_estimate estimated tokens bigger than the one before it
 *        (@p samples before this one; the first sample is taken whole)
 * @return The new factor, or @p factor when the sample says nothing
 */
float llm_compaction_factor_update(float factor, int samples, int d_prompt, int d_estimate);

/** What a conversation's requests say about its model's reading (above). */
typedef struct {
   bool known;        /**< a request of this conversation was measured */
   int last_prompt;   /**< its real size */
   int last_estimate; /**< its history's estimate */
   float last_factor; /**< the density of the model it ran on */
   float factor;      /**< the density of the model this one runs on */
} llm_compaction_calibration_t;

/**
 * @brief A request's real size, its history estimated at @p estimate: the
 *        fixed part (rescaled to this model) plus the history at its density;
 *        @p estimate itself when nothing is known
 */
int llm_compaction_calibrated_tokens(const llm_compaction_calibration_t *cal, int estimate);

/**
 * @brief The inverse: the history estimate a request of @p tokens real
 *        tokens leaves room for (never below 0); @p tokens when nothing is known
 */
int llm_compaction_estimate_budget(const llm_compaction_calibration_t *cal, int tokens);

/** Maximum bytes of a summary (capped when it is made, never on reload). */
#define LLM_COMPACTION_SUMMARY_MAX 16384

/**
 * @brief Summarize @p to_summarize, escalating until the history fits
 *
 * L1 (@p summarize, normal), L2 (@p summarize, aggressive), then L3
 * (mechanical, always accepted): the first whose result, with @p kept_tokens,
 * fits @p target_tokens.  An L1 longer than its input skips L2.  The text is
 * neutralized and @p tag's secret masked: it was written from untrusted
 * history, and is replayed as DAWN's.
 *
 * @param cancel Set when the summary is no longer wanted (NULL: never): a
 *        failed call then ends it, without the mechanical fallback
 * @param level_out The level used
 * @return The summary (heap, caller frees), or NULL if every level failed or
 *         it was cancelled
 */
char *llm_compaction_summarize(struct json_object *to_summarize,
                               int kept_tokens,
                               int target_tokens,
                               const char *tag,
                               llm_compaction_summarize_fn summarize,
                               void *ctx,
                               const atomic_bool *cancel,
                               llm_compaction_level_t *level_out);

#ifdef __cplusplus
}
#endif

#endif /* LLM_COMPACTION_H */
