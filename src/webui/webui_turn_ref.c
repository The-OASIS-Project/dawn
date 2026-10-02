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
 * The client_ref of the text turn a thread is handling (webui_turn_ref.h):
 * thread-local, so the lws thread and each turn's worker hold their own.
 */

#include "webui/webui_turn_ref.h"

#include <stdio.h>

/* The text turn this thread is handling (see webui_turn_ref_set). */
static __thread char t_turn_ref[WEBUI_CLIENT_REF_MAX + 1];

void webui_turn_ref_set(const char *ref) {
   snprintf(t_turn_ref, sizeof(t_turn_ref), "%s", ref ? ref : "");
}

const char *webui_turn_ref_get(void) {
   return t_turn_ref[0] ? t_turn_ref : NULL;
}

bool webui_client_ref_valid(const char *ref) {
   if (!ref || !ref[0]) {
      return false;
   }
   size_t n = 0;
   for (; ref[n]; n++) {
      const unsigned char c = (unsigned char)ref[n];
      if (n >= WEBUI_CLIENT_REF_MAX || c < 0x20 || c > 0x7e) {
         return false;
      }
   }
   return true;
}
