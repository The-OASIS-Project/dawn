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
 * Request origins for the same-origin (CSRF) check (webui_origin.h).
 */

#include "webui/webui_origin.h"

#include <string.h>

bool webui_referer_origin(const char *referer, char *out, size_t out_size) {
   if (!referer || !out || out_size == 0) {
      return false;
   }
   const char *sep = strstr(referer, "://");
   if (!sep || sep == referer) {
      return false;
   }
   const char *authority = sep + 3;
   const size_t authority_len = strcspn(authority, "/?#");
   if (authority_len == 0 || memchr(authority, '@', authority_len) != NULL) {
      return false;
   }
   const size_t len = (size_t)(authority - referer) + authority_len;
   if (len >= out_size) {
      return false;
   }
   memcpy(out, referer, len);
   out[len] = '\0';
   return true;
}
