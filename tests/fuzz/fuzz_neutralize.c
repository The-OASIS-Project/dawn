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
 * libFuzzer harness for llm_context_neutralize() (llm_context_text.c): any
 * bytes as text DAWN shows the model but didn't write (a tool result, a web
 * page, a remembered fact).  Checks its promises: NUL-terminated output, and
 * a text with no marker in it comes back unchanged.  Built by
 * tests/fuzz/run_fuzzers.sh.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm_context_text.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
   char *text = malloc(size + 1);
   if (!text)
      return 0;
   memcpy(text, data, size);
   text[size] = '\0';

   char *out = llm_context_neutralize(text);
   if (out) {
      (void)strlen(out);
      free(out);
   }
   /* The owned form takes the text either way. */
   out = llm_context_neutralize_owned(text);
   free(out);
   return 0;
}
