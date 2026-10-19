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
 * A music player's play history (see music_nav.h).
 */

#include "audio/music_nav.h"

#include <stdlib.h>
#include <string.h>

void music_history_init(music_history_t *h) {
   memset(h, 0, sizeof(*h));
   h->pos = -1;
}

void music_history_clear(music_history_t *h) {
   for (int i = 0; i < h->len; i++) {
      free(h->path[i]);
      h->path[i] = NULL;
   }
   h->len = 0;
   h->pos = -1;
}

void music_history_sync(music_history_t *h, uint32_t queue_epoch) {
   if (h->epoch != queue_epoch) {
      music_history_clear(h);
      h->epoch = queue_epoch;
   }
}

const char *music_history_current(const music_history_t *h) {
   return h->pos >= 0 ? h->path[h->pos] : NULL;
}

/* Remove entry i; an entry before the current one, or the current one itself,
 * moves the current back by one */
static void remove_at(music_history_t *h, int i) {
   free(h->path[i]);
   memmove(&h->path[i], &h->path[i + 1], (size_t)(h->len - i - 1) * sizeof(h->path[0]));
   memmove(&h->hint[i], &h->hint[i + 1], (size_t)(h->len - i - 1) * sizeof(h->hint[0]));
   h->len--;
   h->path[h->len] = NULL;
   if (i <= h->pos) {
      h->pos--;
   }
}

/* Cut the forward entries and append path after the current one; false when
 * it couldn't be copied (the history then just misses this track) */
static bool append_after_current(music_history_t *h, const char *path, int hint) {
   while (h->len > h->pos + 1) {
      remove_at(h, h->len - 1);
   }
   if (h->len == MUSIC_HISTORY_MAX) {
      remove_at(h, 0);
   }
   char *copy = strdup(path);
   if (!copy) {
      return false;
   }
   h->path[h->len] = copy;
   h->hint[h->len] = hint;
   h->len++;
   return true;
}

void music_history_push(music_history_t *h, const char *path, int hint) {
   if (append_after_current(h, path, hint)) {
      h->pos = h->len - 1;
   }
}

void music_history_plan(music_history_t *h, const char *path, int hint) {
   (void)append_after_current(h, path, hint);
}

int music_history_step(music_history_t *h,
                       bool forward,
                       bool move,
                       music_nav_find_fn find,
                       void *ctx) {
   int i = forward ? h->pos + 1 : h->pos - 1;
   while (i >= 0 && i < h->len) {
      int index = find(ctx, h->path[i], h->hint[i]);
      if (index >= 0) {
         if (move) {
            h->pos = i;
         }
         return index;
      }
      /* No longer queued: drop it and try the next one along */
      remove_at(h, i);
      if (!forward) {
         i--;
      }
   }
   return -1;
}

void music_history_drop_current(music_history_t *h) {
   if (h->pos >= 0) {
      remove_at(h, h->pos);
   }
}
