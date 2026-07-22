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
 * @file pending_system_msg.c
 * @brief Implementation of the deferred system-message queue (leaf mutex).
 */

#include "core/pending_system_msg.h"

#include <pthread.h>
#include <string.h>

#include "logging.h"

/* Fixed ring buffer of deferred messages. */
static char g_queue[PENDING_SYSMSG_MAX_ITEMS][PENDING_SYSMSG_MAX_TEXT + 1];
static int g_head = 0;  /* Next slot to read from */
static int g_tail = 0;  /* Next slot to write to */
static int g_count = 0; /* Items currently queued */

/* Leaf lock: no other lock is ever held across these calls. */
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

int pending_sysmsg_push(const char *text) {
   int dropped = 0;
   int truncated = 0;
   size_t text_len;

   if (text == NULL || text[0] == '\0') {
      return 0;
   }

   text_len = strlen(text);
   truncated = (text_len > PENDING_SYSMSG_MAX_TEXT);

   pthread_mutex_lock(&g_mutex);

   /* Full: drop the oldest to make room (FIFO eviction). */
   if (g_count >= PENDING_SYSMSG_MAX_ITEMS) {
      g_head = (g_head + 1) % PENDING_SYSMSG_MAX_ITEMS;
      g_count--;
      dropped = 1;
   }

   strncpy(g_queue[g_tail], text, PENDING_SYSMSG_MAX_TEXT);
   g_queue[g_tail][PENDING_SYSMSG_MAX_TEXT] = '\0';
   g_tail = (g_tail + 1) % PENDING_SYSMSG_MAX_ITEMS;
   g_count++;

   pthread_mutex_unlock(&g_mutex);

   /* Log after releasing the lock. */
   if (dropped) {
      OLOG_WARNING("Pending system-message queue full, dropped oldest item");
   }
   if (truncated) {
      OLOG_WARNING("Pending system message truncated from %zu to %d chars", text_len,
                   PENDING_SYSMSG_MAX_TEXT);
   }

   return 1;
}

int pending_sysmsg_pop(char *out, size_t out_size) {
   int result = 0;

   if (out == NULL || out_size == 0) {
      return 0;
   }

   pthread_mutex_lock(&g_mutex);

   if (g_count > 0) {
      strncpy(out, g_queue[g_head], out_size - 1);
      out[out_size - 1] = '\0';
      g_head = (g_head + 1) % PENDING_SYSMSG_MAX_ITEMS;
      g_count--;
      result = 1;
   }

   pthread_mutex_unlock(&g_mutex);

   return result;
}

int pending_sysmsg_has_item(void) {
   int result;

   pthread_mutex_lock(&g_mutex);
   result = (g_count > 0);
   pthread_mutex_unlock(&g_mutex);

   return result;
}

void pending_sysmsg_clear(void) {
   pthread_mutex_lock(&g_mutex);
   g_head = 0;
   g_tail = 0;
   g_count = 0;
   pthread_mutex_unlock(&g_mutex);
}
