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
 * Row-id lists for batch lookups.  See auth_db_internal.h.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "auth/auth_db_internal.h"

char *auth_db_internal_ids_json(const int64_t *ids, int n) {
   if (!ids || n <= 0) {
      return NULL;
   }
   /* "[", up to 20 digits and a comma per id, "]", NUL. */
   const size_t size = (size_t)n * 21 + 3;
   char *list = malloc(size);
   if (!list) {
      return NULL;
   }
   size_t off = 0;
   list[off++] = '[';
   for (int i = 0; i < n; i++) {
      const int w = snprintf(list + off, size - off, "%s%lld", i ? "," : "", (long long)ids[i]);
      if (w < 0 || (size_t)w >= size - off - 1) {
         free(list);
         return NULL;
      }
      off += (size_t)w;
   }
   list[off++] = ']';
   list[off] = '\0';
   return list;
}
