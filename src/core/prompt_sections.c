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
 * The system prompt's named sections (prompt_parts.h).
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "core/prompt_parts.h"
#include "dawn_error.h"

static bool is_blank(char c) {
   return c == '\n' || c == '\r' || c == ' ' || c == '\t';
}

int prompt_sections_add(composed_prompt_t *cp,
                        const char *name,
                        const char *title,
                        const char *text) {
   if (!cp || !name || !title || strlen(name) >= PROMPT_SECTION_NAME_MAX ||
       strlen(title) >= PROMPT_SECTION_TITLE_MAX) {
      return FAILURE;
   }
   if (!text) {
      return SUCCESS;
   }
   while (is_blank(*text)) {
      text++;
   }
   size_t len = strlen(text);
   while (len > 0 && is_blank(text[len - 1])) {
      len--;
   }
   if (len == 0) {
      return SUCCESS;
   }
   if (cp->n_sections >= PROMPT_SECTIONS_MAX) {
      return FAILURE;
   }
   char *copy = strndup(text, len);
   if (!copy) {
      return FAILURE;
   }
   prompt_section_t *sec = &cp->sections[cp->n_sections++];
   memcpy(sec->name, name, strlen(name) + 1);
   memcpy(sec->title, title, strlen(title) + 1);
   sec->text = copy;
   return SUCCESS;
}

char *prompt_sections_join(const composed_prompt_t *cp) {
   if (!cp || cp->n_sections <= 0) {
      return NULL;
   }
   size_t total = 1;
   for (int i = 0; i < cp->n_sections; i++) {
      total += strlen(cp->sections[i].text) + 2;
   }
   char *out = malloc(total);
   if (!out) {
      return NULL;
   }
   size_t off = 0;
   for (int i = 0; i < cp->n_sections; i++) {
      if (i > 0) {
         out[off++] = '\n';
         out[off++] = '\n';
      }
      const size_t len = strlen(cp->sections[i].text);
      memcpy(out + off, cp->sections[i].text, len);
      off += len;
   }
   out[off] = '\0';
   return out;
}
