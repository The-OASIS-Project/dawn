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
 *  - research_plan   — seed the coverage ledger with sub-questions to close.
 *  - research_record — record ONE evidence claim (claim + source + quote).
 *
 * Both are reachable ONLY inside a research session (the read-only allowlist in
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
   if (ctx == NULL || ctx->research_run_id <= 0) {
      if (round_out) {
         *round_out = 0;
      }
      return 0;
   }
   if (round_out) {
      *round_out = ctx->research_round;
   }
   return ctx->research_run_id;
}

/* =============================================================================
 * research_plan — seed the coverage ledger
 * ============================================================================= */

static char *research_plan_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   if (should_respond) {
      *should_respond = 1;
   }

   int64_t run_id = research_active_run(NULL);
   if (run_id <= 0) {
      return strdup("Error: research_plan is only available inside a research run.");
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
   int64_t question_id = j_qid ? json_object_get_int64(j_qid) : 0;

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
   .description = "Seed the research plan with sub-questions to answer.  Use at the start of a "
                  "research round to record the concrete questions the topic breaks down into, and "
                  "again mid-research when a finding opens a new question worth closing.",
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

int research_plan_tool_register(void) {
   return tool_registry_register(&research_plan_metadata);
}

int research_record_tool_register(void) {
   return tool_registry_register(&research_record_metadata);
}
