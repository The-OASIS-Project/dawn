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
 * Deep-research controller — deterministic core (DEEP_RESEARCH_DESIGN.md §4/§6).
 *
 * The bounded round-digest renderer, coverage-driven question promotion, and the
 * P0 continue/stop decision.  All pure logic over the SQLite ledger — no LLM.
 * The live orchestration (session setup, round loop, synthesis) builds on these
 * and lands with the research_worker.
 */

#include "tools/research_run.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "logging.h"

/* Bounds for the synthesis render. Claims per run are structurally bounded (a
 * few rounds x a handful of questions x a few claims); the caller-allocated
 * snapshot is heap, and the report buffer is capped. */
#define RESEARCH_MAX_REPORT_CLAIMS 256
#define RESEARCH_REPORT_MAX 65536

void research_budgets_defaults(research_budgets_t *out) {
   if (!out) {
      return;
   }
   out->max_rounds = RESEARCH_DEFAULT_MAX_ROUNDS;
   out->max_tool_calls = RESEARCH_DEFAULT_MAX_TOOL_CALLS;
   out->max_input_tokens = RESEARCH_DEFAULT_MAX_INPUT_TOKENS;
   out->min_sources = RESEARCH_DEFAULT_MIN_SOURCES;
   out->round_digest_max_chars = RESEARCH_DEFAULT_ROUND_DIGEST_MAX_CHARS;
   out->top_k_questions = RESEARCH_DEFAULT_TOP_K_QUESTIONS;
}

int research_refresh_coverage(int64_t run_id, int min_sources, int *closed_out, int *total_out) {
   if (closed_out) {
      *closed_out = 0;
   }
   if (total_out) {
      *total_out = 0;
   }
   if (run_id <= 0 || min_sources < 1) {
      return AUTH_DB_INVALID;
   }

   research_question_t questions[RESEARCH_MAX_LEDGER_QUESTIONS];
   int n = 0;
   int rc = research_db_question_list(run_id, questions, RESEARCH_MAX_LEDGER_QUESTIONS, &n);
   if (rc != AUTH_DB_SUCCESS) {
      return rc;
   }
   if (n == RESEARCH_MAX_LEDGER_QUESTIONS) {
      /* The ledger overflowed the per-pass cap — questions past 128 are invisible
       * to coverage, so all_closed could fire prematurely.  Pathological, but log
       * it so the field case is visible rather than silent. */
      OLOG_WARNING("research_refresh_coverage: run %lld has >= %d questions; extras not counted",
                   (long long)run_id, RESEARCH_MAX_LEDGER_QUESTIONS);
   }

   int closed = 0;
   for (int i = 0; i < n; i++) {
      research_question_t *q = &questions[i];
      if (strcmp(q->status, "open") != 0) {
         closed++; /* already answered or unanswerable */
         continue;
      }
      int sources = 0;
      if (research_db_question_coverage(run_id, q->id, &sources) == AUTH_DB_SUCCESS &&
          sources >= min_sources) {
         double confidence = (double)sources / (double)min_sources;
         if (confidence > 1.0) {
            confidence = 1.0;
         }
         if (research_db_question_set_status(q->id, "answered", confidence) == AUTH_DB_SUCCESS) {
            closed++;
         }
      }
   }

   if (closed_out) {
      *closed_out = closed;
   }
   if (total_out) {
      *total_out = n;
   }
   return AUTH_DB_SUCCESS;
}

const char *research_should_stop(const research_run_t *run,
                                 const research_budgets_t *b,
                                 bool all_closed) {
   if (!run || !b) {
      return "failed";
   }
   /* Layer 1 — hard budgets, outermost.  The token ceiling is the real spend
    * control; tool-call count is a loose proxy (§6.1, eff M4). */
   if (b->max_rounds > 0 && run->rounds_run >= b->max_rounds) {
      return "budget";
   }
   if (b->max_tool_calls > 0 && run->tool_calls >= b->max_tool_calls) {
      return "budget";
   }
   if (b->max_input_tokens > 0 && run->input_tokens >= b->max_input_tokens) {
      return "token_budget";
   }
   /* Layer 2 — all questions closed (answered or unanswerable).  Without this a
    * budgets-only P0 burns the full budget on every run (plan MED). */
   if (all_closed) {
      return "coverage";
   }
   return NULL; /* continue */
}

