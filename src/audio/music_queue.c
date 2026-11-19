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
 * Music queue: the per-user registry and the changes every player makes.
 */

#include "audio/music_queue.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "audio/music_queue_db.h"
#include "core/path_utils.h"
#include "dawn_error.h"
#include "logging.h"

/* Shared queues by user; a user's queue stays after its last player goes, so
 * the next one finds it in memory (an unused one is evicted when full) */
#define MAX_USER_QUEUES 32
static user_music_queue_t *s_user_queues[MAX_USER_QUEUES];
static int s_user_queue_count = 0;
static pthread_mutex_t s_user_queues_mutex = PTHREAD_MUTEX_INITIALIZER;

static _Atomic(music_queue_listener_t) s_listener = NULL;

static user_music_queue_t *new_queue(int user_id) {
   user_music_queue_t *uq = calloc(1, sizeof(*uq));
   if (!uq) {
      return NULL;
   }
   uq->user_id = user_id;
   pthread_mutex_init(&uq->queue_mutex, NULL);
   uq->ref_count = 1;
   return uq;
}

static void free_queue(user_music_queue_t *uq) {
   pthread_mutex_destroy(&uq->queue_mutex);
   free(uq);
}

int music_queue_init(const char *db_path) {
   return music_queue_db_init(db_path) == 0 ? SUCCESS : FAILURE;
}

void music_queue_shutdown(void) {
   pthread_mutex_lock(&s_user_queues_mutex);
   for (int i = 0; i < s_user_queue_count; i++) {
      /* A player still holding one (a tool call on a thread nothing joins
       * at exit) keeps it: its last release then finds it allocated */
      if (s_user_queues[i]->ref_count == 0) {
         free_queue(s_user_queues[i]);
      }
      s_user_queues[i] = NULL;
   }
   s_user_queue_count = 0;
   pthread_mutex_unlock(&s_user_queues_mutex);
   music_queue_db_cleanup();
}

/* The user's shared queue, or NULL (none free in the registry, no memory) */
static user_music_queue_t *shared_queue(int user_id) {
   pthread_mutex_lock(&s_user_queues_mutex);
   for (int i = 0; i < s_user_queue_count; i++) {
      if (s_user_queues[i]->user_id == user_id) {
         s_user_queues[i]->ref_count++;
         pthread_mutex_unlock(&s_user_queues_mutex);
         return s_user_queues[i];
      }
   }

   if (s_user_queue_count >= MAX_USER_QUEUES) {
      int idle = -1;
      for (int i = 0; i < s_user_queue_count && idle < 0; i++) {
         if (s_user_queues[i]->ref_count == 0) {
            idle = i;
         }
      }
      if (idle < 0) {
         OLOG_ERROR("Music queue: registry full (%d), all in use", MAX_USER_QUEUES);
         pthread_mutex_unlock(&s_user_queues_mutex);
         return NULL;
      }
      OLOG_INFO("Music queue: evicting idle queue for user %d", s_user_queues[idle]->user_id);
      free_queue(s_user_queues[idle]);
      memmove(&s_user_queues[idle], &s_user_queues[idle + 1],
              (size_t)(s_user_queue_count - idle - 1) * sizeof(s_user_queues[0]));
      s_user_queues[--s_user_queue_count] = NULL;
   }

   user_music_queue_t *uq = new_queue(user_id);
   if (uq) {
      music_queue_db_load(user_id, uq);
      s_user_queues[s_user_queue_count++] = uq;
      OLOG_INFO("Music queue: created shared queue for user %d (%d tracks restored)", user_id,
                uq->queue_length);
   }
   pthread_mutex_unlock(&s_user_queues_mutex);
   return uq;
}

user_music_queue_t *music_queue_acquire(int user_id) {
   user_music_queue_t *uq = user_id > 0 ? shared_queue(user_id) : NULL;
   return uq ? uq : new_queue(0);
}

void music_queue_release(user_music_queue_t *uq) {
   if (!uq) {
      return;
   }
   if (uq->user_id <= 0) {
      free_queue(uq);
      return;
   }
   pthread_mutex_lock(&s_user_queues_mutex);
   uq->ref_count--;
   pthread_mutex_unlock(&s_user_queues_mutex);
}

void music_queue_attach(user_music_queue_t *uq, music_queue_pos_t *pos) {
   pthread_mutex_lock(&uq->queue_mutex);
   pos->index = 0;
   pos->next = uq->positions;
   uq->positions = pos;
   pthread_mutex_unlock(&uq->queue_mutex);
}

void music_queue_detach(user_music_queue_t *uq, music_queue_pos_t *pos) {
   pthread_mutex_lock(&uq->queue_mutex);
   for (music_queue_pos_t **p = &uq->positions; *p; p = &(*p)->next) {
      if (*p == pos) {
         *p = pos->next;
         break;
      }
   }
   pthread_mutex_unlock(&uq->queue_mutex);
}

