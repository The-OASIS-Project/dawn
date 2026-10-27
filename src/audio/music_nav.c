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
 * A music player's play history and the rule for what plays next (see music_nav.h).
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

bool music_history_record(music_history_t *h, const char *path, int hint) {
   const char *current = music_history_current(h);
   if (current && strcmp(current, path) == 0) {
      return false;
   }
   music_history_push(h, path, hint);
   return true;
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

/* ---- What plays next ---------------------------------------------------- */

/* A random position other than exclude (-1 for none) */
static int random_index(int exclude, int length, unsigned int *seed) {
   if (length <= 1) {
      return exclude;
   }
   int index;
   do {
      index = rand_r(seed) % length;
   } while (index == exclude);
   return index;
}

static bool record(music_history_t *h, const music_nav_queue_t *q, int index) {
   return music_history_record(h, q->path_at(q->ctx, index), index);
}

/* Under shuffle: the history's next entry, else a new random pick other than
 * pos, recorded (length > 1) */
static int shuffle_next(music_history_t *h,
                        const music_nav_queue_t *q,
                        int pos,
                        unsigned int *seed) {
   int next = music_history_step(h, true, true, q->find, q->ctx);
   if (next < 0) {
      next = random_index(pos, q->length, seed);
      music_history_push(h, q->path_at(q->ctx, next), next);
   }
   return next;
}

static void pick_auto(music_history_t *h,
                      const music_nav_queue_t *q,
                      const music_nav_request_t *rq,
                      unsigned int *seed,
                      music_nav_pick_t *out) {
   int len = q->length;
   int next = rq->pos;
   bool repeat_one = q->repeat_one && rq->pos >= 0;
   bool recorded = false;

   if (len == 0) {
      out->result = MUSIC_NAV_STOP_END;
      return;
   } else if (rq->track_failed && repeat_one) {
      out->result = MUSIC_NAV_STOP_REPEAT_FAILED;
      return;
   } else if (rq->failures >= len) {
      out->result = MUSIC_NAV_STOP_ALL_FAILED;
      return;
   } else if (rq->failures >= MUSIC_NAV_MAX_FAILED) {
      out->result = MUSIC_NAV_STOP_TOO_MANY;
      return;
   } else if (repeat_one) {
      /* Replay the same track */
   } else if (q->shuffle && rq->failures > 0) {
      /* Step past a failure in order: random picks can repeat, so a few
       * playable tracks could go untried before the cap is reached */
      next = (next + 1 + len) % len;
   } else if (q->shuffle) {
      if (len > 1) {
         next = shuffle_next(h, q, next, seed);
         recorded = true;
      }
   } else if (++next >= len) {
      if (!q->repeat_all) {
         out->result = MUSIC_NAV_STOP_END;
         return;
      }
      next = 0;
   }

   if (next < 0 || next >= len) {
      /* The queue shrank under a repeat-one or stale position, or nothing
       * above picked from it (shuffle with one track, not held) */
      next = 0;
   }
   out->result = MUSIC_NAV_PLAY;
   out->index = next;
   /* A replay of the current track adds nothing */
   out->fresh = recorded || record(h, q, next);
}

static void pick_user(music_history_t *h,
                      const music_nav_queue_t *q,
                      const music_nav_request_t *rq,
                      unsigned int *seed,
                      music_nav_pick_t *out) {
   int len = q->length;
   int pos = rq->pos;
   if (len == 0) {
      return;
   }
   if (q->shuffle && rq->dir == MUSIC_NAV_PREVIOUS) {
      /* Back through what played, whatever the queue's size: nothing earlier
       * is nothing earlier, not a wrap */
      int back = music_history_step(h, false, rq->commit, q->find, q->ctx);
      if (back >= 0) {
         out->result = MUSIC_NAV_PLAY;
         out->index = back;
      }
      return;
   }
   if (q->shuffle && len > 1) {
      out->result = MUSIC_NAV_PLAY;
      out->index = rq->commit ? shuffle_next(h, q, pos, seed) : -1;
      return;
   }
   /* -1 (the queue doesn't hold the track): next is its top, nothing is before it */
   if (pos > len - 1) {
      pos = len - 1; /* removes left the position past the end */
   }
   int next = -1;
   if (rq->dir == MUSIC_NAV_NEXT && pos < len - 1) {
      next = pos + 1;
   } else if (rq->dir == MUSIC_NAV_PREVIOUS && pos > 0) {
      next = pos - 1;
   } else if (q->repeat_all) {
      next = rq->dir == MUSIC_NAV_NEXT ? 0 : len - 1;
   }
   if (next < 0) {
      return;
   }
   if (rq->commit) {
      (void)record(h, q, next);
   }
   out->result = MUSIC_NAV_PLAY;
   out->index = next;
}

static void pick_plan(music_history_t *h,
                      const music_nav_queue_t *q,
                      const music_nav_request_t *rq,
                      unsigned int *seed,
                      music_nav_pick_t *out) {
   int len = q->length;
   int next = -1;
   if (len == 0 || q->repeat_one) {
      return; /* repeat-one replays the current track: nothing to plan */
   }
   if (q->shuffle && len > 1) {
      /* The history's next entry, else a new pick planned as that entry, so
       * next and auto-advance play the track planned here */
      next = music_history_step(h, true, false, q->find, q->ctx);
      if (next < 0) {
         next = random_index(rq->pos, len, seed);
         music_history_plan(h, q->path_at(q->ctx, next), next);
      }
   } else if (q->shuffle) {
      return; /* a single track under shuffle: nothing follows */
   } else if (rq->pos + 1 < len) {
      next = rq->pos + 1;
   } else if (q->repeat_all) {
      next = 0;
   } else {
      return;
   }
   out->result = MUSIC_NAV_PLAY;
   out->index = next;
}

void music_nav_pick(music_history_t *h,
                    const music_nav_queue_t *q,
                    const music_nav_request_t *rq,
                    unsigned int *seed,
                    music_nav_pick_t *out) {
   out->result = MUSIC_NAV_NONE;
   out->index = -1;
   out->fresh = false;
   music_history_sync(h, q->epoch);
   switch (rq->reason) {
      case MUSIC_NAV_AUTO:
         pick_auto(h, q, rq, seed, out);
         break;
      case MUSIC_NAV_USER:
         pick_user(h, q, rq, seed, out);
         break;
      case MUSIC_NAV_PLAN:
         pick_plan(h, q, rq, seed, out);
         break;
   }
}
