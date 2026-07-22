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
 * @file conv_stream.c
 * @brief Implementation of the conversation-keyed live-partial replay ring.
 */

#include "core/conv_stream.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
   int64_t conv_id;    /* 0 = free slot */
   uint32_t stream_id; /* current turn id */
   char *buf;          /* heap partial buffer (NULL until first append) */
   size_t len;         /* bytes used (excludes NUL) */
   size_t cap;         /* bytes allocated */
   bool active;        /* turn in flight */
   bool truncated;     /* partial hit CONV_STREAM_MAX_PARTIAL_BYTES */
   time_t updated_at;  /* last mutation (for evict-grace + LRU reclaim) */
} conv_stream_slot_t;

static conv_stream_slot_t s_slots[CONV_STREAM_MAX_SLOTS];
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;

/* --- helpers (all called with s_mutex held) ------------------------------- */

static conv_stream_slot_t *find_slot(int64_t conv_id) {
   for (int i = 0; i < CONV_STREAM_MAX_SLOTS; i++) {
      if (s_slots[i].conv_id == conv_id) {
         return &s_slots[i];
      }
   }
   return NULL;
}

/* Acquire a slot for conv_id: existing, else a free one, else LRU-reclaim the
 * least-recently-updated slot (freeing its buffer). Never returns NULL. */
static conv_stream_slot_t *acquire_slot(int64_t conv_id, time_t now) {
   conv_stream_slot_t *slot = find_slot(conv_id);
   if (slot != NULL) {
      return slot;
   }

   /* Prefer reclaiming a non-active slot; only evict an active (mid-stream) one
    * when all slots are active. */
   conv_stream_slot_t *lru_any = NULL;
   conv_stream_slot_t *lru_inactive = NULL;
   for (int i = 0; i < CONV_STREAM_MAX_SLOTS; i++) {
      if (s_slots[i].conv_id == 0) {
         s_slots[i].conv_id = conv_id;
         s_slots[i].stream_id = 0;
         s_slots[i].len = 0;
         if (s_slots[i].cap > 0 && s_slots[i].buf != NULL) {
            s_slots[i].buf[0] = '\0';
         }
         s_slots[i].active = false;
         s_slots[i].truncated = false;
         s_slots[i].updated_at = now;
         return &s_slots[i];
      }
      if (lru_any == NULL || s_slots[i].updated_at < lru_any->updated_at) {
         lru_any = &s_slots[i];
      }
      if (!s_slots[i].active &&
          (lru_inactive == NULL || s_slots[i].updated_at < lru_inactive->updated_at)) {
         lru_inactive = &s_slots[i];
      }
   }

   /* Table full: reclaim the LRU (non-active if any). */
   conv_stream_slot_t *victim = (lru_inactive != NULL) ? lru_inactive : lru_any;
   free(victim->buf);
   victim->buf = NULL;
   victim->cap = 0;
   victim->len = 0;
   victim->conv_id = conv_id;
   victim->stream_id = 0;
   victim->active = false;
   victim->truncated = false;
   victim->updated_at = now;
   return victim;
}

static void slot_reset_partial(conv_stream_slot_t *slot) {
   slot->len = 0;
   slot->truncated = false;
   if (slot->buf != NULL && slot->cap > 0) {
      slot->buf[0] = '\0';
   }
}

/* --- public API ----------------------------------------------------------- */

void conv_stream_begin(int64_t conversation_id, uint32_t stream_id) {
   if (conversation_id <= 0) {
      return;
   }
   time_t now = time(NULL);
   pthread_mutex_lock(&s_mutex);
   conv_stream_slot_t *slot = acquire_slot(conversation_id, now);
   slot->stream_id = stream_id;
   slot->active = true;
   slot->updated_at = now;
   slot_reset_partial(slot);
   pthread_mutex_unlock(&s_mutex);
}