int music_queue_find(const user_music_queue_t *uq, const char *path, int hint) {
   if (hint >= 0 && hint < uq->queue_length && strcmp(uq->queue[hint].path, path) == 0) {
      return hint; /* still where it was: the nearest copy */
   }
   int best = -1;
   for (int i = 0; i < uq->queue_length; i++) {
      if (strcmp(uq->queue[i].path, path) == 0 && (best < 0 || abs(i - hint) < abs(best - hint))) {
         best = i;
      }
   }
   return best;
}

int music_queue_insert(user_music_queue_t *uq, int index, const music_search_result_t *track) {
   if (index < 0 || index > uq->queue_length) {
      return FAILURE;
   }
   if (uq->queue_length >= MUSIC_QUEUE_MAX && index >= MUSIC_QUEUE_MAX) {
      return FAILURE;
   }
   if (index < uq->queue_length) { /* an append moves nothing */
      /* A full queue drops its last track: a place on it (or already past
       * the end) stays just past the end */
      int end = uq->queue_length < MUSIC_QUEUE_MAX ? uq->queue_length + 1 : MUSIC_QUEUE_MAX;
      for (music_queue_pos_t *p = uq->positions; p; p = p->next) {
         if (p->index >= index) {
            p->index = p->index < end ? p->index + 1 : end;
         }
      }
   }
   if (uq->queue_length >= MUSIC_QUEUE_MAX) {
      uq->queue_length--; /* full: the last track makes room */
   }
   memmove(&uq->queue[index + 1], &uq->queue[index],
           (size_t)(uq->queue_length - index) * sizeof(uq->queue[0]));
   uq->queue_length++;

   music_queue_entry_t *entry = &uq->queue[index];
   safe_strncpy(entry->path, track->path, sizeof(entry->path));
   safe_strncpy(entry->title, track->title, sizeof(entry->title));
   safe_strncpy(entry->artist, track->artist, sizeof(entry->artist));
   safe_strncpy(entry->album, track->album, sizeof(entry->album));
   entry->duration_sec = track->duration_sec;
   return SUCCESS;
}

void music_queue_remove_at(user_music_queue_t *uq, int index) {
   if (index < 0 || index >= uq->queue_length) {
      return;
   }
   memmove(&uq->queue[index], &uq->queue[index + 1],
           (size_t)(uq->queue_length - index - 1) * sizeof(uq->queue[0]));
   uq->queue_length--;
   for (music_queue_pos_t *p = uq->positions; p; p = p->next) {
      if (p->index > index) {
         p->index--;
      }
   }
}

void music_queue_clear(user_music_queue_t *uq) {
   uq->queue_length = 0;
   uq->replace_epoch++;
   for (music_queue_pos_t *p = uq->positions; p; p = p->next) {
      p->index = 0;
   }
}

void music_queue_changed(user_music_queue_t *uq, bool tracks) {
   uq->generation++;
   if (tracks) {
      music_queue_db_save(uq->user_id, uq);
   } else {
      music_queue_db_save_state(uq->user_id, uq);
   }
}

void music_queue_add(user_music_queue_t *uq,
                     const music_search_result_t *tracks,
                     int n,
                     bool replace,
                     music_queue_outcome_t *outcomes,
                     int *added_out) {
   int added = 0;
   pthread_mutex_lock(&uq->queue_mutex);
   if (replace) {
      music_queue_clear(uq);
   }
   for (int i = 0; i < n; i++) {
      music_queue_outcome_t oc;
      if (tracks[i].path[0] == '\0') {
         oc = MUSIC_QUEUE_NOT_FOUND;
      } else if (music_queue_find(uq, tracks[i].path, -1) >= 0) {
         oc = MUSIC_QUEUE_DUPLICATE; /* also a repeat within tracks */
      } else if (music_queue_insert(uq, uq->queue_length, &tracks[i]) != SUCCESS) {
         oc = MUSIC_QUEUE_FULL;
      } else {
         oc = MUSIC_QUEUE_ADDED;
         added++;
      }
      if (outcomes) {
         outcomes[i] = oc;
      }
   }
   music_queue_changed(uq, true);
   pthread_mutex_unlock(&uq->queue_mutex);
   if (added_out) {
      *added_out = added;
   }
}

void music_queue_set_listener(music_queue_listener_t fn) {
   atomic_store(&s_listener, fn);
}

void music_queue_announce(user_music_queue_t *uq, const void *origin) {
   music_queue_listener_t fn = atomic_load(&s_listener);
   if (fn && uq) {
      fn(uq, origin);
   }
}
