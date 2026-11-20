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
 * Cloud-to-local fallback state: which cloud providers are in an outage, how long
 * until each is tried again, and the note a local answer carries.  The decision
 * itself is in llm_chat_completion_streaming_with_config().
 */

#ifndef LLM_LOCAL_FALLBACK_H
#define LLM_LOCAL_FALLBACK_H

#include <stdbool.h>
#include <stdint.h>

#include "llm/llm_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The first outage window, doubled while the provider stays unreachable. */
#define LLM_CLOUD_RETRY_FIRST_SEC 60
/** The longest window between attempts at a provider that stays unreachable. */
#define LLM_CLOUD_RETRY_MAX_SEC 600

/** What the first local answer of an outage (or of a failing turn) starts with. */
#define LLM_LOCAL_FALLBACK_NOTE \
   "The cloud model isn't answering right now, so this answer is from the local model. "

/**
 * @brief Whether @p provider is inside its outage window at @p now (monotonic
 *        seconds): its turns go to the local LLM without trying it.
 */
bool llm_local_fallback_cloud_down(cloud_provider_t provider, int64_t now);

/** @brief Whether @p provider is in an outage (window running or expired, not yet answered). */
bool llm_local_fallback_in_outage(cloud_provider_t provider);

/**
 * @brief @p provider was unreachable at @p now and the local LLM answered: start
 *        or extend its outage (LLM_CLOUD_RETRY_FIRST_SEC, doubling to
 *        LLM_CLOUD_RETRY_MAX_SEC).
 */
void llm_local_fallback_mark_down(cloud_provider_t provider, int64_t now);

/** @brief @p provider answered: its outage is over. */
void llm_local_fallback_mark_up(cloud_provider_t provider);

/** @brief Try @p provider on its next turn (the local LLM failed); the outage goes on. */
void llm_local_fallback_retry_now(cloud_provider_t provider);

/**
 * @brief @p answer with LLM_LOCAL_FALLBACK_NOTE in front (takes ownership).
 *
 * @return The noted answer, @p answer itself when it's empty or on allocation
 *         failure, NULL when @p answer is NULL
 */
char *llm_local_fallback_with_note(char *answer);

#ifdef __cplusplus
}
#endif

#endif /* LLM_LOCAL_FALLBACK_H */
