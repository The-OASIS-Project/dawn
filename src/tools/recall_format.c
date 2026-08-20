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
 * Recall tool result formatter — see recall_format.h.
 */

#include "tools/recall_format.h"

#include <stdlib.h>
#include <string.h>

#include "core/strbuf.h"
#include "dawn_error.h"
#include "utils/string_utils.h"

/* Per-line one-liner cap — keeps a single candidate from dominating the budget
 * the engine already bounded; the read-pointer tells the LLM where the full
 * text lives. */
#define RECALL_LINE_TEXT_MAX 240

/* Stack buffer for a parsed document/note label.  DOC_FILENAME_MAX is 256 in
 * document_db.h; 300 leaves margin without coupling this file to that header. */
#define RECALL_FNAME_MAX 300

/* Source families, in render order. */
typedef enum {
   FAM_MEMORY = 0, /* memory_fact / memory_entity / memory_relation */
   FAM_SUMMARY,    /* memory_summary */
   FAM_DOC,        /* document_chunk (notes + documents) */
   FAM_CALENDAR,   /* calendar_event */
   FAM_OTHER,      /* anything else / future sources */
   FAM_COUNT
} recall_family_t;

static const char *const k_family_title[FAM_COUNT] = {
   "MEMORY — facts & relationships",
   "MEMORY — past conversation summaries",
   "NOTES & DOCUMENTS",
   "CALENDAR",
   "OTHER",
};

static recall_family_t family_of(const char *source_id) {
   if (source_id == NULL)
      return FAM_OTHER;
   if (strcmp(source_id, "memory_fact") == 0 || strcmp(source_id, "memory_entity") == 0 ||
       strcmp(source_id, "memory_relation") == 0)
      return FAM_MEMORY;
   if (strcmp(source_id, "memory_summary") == 0)
      return FAM_SUMMARY;
   if (strcmp(source_id, "document_chunk") == 0)
      return FAM_DOC;
   if (strcmp(source_id, "calendar_event") == 0)
      return FAM_CALENDAR;
   return FAM_OTHER;
}

/* item_id is "<prefix>:<numeric id>" (focus_candidate_format_item_id).  Return a
 * pointer to the id substring, or NULL if no ':' present. */
static const char *id_after_colon(const char *item_id) {
   if (item_id == NULL)
      return NULL;
   const char *c = strchr(item_id, ':');
   return (c && c[1] != '\0') ? c + 1 : NULL;
}

/* Document/note candidate text is rendered "[<filename>] <chunk text>".  Copy
 * the bracketed label into buf; return true if found. */
static bool filename_from_text(const char *text, char *buf, size_t buflen) {
   if (text == NULL || text[0] != '[')
      return false;
   const char *close = strchr(text, ']');
   if (close == NULL || close == text + 1)
      return false;
   size_t n = (size_t)(close - (text + 1));
   if (n >= buflen)
      n = buflen - 1;
   memcpy(buf, text + 1, n);
   buf[n] = '\0';
   return true;
}

/* Append `text` as a single line: collapse any CR/LF/tab to spaces and cap at
 * RECALL_LINE_TEXT_MAX bytes (with an ellipsis when truncated). */
static void append_oneline(strbuf_t *sb, const char *text) {
   if (text == NULL) {
      (void)strbuf_append(sb, "(no text)");
      return;
   }
   char line[RECALL_LINE_TEXT_MAX + 4];
   size_t w = 0;
   bool prev_space = false;
   const char *p = text;
   for (; *p && w < RECALL_LINE_TEXT_MAX; p++) {
      char c = *p;
      if (c == '\n' || c == '\r' || c == '\t')
         c = ' ';
      if (c == ' ') {
         if (prev_space)
            continue; /* collapse runs of whitespace */
         prev_space = true;
      } else {
         prev_space = false;
      }
      line[w++] = c;
   }
   line[w] = '\0';
   (void)strbuf_append(sb, line);
   /* `*p` non-NUL ⇒ the loop stopped at the cap, not the terminator: truncated.
    * (Tracking the loop exit avoids a full strlen rescan and is also correct
    * under whitespace collapse, which a raw strlen>cap test is not.) */
   if (*p)
      (void)strbuf_append(sb, "...");
}

static bool is_injected(const char *item_id, const char *const *injected_ids, int n) {
   if (item_id == NULL || injected_ids == NULL)
      return false;
   for (int i = 0; i < n; i++) {
      if (injected_ids[i] && strcmp(injected_ids[i], item_id) == 0)
         return true;
   }
   return false;
}

/* Emit one candidate's bullet line (without leading marker), including its
 * source-appropriate read-pointer. */
