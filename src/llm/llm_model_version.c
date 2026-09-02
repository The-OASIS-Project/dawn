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
 * Provider-neutral model-id version parsing, shared by the OpenAI and Claude
 * capability gates.
 */

#include "llm/llm_model_version.h"

#include <stdlib.h>

void llm_parse_model_version(const char *model_name, int *major_out, int *minor_out) {
   int major = 0, minor = 0;
   const char *p = model_name ? model_name : "";
   while (*p) {
      if ((*p >= '0' && *p <= '9') && (p == model_name || *(p - 1) == '-' || *(p - 1) == '.')) {
         major = atoi(p);
         while (*p >= '0' && *p <= '9') {
            p++;
         }
         if ((*p == '-' || *p == '.') && (*(p + 1) >= '0' && *(p + 1) <= '9')) {
            minor = atoi(p + 1);
         }
         break;
      }
      p++;
   }
   if (major_out) {
      *major_out = major;
   }
   if (minor_out) {
      *minor_out = minor;
   }
}
