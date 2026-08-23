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
 * Unified LLM response finalizer — see include/core/llm_response_finalize.h.
 */

#include "core/llm_response_finalize.h"

#include <stdlib.h>
#include <string.h>

#include "core/text_filter.h"
#include "dawn_error.h"
#include "memory/memory_citation.h"

/* Trim trailing ASCII whitespace in place (Claude rejects assistant turns that
 * end in whitespace; harmless-to-beneficial for every other surface). */
static void rtrim(char *text) {
   size_t len = strlen(text);
   while (len > 0) {
      char c = text[len - 1];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
         break;
      }
      text[--len] = '\0';
   }
}

int llm_response_finalize(session_t *session, const char *raw_response, response_final_t *out) {
   if (out == NULL) {
      return FAILURE;
   }
   out->text = NULL;
   out->length = 0;

   char *clean = strdup(raw_response != NULL ? raw_response : "");
   if (clean == NULL) {
      return FAILURE;
   }

   /* Memory citation capture (self-gating no-op unless enabled + session has a
    * stash).  Runs BEFORE stripping so an <end_of_turn> truncation cannot hide a
    * trailing <cited> tag from the parser. */
   memory_citation_capture(session, clean);

   text_filter_command_strip(clean, false); /* complete response: leave an orphan <command> */
   text_filter_cited_strip(clean);          /* <cited>…</cited> (always removed) */
   rtrim(clean);

   out->text = clean;
   out->length = strlen(clean);
   return SUCCESS;
}

void response_final_free(response_final_t *out) {
   if (out == NULL) {
      return;
   }
   free(out->text);
   out->text = NULL;
   out->length = 0;
}
