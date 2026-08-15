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
 * In-loop deep-research tools (DEEP_RESEARCH_DESIGN.md §7):
 *
 *  - research_plan             — seed the coverage ledger with sub-questions.
 *  - research_record           — record ONE evidence claim (claim + source + quote).
 *  - research_conclude         — the agent signals the brief is covered (§6).
 *  - research_mark_unanswerable — the agent declares a sub-question unanswerable.
 *
 * All are reachable ONLY inside a research session (the read-only allowlist in
 * llm_tools.c gates them at schema advertisement AND execution).  Each reads the
 * active run id + round from the thread-local command-context session, so a
 * caller can never write to another run.  BOTH injection-gate the LLM-authored
 * text they store (claim text and question text) before it lands in the ledger —
 * a memory-POISONING gate on what gets stored, since both persist into the
 * RAG-retrievable report note, NOT an exfil control (§11).  Status transitions
 * are deliberately NOT LLM-driven: the deterministic controller owns them from
 * distinct-source coverage (§6).
 */

#include "tools/research_tools.h"

#include <json-c/json.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h" /* g_config.research.plan_freeze_round */
#include "core/conv_event.h"
#include "core/event_payload.h"
#include "core/memory_filter.h"
#include "core/session_manager.h"
#include "logging.h"
#include "tools/tool_registry.h"

/* Bound how many questions one research_plan call may add (keeps a single call
 * from flooding the ledger; the controller adds the top-level plan separately). */
#define RESEARCH_PLAN_MAX_QUESTIONS 32

/* The active research run for the calling session, or 0 if this is not a
 * research session (a hallucinated call from a non-research context). */
static int64_t research_active_run(int *round_out) {
   session_t *ctx = session_get_command_context();
   int64_t run_id = (ctx != NULL) ? atomic_load(&ctx->research_run_id) : 0;
   if (run_id <= 0) {
      if (round_out) {
         *round_out = 0;
      }
      return 0;
   }
   if (round_out) {
      *round_out = atomic_load(&ctx->research_round);
   }
   return run_id;
}

/* =============================================================================
 * research_plan — seed the coverage ledger
 * ============================================================================= */

static char *research_plan_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   if (should_respond) {
      *should_respond = 1;
   }

   int round = 0;
   int64_t run_id = research_active_run(&round);
   if (run_id <= 0) {
      return strdup("Error: research_plan is only available inside a research run.");
   }
   /* Plan freeze (§6): once past plan_freeze_round the model must converge on the
    * plan it has rather than keep expanding the denominator (runs 2/4/5 grew the plan
    * mid-run, tanking the coverage fraction and driving spend to the fuse).  Round 1
    * always plans (empty ledger); the clamp keeps plan_freeze_round >= 1. */
   if (round > g_config.research.plan_freeze_round) {
      OLOG_INFO("research_plan: plan frozen at round %d (run %lld) — refusing new questions", round,
                (long long)run_id);
      return strdup(
          "The research plan for this run is set — no new sub-questions now. Focus on closing the "
          "open questions with more sources; mark any you genuinely cannot answer with "
          "research_mark_unanswerable, or call research_conclude if you're done.");
   }
   if (!value || value[0] == '\0') {
      return strdup("Error: research_plan needs a JSON object: {\"questions\": [\"...\"]}.");
   }

   struct json_object *root = json_tokener_parse(value);
   if (!root) {
      return strdup("Error: research_plan arguments were not valid JSON.");
   }

   struct json_object *questions = NULL;
   if (!json_object_object_get_ex(root, "questions", &questions) ||
       !json_object_is_type(questions, json_type_array)) {
      json_object_put(root);
      return strdup("Error: research_plan needs a \"questions\" array of sub-question strings.");
   }

   /* Accumulate the "[qID] text" list of what was added, so the model can attach
    * SAME-ROUND findings to the right question_id via research_record.  Without
    * this the model has no ids until a later round's digest surfaces them, so the
    * round that plans records everything as question_id=0 (general), which never
    * advances per-question coverage. */
   char list[1024];
   size_t off = 0;
   int added = 0;
   int total = (int)json_object_array_length(questions);
   for (int i = 0; i < total && added < RESEARCH_PLAN_MAX_QUESTIONS; i++) {
      struct json_object *item = json_object_array_get_idx(questions, i);
      const char *q = item ? json_object_get_string(item) : NULL;
      if (!q || q[0] == '\0') {
         continue;
      }
      /* Injection-gate question text before storage, mirroring research_record:
       * a question is free-form output of an LLM being fed untrusted web content,
       * and it persists verbatim as a report-note heading that can be RAG-
       * retrieved into a full-tool session later (§11). */
      if (memory_filter_check_injection_commands(q)) {
         OLOG_WARNING(
             "research_plan: refused a question flagged by the injection filter (run %lld)",
             (long long)run_id);
         continue;
      }
      int64_t qid = 0;
      if (research_db_question_add(run_id, q, 0, &qid) == AUTH_DB_SUCCESS) {
         added++;
         int n = snprintf(list + off, sizeof(list) - off, "[q%lld] %.80s\n", (long long)qid, q);
         if (n > 0 && (size_t)n < sizeof(list) - off) {
            off += (size_t)n;
         }
      }
   }
   json_object_put(root);

   char buf[1024 + 256];
   if (added == 0) {
      snprintf(buf, sizeof(buf),
               "No questions added (empty, all rejected by the safety filter, or invalid).");
   } else {
      /* Only warn about the cap when it ACTUALLY truncated — i.e. we hit the cap
       * AND there were more array items past it.  Keying on raw `total` alone
       * would falsely warn when the extras were just empty/rejected items. */
      bool capped = (added == RESEARCH_PLAN_MAX_QUESTIONS && total > RESEARCH_PLAN_MAX_QUESTIONS);
      snprintf(buf, sizeof(buf),
               "Added %d research question%s. Pass the matching question_id when you record a "
               "finding with research_record:\n%s%s",
               added, added == 1 ? "" : "s", list,
               capped ? "(Extra questions past the per-call cap were dropped — add them in a later "
                        "call.)"
                      : "");
   }
   return strdup(buf);
}

