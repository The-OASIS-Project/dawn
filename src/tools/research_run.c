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

#include <json-c/json.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "logging.h"
#include "utils/string_utils.h" /* sanitize_utf8_for_json */

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
   out->max_input_tokens = RESEARCH_DEFAULT_MAX_INPUT_TOKENS;
   out->min_sources = RESEARCH_DEFAULT_MIN_SOURCES;
   out->round_digest_max_chars = RESEARCH_DEFAULT_ROUND_DIGEST_MAX_CHARS;
   out->top_k_questions = RESEARCH_DEFAULT_TOP_K_QUESTIONS;
   out->saturation_rounds = RESEARCH_DEFAULT_SATURATION_ROUNDS;
   out->stale_rounds = RESEARCH_DEFAULT_STALE_ROUNDS;
   out->critic_max_rearm = RESEARCH_DEFAULT_CRITIC_MAX_REARM;
}

bool research_is_natural_end(const char *stop_reason) {
   if (stop_reason == NULL) {
      return false; /* fail closed — an unknown/absent stop is not a natural end */
   }
   return strcmp(stop_reason, "concluded") == 0 || strcmp(stop_reason, "coverage") == 0 ||
          strcmp(stop_reason, "saturation") == 0;
}

int research_critic_parse_verdict(const char *response, research_critic_verdict_t *out) {
   if (out == NULL) {
      return AUTH_DB_INVALID;
   }
   out->re_arm = false; /* fail-safe default: confirm the pending stop */
   out->n_gaps = 0;
   if (response == NULL || response[0] == '\0') {
      return AUTH_DB_SUCCESS; /* nothing to parse — stop stands */
   }

   /* Tolerant extraction: the judge is asked for bare JSON, but models wrap it in
    * prose/```json fences, so parse from the first '{' to its matching close rather
    * than assuming the whole response is JSON.  A brace-depth scan (skipping string
    * literals + escapes) finds the object bounds without a full pre-parse. */
   const char *start = strchr(response, '{');
   if (start == NULL) {
      return AUTH_DB_SUCCESS; /* no JSON object at all → stop */
   }
   int depth = 0;
   bool in_str = false, esc = false;
   const char *end = NULL;
   for (const char *p = start; *p != '\0'; p++) {
      char c = *p;
      if (esc) {
         esc = false;
         continue;
      }
      if (in_str) {
         if (c == '\\') {
            esc = true;
         } else if (c == '"') {
            in_str = false;
         }
         continue;
      }
      if (c == '"') {
         in_str = true;
      } else if (c == '{') {
         depth++;
      } else if (c == '}') {
         if (--depth == 0) {
            end = p;
            break;
         }
      }
   }
   if (end == NULL) {
      return AUTH_DB_SUCCESS; /* unbalanced braces → stop */
   }

   size_t len = (size_t)(end - start) + 1;
   char *json = malloc(len + 1);
   if (json == NULL) {
      return AUTH_DB_SUCCESS; /* OOM → stop (safe) */
   }
   memcpy(json, start, len);
   json[len] = '\0';

   struct json_object *root = json_tokener_parse(json);
   free(json);
   if (root == NULL || !json_object_is_type(root, json_type_object)) {
      if (root) {
         json_object_put(root);
      }
      return AUTH_DB_SUCCESS; /* not an object → stop */
   }

   struct json_object *j_decision = NULL;
   bool wants_continue = false;
   if (json_object_object_get_ex(root, "decision", &j_decision) &&
       json_object_is_type(j_decision, json_type_string)) {
      wants_continue = (strcmp(json_object_get_string(j_decision), "continue") == 0);
   }

   struct json_object *j_gaps = NULL;
   if (wants_continue && json_object_object_get_ex(root, "gaps", &j_gaps) &&
       json_object_is_type(j_gaps, json_type_array)) {
      size_t n = json_object_array_length(j_gaps);
      for (size_t i = 0; i < n && out->n_gaps < RESEARCH_CRITIC_MAX_GAPS; i++) {
         struct json_object *item = json_object_array_get_idx(j_gaps, i);
         if (item == NULL || !json_object_is_type(item, json_type_object)) {
            continue;
         }
         struct json_object *j_q = NULL;
         if (!json_object_object_get_ex(item, "question", &j_q) ||
             !json_object_is_type(j_q, json_type_string)) {
            continue;
         }
         const char *q = json_object_get_string(j_q);
         if (q == NULL || q[0] == '\0') {
            continue; /* skip empty questions */
         }
         char *slot = out->gaps[out->n_gaps];
         strncpy(slot, q, RESEARCH_QUESTION_MAX - 1);
         slot[RESEARCH_QUESTION_MAX - 1] = '\0';
         sanitize_utf8_for_json(slot); /* model-authored → keep it WS-frame-safe */
         out->n_gaps++;
      }
   }
   json_object_put(root);

   /* Re-arm ONLY on a clean "continue" that actually named at least one gap. */
   out->re_arm = (wants_continue && out->n_gaps > 0);
   return AUTH_DB_SUCCESS;
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
         /* Answered on coverage — no resolution reason (it was actually closed). */
         if (research_db_question_set_status(q->id, "answered", confidence, NULL) ==
             AUTH_DB_SUCCESS) {
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

int research_retire_stale_questions(int64_t run_id,
                                    int min_sources,
                                    int stale_threshold,
                                    research_stale_entry_t *tracker,
                                    int *tracker_n,
                                    int tracker_max,
                                    int64_t *retired_out,
                                    int retired_max,
                                    int *retired_n_out) {
   (void)min_sources; /* refresh_coverage owns the answered threshold; staleness is a
                         separate "no NEW source" signal that ignores the absolute count. */
   if (retired_n_out) {
      *retired_n_out = 0;
   }
   if (stale_threshold <= 0) {
      return AUTH_DB_SUCCESS; /* feature disabled — no-op */
   }
   if (run_id <= 0 || !tracker || !tracker_n || tracker_max < 0) {
      return AUTH_DB_INVALID;
   }

   research_question_t questions[RESEARCH_MAX_LEDGER_QUESTIONS];
   int n = 0;
   int rc = research_db_question_list(run_id, questions, RESEARCH_MAX_LEDGER_QUESTIONS, &n);
   if (rc != AUTH_DB_SUCCESS) {
      return rc; /* transient read fault — leave the tracker untouched, try next round */
   }

   int retired = 0;
   for (int i = 0; i < n; i++) {
      research_question_t *q = &questions[i];
      /* Only OPEN questions can be stale: refresh_coverage ran first, so anything
       * answered/unanswerable is already off the open set. */
      if (strcmp(q->status, "open") != 0) {
         continue;
      }

      int sources = 0;
      if (research_db_question_coverage(run_id, q->id, &sources) != AUTH_DB_SUCCESS) {
         continue; /* skip on a per-question read error — never fabricate staleness */
      }

      /* Find this question's tracker slot (linear over a small, run-bounded set). */
      research_stale_entry_t *e = NULL;
      for (int k = 0; k < *tracker_n; k++) {
         if (tracker[k].qid == q->id) {
            e = &tracker[k];
            break;
         }
      }
      if (e == NULL) {
         /* First time we see this question: seed a baseline, don't count it as dry
          * (a question added this round has had no chance to gather a source yet). */
         if (*tracker_n >= tracker_max) {
            continue; /* tracker full (pathological, > RESEARCH_MAX_LEDGER_QUESTIONS) */
         }
         e = &tracker[(*tracker_n)++];
         e->qid = q->id;
         e->last_sources = sources;
         e->stale_rounds = 0;
         continue;
      }

      if (sources > e->last_sources) {
         e->last_sources = sources; /* progress — a new distinct source arrived */
         e->stale_rounds = 0;
         continue;
      }

      /* Dry round: no new distinct source for this question. */
      if (++e->stale_rounds < stale_threshold) {
         continue;
      }
      if (research_db_question_set_status(q->id, "unanswerable", 0.0, "stale") == AUTH_DB_SUCCESS) {
         /* Count only what we actually write, so *retired_n_out always bounds a safe
          * iteration of retired_out[0..n): a question past retired_max (or when
          * retired_out is NULL) is still retired in the DB — the status flip above
          * already happened — it is simply not listed back to the caller. */
         if (retired_out && retired < retired_max) {
            retired_out[retired++] = q->id;
         }
         e->stale_rounds = 0; /* retired — it is no longer 'open', so stop re-counting */
      }
   }

   if (retired_n_out) {
      *retired_n_out = retired;
   }
   return AUTH_DB_SUCCESS;
}

const char *research_should_stop(const research_run_t *run,
                                 const research_budgets_t *b,
                                 bool all_closed,
                                 int no_progress_rounds,
                                 bool concluded) {
   if (!run || !b) {
      return "failed";
   }
   /* Natural-end reasons first, so the stop_reason reflects WHY the run is done
    * rather than merely that a ceiling was crossed on the same round.  These never
    * let a run overrun a budget: the hard checks below still fire every boundary,
    * so ordering only picks the label when two conditions coincide. */
   if (concluded) {
      /* The agent judged the brief covered (research_conclude).  Advisory only —
       * the controller still owns the stop; the caller has already gated this on
       * the run having recorded findings, so an empty "conclude" can't end a run. */
      return "concluded";
   }
   if (all_closed) {
      /* Every question answered or unanswerable — the ideal stop (plan MED). */
      return "coverage";
   }
   if (b->saturation_rounds > 0 && no_progress_rounds >= b->saturation_rounds) {
      /* Diminishing returns: N consecutive rounds closed no new question.  Stops a
       * run that keeps searching (and adding questions) without converging, instead
       * of grinding to the token ceiling (live run 2: round 3 spent 35% of the
       * budget and closed nothing). */
      return "saturation";
   }
   /* Hard fuses — the backstop for a run that keeps making progress but won't end.
    * Two, not three: the token ceiling is the real spend control, and max_rounds
    * bounds the loop depth.  max_tool_calls was RETIRED (§6.1) — it metered LLM
    * round-trips, which the per-round iteration cap already bounds at
    * max_rounds x LLM_TOOLS_MAX_ITERATIONS, so it added no distinct safety property
    * and, set below that structural ceiling, only guillotined productive runs early. */
   if (b->max_rounds > 0 && run->rounds_run >= b->max_rounds) {
      return "budget";
   }
   if (b->max_input_tokens > 0 && run->input_tokens >= b->max_input_tokens) {
      return "token_budget";
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

int research_render_critic_digest(int64_t run_id,
                                  const char *brief,
                                  const research_budgets_t *b,
                                  const research_run_t *cur,
                                  const char *stop_reason,
                                  int rearm_num,
                                  char *out,
                                  size_t out_size) {
   if (!out || out_size == 0 || !b || !cur) {
      return AUTH_DB_INVALID;
   }
   out[0] = '\0';
   size_t cap = out_size;
   if (b->round_digest_max_chars > 0 && (size_t)b->round_digest_max_chars < cap) {
      cap = (size_t)b->round_digest_max_chars;
   }
   size_t off = 0;

   digest_append(out, cap, &off, "Research brief:\n%s\n", brief ? brief : "(none)");
   digest_append(out, cap, &off,
                 "\nThe run is about to stop (reason: %s). Rounds used: %d/%d. Input tokens: "
                 "%lld/%lld. This would be re-arm %d of at most %d.\n",
                 stop_reason ? stop_reason : "?", cur->rounds_run, b->max_rounds,
                 (long long)cur->input_tokens, (long long)b->max_input_tokens, rearm_num,
                 b->critic_max_rearm);

   if (run_id <= 0) {
      return AUTH_DB_INVALID;
   }
   research_question_t questions[RESEARCH_MAX_LEDGER_QUESTIONS];
   int n = 0;
   int rc = research_db_question_list(run_id, questions, RESEARCH_MAX_LEDGER_QUESTIONS, &n);
   if (rc != AUTH_DB_SUCCESS) {
      return rc; /* out holds the brief + stop context — degrade, don't fail hard */
   }

   digest_append(out, cap, &off, "\nQuestions and coverage (status — why — sources):\n");
   for (int i = 0; i < n; i++) {
      research_question_t *q = &questions[i];
      int sources = 0;
      research_db_question_coverage(run_id, q->id, &sources);
      /* status[: reason] — the reason distinguishes an EXHAUSTED gap ("stale") from a
       * model-judged dead-end ("agent"); absent for open/answered. */
      if (q->resolution_reason[0]) {
         if (!digest_append(out, cap, &off, "  [q%lld] %s — %s: %s (sources %d/%d)\n",
                            (long long)q->id, q->question, q->status, q->resolution_reason, sources,
                            b->min_sources)) {
            break;
         }
      } else if (!digest_append(out, cap, &off, "  [q%lld] %s — %s (sources %d/%d)\n",
                                (long long)q->id, q->question, q->status, sources,
                                b->min_sources)) {
         break;
      }
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
   }
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
   if (cn == 0 && ccount > 0) {
      /* claim_count saw rows but the list read returned none — a transient
       * lock/IO fault, not an empty run.  Don't render (and let the caller
       * persist) a findings-less report that reads as authoritative when the
       * ledger actually holds evidence; surface the read failure instead. */
      OLOG_WARNING("research_render_report: run %lld count=%d but list returned 0 — read fault",
                   (long long)run_id, ccount);
      free(claims);
      digest_append(buf, cap, &off, "\n_The findings for this brief could not be read._\n");
      *out_markdown = buf;
      return AUTH_DB_SUCCESS;
   }
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

   /* The claim text is model-authored over non-ASCII web content, and both
    * digest_append's byte-bounded vsnprintf and the truncation rewind above can
    * land mid-codepoint.  This report is stored as a note and later emitted into
    * JSON WS frames (doc-library), where a malformed UTF-8 byte makes the browser's
    * JSON.parse throw and drops the whole frame (project invariant
    * tool_desc_utf8_truncation).  Sanitize once: valid multi-byte sequences are
    * preserved, only invalid/truncated ones become '?'. */
   sanitize_utf8_for_json(buf);

   *out_markdown = buf;
   return AUTH_DB_SUCCESS;
}
