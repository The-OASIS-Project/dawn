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
 * libFuzzer harness for the TTS text preprocessing (tts_preprocessing.cpp):
 * any bytes as a reply about to be spoken, which repeats whatever the model
 * read (web pages, mail, messages).  The first byte picks the output buffer:
 * roomy, as callers size it, or small, to exercise the cut.  Built by
 * tests/fuzz/run_fuzzers.sh.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tts/tts_preprocessing.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
   if (size < 1)
      return 0;
   const size_t out_size = (data[0] & 1) ? 16 : (size - 1) * 4 + 64;
   data++;
   size--;

   char *in = malloc(size + 1);
   char *out = malloc(out_size);
   if (!in || !out) {
      free(in);
      free(out);
      return 0;
   }
   memcpy(in, data, size);
   in[size] = '\0';

   int written = -1;
   if (preprocess_text_for_tts_c(in, out, out_size, &written) == 0) {
      /* The output is NUL-terminated where it says it ends. */
      if (written < 0 || (size_t)written >= out_size || strlen(out) != (size_t)written)
         abort();
   }
   free(in);
   free(out);
   return 0;
}
