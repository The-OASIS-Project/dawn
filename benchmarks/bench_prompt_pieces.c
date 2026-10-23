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
 * Splits a DAWN_EXTRACTION_PROMPT_FILE template into literal pieces.
 */

#include "bench_prompt_pieces.h"

int bench_prompt_split(char *text, const char *pieces[MEMORY_EXTRACTION_TEMPLATE_PIECES]) {
   int n = 0;
   char *out = text; /* never passes the read position: each %x pair writes one byte */
   pieces[n++] = out;
   for (const char *in = text; *in; in++) {
      if (*in != '%') {
         *out++ = *in;
         continue;
      }
      in++;
      if (*in == '%') {
         *out++ = '%';
      } else if (*in == 's' && n < MEMORY_EXTRACTION_TEMPLATE_PIECES) {
         *out++ = '\0';
         pieces[n++] = out;
      } else {
         return 1; /* another conversion, a fifth %s, or a lone % at the end */
      }
   }
   *out = '\0';
   return n == MEMORY_EXTRACTION_TEMPLATE_PIECES ? 0 : 1;
}
