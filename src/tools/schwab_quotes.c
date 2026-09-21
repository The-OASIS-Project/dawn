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
 * Charles Schwab /quotes response parser — see schwab_quotes.h.
 */

#include "tools/schwab_quotes.h"

#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

#include "dawn_error.h"

/* Read a string member, "" if absent. */
static const char *jqs(struct json_object *o, const char *k) {
   struct json_object *v = NULL;
   return (o && json_object_object_get_ex(o, k, &v)) ? json_object_get_string(v) : "";
}

int schwab_quotes_parse(struct json_object *root, schwab_quote_t *out, int max, int *n_out) {
   if (n_out) {
      *n_out = 0;
   }
   if (!out || max <= 0 || !root || !json_object_is_type(root, json_type_object)) {
      return FAILURE;
   }

   int n = 0;
   json_object_object_foreach(root, key, val) {
      if (n >= max) {
         break;
      }
      /* The response mixes symbol entries with an "errors" object (invalidSymbols
       * etc.); skip that and any non-object member. */
      if (strcmp(key, "errors") == 0 || !json_object_is_type(val, json_type_object)) {
         continue;
      }
      schwab_quote_t *q = &out[n];
      memset(q, 0, sizeof(*q));
      snprintf(q->symbol, sizeof(q->symbol), "%s", key);
      struct json_object *ref = NULL;
      if (json_object_object_get_ex(val, "reference", &ref)) {
         snprintf(q->description, sizeof(q->description), "%s", jqs(ref, "description"));
      }
      n++;
   }

   if (n_out) {
      *n_out = n;
   }
   return SUCCESS;
}
