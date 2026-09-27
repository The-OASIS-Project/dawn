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
 * The cache monitor's pure parts, for its unit test.
 */

#ifndef LLM_CACHE_MONITOR_INTERNAL_H
#define LLM_CACHE_MONITOR_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "llm/llm_cache_monitor.h"

/** A request's cacheable prefix, hashed by part so a change can be named. */
typedef struct {
   uint32_t tools;    /**< The tools JSON (0 = none) */
   uint32_t system;   /**< The stable system text */
   uint32_t model;    /**< The model name */
   uint32_t thinking; /**< The thinking / effort settings */
} llm_cache_prefix_t;

/**
 * @brief The tokens a conversation call should find cached, then remember it
 *
 * The previous call on the key (@p session_id, @p conversation_id) left
 * @c cached_after (its read + write) in the cache; this call should read it,
 * unless it is the first call, more than @p ttl_ms after the previous one, or
 * under a different prefix: then 0, and @p state says which.  Records this
 * call (@p cached_after at @p now) for the next.  @p gap_ms gets the time since
 * the previous call (0 when none).
 */
int llm_cache_monitor_expected_read_at(uint32_t session_id,
                                       int64_t conversation_id,
                                       const llm_cache_prefix_t *prefix,
                                       int ttl_ms,
                                       int cached_after,
                                       const char *message_id,
                                       uint64_t now,
                                       uint64_t *gap_ms,
                                       llm_cache_state_t *state);

/** The calling thread's noted request: its prefix and model; false if none. */
bool llm_cache_monitor_noted(llm_cache_prefix_t *prefix, const char **model);

/** Forget every cache key (tests). */
void llm_cache_monitor_reset_keys(void);

#endif /* LLM_CACHE_MONITOR_INTERNAL_H */
