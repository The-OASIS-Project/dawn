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
 * Cloud-to-local fallback state (see llm_local_fallback.h).  Per provider, lock
 * free: a window end (monotonic seconds) and its length, 0 when not in an outage.
 * Two turns racing at a window's end both try the provider; that's harmless.
 */

#include "llm/llm_local_fallback.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Atomic int64_t s_until[CLOUD_PROVIDER_NONE];
static _Atomic int s_window_sec[CLOUD_PROVIDER_NONE];

static bool known(cloud_provider_t provider) {
   return (unsigned)provider < (unsigned)CLOUD_PROVIDER_NONE;
}

bool llm_local_fallback_cloud_down(cloud_provider_t provider, int64_t now) {
   return known(provider) && now < atomic_load(&s_until[provider]);
}

bool llm_local_fallback_in_outage(cloud_provider_t provider) {
   return known(provider) && atomic_load(&s_window_sec[provider]) > 0;
}

void llm_local_fallback_mark_down(cloud_provider_t provider, int64_t now) {
   if (!known(provider)) {
      return;
   }
   const int prev = atomic_load(&s_window_sec[provider]);
   int window = prev > 0 ? prev * 2 : LLM_CLOUD_RETRY_FIRST_SEC;
   if (window > LLM_CLOUD_RETRY_MAX_SEC) {
      window = LLM_CLOUD_RETRY_MAX_SEC;
   }
   /* A mark_up racing these stores can leave an end with no length or a length
    * with no end; either clears at the next cloud answer, at worst costing one
    * window or one note. */
   atomic_store(&s_until[provider], now + window);
   atomic_store(&s_window_sec[provider], window);
}

void llm_local_fallback_mark_up(cloud_provider_t provider) {
   if (known(provider)) {
      atomic_store(&s_until[provider], 0);
      atomic_store(&s_window_sec[provider], 0);
   }
}

void llm_local_fallback_retry_now(cloud_provider_t provider) {
   if (known(provider)) {
      atomic_store(&s_until[provider], 0);
   }
}

char *llm_local_fallback_with_note(char *answer) {
   if (!answer || answer[0] == '\0') {
      return answer;
   }
   const size_t size = strlen(LLM_LOCAL_FALLBACK_NOTE) + strlen(answer) + 1;
   char *noted = malloc(size);
   if (!noted) {
      return answer;
   }
   snprintf(noted, size, "%s%s", LLM_LOCAL_FALLBACK_NOTE, answer);
   free(answer);
   return noted;
}
