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
 * Phone numbers in one form: see phone_number.h.
 */

#include "tools/phone_number.h"

#include <ctype.h>
#include <stdio.h>

void phone_number_normalize(const char *in, char *out, size_t out_size) {
   if (!out || out_size == 0)
      return;
   out[0] = '\0';
   if (!in)
      return;

   char digits[32];
   size_t di = 0;
   int leading_plus = 0;

   /* Skip leading whitespace */
   while (*in == ' ' || *in == '\t')
      in++;
   if (*in == '+') {
      leading_plus = 1;
      in++;
   }

   /* Copy digits, ignore spaces/dashes/parens/dots */
   while (*in && di < sizeof(digits) - 1) {
      if (isdigit((unsigned char)*in))
         digits[di++] = *in;
      in++;
   }
   digits[di] = '\0';

   /* Bare 10-digit US number → prefix +1. Bare 11-digit starting with 1 → +. */
   if (!leading_plus) {
      if (di == 10) {
         snprintf(out, out_size, "+1%s", digits);
         return;
      }
      if (di == 11 && digits[0] == '1') {
         snprintf(out, out_size, "+%s", digits);
         return;
      }
      /* Otherwise pass through as-is (no + prefix) for short codes etc. */
      snprintf(out, out_size, "%s", digits);
      return;
   }

   snprintf(out, out_size, "+%s", digits);
}
