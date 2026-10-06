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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

char *prompt_turn_head(time_t now) {
   struct tm tm_storage;
   const struct tm *tm_info = localtime_r(&now, &tm_storage);
   char human[64];
   char iso_local[32];
   char iso_offset[8];
   char line[512];
   if (tm_info == NULL || strftime(human, sizeof(human), "%A, %Y-%m-%d %H:%M %Z", tm_info) == 0 ||
       strftime(iso_local, sizeof(iso_local), "%Y-%m-%dT%H:%M:%S", tm_info) == 0 ||
       strftime(iso_offset, sizeof(iso_offset), "%z", tm_info) == 0) {
      snprintf(line, sizeof(line),
               PROMPT_TIME_LINE " Current time: unavailable this turn.  Use the `time` tool "
                                "when the time matters.\n");
      return strdup(line);
   }
   char iso_offset_colon[8] = "Z";
   if ((iso_offset[0] == '+' || iso_offset[0] == '-') && strlen(iso_offset) >= 5) {
      snprintf(iso_offset_colon, sizeof(iso_offset_colon), "%c%c%c:%c%c", iso_offset[0],
               iso_offset[1], iso_offset[2], iso_offset[3], iso_offset[4]);
   }
   snprintf(line, sizeof(line),
            PROMPT_TIME_LINE " Current time: %s (ISO: %s%s).  This timestamp is fresh as of this "
                             "turn; use it for relative-time computations and tool args like "
                             "`fire_at`.  The `time` tool is only needed for sub-second "
                             "precision.\n",
            human, iso_local, iso_offset_colon);
   return strdup(line);
}

/* The open or close line of a framed block into @p out (NULL: measure). */
static size_t frame_line(char *out,
                         size_t size,
                         const char *end,
                         const char *name,
                         const char *tag) {
   const int n = tag ? snprintf(out, size, "--- %s%s (%s) ---\n", end, name, tag)
                     : snprintf(out, size, "--- %s%s ---\n", end, name);
   return n > 0 ? (size_t)n : 0;
}

char *prompt_framed(const char *name,
                    const char *tag,
                    const char *const pieces[PROMPT_FRAMED_PIECES]) {
   if (!name) {
      return NULL;
   }
   size_t len = frame_line(NULL, 0, "", name, tag) + frame_line(NULL, 0, "END ", name, tag);
   for (int i = 0; i < PROMPT_FRAMED_PIECES; i++) {
      const size_t n = pieces[i] ? strlen(pieces[i]) : 0;
      len += n + (n > 0 && pieces[i][n - 1] != '\n');
   }
   char *out = malloc(len + 1);
   if (!out) {
      return NULL;
   }
   size_t off = frame_line(out, len + 1, "", name, tag);
   for (int i = 0; i < PROMPT_FRAMED_PIECES; i++) {
      const size_t n = pieces[i] ? strlen(pieces[i]) : 0;
      if (n > 0) {
         memcpy(out + off, pieces[i], n);
         off += n;
         if (pieces[i][n - 1] != '\n') {
            out[off++] = '\n';
         }
      }
   }
   frame_line(out + off, len + 1 - off, "END ", name, tag);
   return out;
}
