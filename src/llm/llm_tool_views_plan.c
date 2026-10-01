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
 * The view stage's planning (llm_tool_views.h): the batch budget, its split,
 * and the header.  Pure.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "llm/llm_tool_views.h"

size_t llm_tool_views_budget_chars(int window, int room_tokens, bool room_known) {
   double chars = LLM_TOOL_VIEWS_REF_CHARS;
   if (window > 0 && window < LLM_TOOL_VIEWS_REF_WINDOW) {
      const double k = log((double)LLM_TOOL_VIEWS_REF_CHARS / LLM_TOOL_VIEWS_SMALL_CHARS) /
                       log((double)LLM_TOOL_VIEWS_REF_WINDOW / LLM_TOOL_VIEWS_SMALL_WINDOW);
      chars = LLM_TOOL_VIEWS_REF_CHARS * pow((double)window / LLM_TOOL_VIEWS_REF_WINDOW, k);
   }
   if (room_known) {
      const double room = room_tokens > 0
                              ? (double)room_tokens * LLM_TOOL_VIEWS_WORST_CHARS_PER_TOKEN
                              : 0.0;
      chars = room < chars ? room : chars;
   }
   /* Even a full context's view keeps its header and a little of the result,
    * so the data stays reachable; what follows is the seam's job. */
   return chars > LLM_TOOL_VIEWS_FLOOR_CHARS ? (size_t)chars : LLM_TOOL_VIEWS_FLOOR_CHARS;
}

void llm_tool_views_split(const size_t *demands,
                          const bool *first,
                          int n,
                          size_t budget,
                          size_t *shares_out) {
   if (!demands || !shares_out || n <= 0) {
      return;
   }
   if (n > LLM_TOOL_VIEWS_BATCH_MAX) {
      n = LLM_TOOL_VIEWS_BATCH_MAX;
   }
   bool settled[LLM_TOOL_VIEWS_BATCH_MAX] = { false };
   size_t left = budget;
   int open = 0;
   for (int i = 0; i < n; i++) {
      if (demands[i] == SIZE_MAX) {
         shares_out[i] = SIZE_MAX; /* left alone: takes no share */
         settled[i] = true;
      } else {
         open++;
      }
   }
   /* Served first, smallest first: each whole while it fits in what's left
    * (one that doesn't joins the rest). */
   for (bool served = first != NULL; served;) {
      served = false;
      int pick = -1;
      for (int i = 0; i < n; i++) {
         if (!settled[i] && first[i] && demands[i] <= left &&
             (pick < 0 || demands[i] < demands[pick])) {
            pick = i;
         }
      }
      if (pick >= 0) {
         shares_out[pick] = demands[pick];
         left -= demands[pick];
         settled[pick] = true;
         open--;
         served = true;
      }
   }
   /* Water-filling: whatever fits under the fair share of what's left passes
    * whole, which raises the fair share of the rest; repeat until none does. */
   for (bool moved = true; moved && open > 0;) {
      moved = false;
      const size_t fair = left / (size_t)open;
      for (int i = 0; i < n; i++) {
         if (!settled[i] && demands[i] <= fair) {
            shares_out[i] = demands[i];
            left -= demands[i];
            settled[i] = true;
            open--;
            moved = true;
         }
      }
   }
   if (open == 0) {
      return;
   }
   /* The rest share what's left in proportion to their size. */
   long double total = 0;
   for (int i = 0; i < n; i++) {
      if (!settled[i]) {
         total += (long double)demands[i];
      }
   }
   for (int i = 0; i < n; i++) {
      if (!settled[i]) {
         shares_out[i] = (size_t)((long double)left * (long double)demands[i] / total);
      }
   }
}

size_t llm_tool_views_read_chars(size_t batch_chars, int calls) {
   const size_t fair = batch_chars / (size_t)(calls > 1 ? calls : 1);
   return fair > LLM_TOOL_VIEWS_HEADER_MAX + LLM_TOOL_VIEWS_READ_MIN_CHARS
              ? fair - LLM_TOOL_VIEWS_HEADER_MAX
              : LLM_TOOL_VIEWS_READ_MIN_CHARS;
}