/* Bounded append: writes at most (cap - *off - 1) chars of the formatted string
 * into out+*off and advances *off.  Never overflows; leaves out NUL-terminated.
 * Returns false once the cap is reached so the caller can stop early. */
static bool digest_append(char *out, size_t cap, size_t *off, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static bool digest_append(char *out, size_t cap, size_t *off, const char *fmt, ...) {
   if (*off + 1 >= cap) {
      return false;
   }
   size_t remaining = cap - *off;
   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(out + *off, remaining, fmt, ap);
   va_end(ap);
   if (n < 0) {
      return false;
   }
   if ((size_t)n >= remaining) {
      *off = cap - 1; /* truncated — full */
      return false;
   }
   *off += (size_t)n;
   return true;
}

int research_render_round_digest(int64_t run_id,
                                 const char *brief,
                                 const research_budgets_t *b,
                                 char *out,
                                 size_t out_size) {
   if (!out || out_size == 0 || !b) {
      return AUTH_DB_INVALID;
   }
   out[0] = '\0';

   /* Effective cap = min(caller buffer, the configured digest ceiling). */
   size_t cap = out_size;
   if (b->round_digest_max_chars > 0 && (size_t)b->round_digest_max_chars < cap) {
      cap = (size_t)b->round_digest_max_chars;
   }
   size_t off = 0;

   digest_append(out, cap, &off, "Research brief:\n%s\n", brief ? brief : "(none)");

   if (run_id <= 0) {
      return AUTH_DB_INVALID;
   }

   research_question_t questions[RESEARCH_MAX_LEDGER_QUESTIONS];
   int n = 0;
   int rc = research_db_question_list(run_id, questions, RESEARCH_MAX_LEDGER_QUESTIONS, &n);
   if (rc != AUTH_DB_SUCCESS) {
      /* out already holds the brief-only digest — degrade, don't fail hard. */
      return rc;
   }

   digest_append(out, cap, &off, "\nOpen questions to close this round:\n");
   int shown = 0;
   int max_k = b->top_k_questions > 0 ? b->top_k_questions : RESEARCH_DEFAULT_TOP_K_QUESTIONS;
   for (int i = 0; i < n && shown < max_k; i++) {
      research_question_t *q = &questions[i];
      if (strcmp(q->status, "open") != 0) {
         continue;
      }
      int sources = 0;
      research_db_question_coverage(run_id, q->id, &sources);
      if (!digest_append(out, cap, &off, "  [q%lld] %s (sources %d/%d)\n", (long long)q->id,
                         q->question, sources, b->min_sources)) {
         break; /* hit the cap */
      }
      /* Carry the findings SO FAR for this question across the per-round history
       * reset (§4a): without it round N+1 only sees the source COUNT and re-
       * researches from scratch.  A couple of earlier claim texts (capped) is
       * enough to build on and to avoid re-fetching the same ground. */
      research_claim_t gloss[RESEARCH_DIGEST_GLOSS_CLAIMS];
      int gn = 0;
      if (research_db_question_claims(run_id, q->id, gloss, RESEARCH_DIGEST_GLOSS_CLAIMS, &gn) ==
              AUTH_DB_SUCCESS &&
          gn > 0) {
         for (int g = 0; g < gn; g++) {
            if (!digest_append(out, cap, &off, "      - found: %.240s\n", gloss[g].claim)) {
               break;
            }
         }
      }
      shown++;
   }
   if (shown == 0) {
      /* Distinguish an empty ledger (round 1 — plan first) from all-closed. */
      digest_append(out, cap, &off,
                    n == 0 ? "  (none yet — start by calling research_plan to break the brief "
                             "into concrete sub-questions.)\n"
                           : "  (all questions are closed.)\n");
   }

   /* Last revision prose only (never every revision).  Absent in P0 unless
    * debug-gated revisions are on — omit cleanly when there is none. */
   char *last_md = NULL;
   if (research_db_revision_get_latest(run_id, &last_md) == AUTH_DB_SUCCESS && last_md) {
      digest_append(out, cap, &off, "\nWorking report so far:\n%s\n", last_md);
   }
   free(last_md);

   return AUTH_DB_SUCCESS;
}

/* =============================================================================
 * Synthesis — report = view over research_claims (§4/§8)
 *
 * MED-1 (untrusted-content egress): the report embeds each claim's model-authored
 * text (already injection-command-gated at ingest by research_record) plus its
 * source_url as a markdown link — it DELIBERATELY does NOT emit the raw `quote`
 * (the verbatim untrusted web excerpt), which stays in research_claims for the P3
 * audit surface only.  So the worst untrusted string reaching the note is a URL,
 * neutralized by the notes render pipeline (marked + DOMPurify) on the human side;
 * and reinvoke is disabled, so nothing re-injects the report into an LLM turn
 * automatically (§8/§11).
 * ============================================================================= */

int research_render_report(int64_t run_id, const char *brief, char **out_markdown) {
   if (!out_markdown) {
      return AUTH_DB_INVALID;
   }
   *out_markdown = NULL;
   if (run_id <= 0) {
      return AUTH_DB_INVALID;
   }

   char *buf = malloc(RESEARCH_REPORT_MAX);
   if (!buf) {
      return AUTH_DB_FAILURE;
   }
   buf[0] = '\0';
   size_t cap = RESEARCH_REPORT_MAX;
   size_t off = 0;

   digest_append(buf, cap, &off, "# Research report\n\n**Brief:** %s\n", brief ? brief : "(none)");

   /* Questions, for grouping claims under their sub-question headings. */
   research_question_t questions[RESEARCH_MAX_LEDGER_QUESTIONS];
   int qn = 0;
   research_db_question_list(run_id, questions, RESEARCH_MAX_LEDGER_QUESTIONS, &qn);

   int ccount = 0;
   int crc = research_db_claim_count(run_id, &ccount);
   if (crc != AUTH_DB_SUCCESS) {
      /* A read failure is NOT the same as an empty run — don't persist a false
       * "no findings" report over a transient DB error. */
      digest_append(buf, cap, &off, "\n_The findings for this brief could not be read._\n");
      *out_markdown = buf;
      return AUTH_DB_SUCCESS;
   }
   if (ccount <= 0) {
      digest_append(buf, cap, &off, "\n_No findings were recorded for this brief._\n");
      *out_markdown = buf;
      return AUTH_DB_SUCCESS;
   }

   int ccap = ccount < RESEARCH_MAX_REPORT_CLAIMS ? ccount : RESEARCH_MAX_REPORT_CLAIMS;
   research_claim_t *claims = calloc((size_t)ccap, sizeof(research_claim_t));
   if (!claims) {
      /* Out of memory for the snapshot — return the brief-only header rather
       * than failing the whole synthesis. */
      *out_markdown = buf;
      return AUTH_DB_SUCCESS;
   }
   int cn = 0;
   research_db_claim_list(run_id, claims, ccap, &cn);
   if (cn < ccount) {
      OLOG_WARNING("research_render_report: run %lld has %d claims; report capped at %d",
                   (long long)run_id, ccount, ccap);
   }

   /* claims arrive ordered by (question_id ASC, id ASC), so a single pass groups
    * them under headings. question_id 0 = general findings. */
   int64_t cur_q = -1;
   bool truncated = false;
   for (int i = 0; i < cn && !truncated; i++) {
      research_claim_t *c = &claims[i];
      if (c->question_id != cur_q) {
         cur_q = c->question_id;
         const char *qtext = "General findings";
         for (int k = 0; k < qn; k++) {
            if (questions[k].id == cur_q) {
               qtext = questions[k].question;
               break;
            }
         }
         if (!digest_append(buf, cap, &off, "\n## %s\n\n", qtext)) {
            truncated = true;
            break;
         }
      }
      bool ok = c->source_url[0] ? digest_append(buf, cap, &off, "- %s ([source](%s))\n", c->claim,
                                                 c->source_url)
                                 : digest_append(buf, cap, &off, "- %s\n", c->claim);
      if (!ok) {
         truncated = true;
      }
   }
   free(claims);

   /* Mark truncation so the reader knows the report is incomplete — from either
    * the byte cap (digest_append refused) or the claim-snapshot cap (cn<ccount).
    * Rewind if needed to guarantee the marker fits. */
   if (truncated || cn < ccount) {
      const size_t reserve = 96;
      if (cap > reserve && off > cap - reserve) {
         off = cap - reserve;
      }
      digest_append(buf, cap, &off,
                    "\n\n_(Report truncated — too many findings to render in full.)_\n");
   }

   *out_markdown = buf;
   return AUTH_DB_SUCCESS;
}
