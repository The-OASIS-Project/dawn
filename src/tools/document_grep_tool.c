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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * document_grep tool — literal substring search over indexed document/note
 * chunks, returning each match WITH its adjacent chunks.  Complements
 * document_search (semantic + BM25, which fuzzes exact strings): grep is
 * deterministic and exact, the right tool for IDs, codes, and exact field
 * values in structured/reference data (e.g. `birthday: '1975` in a YAML).
 * The "adjacent rows" come from a gap-safe chunk_index window so a hit that
 * lands mid-record shows the surrounding fields.
 */

#define _GNU_SOURCE /* strcasestr */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dawn_error.h"
#include "logging.h"
#include "tools/document_db.h"
#include "tools/document_grep.h"
#include "tools/tool_registry.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define DOC_GREP_PAGE 50            /* max matches returned per call (paged via offset) */
#define DOC_GREP_CONTEXT_DEFAULT 1  /* adjacent chunks shown each side of a hit */
#define DOC_GREP_CONTEXT_MAX 2      /* hard cap — keeps expanded output within budget */
#define DOC_GREP_TOKEN_BUDGET 2000  /* ~tokens of chunk text rendered before truncating */
#define DOC_GREP_RANGE_BATCH 16     /* chunks read per range query (reused buffer) */
#define DOC_GREP_MATCH_LINE_MAX 256 /* max length of the matched-line headline */
/* Output buffer: token budget × ~4 bytes/token for context, plus headroom for up
 * to DOC_GREP_PAGE per-match headlines (always emitted, NOT charged to the token
 * budget — a page of matched lines + citations runs a few KB on its own). */
#define DOC_GREP_RESULT_BUF_SIZE (DOC_GREP_TOKEN_BUDGET * 8)

/* =============================================================================
 * Forward Declarations
 * ============================================================================= */

static char *doc_grep_callback(const char *action, char *value, int *should_respond);
static bool doc_grep_is_available(void);

/* =============================================================================
 * Tool Metadata
 * ============================================================================= */

