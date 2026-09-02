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
 * Memory citation — pure parse/format core (no I/O), split out so the <cited>
 * tokenizer is directly unit-testable.  See memory_citation_internal.h.
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/text_filter.h" /* CITED_TAG_OPEN/CLOSE(_LEN) — single-sourced grammar */
#include "memory/memory_citation_internal.h"

void memory_citation_csv_append(char *buf, size_t bufsz, size_t *len, const char *s) {
   if (buf == NULL || bufsz == 0 || *len >= bufsz - 1) {
      return;
   }
   int n = snprintf(buf + *len, bufsz - *len, "%s%s", (*len > 0) ? "," : "", s);
   if (n > 0) {
      *len += ((size_t)n < bufsz - *len) ? (size_t)n : (bufsz - *len - 1);
   }
}

/* Add a resolved canonical id (e.g. "fact:5881") to the cited_all dedup set.
 * Returns true if newly added (append it to the CSV), false if already present
 * (cross-provenance / repeated-token dedup).  cap = distinct-citeable ceiling, so
 * it is never reached before every distinct id is stored. */
static bool cited_all_add(char seen_ids[][64], int *n_seen, int cap, const char *id) {
   for (int i = 0; i < *n_seen; i++) {
      if (strcmp(seen_ids[i], id) == 0) {
         return false;
      }
   }
   if (*n_seen < cap) {
      strncpy(seen_ids[*n_seen], id, 63);
      seen_ids[*n_seen][63] = '\0';
      (*n_seen)++;
   }
   return true;
}