/* @p v with thousands separators ("64,814"). */
static void grouped(size_t v, char *out, size_t size) {
   char digits[32];
   const int len = snprintf(digits, sizeof(digits), "%zu", v);
   size_t o = 0;
   for (int i = 0; i < len && o + 1 < size; i++) {
      if (i > 0 && (len - i) % 3 == 0 && o + 2 < size) {
         out[o++] = ',';
      }
      out[o++] = digits[i];
   }
   out[o < size ? o : size - 1] = '\0';
}

static size_t header_full(const llm_tool_views_header_t *h, char *out, size_t size) {
   char chars[32];
   grouped(h->chars, chars, sizeof(chars));
   const bool tagged = h->tag && h->tag[0];
   size_t o = 0;
   int n = snprintf(out, size, "[Tool result shortened%s%s%s: %s chars, shown as a view.",
                    tagged ? " (" : "", tagged ? h->tag : "", tagged ? ")" : "", chars);
   if (n < 0 || (size_t)n >= size) {
      goto overflow;
   }
   o = (size_t)n;
   if (h->handle && h->offers_read) {
      n = snprintf(out + o, size - o,
                   h->json ? "\n Full result: [tool-result %s]. result_read: read (lines), "
                             "path ($.a[10:20]), grep (text), count / distinct ($.a, field)."
                           : "\n Full result: [tool-result %s]. result_read: read (lines), "
                             "grep (text).",
                   h->handle);
      if (n >= 0 && (size_t)n < size - o && h->head_tail_only) {
         o += (size_t)n;
         n = snprintf(out + o, size - o,
                      " It was too big to keep whole: its first and last parts are kept.");
      }
   } else if (h->is_read) {
      n = snprintf(out + o, size - o,
                   "\n Read a smaller part: a line range, a deeper path, or "
                   "a narrower pattern.");
   } else {
      n = snprintf(out + o, size - o, "\n Call the tool again with narrower arguments for more.");
   }
   if (n < 0 || (size_t)n >= size - o) {
      goto overflow;
   }
   o += (size_t)n;
   if (h->narrow && h->narrow[0] && !h->is_read) {
      n = snprintf(out + o, size - o,
                   "\n The tool takes \"%s\": a narrower call may be all you need.", h->narrow);
      if (n < 0 || (size_t)n >= size - o) {
         goto overflow;
      }
      o += (size_t)n;
   }
   if (o + 3 > size) {
      goto overflow;
   }
   out[o++] = ']';
   out[o++] = '\n';
   out[o] = '\0';
   return o;
overflow:
   out[0] = '\0';
   return 0;
}

/* The header at its shortest (the full one didn't fit): still says the result
 * was shortened, and names the handle when it may be read. */
static size_t header_minimal(const llm_tool_views_header_t *h, char *out, size_t size) {
   char chars[32];
   grouped(h->chars, chars, sizeof(chars));
   const bool tagged = h->tag && h->tag[0];
   const bool handle = h->handle && h->offers_read;
   const int n = snprintf(out, size, "[Tool result shortened%s%s%s: %s chars.%s%s%s]\n",
                          tagged ? " (" : "", tagged ? h->tag : "", tagged ? ")" : "", chars,
                          handle ? " Full result: [tool-result " : "", handle ? h->handle : "",
                          handle ? "]." : "");
   if (n < 0 || (size_t)n >= size) {
      out[0] = '\0';
      return 0;
   }
   return (size_t)n;
}

size_t llm_tool_views_header(const llm_tool_views_header_t *h, char *out, size_t size) {
   if (!h || !out || size == 0) {
      return 0;
   }
   const size_t n = header_full(h, out, size);
   return n > 0 ? n : header_minimal(h, out, size);
}

bool llm_tool_views_narrows(const char *name) {
   static const char *const k_narrowing[] = { "limit",  "max_results", "count",  "page",
                                              "offset", "query",       "filter", "fields" };
   if (!name) {
      return false;
   }
   for (size_t i = 0; i < sizeof(k_narrowing) / sizeof(k_narrowing[0]); i++) {
      if (strcmp(name, k_narrowing[i]) == 0) {
         return true;
      }
   }
   return false;
}