static void append_candidate_line(strbuf_t *sb, const focus_candidate_t *c, recall_family_t fam) {
   append_oneline(sb, c->text);
   switch (fam) {
      case FAM_MEMORY:
         /* Only facts carry a directly-fetchable id; entities/relations are
          * self-describing context, no precise fetch verb. */
         if (c->source_id && strcmp(c->source_id, "memory_fact") == 0) {
            const char *id = id_after_colon(c->item_id);
            if (id)
               (void)strbuf_appendf(sb, "   [memory id %s]", id);
         }
         break;
      case FAM_SUMMARY:
         /* No fetch pointer: summary item_ids are `summary:<id>`, a different
          * id space than facts, and `memory get` resolves only fact ids — a
          * `[memory id N]` here would dead-end.  Summaries are self-describing
          * narrative context, like entities/relations above. */
         break;
      case FAM_DOC: {
         char fname[RECALL_FNAME_MAX];
         if (filename_from_text(c->text, fname, sizeof(fname)))
            (void)strbuf_appendf(sb, "   -> document_read \"%s\"", fname);
         break;
      }
      case FAM_CALENDAR:
         (void)strbuf_append(sb, "   -> calendar (query by date/title)");
         break;
      default:
         break;
   }
}

char *recall_format_result(const char *query,
                           const focus_compose_result_t *result,
                           const char *const *injected_ids,
                           int injected_count) {
   const int n = (result != NULL) ? result->candidate_count : 0;

   /* Zero-result: be explicit so the LLM doesn't hallucinate coverage. */
   if (n <= 0) {
      strbuf_t z;
      strbuf_init(&z, 160);
      (void)strbuf_appendf(&z,
                           "I have nothing on file about \"%s\" in memory, notes, documents, "
                           "or the calendar.",
                           query ? query : "");
      char *out = strbuf_steal(&z);
      strbuf_free(&z);
      return out ? out : strdup("(recall: nothing found)");
   }

   strbuf_t sb;
   strbuf_init(&sb, 2048);
   (void)strbuf_appendf(&sb, "What I know about \"%s\":\n", query ? query : "");

   int per_family[FAM_COUNT] = { 0 };
   int injected_shown = 0;

   for (int fam = 0; fam < FAM_COUNT; fam++) {
      /* First pass: count this family so we can print a header with a count. */
      int count = 0;
      for (int i = 0; i < n; i++) {
         if (family_of(result->candidates[i].source_id) == (recall_family_t)fam)
            count++;
      }
      per_family[fam] = count;
      if (count == 0)
         continue;

      (void)strbuf_appendf(&sb, "\n%s (%d)\n", k_family_title[fam], count);
      for (int i = 0; i < n; i++) {
         const focus_candidate_t *c = &result->candidates[i];
         if (family_of(c->source_id) != (recall_family_t)fam)
            continue;
         bool dup = is_injected(c->item_id, injected_ids, injected_count);
         (void)strbuf_append(&sb, dup ? "  · " : "  • ");
         append_candidate_line(&sb, c, (recall_family_t)fam);
         if (dup) {
            (void)strbuf_append(&sb, "   (already in current context)");
            injected_shown++;
         }
         (void)strbuf_append(&sb, "\n");
      }
   }

   /* Footer: preserve-specifics instruction (Phase 1.5 — counters the
    * observed failure where the broad gather makes the model write a vague
    * high-level summary that drops the specific facts it just retrieved),
    * then how to get exact/full text, and which sources were empty. */
   (void)strbuf_append(&sb,
                       "\nWhen you answer from this, KEEP the specific facts, dates, counts, "
                       "names, and statuses above — list them, don't flatten them into a vague "
                       "summary. If the user asked \"how are we looking / where do things stand\", "
                       "lead with the concrete items (e.g. \"4 of 8 tasks done\", exact dates), "
                       "not just a percentage or a headline.\n");
   (void)strbuf_append(&sb,
                       "For exact/full text, follow a pointer: document_read \"<label>\" for a "
                       "note/document, or memory get <id> for a fact.\n");

   strbuf_t empties;
   strbuf_init(&empties, 64);
   int empty_n = 0;
   for (int fam = 0; fam < FAM_OTHER; fam++) {
      if (per_family[fam] == 0) {
         (void)strbuf_appendf(&empties, "%s%s", empty_n ? ", " : "", k_family_title[fam]);
         empty_n++;
      }
   }
   if (empty_n > 0)
      (void)strbuf_appendf(&sb, "Nothing found in: %s.\n", strbuf_str(&empties));
   strbuf_free(&empties);

   /* Dedup fallback note (design §4.2a): when the caller can't supply the
    * turn's injected-id set, flag the likely overlap rather than silently
    * re-stating context the LLM already has. */
   if (injected_ids == NULL)
      (void)strbuf_append(&sb,
                          "(Some top items may already be in this turn's injected context.)\n");
   else if (injected_shown > 0)
      (void)strbuf_appendf(&sb, "(%d item(s) marked · were already in your current context.)\n",
                           injected_shown);

   if (strbuf_oom(&sb)) {
      strbuf_free(&sb);
      return strdup("recall: result too large to format.");
   }
   char *out = strbuf_steal(&sb);
   strbuf_free(&sb);
   /* Guarantee the tool result is valid UTF-8.  The per-line byte-count caps
    * (append_oneline / filename_from_text) can stop mid-codepoint, leaving a
    * lone lead byte — one such byte 400s the Claude request ("surrogates not
    * allowed") AND wedges the WebUI socket ("Invalid UTF-8 in text frame") into
    * a reconnect loop.  sanitize repairs any partial/invalid sequence in place. */
   if (out != NULL) {
      sanitize_utf8_for_json(out);
   }
   return out ? out : strdup("recall: allocation failed.");
}
