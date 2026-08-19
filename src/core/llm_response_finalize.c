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

#include "dawn_error.h"

/* Remove every `<command>…</command>` pair in place, then truncate at the first
 * `<end_of_turn>` marker (emitted by some local models).  Mirrors the historical
 * strip_command_tags() so finalized output matches the per-seam strips it
 * replaces; kept here as the one canonical definition. */
static void strip_residual_tags(char *text) {
   static const char *const open = "<command>";
   static const char *const close = "</command>";
   const size_t close_len = strlen(close);

   char *cmd_start;
   while ((cmd_start = strstr(text, open)) != NULL) {
      char *cmd_end = strstr(cmd_start, close);
      if (cmd_end == NULL) {
         break; /* unclosed tag — leave the remainder untouched */
      }
      cmd_end += close_len;
      memmove(cmd_start, cmd_end, strlen(cmd_end) + 1);
   }

   char *eot = strstr(text, "<end_of_turn>");
   if (eot != NULL) {
      *eot = '\0';
   }
}

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
   (void)session; /* reserved for Phase-1 citation resolution */

   if (out == NULL) {
      return FAILURE;
   }
   out->text = NULL;
   out->length = 0;

   char *clean = strdup(raw_response != NULL ? raw_response : "");
   if (clean == NULL) {
      return FAILURE;
   }

   strip_residual_tags(clean);
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
