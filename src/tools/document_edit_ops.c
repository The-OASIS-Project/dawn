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
 * Pure text-edit operations for the document_manage tool's edit/append actions
 * (B1a).  Kept in their own translation unit — no DB / embedding / registry
 * dependencies — so the find/replace + append CONTRACT can be unit-tested in
 * isolation from the storage layer.  See document_manage_tool.c for the wiring.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "tools/document_manage.h"

int docmgmt_find_replace_once(const char *src, const char *find, const char *replace, char **out) {
   *out = NULL;
   if (!src || !find || !find[0] || !replace)
      return 0;
   const char *first = strstr(src, find);
   if (!first)
      return 0;
   if (strstr(first + 1, find))
      return 2; /* two or more — ambiguous; never guess which */
   size_t find_len = strlen(find);
   size_t rep_len = strlen(replace);
   size_t prefix = (size_t)(first - src);
   char *n = malloc(strlen(src) - find_len + rep_len + 1);
   if (!n)
      return 1; /* unique match but OOM — *out stays NULL */
   memcpy(n, src, prefix);
   /* the strcpy that follows copies the tail with its NUL */
   // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
   memcpy(n + prefix, replace, rep_len);
   strcpy(n + prefix + rep_len, first + find_len);
   *out = n;
   return 1;
}

char *docmgmt_append_text(const char *src, const char *text) {
   if (!src)
      src = "";
   if (!text)
      text = "";
   size_t sl = strlen(src), tl = strlen(text);
   /* Insert a single separating newline only when NEITHER side already provides
    * one — the LLM often prepends its own '\n' to the appended text, and adding
    * ours on top of that would leave a blank line. */
   bool need_nl = (sl > 0 && src[sl - 1] != '\n') && (tl > 0 && text[0] != '\n');
   char *n = malloc(sl + (need_nl ? 1 : 0) + tl + 1);
   if (!n)
      return NULL;
   memcpy(n, src, sl);
   size_t pos = sl;
   if (need_nl)
      n[pos++] = '\n';
   memcpy(n + pos, text, tl);
   n[pos + tl] = '\0';
   return n;
}
