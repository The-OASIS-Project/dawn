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
#include <strings.h>
#include <time.h>

#include "core/prompt_parts.h"
#include "core/strbuf.h"
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

char *prompt_turn_head(const struct tm *tm_info, const char *zone) {
   char human[64];
   char iso_local[32];
   char iso_offset[8];
   char line[512];
   if (tm_info == NULL || zone == NULL ||
       strftime(human, sizeof(human), "%A, %Y-%m-%d %H:%M", tm_info) == 0 ||
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
            PROMPT_TIME_LINE
            " Current time: %s %s (ISO: %s%s).  This timestamp is fresh as of this "
            "turn; use it for relative-time computations and tool args like "
            "`fire_at`.  The `time` tool is only needed for sub-second "
            "precision.\n",
            human, zone, iso_local, iso_offset_colon);
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

/* The identity block of a user who gave an address but no real name. */
#define IDENTITY_ADDRESS_ONLY_FMT "\n\n## User Identity\nThe user prefers to be addressed as %s.\n"

/* The most aliases the identity block names. */
#define IDENTITY_ALIASES_MAX 16

static bool is_line_blank(char c) {
   return c == ' ' || c == '\t' || c == '\r';
}

/* " They may also be referred to as: A, B." onto @p sb from @p aliases (one
 * per line): each trimmed, empties dropped, repeats (case aside) named once.
 * Nothing when none is left. */
static void append_aliases(strbuf_t *sb, const char *aliases) {
   char *buf = strdup(aliases);
   if (!buf) {
      return;
   }
   const char *seen[IDENTITY_ALIASES_MAX];
   int seen_count = 0;
   char *save = NULL;
   for (char *line = strtok_r(buf, "\n", &save); line != NULL && seen_count < IDENTITY_ALIASES_MAX;
        line = strtok_r(NULL, "\n", &save)) {
      while (is_line_blank(*line))
         line++;
      char *end = line + strlen(line);
      while (end > line && is_line_blank(end[-1]))
         end--;
      *end = '\0';
      bool dup = (*line == '\0');
      for (int i = 0; i < seen_count && !dup; i++) {
         dup = strcasecmp(seen[i], line) == 0;
      }
      if (dup)
         continue;
      strbuf_append(sb, seen_count == 0 ? " They may also be referred to as: " : ", ");
      strbuf_append(sb, line);
      seen[seen_count++] = line;
   }
   if (seen_count > 0) {
      strbuf_append(sb, ".");
   }
   free(buf);
}

char *prompt_identity_text(const char *real_name, const char *address, const char *aliases) {
   real_name = real_name ? real_name : "";
   address = address ? address : "";
   aliases = aliases ? aliases : "";
   strbuf_t sb;
   strbuf_init(&sb, 0);
   if (real_name[0] == '\0') {
      if (address[0] == '\0') {
         return NULL;
      }
      strbuf_appendf(&sb, IDENTITY_ADDRESS_ONLY_FMT, address);
      return strbuf_steal_or_null(&sb);
   }
   strbuf_appendf(&sb, "\n\n## User Identity\nYou are speaking with %s.", real_name);
   if (address[0] != '\0') {
      strbuf_appendf(&sb, " They prefer to be addressed as %s.", address);
   }
   append_aliases(&sb, aliases);
   strbuf_append(&sb, " Use this information to recognize when memory facts or extracted entities "
                      "refer to them.\n");
   return strbuf_steal_or_null(&sb);
}