void memory_citation_resolve_cited(const char *text,
                                   const citation_stash_t *stash,
                                   const tool_cited_set_t *tool_set,
                                   char *cited_all,
                                   size_t cited_all_sz,
                                   char *cited_focus,
                                   size_t cited_focus_sz,
                                   int *out_focus_count,
                                   int *out_tool_count,
                                   int *out_dropped,
                                   int *out_dropped_tool) {
   size_t all_len = 0;
   size_t focus_len = 0;
   if (cited_all_sz > 0) {
      cited_all[0] = '\0';
   }
   if (cited_focus_sz > 0) {
      cited_focus[0] = '\0';
   }
   int cited_focus_count = 0;
   int cited_tool_count = 0;
   int dropped = 0;      /* focus ordinal out-of-range / duplicate */
   int dropped_tool = 0; /* ID: not in the surfaced set (mis-copied / hallucinated) */

   int stash_count = (stash != NULL) ? stash->count : 0;
   if (stash_count > MAX_CITATION_STASH) {
      stash_count = MAX_CITATION_STASH;
   }
   int tool_count = (tool_set != NULL) ? tool_set->count : 0;
   if (tool_count > MAX_TOOL_CITED_FACTS) {
      tool_count = MAX_TOOL_CITED_FACTS;
   }

   bool seen_ord[MAX_CITATION_STASH + 1] = { false };            /* focus-ordinal dedup */
   char seen_ids[MAX_CITATION_STASH + MAX_TOOL_CITED_FACTS][64]; /* cited_all cross-dedup */
   int n_seen = 0;
   const int seen_cap = MAX_CITATION_STASH + MAX_TOOL_CITED_FACTS;

   const char *scan = (text != NULL) ? text : "";
   const char *open;
   while ((open = strstr(scan, CITED_TAG_OPEN)) != NULL) {
      const char *inner = open + CITED_TAG_OPEN_LEN;
      const char *close = strstr(inner, CITED_TAG_CLOSE);
      const char *end = (close != NULL) ? close : (inner + strlen(inner)); /* orphan-tolerant */
      const char *p = inner;
      while (p < end) {
         if (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n') {
            p++; /* separator */
            continue;
         }
         if ((*p == 'I' || *p == 'i') && (p + 1) < end && (p[1] == 'D' || p[1] == 'd')) {
            /* candidate ID: token — require a ':' (optional spaces) then digits */
            const char *q = p + 2;
            while (q < end && *q == ' ') {
               q++;
            }
            if (q < end && *q == ':') {
               q++;
               while (q < end && *q == ' ') {
                  q++;
               }
               if (q < end && isdigit((unsigned char)*q)) {
                  int64_t fid = 0;
                  int digits = 0;
                  while (q < end && isdigit((unsigned char)*q) && digits < 18) {
                     fid = fid * 10 + (*q - '0');
                     q++;
                     digits++;
                  }
                  while (q < end && isdigit((unsigned char)*q)) {
                     q++; /* consume an over-long tail so it can't reparse as a token */
                  }
                  bool in_set = false;
                  for (int i = 0; i < tool_count; i++) {
                     if (tool_set->entries[i].fact_id == fid) {
                        in_set = true;
                        break;
                     }
                  }
                  if (in_set) {
                     char canon[32];
                     snprintf(canon, sizeof(canon), "fact:%lld", (long long)fid);
                     /* cited_all_add false => already cited (focus or a prior token):
                      * dedup, not a drop.  cited_tool_count is derived after the loop
                      * (n_seen - focus) so it is order-independent — do NOT bump here. */
                     if (cited_all_add(seen_ids, &n_seen, seen_cap, canon)) {
                        memory_citation_csv_append(cited_all, cited_all_sz, &all_len, canon);
                     }
                  } else {
                     dropped_tool++; /* mis-copied / hallucinated tool id */
                  }
               }
               p = q;
               continue;
            }
            /* "ID" not followed by ':' — consume it (and any trailing digits) so a
             * malformed token can't false-validate as a focus ordinal. */
            p += 2;
            while (p < end && isdigit((unsigned char)*p)) {
               p++;
            }
            continue;
         }
         if (*p == 'M' || *p == 'm') {
            p++; /* focus-ordinal prefix; skip optional spaces then read the digits */
            while (p < end && *p == ' ') {
               p++;
            }
         }
         if (p < end && isdigit((unsigned char)*p)) {
            int ord = 0;
            int digits = 0;
            while (p < end && isdigit((unsigned char)*p) && digits < 7) {
               ord = ord * 10 + (*p - '0');
               p++;
               digits++;
            }
            while (p < end && isdigit((unsigned char)*p)) {
               p++; /* consume an over-long tail so it can't reparse as a token */
            }
            if (ord >= 1 && ord <= stash_count && !seen_ord[ord]) {
               seen_ord[ord] = true;
               /* item_id[64] is contractually NUL-terminated by its producer
                * (build_focus_block memsets the stash + writes <=63 chars). */
               const char *id = stash->entries[ord - 1].item_id;
               memory_citation_csv_append(cited_focus, cited_focus_sz, &focus_len, id);
               cited_focus_count++;
               if (cited_all_add(seen_ids, &n_seen, seen_cap, id)) {
                  memory_citation_csv_append(cited_all, cited_all_sz, &all_len, id);
               }
            } else {
               dropped++; /* out-of-range (hallucinated/stale) or duplicate ordinal */
            }
            continue;
         }
         p++; /* any other char */
      }
      if (close == NULL) {
         break; /* orphan opener — no further well-formed tags */
      }
      scan = close + CITED_TAG_CLOSE_LEN;
   }

   /* Tool cites = distinct cited ids (n_seen = |cited_all|) minus the focus-cited
    * subset.  Derived rather than incremented so a fact cited via BOTH a focus
    * ordinal and its tool id counts as focus-only, independent of token order. */
   cited_tool_count = n_seen - cited_focus_count;
   if (cited_tool_count < 0) {
      cited_tool_count = 0; /* unreachable: every focus cite is in seen_ids */
   }

   if (out_focus_count != NULL) {
      *out_focus_count = cited_focus_count;
   }
   if (out_tool_count != NULL) {
      *out_tool_count = cited_tool_count;
   }
   if (out_dropped != NULL) {
      *out_dropped = dropped;
   }
   if (out_dropped_tool != NULL) {
      *out_dropped_tool = dropped_tool;
   }
}

int memory_citation_extract_fact_ids(const char *cited_all, int64_t *out_ids, int max) {
   if (cited_all == NULL || out_ids == NULL || max <= 0) {
      return 0;
   }
   int n = 0;
   const char *p = cited_all;
   /* Tokens are comma-separated "kind:<digits>"; emit ids for the "fact:" kind only.
    * The resolver already de-duplicated, so no cross-token dedup is needed here. */
   while (*p != '\0' && n < max) {
      /* Skip leading separators / whitespace. */
      while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\n') {
         p++;
      }
      if (*p == '\0') {
         break;
      }
      const char *tok = p;
      while (*p != '\0' && *p != ',') {
         p++;
      }
      /* [tok, p) is one token. */
      if ((size_t)(p - tok) > 5 && strncmp(tok, "fact:", 5) == 0) {
         const char *q = tok + 5;
         int64_t id = 0;
         int digits = 0;
         while (q < p && *q >= '0' && *q <= '9' && digits < 18) {
            id = id * 10 + (*q - '0');
            q++;
            digits++;
         }
         if (digits > 0) {
            out_ids[n++] = id;
         }
      }
   }
   return n;
}
