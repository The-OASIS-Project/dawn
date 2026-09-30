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
 * One login cookie per app (webui_login_cookie.h).
 */

#include "webui/webui_login_cookie.h"

#include <stdio.h>
#include <string.h>

static bool app_name_valid(const char *app) {
   const size_t len = strlen(app);
   if (len == 0 || len > WEBUI_APP_NAME_MAX) {
      return false;
   }
   for (size_t i = 0; i < len; i++) {
      const char c = app[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
         return false;
      }
   }
   return true;
}

bool webui_login_cookie_name(const char *app, char *out, size_t out_size) {
   if (!out || out_size == 0) {
      return false;
   }
   out[0] = '\0';
   int n;
   if (!app || app[0] == '\0' || strcmp(app, WEBUI_APP_DEFAULT) == 0) {
      n = snprintf(out, out_size, "%s", WEBUI_LOGIN_COOKIE_BASE);
   } else if (app_name_valid(app)) {
      n = snprintf(out, out_size, "%s_%s", WEBUI_LOGIN_COOKIE_BASE, app);
   } else {
      return false;
   }
   if (n < 0 || (size_t)n >= out_size) {
      out[0] = '\0';
      return false;
   }
   return true;
}

bool webui_cookie_value(const char *header, const char *name, char *out, size_t out_size) {
   if (!header || !name || !name[0] || !out || out_size == 0) {
      return false;
   }
   out[0] = '\0';
   const size_t name_len = strlen(name);
   const char *p = header;
   while (*p) {
      while (*p == ' ' || *p == ';') {
         p++;
      }
      const char *end = strchr(p, ';');
      const size_t pair_len = end ? (size_t)(end - p) : strlen(p);
      if (pair_len > name_len && strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
         const char *value = p + name_len + 1;
         size_t value_len = pair_len - name_len - 1;
         while (value_len > 0 && value[value_len - 1] == ' ') {
            value_len--;
         }
         if (value_len == 0 || value_len >= out_size) {
            return false;
         }
         memcpy(out, value, value_len);
         out[value_len] = '\0';
         return true;
      }
      if (!end) {
         break;
      }
      p = end + 1;
   }
   return false;
}
