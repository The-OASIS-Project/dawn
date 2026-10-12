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
 * The memory tool's 'list_merged' and 'restore' actions: what was merged into
 * a fact (or replaced by a newer one), and bringing one of those back.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/strbuf.h"
#include "memory/memory_callback_internal.h"
#include "memory/memory_db.h"
#include "tools/tool_registry.h"

/* Superseded facts one 'list_merged' shows. */
#define LIST_MERGED_MAX 20

char *memory_action_list_merged(int user_id, const char *query) {
   const char *q = query ? query : "";
   while (*q == ' ') {
      q++;
   }
   /* A number names the kept fact; anything else filters by text. */
   char *end = NULL;
   const long long keeper = strtoll(q, &end, 10);
   while (end && (*end == ' ' || *end == '\t')) {
      end++;
   }
   const bool by_keeper = q[0] && end && *end == '\0' && keeper > 0;

   memory_superseded_fact_t *rows = calloc(LIST_MERGED_MAX, sizeof(*rows));
   if (!rows) {
      return strdup(TOOL_RESULT_ERROR_MARK "Memory list_merged failed: out of memory.");
   }
   int n = 0;
   if (memory_db_fact_list_superseded(user_id, by_keeper ? keeper : 0,
                                      by_keeper || !q[0] ? NULL : q, rows, LIST_MERGED_MAX,
                                      &n) != MEMORY_DB_SUCCESS) {
      free(rows);
      return strdup(TOOL_RESULT_ERROR_MARK "Memory list_merged failed.");
   }
   strbuf_t sb;
   strbuf_init(&sb, 1024);
   if (n == 0) {
      strbuf_append(&sb, by_keeper ? "Nothing was merged into that memory."
                                   : "No merged or replaced memories found.");
   } else {
      strbuf_appendf(&sb,
                     "%d merged or replaced memor%s (newest first; 'restore' with the ID "
                     "brings one back):\n",
                     n, n == 1 ? "y" : "ies");
      for (int i = 0; i < n; i++) {
         strbuf_appendf(&sb, "- [%lld] %s\n    now in [%lld] %s\n", (long long)rows[i].id,
                        rows[i].text, (long long)rows[i].keeper_id,
                        rows[i].keeper_text[0] ? rows[i].keeper_text : "(gone)");
      }
   }
   free(rows);
   char *out = strbuf_steal(&sb);
   return out ? out : strdup(TOOL_RESULT_ERROR_MARK "Memory list_merged failed: out of memory.");
}

char *memory_action_restore(int user_id, const char *query) {
   const char *q = query ? query : "";
   char *end = NULL;
   const long long id = strtoll(q, &end, 10);
   while (end && (*end == ' ' || *end == '\t')) {
      end++;
   }
   if (id <= 0 || !end || *end != '\0') {
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Memory restore needs one numeric ID of a merged or replaced memory "
                    "(from 'list_merged').");
   }
   memory_restore_result_t rr = { 0 };
   const int rc = memory_db_fact_restore(user_id, (int64_t)id, &rr);
   if (rc == MEMORY_DB_NOT_FOUND) {
      return strdup(TOOL_RESULT_ERROR_MARK
                    "No merged or replaced memory with that ID (it may be current already, "
                    "or gone).");
   }
   if (rc != MEMORY_DB_SUCCESS) {
      return strdup(TOOL_RESULT_ERROR_MARK "Memory restore failed.");
   }
   char buf[256];
   snprintf(buf, sizeof(buf), "Restored memory %lld; it is current again%s.%s", id,
            rr.reopened > 0 ? ", with what it said about relationships and contact details" : "",
            rr.replaced ? " The value that replaced it is still current too: forget one of "
                          "them if only one is right."
                        : "");
   return strdup(buf);
}
