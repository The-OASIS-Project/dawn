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
 * Music queue: adding library tracks by path (each checked and looked up in
 * the music library first).  Apart from music_queue.c so the queue itself
 * doesn't depend on the library.
 */

#include <stdlib.h>

#include "audio/music_db.h"
#include "audio/music_open.h"
#include "audio/music_queue.h"
#include "dawn_error.h"

int music_queue_apply_paths(user_music_queue_t *uq,
                            const char *const *paths,
                            int n,
                            bool replace,
                            music_queue_outcome_t *outcomes) {
   if (!uq || !paths || n < 0) {
      return FAILURE;
   }

   /* Resolve before the queue lock (DB lookups are slow, and the stream
    * thread takes queue_mutex); a path left empty is no track */
   music_search_result_t *resolved = NULL;
   if (n > 0) {
      resolved = calloc((size_t)n, sizeof(*resolved));
      if (!resolved) {
         return FAILURE;
      }
   }
   for (int i = 0; i < n; i++) {
      bool found = false;
      if (!paths[i] || !music_path_is_valid(paths[i]) ||
          music_db_get_by_path(paths[i], &resolved[i], &found) != SUCCESS || !found) {
         resolved[i].path[0] = '\0';
      }
   }
   music_queue_add(uq, resolved, n, replace, outcomes, NULL);
   free(resolved);
   return SUCCESS;
}