/* =============================================================================
 * research_record — record one evidence claim
 * ============================================================================= */

/* Normalize a model-supplied question_id to the integer ledger id, tolerating the
 * exact token the round digest SHOWS the model ("[q5]" / "q5") as well as a bare
 * JSON int or a numeric string.  json_object_get_int64() returns 0 for a
 * non-numeric string, so a model that echoes the displayed "[q5]" token would
 * otherwise de-attribute EVERY finding to 0 (general) — the first-live-run
 * "0 answered" failure the attribution prompt was meant to close, reachable again
 * through the parser.  Returns 0 (general) when nothing positive-numeric is found. */
static int64_t research_parse_question_id(struct json_object *j_qid) {
   if (!j_qid) {
      return 0;
   }
   if (json_object_is_type(j_qid, json_type_int)) {
      int64_t v = json_object_get_int64(j_qid);
      return v > 0 ? v : 0;
   }
   const char *s = json_object_get_string(j_qid);
   if (!s) {
      return 0;
   }
   while (*s == ' ' || *s == '[' || *s == 'q' || *s == 'Q') {
      s++; /* strip the "[q" / "q" wrapper the digest renders around the id */
   }
   char *end = NULL;
   long long v = strtoll(s, &end, 10);
   return (end != s && v > 0) ? (int64_t)v : 0;
}

