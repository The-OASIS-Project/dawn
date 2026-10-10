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
 * Someone else's text framed as such (llm_third_party.h).
 */

#include "llm/llm_third_party.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/prompt_parts.h"
#include "prompts.h"
#include "tools/tool_registry.h"

/* Each frame (TOOL_FRAMES_ALL) and what it holds, for its first line. */
static const struct {
   const char *name;
   const char *what;
} k_frames[] = {
   { TOOL_FRAME_EMAIL, THIRD_PARTY_WHAT_EMAIL },
   { TOOL_FRAME_WEB, THIRD_PARTY_WHAT_WEB },
};

char *llm_third_party_frame(const char *name, const char *tag, const char *body) {
   const char *what = NULL;
   for (size_t i = 0; name && i < sizeof(k_frames) / sizeof(k_frames[0]); i++) {
      if (strcmp(name, k_frames[i].name) == 0) {
         what = k_frames[i].what;
      }
   }
   if (!what || !body) {
      return NULL;
   }
   const int n = snprintf(NULL, 0, THIRD_PARTY_FRAME_LEAD_TEMPLATE, what);
   char *lead = n > 0 ? malloc((size_t)n + 1) : NULL;
   if (!lead) {
      return NULL;
   }
   snprintf(lead, (size_t)n + 1, THIRD_PARTY_FRAME_LEAD_TEMPLATE, what);
   const char *pieces[PROMPT_FRAMED_PIECES] = { lead, body, NULL, NULL, NULL };
   char *framed = prompt_framed(name, tag, pieces);
   free(lead);
   return framed;
}

/* Whether @p p, just past "--- NAME", ends the opening line prompt_framed
 * writes: " ---" (no tag) or " (dawn-ctx-<8 hex>) ---", then the line's end.
 * Prose that only starts like a frame ("--- WEB CONTENT ACCESSIBILITY") isn't. */
static bool frame_open_rest(const char *p) {
   static const char k_tag[] = " (dawn-ctx-";
   if (strncmp(p, k_tag, sizeof(k_tag) - 1) == 0) {
      p += sizeof(k_tag) - 1;
      for (int i = 0; i < 8; i++, p++) {
         if (!isxdigit((unsigned char)*p)) {
            return false;
         }
      }
      if (*p++ != ')') {
         return false;
      }
   }
   if (strncmp(p, " ---", 4) != 0) {
      return false;
   }
   p += 4;
   return *p == '\0' || *p == '\n' || (*p == '\r' && (p[1] == '\n' || p[1] == '\0'));
}

const char *llm_third_party_present(const char *text) {
   if (!text) {
      return NULL;
   }
   for (size_t i = 0; i < sizeof(k_frames) / sizeof(k_frames[0]); i++) {
      char open[48];
      const int n = snprintf(open, sizeof(open), "--- %s", k_frames[i].name);
      if (n <= 0 || (size_t)n >= sizeof(open)) {
         continue;
      }
      for (const char *p = strstr(text, open); p; p = strstr(p + 1, open)) {
         /* A whole opening line: "--- EMAIL CONTENT (tag) ---" or "... ---". */
         if ((p == text || p[-1] == '\n') && frame_open_rest(p + n)) {
            return k_frames[i].name;
         }
      }
   }
   return NULL;
}