static const treg_param_t doc_grep_params[] = {
   {
       .name = "query",
       .description = "The EXACT text to find (literal substring, not a concept). Use this for "
                      "specific strings semantic search would blur — an ID, a code, an exact "
                      "field value like \"birthday: '1975\", a proper name.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = true,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
   {
       .name = "context",
       .description = "Adjacent chunks to show on each side of a match (0-2, default 1). Use 0 for "
                      "structured/record data (YAML/CSV — each record is its own chunk, so a match "
                      "already carries the whole record); use 1-2 only for prose/logs where a "
                      "record may span chunks. Higher context on record data just adds neighbor "
                      "noise.",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "context",
   },
   {
       .name = "case_sensitive",
       .description = "Match case exactly (default false / case-insensitive).",
       .type = TOOL_PARAM_TYPE_BOOL,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "case_sensitive",
   },
   {
       .name = "offset",
       .description = "Skip this many matches before returning (default 0). Use to page through "
                      "more results when a search reports additional matches.",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "offset",
   },
};

static const tool_metadata_t doc_grep_metadata = {
   .name = "document_grep",
   .device_string = "document grep",
   .description = "Find an EXACT string in the user's saved documents and notes and return each "
                  "match with its neighboring chunks. Use this instead of document_search when "
                  "you need literal/exact matches — IDs, codes, exact field values, or a specific "
                  "phrase — especially in structured data (YAML/CSV/logs) where a record spans "
                  "chunks. Deterministic: no ranking, no embeddings. Paginated via offset.",
   .params = doc_grep_params,
   .param_count = 4,
   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = 0,
   .is_getter = true,
   .is_available = doc_grep_is_available,
   .callback = doc_grep_callback,
};

/* =============================================================================
 * Registration
 * ============================================================================= */

int document_grep_tool_register(void) {
   return tool_registry_register(&doc_grep_metadata);
}

/* =============================================================================
 * Availability — grep needs only the document DB, NOT the embedding engine.
 * ============================================================================= */

static bool doc_grep_is_available(void) {
   /* Intentionally available without the embedding engine (unlike document_search/
    * document_read): grep does no embedding, so already-indexed chunks stay
    * greppable even if the embedder is down. Do not "align" this with the siblings. */
   return true;
}

/* =============================================================================
 * Internal: bounded result append
 * ============================================================================= */

/* Append into result[*pos] safely. snprintf returns the would-have-written
 * length, so a naive `*pos += snprintf(...)` can drive *pos PAST the buffer,
 * after which `(size_t)(bufsz - *pos)` underflows and the next write corrupts the
 * heap. This helper never advances *pos past bufsz and flips *truncated on a full
 * buffer, so callers just check *truncated to stop. */
static void grep_append(char *buf, int bufsz, int *pos, bool *truncated, const char *fmt, ...) {
   if (*truncated || *pos >= bufsz) {
      *truncated = true;
      return;
   }
   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(buf + *pos, (size_t)(bufsz - *pos), fmt, ap);
   va_end(ap);
   if (n < 0 || n >= bufsz - *pos) {
      *pos = bufsz; /* buffer full — clamp so later size args can't underflow */
      *truncated = true;
      return;
   }
   *pos += n;
}

/* Extract the single line of `text` that contains `needle` (the literal that
 * grep matched), with leading indentation trimmed, into out[outsz]. This is the
 * headline shown for every match so the actual hit is always visible even when
 * the budget can't afford the surrounding chunk(s). */
static void extract_matched_line(const char *text,
                                 const char *needle,
                                 bool case_sensitive,
                                 char *out,
                                 size_t outsz) {
   if (outsz == 0)
      return;
   out[0] = '\0';
   if (!text || !needle)
      return;

   const char *hit = case_sensitive ? strstr(text, needle) : strcasestr(text, needle);
   if (!hit)
      hit = text; /* grep matched the chunk, so this is just a belt-and-suspenders */

   const char *ls = hit;
   while (ls > text && ls[-1] != '\n')
      ls--;
   while (*ls == ' ' || *ls == '\t')
      ls++; /* trim leading indent */
   const char *le = hit;
   while (*le && *le != '\n')
      le++;

   size_t len = (size_t)(le - ls);
   if (len >= outsz)
      len = outsz - 1;
   memcpy(out, ls, len);
   out[len] = '\0';
}

/* =============================================================================
 * Callback
 * ============================================================================= */

static char *doc_grep_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   *should_respond = 1;

   if (!value || value[0] == '\0')
      return strdup("Error: no grep query provided.");

   /* Custom params (read before stripping the primary value). */
   int context = DOC_GREP_CONTEXT_DEFAULT;
   bool case_sensitive = false;
   int offset = 0;
   char tmp[32];
   if (tool_param_extract_custom(value, "context", tmp, sizeof(tmp))) {
      context = atoi(tmp);
      if (context < 0)
         context = 0;
      if (context > DOC_GREP_CONTEXT_MAX)
         context = DOC_GREP_CONTEXT_MAX;
   }
   if (tool_param_extract_custom(value, "case_sensitive", tmp, sizeof(tmp)))
      case_sensitive = (strcmp(tmp, "true") == 0);
   if (tool_param_extract_custom(value, "offset", tmp, sizeof(tmp))) {
      offset = atoi(tmp);
      if (offset < 0)
         offset = 0;
   }

   /* The query is everything before the first "::" custom-param separator. */
   char needle[DOC_CHUNK_TEXT_MAX];
   const char *sep = strstr(value, "::");
   if (sep) {
      size_t len = (size_t)(sep - value);
      if (len >= sizeof(needle))
         len = sizeof(needle) - 1;
      memcpy(needle, value, len);
      needle[len] = '\0';
   } else {
      snprintf(needle, sizeof(needle), "%s", value);
   }
   if (needle[0] == '\0')
      return strdup("Error: empty grep query.");

   int user_id = tool_get_current_user_id();

   doc_grep_hit_t hits[DOC_GREP_PAGE];
   int nhits = 0;
   bool more = false;
   if (document_db_chunk_grep(user_id, needle, case_sensitive, offset, hits, DOC_GREP_PAGE, &nhits,
                              &more) != SUCCESS) {
      OLOG_ERROR("document_grep: chunk_grep failed for user %d", user_id);
      return strdup("Error: document grep failed.");
   }

   if (nhits == 0) {
      char msg[256];
      if (offset > 0)
         snprintf(msg, sizeof(msg), "No more matches for \"%.120s\" past offset %d.", needle,
                  offset);
      else
         snprintf(msg, sizeof(msg),
                  "No matches found for \"%.120s\" in the user's documents/notes.", needle);
      return strdup(msg);
   }

   char *result = malloc((size_t)DOC_GREP_RESULT_BUF_SIZE);
   document_chunk_t *rbuf = malloc(DOC_GREP_RANGE_BATCH * sizeof(document_chunk_t));
   if (!result || !rbuf) {
      free(result);
      free(rbuf);
      return strdup("Error: memory allocation failed.");
   }

   int pos = 0;
   int budget = DOC_GREP_TOKEN_BUDGET;
   bool truncated = false;

   grep_append(result, DOC_GREP_RESULT_BUF_SIZE, &pos, &truncated,
               "GREP \"%.120s\" — %d match%s (from result %d)%s:\n", needle, nhits,
               nhits == 1 ? "" : "es", offset + 1, case_sensitive ? " [case-sensitive]" : "");

   /* One block per match (SQL orders by document_id, chunk_index). The matched
    * LINE is emitted for EVERY hit — it's tiny and not charged against the token
    * budget — so the actual match is always visible. The surrounding chunk(s)
    * are budget-gated and follow; when the budget is spent we keep emitting
    * headlines but skip context. This is the fix for a large leading-context
    * chunk truncating the real hit out of its own result. */
   int64_t prev_doc = -1;
   int prev_hi = -1; /* highest chunk_index already rendered for prev_doc (context dedup) */
   int ctx_omitted = 0;

   for (int i = 0; i < nhits; i++) {
      int64_t doc = hits[i].document_id;
      int idx = hits[i].chunk_index;
      int nc = hits[i].num_chunks;
      if (doc != prev_doc) {
         prev_doc = doc;
         prev_hi = -1;
      }

      /* Headline: the matched line, pulled from the hit chunk. Always shown. */
      char matched[DOC_GREP_MATCH_LINE_MAX] = "";
      int one = 0;
      if (document_db_chunk_read_range(doc, idx, idx, rbuf, 1, &one) == SUCCESS && one == 1)
         extract_matched_line(rbuf[0].text, needle, case_sensitive, matched, sizeof(matched));
      grep_append(result, DOC_GREP_RESULT_BUF_SIZE, &pos, &truncated,
                  "\n[%d] %.255s · chunk %d: %s\n", i + 1, hits[i].filename, idx + 1, matched);

      /* Surrounding chunks (window includes the hit chunk itself, so even
       * context=0 shows the full matched chunk). Budget-gated; skip already-
       * rendered chunks from a previous nearby hit in the same doc. */
      int lo = idx - context;
      if (lo < 0)
         lo = 0;
      if (lo <= prev_hi)
         lo = prev_hi + 1;
      int hi = idx + context;
      if (nc > 0 && hi > nc - 1)
         hi = nc - 1;

      if (budget <= 0) {
         ctx_omitted++;
         continue;
      }

      int rstart = lo;
      while (rstart <= hi && budget > 0) {
         int cnt = 0;
         if (document_db_chunk_read_range(doc, rstart, hi, rbuf, DOC_GREP_RANGE_BATCH, &cnt) !=
                 SUCCESS ||
             cnt == 0)
            break;
         for (int c = 0; c < cnt; c++) {
            int ctoks = ((int)strlen(rbuf[c].text) + 3) / 4;
            if (ctoks > budget) {
               budget = 0;
               break;
            }
            budget -= ctoks;
            grep_append(result, DOC_GREP_RESULT_BUF_SIZE, &pos, &truncated, "%s[%d] %s\n",
                        rbuf[c].chunk_index == idx ? ">>> " : "    ", rbuf[c].chunk_index + 1,
                        rbuf[c].text);
         }
         rstart = rbuf[cnt - 1].chunk_index + 1;
      }
      if (hi > prev_hi)
         prev_hi = hi;
   }

   /* Footer: more matches to page through, and/or context trimmed to fit. */
   if (more || ctx_omitted > 0 || truncated) {
      if (pos > DOC_GREP_RESULT_BUF_SIZE - 200)
         pos = DOC_GREP_RESULT_BUF_SIZE - 200;
      bool foot_trunc = false;
      if (more)
         grep_append(result, DOC_GREP_RESULT_BUF_SIZE, &pos, &foot_trunc,
                     "\n[More matches available — pass offset=%d for the next set, or narrow the "
                     "query.]",
                     offset + nhits);
      if (ctx_omitted > 0 || truncated)
         grep_append(
             result, DOC_GREP_RESULT_BUF_SIZE, &pos, &foot_trunc,
             "\n[Surrounding-chunk context trimmed to fit for %d match%s; all matched lines "
             "are shown above. Ask for a specific match or use document_read for full "
             "context.]",
             ctx_omitted, ctx_omitted == 1 ? "" : "es");
   }

   free(rbuf);
   return result; /* caller takes ownership */
}