static char *research_record_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   if (should_respond) {
      *should_respond = 1;
   }

   int round = 0;
   int64_t run_id = research_active_run(&round);
   if (run_id <= 0) {
      return strdup("Error: research_record is only available inside a research run.");
   }
   if (!value || value[0] == '\0') {
      return strdup("Error: research_record needs a JSON object with at least \"claim\".");
   }

   struct json_object *root = json_tokener_parse(value);
   if (!root) {
      return strdup("Error: research_record arguments were not valid JSON.");
   }

   struct json_object *j_claim = NULL, *j_url = NULL, *j_kind = NULL, *j_quote = NULL,
                      *j_qid = NULL;
   json_object_object_get_ex(root, "claim", &j_claim);
   json_object_object_get_ex(root, "source_url", &j_url);
   json_object_object_get_ex(root, "source_kind", &j_kind);
   json_object_object_get_ex(root, "quote", &j_quote);
   json_object_object_get_ex(root, "question_id", &j_qid);

   const char *claim = j_claim ? json_object_get_string(j_claim) : NULL;
   if (!claim || claim[0] == '\0') {
      json_object_put(root);
      return strdup("Error: research_record needs a non-empty \"claim\".");
   }

   /* Memory-poisoning gate: refuse to STORE a claim that carries injected
    * commands (it would later re-enter synthesis / a full-tool session).  This
    * is not an exfil control — that boundary is the read-only allowlist + the
    * bare memory-free fetch session (§11). */
   if (memory_filter_check_injection_commands(claim)) {
      json_object_put(root);
      OLOG_WARNING("research_record: refused a claim flagged by the injection filter (run %lld)",
                   (long long)run_id);
      return strdup("Error: that claim was rejected by the safety filter and NOT recorded. "
                    "Record the factual finding in your own words without any embedded "
                    "instructions.");
   }

   const char *source_url = j_url ? json_object_get_string(j_url) : NULL;
   const char *source_kind = j_kind ? json_object_get_string(j_kind) : NULL;
   const char *quote = j_quote ? json_object_get_string(j_quote) : NULL;

   /* Egress gate on source_url: it is web-derived and lands verbatim in the report
    * note (later RAG-retrievable into a full-tool session), so it goes through the
    * same injection-command filter as the claim/question text — the §11.2 "every
    * stored output field is gated" invariant, which the URL had slipped.  A flagged
    * URL is dropped to NULL: the finding itself is still worth keeping, just
    * without the tainted citation. */
   if (source_url && source_url[0] && memory_filter_check_injection_commands(source_url)) {
      OLOG_WARNING(
          "research_record: dropped a source_url flagged by the injection filter (run %lld)",
          (long long)run_id);
      source_url = NULL;
   }

   /* Require a real HTTP(S) URL.  source_url is model-authored and feeds the
    * controller's coverage signal via COUNT(DISTINCT source_url) — an arbitrary
    * string ("source A", "the docs") or a non-web scheme would count as an
    * independent web source and let a run declare `coverage` without genuinely
    * distinct evidence.  A non-URL is dropped to NULL (the finding is still kept,
    * just uncited) so it neither counts toward coverage nor renders a broken link.
    * (Canonicalizing fragment/tracking-param variants of one page is a further
    * refinement, tracked separately.) */
   if (source_url && strncmp(source_url, "http://", 7) != 0 &&
       strncmp(source_url, "https://", 8) != 0) {
      OLOG_WARNING("research_record: dropped a non-HTTP(S) source_url (run %lld)",
                   (long long)run_id);
      source_url = NULL;
   }

   /* Attribute to the plan question the model named, tolerating the "[q5]" token
    * shape (research_parse_question_id), then VALIDATE it belongs to this run: an
    * id naming no question here (a hallucinated number, or one from another run) is
    * de-attributed to 0 rather than stored as an orphan that closes nothing and
    * later spawns a duplicate "General findings" heading in the report. */
   int64_t question_id = research_parse_question_id(j_qid);
   if (question_id > 0) {
      bool belongs = false;
      if (research_db_question_belongs(run_id, question_id, &belongs) != AUTH_DB_SUCCESS ||
          !belongs) {
         question_id = 0;
      }
   }

   int rc = research_db_claim_add(run_id, question_id, claim, source_url, source_kind, quote,
                                  round);

   /* Observe/replay (§10): emit a research_claim event with the recorded finding.
    * The claim is already injection-command-gated above; source_url/kind are
    * web-derived and UTF-8-sanitized by the payload builder.  Best-effort — an
    * observe event must never fail the record. */
   if (rc == AUTH_DB_SUCCESS) {
      session_t *ctx = session_get_command_context();
      if (ctx != NULL) {
         conv_event_emit(atomic_load(&ctx->stream_conversation_id), ctx->metrics.user_id,
                         CONV_EVENT_RESEARCH_CLAIM,
                         event_payload_research_claim(round, question_id, source_url, source_kind,
                                                      claim));
      }
   }
   json_object_put(root);

   if (rc != AUTH_DB_SUCCESS) {
      return strdup("Error: failed to record the claim.");
   }
   return strdup(source_url && source_url[0] ? "Claim recorded with its source."
                                             : "Claim recorded (no source URL given).");
}

