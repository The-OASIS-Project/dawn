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
 * @file input_queue.c
 * @brief Implementation of unified input queue.
 */

#include "input_queue.h"

#include <pthread.h>
#include <string.h>

#include "logging.h"
#include "utils/string_utils.h"

/* Circular buffer for queued items */
static queued_input_t g_queue[INPUT_QUEUE_MAX_ITEMS];
static int g_head = 0;  /* Next position to read from */
static int g_tail = 0;  /* Next position to write to */
static int g_count = 0; /* Number of items in queue */

/* Mutex for thread-safe access */
static pthread_mutex_t g_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Source name strings */
static const char *g_source_names[] = {
   "voice",    /* INPUT_SOURCE_VOICE */
   "TUI",      /* INPUT_SOURCE_TUI */
   "MQTT",     /* INPUT_SOURCE_MQTT */
   "network",  /* INPUT_SOURCE_NETWORK */
   "REST",     /* INPUT_SOURCE_REST */
   "WebSocket" /* INPUT_SOURCE_WEBSOCKET */
};

int input_queue_has_item(void) {
   int result;

   pthread_mutex_lock(&g_queue_mutex);
   result = (g_count > 0);
   pthread_mutex_unlock(&g_queue_mutex);

   return result;
}

int input_queue_get_count(void) {
   int count;

   pthread_mutex_lock(&g_queue_mutex);
   count = g_count;
   pthread_mutex_unlock(&g_queue_mutex);

   return count;
}

int input_queue_pop(queued_input_t *out) {
   int result = 0;

   if (out == NULL) {
      return 0;
   }

   pthread_mutex_lock(&g_queue_mutex);

   if (g_count > 0) {
      /* Copy item to output */
      *out = g_queue[g_head];

      /* Advance head pointer */
      g_head = (g_head + 1) % INPUT_QUEUE_MAX_ITEMS;
      g_count--;

      result = 1;
   }

   pthread_mutex_unlock(&g_queue_mutex);

   return result;
}

int input_queue_push(input_source_t source, const char *text) {
   input_source_t dropped_source = INPUT_SOURCE_COUNT; /* Sentinel for "no drop" */
   size_t text_len;
   int truncated = 0;

   if (text == NULL) {
      return 0;
   }

   /* Validate source */
   if (source < 0 || source >= INPUT_SOURCE_COUNT) {
      OLOG_ERROR("Invalid input source: %d", (int)source);
      return 0;
   }

   text_len = strlen(text);
   truncated = (text_len > INPUT_QUEUE_MAX_TEXT);

   pthread_mutex_lock(&g_queue_mutex);

   /* Check if queue is full - drop oldest if so (capture info for logging) */
   if (g_count >= INPUT_QUEUE_MAX_ITEMS) {
      dropped_source = g_queue[g_head].source;
      g_head = (g_head + 1) % INPUT_QUEUE_MAX_ITEMS;
      g_count--;
   }

   /* Add new item at tail */
   g_queue[g_tail].source = source;
   safe_strscpy(g_queue[g_tail].text, text);

   /* Advance tail pointer */
   g_tail = (g_tail + 1) % INPUT_QUEUE_MAX_ITEMS;
   g_count++;

   pthread_mutex_unlock(&g_queue_mutex);

   /* Log AFTER releasing mutex to avoid blocking while holding lock */
   if (dropped_source != INPUT_SOURCE_COUNT) {
      OLOG_WARNING("Input queue full, dropped oldest item from %s",
                   input_source_name(dropped_source));
   }

   if (truncated) {
      OLOG_WARNING("Input from %s truncated from %zu to %d chars", input_source_name(source),
                   text_len, INPUT_QUEUE_MAX_TEXT);
   }

   OLOG_INFO("Input queued from %s: %.50s%s", input_source_name(source), text,
             text_len > 50 ? "..." : "");

   return 1;
}

const char *input_source_name(input_source_t source) {
   if (source >= 0 && source < INPUT_SOURCE_COUNT) {
      return g_source_names[source];
   }
   return "unknown";
}

void input_queue_clear(void) {
   pthread_mutex_lock(&g_queue_mutex);

   g_head = 0;
   g_tail = 0;
   g_count = 0;

   pthread_mutex_unlock(&g_queue_mutex);

   OLOG_INFO("Input queue cleared");
}
