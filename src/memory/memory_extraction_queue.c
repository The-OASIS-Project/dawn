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
 * Conversations waiting for memory extraction (memory_extraction_queue.h).
 * The caller holds the extraction mutex around every call.
 */

#define MEMORY_EXTRACTION_QUEUE_ALLOWED
#include "memory/memory_extraction_queue.h"

#include <string.h>

static extraction_queue_entry_t s_queue[EXTRACTION_QUEUE_MAX];
static int s_count;

/* Remove entry @p i, keeping the rest in order. */
static void remove_at(int i) {
   memmove(&s_queue[i], &s_queue[i + 1], (size_t)(s_count - i - 1) * sizeof(s_queue[0]));
   s_count--;
}

bool extraction_queue_push(const extraction_queue_entry_t *e) {
   if (!e || e->user_id <= 0 || e->conversation_id <= 0) {
      return false;
   }
   for (int i = 0; i < s_count; i++) {
      if (s_queue[i].conversation_id == e->conversation_id) {
         /* The newest request's session and model; its place in line stays. */
         s_queue[i] = *e;
         return true;
      }
   }
   if (s_count >= EXTRACTION_QUEUE_MAX) {
      return false;
   }
   s_queue[s_count++] = *e;
   return true;
}

bool extraction_queue_take(bool (*ready)(int user_id, void *ctx),
                           void *ctx,
                           extraction_queue_entry_t *out) {
   if (!ready || !out) {
      return false;
   }
   for (int i = 0; i < s_count; i++) {
      if (ready(s_queue[i].user_id, ctx)) {
         *out = s_queue[i];
         remove_at(i);
         return true;
      }
   }
   return false;
}

void extraction_queue_drop_conversations(int user_id, const int64_t *ids, int n) {
   for (int i = s_count - 1; i >= 0; i--) {
      if (s_queue[i].user_id != user_id) {
         continue;
      }
      for (int j = 0; ids && j < n; j++) {
         if (s_queue[i].conversation_id == ids[j]) {
            remove_at(i);
            break;
         }
      }
   }
}

void extraction_queue_drop_user(int user_id) {
   for (int i = s_count - 1; i >= 0; i--) {
      if (s_queue[i].user_id == user_id) {
         remove_at(i);
      }
   }
}

void extraction_queue_clear(void) {
   s_count = 0;
}

int extraction_queue_count(void) {
   return s_count;
}