/* =============================================================================
 * Metadata + registration
 * ============================================================================= */

static const treg_param_t research_plan_params[] = {
   {
       .name = "questions",
       .description =
           "JSON object {\"questions\": [\"sub-question 1\", \"sub-question 2\", ...]} — "
           "the specific open questions this research must answer.  Add concrete, "
           "independently-answerable sub-questions as you discover the shape of the "
           "topic.  Question STATUS is tracked automatically from source coverage; you "
           "do not mark questions answered.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = true,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

static const tool_metadata_t research_plan_metadata = {
   .name = "research_plan",
   .device_string = "research_plan",
   .topic = "dawn",
   .description =
       "Seed the research plan with sub-questions to answer.  Record the concrete "
       "questions the topic breaks down into — decompose thoroughly in the EARLY rounds, "
       "because after the first couple of rounds the plan is frozen and you converge on "
       "it (you cannot keep adding questions late).",
   .params = research_plan_params,
   .param_count = 1,
   .capabilities = TOOL_CAP_NONE,
   .default_local = true,
   .default_remote = true,
   .callback = research_plan_callback,
};

static const treg_param_t research_record_params[] = {
   {
       .name = "details",
       .description =
           "JSON object recording ONE evidence claim: {claim (required — the factual finding in "
           "your own words), source_url (the page it came from), source_kind ('web' default, or "
           "'document'/'memory'/'note'), quote (the exact supporting excerpt), question_id (the "
           "[qID] of the plan sub-question this finding answers — set this whenever the finding "
           "answers a planned question; coverage is tracked PER question, so a finding left "
           "unattributed (question_id 0) never helps close one)}.  Record one claim per call as "
           "you find it.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = true,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

static const tool_metadata_t research_record_metadata = {
   .name = "research_record",
   .device_string = "research_record",
   .topic = "dawn",
   .description = "Record one evidence claim (a factual finding + where it came from) into the "
                  "research ledger.  The final report is built from these recorded claims, so "
                  "record every finding you want represented, each with its source.",
   .params = research_record_params,
   .param_count = 1,
   .capabilities = TOOL_CAP_NONE,
   .default_local = true,
   .default_remote = true,
   .callback = research_record_callback,
};

/* =============================================================================
 * research_conclude — the agent signals the brief is covered
 * ============================================================================= */

static char *research_conclude_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   (void)value; /* no arguments — this is a signal, not a data write */
   if (should_respond) {
      *should_respond = 1;
   }

   int round = 0;
   int64_t run_id = research_active_run(&round);
   if (run_id <= 0) {
      return strdup("Error: research_conclude is only available inside a research run.");
   }

   /* Raise the flag on the command-context session.  The controller reads it at the
    * next round boundary (after this dispatch's tool workers join) and, once the run
    * has recorded findings, stops with reason "concluded" and synthesizes the
    * report.  The controller — not this tool — owns the stop (§6). */
   session_t *ctx = session_get_command_context();
   if (ctx == NULL) {
      /* Unreachable in practice: research_active_run above already read run_id > 0
       * from this same thread-local context.  Fail loud rather than telling the
       * agent the run ended when the flag was never actually set. */
      return strdup("Error: research_conclude could not reach the research session; keep going.");
   }
   session_research_mark_concluded(ctx);
   /* Observe (§10): make the agent-judgment path visible in the panel rather than
    * only inferable from the terminal stop reason. */
   conv_event_emit(atomic_load(&ctx->stream_conversation_id), ctx->metrics.user_id,
                   CONV_EVENT_RESEARCH_CONCLUDE, event_payload_research_conclude(round));
   OLOG_INFO("research_conclude: agent signalled completion for run %lld", (long long)run_id);
   return strdup("Research marked complete. The controller will finish this run and build the "
                 "report from your recorded findings — stop calling tools now.");
}

/* =============================================================================
 * research_mark_unanswerable — the agent declares a sub-question unanswerable
 * ============================================================================= */

static char *research_mark_unanswerable_callback(const char *action,
                                                 char *value,
                                                 int *should_respond) {
   (void)action;
   if (should_respond) {
      *should_respond = 1;
   }

   int64_t run_id = research_active_run(NULL);
   if (run_id <= 0) {
      return strdup("Error: research_mark_unanswerable is only available inside a research run.");
   }
   if (!value || value[0] == '\0') {
      return strdup("Error: research_mark_unanswerable needs {\"question_id\": N}.");
   }

   struct json_object *root = json_tokener_parse(value);
   if (!root) {
      return strdup("Error: research_mark_unanswerable arguments were not valid JSON.");
   }
   struct json_object *j_qid = NULL;
   json_object_object_get_ex(root, "question_id", &j_qid);
   int64_t qid = research_parse_question_id(j_qid);
   json_object_put(root);

   /* Fetch the question (scoped to this run) — this both validates the id (the agent
    * may only mark ITS OWN questions dead) and gives the text for the observe event.
    * The agent may only mark unanswerable, never answered: distinct-source coverage
    * stays the controller's deterministic call (§6). */
   research_question_t q;
   if (qid <= 0 || research_db_question_get(run_id, qid, &q) != AUTH_DB_SUCCESS) {
      return strdup("Error: no such question in this run. Use the [qID] from the directive.");
   }
   if (research_db_question_set_status(qid, "unanswerable", 0.0, "agent") != AUTH_DB_SUCCESS) {
      return strdup("Error: failed to mark the question unanswerable.");
   }

   session_t *ctx = session_get_command_context();
   if (ctx != NULL) {
      conv_event_emit(atomic_load(&ctx->stream_conversation_id), ctx->metrics.user_id,
                      CONV_EVENT_RESEARCH_UNANSWERABLE,
                      event_payload_research_unanswerable(qid, q.question, "agent"));
   }
   OLOG_INFO("research_mark_unanswerable: run %lld question %lld marked unanswerable",
             (long long)run_id, (long long)qid);
   return strdup("Question marked unanswerable — it no longer blocks completion. Keep going on the "
                 "others, or research_conclude if you're done.");
}

static const tool_metadata_t research_conclude_metadata = {
   .name = "research_conclude",
   .device_string = "research_conclude",
   .topic = "dawn",
   .description =
       "Signal that the brief has been researched thoroughly and further searching would "
       "add little — you have answered the sub-questions you can, each with enough "
       "independent sources. Ends the run and builds the report from your recorded "
       "findings. Do NOT call before recording findings; if a question cannot be "
       "answered, stop researching it and conclude rather than looping.",
   .params = NULL,
   .param_count = 0,
   .capabilities = TOOL_CAP_NONE,
   .default_local = true,
   .default_remote = true,
   .callback = research_conclude_callback,
};

static const treg_param_t research_unanswerable_params[] = {
   {
       .name = "details",
       .description =
           "JSON object {\"question_id\": N} — the [qID] of a sub-question that cannot be answered "
           "from available sources (evidence of absence, not just absence of effort). Marking it "
           "stops it from blocking completion.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = true,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

static const tool_metadata_t research_mark_unanswerable_metadata = {
   .name = "research_mark_unanswerable",
   .device_string = "research_mark_unanswerable",
   .topic = "dawn",
   .description =
       "Mark a planned sub-question as unanswerable when you have genuinely tried and the "
       "sources don't contain the answer. It stops blocking completion so the run can "
       "finish on the questions you CAN answer instead of grinding to the budget on one "
       "you can't. Use sparingly and only after real effort.",
   .params = research_unanswerable_params,
   .param_count = 1,
   .capabilities = TOOL_CAP_NONE,
   .default_local = true,
   .default_remote = true,
   .callback = research_mark_unanswerable_callback,
};

int research_plan_tool_register(void) {
   return tool_registry_register(&research_plan_metadata);
}

int research_record_tool_register(void) {
   return tool_registry_register(&research_record_metadata);
}

int research_conclude_tool_register(void) {
   return tool_registry_register(&research_conclude_metadata);
}

int research_mark_unanswerable_tool_register(void) {
   return tool_registry_register(&research_mark_unanswerable_metadata);
}