void conv_stream_append(int64_t conversation_id, uint32_t stream_id, const char *text) {
   if (conversation_id <= 0 || text == NULL || text[0] == '\0') {
      return;
   }
   size_t add = strlen(text);
   time_t now = time(NULL);

   pthread_mutex_lock(&s_mutex);
   conv_stream_slot_t *slot = acquire_slot(conversation_id, now);

   /* A newer turn supersedes (implicit begin) even if the previous turn is still
    * marked active (its end was missed); an older/stale turn id is ignored; an
    * equal id continues the current partial.  stream_id is monotonic per
    * conversation (mirrors session->current_stream_id). */
   if (stream_id > slot->stream_id) {
      slot->stream_id = stream_id;
      slot->active = true;
      slot_reset_partial(slot);
   } else if (stream_id != slot->stream_id) {
      pthread_mutex_unlock(&s_mutex);
      return;
   }
   if (slot->truncated) {
      slot->updated_at = now;
      pthread_mutex_unlock(&s_mutex);
      return;
   }

   /* Clamp to the per-turn cap.  Only a genuine overflow past the cap is
    * truncation; an append that exactly fills to the cap loses nothing. */
   size_t room = CONV_STREAM_MAX_PARTIAL_BYTES - slot->len;
   if (add > room) {
      add = room;
      slot->truncated = true;
   }
   if (add > 0) {
      size_t need = slot->len + add + 1; /* +1 NUL */
      if (need > slot->cap) {
         size_t new_cap = (slot->cap == 0) ? 256 : slot->cap;
         while (new_cap < need) {
            new_cap *= 2;
         }
         if (new_cap > CONV_STREAM_MAX_PARTIAL_BYTES + 1) {
            new_cap = CONV_STREAM_MAX_PARTIAL_BYTES + 1;
         }
         char *nb = realloc(slot->buf, new_cap);
         if (nb == NULL) {
            /* Allocation failure: keep what we have, mark truncated. */
            slot->truncated = true;
            slot->updated_at = now;
            pthread_mutex_unlock(&s_mutex);
            return;
         }
         slot->buf = nb;
         slot->cap = new_cap;
      }
      memcpy(slot->buf + slot->len, text, add);
      slot->len += add;
      slot->buf[slot->len] = '\0';
   }
   slot->updated_at = now;
   pthread_mutex_unlock(&s_mutex);
}

void conv_stream_end(int64_t conversation_id, uint32_t stream_id) {
   if (conversation_id <= 0) {
      return;
   }
   pthread_mutex_lock(&s_mutex);
   conv_stream_slot_t *slot = find_slot(conversation_id);
   if (slot != NULL && slot->stream_id == stream_id) {
      slot->active = false;
      slot->updated_at = time(NULL);
   }
   pthread_mutex_unlock(&s_mutex);
}

char *conv_stream_dup_partial(int64_t conversation_id,
                              bool *active_out,
                              uint32_t *stream_id_out,
                              bool *truncated_out) {
   if (conversation_id <= 0) {
      return NULL;
   }
   char *copy = NULL;
   pthread_mutex_lock(&s_mutex);
   conv_stream_slot_t *slot = find_slot(conversation_id);
   if (slot != NULL && slot->len > 0 && slot->buf != NULL) {
      copy = malloc(slot->len + 1);
      if (copy != NULL) {
         memcpy(copy, slot->buf, slot->len);
         copy[slot->len] = '\0';
         if (active_out != NULL) {
            *active_out = slot->active;
         }
         if (stream_id_out != NULL) {
            *stream_id_out = slot->stream_id;
         }
         if (truncated_out != NULL) {
            *truncated_out = slot->truncated;
         }
      }
   }
   pthread_mutex_unlock(&s_mutex);
   return copy;
}

void conv_stream_clear(int64_t conversation_id) {
   if (conversation_id <= 0) {
      return;
   }
   pthread_mutex_lock(&s_mutex);
   conv_stream_slot_t *slot = find_slot(conversation_id);
   if (slot != NULL) {
      free(slot->buf);
      memset(slot, 0, sizeof(*slot));
   }
   pthread_mutex_unlock(&s_mutex);
}

void conv_stream_evict_stale(time_t now) {
   pthread_mutex_lock(&s_mutex);
   for (int i = 0; i < CONV_STREAM_MAX_SLOTS; i++) {
      conv_stream_slot_t *slot = &s_slots[i];
      if (slot->conv_id == 0) {
         continue;
      }
      time_t age = now - slot->updated_at;
      bool aged_finalized = !slot->active && age > CONV_STREAM_EVICT_GRACE_SEC;
      /* Self-healing backstop: an active turn left stale this long lost its end. */
      bool abandoned_active = slot->active && age > CONV_STREAM_ACTIVE_MAX_AGE_SEC;
      if (aged_finalized || abandoned_active) {
         free(slot->buf);
         memset(slot, 0, sizeof(*slot));
      }
   }
   pthread_mutex_unlock(&s_mutex);
}

void conv_stream_reset(void) {
   pthread_mutex_lock(&s_mutex);
   for (int i = 0; i < CONV_STREAM_MAX_SLOTS; i++) {
      free(s_slots[i].buf);
      memset(&s_slots[i], 0, sizeof(s_slots[i]));
   }
   pthread_mutex_unlock(&s_mutex);
}
