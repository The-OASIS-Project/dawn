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

/* @p text's length without its trailing whitespace. */
static size_t trimmed_len(const char *text) {
   size_t len = strlen(text);
   while (len > 0 && is_blank(text[len - 1])) {
      len--;
   }
   return len;
}

const char *prompt_persona_base(persona_half_t half,
                                const char *description,
                                const char *configured,
                                const char *builtin) {
   if (description && trimmed_len(description) > 0) {
      return half == PERSONA_VALUES ? description : "";
   }
   if (configured && trimmed_len(configured) > 0) {
      return configured;
   }
   return builtin ? builtin : "";
}

/* Two texts a blank line apart, @p first's trailing whitespace trimmed at the
 * seam (@p second's length given).  An empty @p second leaves @p first as it
 * is; a blank @p first gives @p second alone. */
static char *join_blank_line(const char *first, const char *second, size_t second_len) {
   strbuf_t sb;
   strbuf_init(&sb, 0);
   const size_t first_len = trimmed_len(first);
   if (second_len == 0) {
      strbuf_append(&sb, first);
   } else if (first_len == 0) {
      strbuf_append_n(&sb, second, second_len);
   } else {
      strbuf_append_n(&sb, first, first_len);
      strbuf_append(&sb, "\n\n");
      strbuf_append_n(&sb, second, second_len);
   }
   return strbuf_steal(&sb);
}

char *prompt_persona_half(const char *base, const char *mine, const char *mode) {
   base = base ? base : "";
   mine = mine ? mine : "";
   while (is_blank(*mine)) {
      mine++;
   }
   const size_t mine_len = trimmed_len(mine);
   if (mine_len > 0 && mode && strcmp(mode, "replace") == 0) {
      return strndup(mine, mine_len);
   }
   return join_blank_line(base, mine, mine_len);
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

/* The most entries the identity block names from one list (aliases, or the
 * ways to address the user). */
#define IDENTITY_LIST_MAX 16

/* How the model may address the user (after "When you address them
 * directly, "): a single form, or a choice.  The list follows the colon. */
#define IDENTITY_ADDRESS_ONE "use: "
#define IDENTITY_ADDRESS_ANY "use any of: "
#define IDENTITY_ADDRESS_END "; most replies don't need one."

/* Split @p buf (modified in place) on @p sep into @p out: each entry trimmed,
 * empties dropped, repeats (case aside) kept once, at most IDENTITY_LIST_MAX.
 * Returns the count. */
static int split_list(char *buf, const char *sep, const char *out[IDENTITY_LIST_MAX]) {
   int n = 0;
   char *save = NULL;
   for (char *item = strtok_r(buf, sep, &save); item != NULL && n < IDENTITY_LIST_MAX;
        item = strtok_r(NULL, sep, &save)) {
      while (is_blank(*item)) {
         item++;
      }
      item[trimmed_len(item)] = '\0';
      bool dup = (*item == '\0');
      for (int i = 0; i < n && !dup; i++) {
         dup = strcasecmp(out[i], item) == 0;
      }
      if (!dup) {
         out[n++] = item;
      }
   }
   return n;
}

/* @p items onto @p sb, comma-separated. */
static void append_joined(strbuf_t *sb, const char *const items[], int n) {
   for (int i = 0; i < n; i++) {
      if (i > 0) {
         strbuf_append(sb, ", ");
      }
      strbuf_append(sb, items[i]);
   }
}

/* "use: A; most replies…" or "use any of: A, B; most replies…" onto @p sb. */
static void append_address(strbuf_t *sb, const char *const forms[], int n) {
   strbuf_append(sb, n == 1 ? IDENTITY_ADDRESS_ONE : IDENTITY_ADDRESS_ANY);
   append_joined(sb, forms, n);
   strbuf_append(sb, IDENTITY_ADDRESS_END);
}

char *prompt_identity_text(const char *real_name, const char *address, const char *aliases) {
   real_name = real_name ? real_name : "";
   char *address_buf = strdup(address ? address : "");
   char *aliases_buf = strdup(aliases ? aliases : "");
   if (!address_buf || !aliases_buf) {
      free(address_buf);
      free(aliases_buf);
      return NULL;
   }
   const char *forms[IDENTITY_LIST_MAX];
   const int n_forms = split_list(address_buf, ",", forms);
   strbuf_t sb;
   strbuf_init(&sb, 0);
   if (real_name[0] == '\0') {
      /* Aliases name the user for memory matching, which needs the name. */
      if (n_forms > 0) {
         strbuf_append(&sb, "\n\n## User Identity\nWhen you address the user directly, ");
         append_address(&sb, forms, n_forms);
         strbuf_append(&sb, "\n");
      }
   } else {
      strbuf_appendf(&sb, "\n\n## User Identity\nYou are speaking with %s.", real_name);
      if (n_forms > 0) {
         strbuf_append(&sb, " When you address them directly, ");
         append_address(&sb, forms, n_forms);
      }
      const char *names[IDENTITY_LIST_MAX];
      const int n_names = split_list(aliases_buf, "\n", names);
      if (n_names > 0) {
         strbuf_append(&sb, " They may also be referred to as: ");
         append_joined(&sb, names, n_names);
         strbuf_append(&sb, ".");
      }
      strbuf_append(&sb, " Use this information to recognize when memory facts or extracted "
                         "entities refer to them.\n");
   }
   free(address_buf);
   free(aliases_buf);
   return strbuf_steal_or_null(&sb);
}
