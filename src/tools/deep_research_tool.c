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
 * Deep-research tool (DEEP_RESEARCH_DESIGN.md §7/§7a/§15 Step 7).  See header.
 *
 * start is CONFIRMATION-GATED (§7a, locked): the first call with confirm=false
 * (the default) proposes the run + its cost envelope and writes NOTHING; only a
 * follow-up confirm=true — after the user says yes — creates the job + run and
 * spawns the controller.  This mirrors DAWN's two-step confirm (email send/phone
 * dial): a research run costs real time/tokens/money, so Friday proposes and
 * never spins one up autonomously.  Completion is notify + report link, NOT
 * reinvoke_parent (§11 HIGH-2 — re-injecting web-derived report text into a
 * full-tool session is the injection vector).  Layer 3.
 */

#include <json-c/json.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/conv_event.h"
#include "core/event_payload.h"
#include "core/job_dispatch.h"
#include "core/job_manager.h"
#include "core/memory_filter.h"
#include "core/research_worker.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/research_run.h"
#include "tools/tool_registry.h"

/* Cap on ledger questions we tally for the status digest (matches the core's
 * per-pass ceiling). */
#define DR_STATUS_MAX_QUESTIONS RESEARCH_MAX_LEDGER_QUESTIONS

/* --- start: propose (unconfirmed) or spawn (confirmed) --------------------- */

/* Build the pre-spawn proposal + cost envelope (§7a step 2).  Addressed to the
 * model: it relays this to the user and, on a yes, calls start again with
 * confirm=true.  We state the honest budget (rounds + provider) rather than a
 * fabricated dollar figure — and it MUST reflect the budget the run will actually
 * use, so read the runtime config (research_budgets_load) rather than the compile-
 * time defaults: an operator who raised [research] max_input_tokens/max_rounds would
 * otherwise be shown, and relay to the user, the wrong (default) envelope. */
static char *research_build_proposal(const char *brief, bool private_requested) {
   research_budgets_t b;
   research_budgets_load(&b);
   const bool local = (job_provider_from_default() == JOB_PROVIDER_LOCAL);

   /* Sized to hold a full-length brief (RESEARCH_BRIEF_MAX) plus the fixed prose,
    * so a long brief is never silently truncated. */
   char out[RESEARCH_BRIEF_MAX + 768];
   int n = snprintf(
       out, sizeof(out),
       "Proposed deep-research run on: \"%s\".\n"
       "Plan: up to %d rounds of web search, recording each finding with its source, then a "
       "written report with citations. Budget ~%lldk input tokens.\n"
       "Cost: %s\n"
       "%s"
       "This will run in the background and can take a while. Confirm with the user first; if they "
       "say yes, call deep_research start again with the same brief and confirm=true. Do NOT start "
       "it without their go-ahead.",
       brief, b.max_rounds, (long long)(b.max_input_tokens / 1000),
       local ? "runs on the local model — no API cost, but slower."
             : "runs on the cloud model — this spends paid API tokens.",
       private_requested
           ? "Note: searching the user's own notes/documents isn't available yet, so this run is "
             "web-only. Let them know.\n"
           : "");
   if (n < 0) {
      return strdup("Error: failed to build the research proposal.");
   }
   return strdup(out);
}

/* Shared spawn path (§16.3) — called by handle_start (after its confirm gate)
 * and the headless admin verb.  Preserves the exact ordering that carries the
 * safety invariants (fail-closed job_kind stamp; run row created BEFORE spawn);
 * do NOT reorder.  Writes the specific failure reason into `err` so both callers
 * relay the same message.  P0 forces mode='web'. */
int research_spawn_run(int user_id,
                       int64_t parent_conv,
                       const char *brief,
                       const char *deliver_to,
                       int64_t *run_id_out,
                       int64_t *conv_id_out,
                       char *err,
                       size_t err_len) {
   if (run_id_out != NULL) {
      *run_id_out = 0;
   }
   if (conv_id_out != NULL) {
      *conv_id_out = 0;
   }
   if (brief == NULL || brief[0] == '\0') {
      snprintf(err, err_len, "Error: 'brief' is required — the question or topic to research.");
      return FAILURE;
   }
   /* Re-filter here so BOTH spawn surfaces gate the brief (§16.4); the tool also
    * filters early, before its proposal, so a bad brief never gets a proposal. */
   if (memory_filter_check(brief)) {
      snprintf(err, err_len, "Error: that research brief was rejected by the safety filter.");
      return FAILURE;
   }

   /* Web research is useless without a search backend: with search disabled the
    * fetch loop can only plan + record, finds nothing, and would deliver a
    * confusing empty "done" report.  Refuse up front instead.  Checked via the
    * registry so this tracks the search tool's own availability rule (SearXNG
    * endpoint / Tavily) without duplicating it. */
   const tool_metadata_t *search_meta = tool_registry_find("search");
   if (search_meta == NULL || (search_meta->is_available != NULL && !search_meta->is_available())) {
      snprintf(err, err_len,
               "Can't start deep research — web search isn't configured. Set up a search "
               "backend (SearXNG or Tavily) first, then try again.");
      return FAILURE;
   }

   /* Refuse cleanly past a running cap before creating any row. */
   job_provider_class_t provider = job_provider_from_default();
   int cap = job_manager_capacity(user_id, provider);
   if (cap != JOB_MGR_OK) {
      const char *why = (cap == JOB_MGR_CAP_GLOBAL) ? "the system is at its background-job limit"
                        : (cap == JOB_MGR_CAP_PROVIDER)
                            ? "too many jobs are already running on this model"
                        : (cap == JOB_MGR_CAP_USER)
                            ? "you already have the maximum number of background jobs running"
                            : "background jobs are unavailable";
      snprintf(err, err_len,
               "Can't start that research run right now — %s. Try again once one finishes.", why);
      return FAILURE;
   }

   char title[CONV_TITLE_MAX];
   conv_generate_title(brief, title, sizeof(title));

   /* Create the job conversation.  on_complete='notify' — research NEVER uses
    * reinvoke_parent (§11 HIGH-2). */
   int64_t conv_id = 0;
   if (conv_db_create_job_ex(user_id, title, parent_conv, "detached", "notify", deliver_to, 1,
                             brief, job_spawn_origin_string(), &conv_id) != AUTH_DB_SUCCESS) {
      snprintf(err, err_len, "Error: failed to create the research job.");
      return FAILURE;
   }

   /* Mark it a research job so the plain worker can never resume it (§5.5).  Set
    * while the row is 'queued' and before the worker exists, so nothing races.
    * This is the SOLE enforcer of the no-plain-resume invariant, so a failure to
    * stamp must fail the start CLOSED: an unmarked research job is plain-resumable
    * and a plain resume would corrupt the run.  The run row doesn't exist yet, so
    * only the job row needs retiring. */
   if (conv_db_job_set_kind(conv_id, "research") != AUTH_DB_SUCCESS) {
      job_manager_set_terminal(conv_id, user_id, "failed", "failed to mark research job",
                               time(NULL), 0);
      job_manager_mark_dirty();
      OLOG_ERROR("deep_research: failed to set job_kind on conv %lld — failing start",
                 (long long)conv_id);
      snprintf(err, err_len, "Error: failed to initialize the research run.");
      return FAILURE;
   }

   /* Create the run row BEFORE spawning — the worker reads it by conversation id
    * the instant it starts.  P0 forces mode='web'. */
   int64_t run_id = 0;
   if (research_db_run_create(user_id, conv_id, brief, "web", &run_id) != AUTH_DB_SUCCESS) {
      job_manager_set_terminal(conv_id, user_id, "failed", "failed to create research run",
                               time(NULL), 0);
      job_manager_mark_dirty();
      snprintf(err, err_len, "Error: failed to create the research run.");
      return FAILURE;
   }

   /* Push the 'queued' row BEFORE the spawn (same ordering rule as job_tool: the
    * detached worker can reach a terminal disposition immediately, so emitting
    * after would let this thread enqueue a stale active row behind the worker's
    * terminal frame). */
   job_update_emit(conv_id, user_id);

   /* Tree edge on the parent's event stream (the panel shows the spawn). */
   if (parent_conv > 0) {
      conv_event_emit(parent_conv, user_id, CONV_EVENT_SPAWN, event_payload_spawn(conv_id, title));
   }

   if (research_worker_spawn(user_id, conv_id) != SUCCESS) {
      research_db_run_set_terminal(run_id, "failed", "failed", time(NULL));
      job_manager_set_terminal(conv_id, user_id, "failed", "research worker spawn failed",
                               time(NULL), 0);
      job_manager_mark_dirty();
      snprintf(err, err_len, "Error: failed to start the research worker.");
      return FAILURE;
   }

   if (run_id_out != NULL) {
      *run_id_out = run_id;
   }
   if (conv_id_out != NULL) {
      *conv_id_out = conv_id;
   }
   return SUCCESS;
}

static char *handle_start(struct json_object *details, int user_id, int64_t parent_conv) {
   struct json_object *jbrief = NULL, *jmode = NULL, *jdt = NULL, *jconfirm = NULL;

   const char *brief = (json_object_object_get_ex(details, "brief", &jbrief) && jbrief)
                           ? json_object_get_string(jbrief)
                           : NULL;
   if (brief == NULL || brief[0] == '\0') {
      return strdup("Error: 'brief' is required — the question or topic to research.");
   }
   if (memory_filter_check(brief)) {
      return strdup("Error: that research brief was rejected by the safety filter.");
   }

   /* mode is validated but P0 supports web only (private/both need the P2 egress
    * control, §11). A private/both request is downgraded to web with a note. */
   const char *mode = (json_object_object_get_ex(details, "mode", &jmode) && jmode)
                          ? json_object_get_string(jmode)
                          : "web";
   if (strcmp(mode, "web") != 0 && strcmp(mode, "private") != 0 && strcmp(mode, "both") != 0) {
      return strdup("Error: mode must be 'web', 'private', or 'both'.");
   }
   const bool private_requested = (strcmp(mode, "web") != 0);

   /* Strict boolean (matches the dispatch gate): a coerced "false" string must
    * NOT start a run. */
   const bool confirm = json_object_object_get_ex(details, "confirm", &jconfirm) && jconfirm &&
                        json_object_is_type(jconfirm, json_type_boolean) &&
                        json_object_get_boolean(jconfirm);

   /* Unconfirmed: propose only — write nothing (§7a). */
   if (!confirm) {
      return research_build_proposal(brief, private_requested);
   }

   const char *deliver_to = (json_object_object_get_ex(details, "deliver_to", &jdt) && jdt)
                                ? json_object_get_string(jdt)
                                : NULL;
   if (deliver_to != NULL && (deliver_to[0] == '\0' || memory_filter_check(deliver_to))) {
      deliver_to = NULL; /* ignore empty / suspicious delivery target */
   }

   /* All the ordering-sensitive spawn work (search-backend check, capacity, job
    * creation, fail-closed research stamp, run row before spawn, spawn) lives in
    * the shared research_spawn_run so the headless admin verb runs the identical
    * sequence (§16.3). */
   int64_t run_id = 0, conv_id = 0;
   char err[256];
   if (research_spawn_run(user_id, parent_conv, brief, deliver_to, &run_id, &conv_id, err,
                          sizeof(err)) != SUCCESS) {
      return strdup(err);
   }

   char title[CONV_TITLE_MAX];
   conv_generate_title(brief, title, sizeof(title));
   char buf[CONV_TITLE_MAX + 224];
   snprintf(buf, sizeof(buf),
            "Started deep-research run #%lld: \"%s\". I'll research this in the background and let "
            "you know when the report is ready — check on it with deep_research status %lld.",
            (long long)run_id, title, (long long)run_id);
   return strdup(buf);
}

/* --- status ---------------------------------------------------------------- */

static char *handle_status(struct json_object *details, int user_id) {
   struct json_object *jid = NULL;
   int64_t run_id = (json_object_object_get_ex(details, "run_id", &jid) && jid)
                        ? json_object_get_int64(jid)
                        : 0;
   if (run_id <= 0) {
      return strdup("Error: 'run_id' is required (the number from deep_research start).");
   }

   research_run_t run;
   int rc = research_db_run_get(run_id, user_id, &run);
   if (rc != AUTH_DB_SUCCESS) {
      /* Collapse NOT_FOUND and any not-owned answer to ONE message: run ids are
       * small sequential integers, so a distinct "belongs to someone else" reply
       * is an enumeration oracle (mirrors job_tool handle_status). */
      return strdup("No such research run.");
   }

   /* Tally coverage from the ledger (open / answered / unanswerable). */
   research_question_t qs[DR_STATUS_MAX_QUESTIONS];
   int nq = 0;
   int open = 0, answered = 0, unanswerable = 0;
   if (research_db_question_list(run_id, qs, DR_STATUS_MAX_QUESTIONS, &nq) == AUTH_DB_SUCCESS) {
      for (int i = 0; i < nq; i++) {
         if (strcmp(qs[i].status, "answered") == 0) {
            answered++;
         } else if (strcmp(qs[i].status, "unanswerable") == 0) {
            unanswerable++;
         } else {
            open++;
         }
      }
   }

   const bool terminal = (strcmp(run.status, "done") == 0 || strcmp(run.status, "failed") == 0 ||
                          strcmp(run.status, "cancelled") == 0);

   /* Sized to hold a full-length brief plus the fixed digest lines. */
   char digest[RESEARCH_BRIEF_MAX + 512];
   snprintf(digest, sizeof(digest),
            "Research run #%lld — %s.\n"
            "Brief: \"%s\"\n"
            "Rounds run: %d · tool calls: %d\n"
            "Questions: %d open / %d answered / %d unanswerable (of %d)\n"
            "%s%s%s",
            (long long)run.id, run.status, run.brief, run.rounds_run, run.tool_calls, open,
            answered, unanswerable, nq, terminal && run.stop_reason[0] ? "Stopped: " : "",
            terminal && run.stop_reason[0] ? run.stop_reason : "",
            run.report_doc_id > 0 ? "\nA written report is ready in your notes." : "");

   /* The digest can carry model/web-derived question text; gate it with the
    * narrowed injection-command filter before it re-enters the caller's full-tool
    * session (sec HIGH-2/MED-4).  The full memory_filter_check is deliberately
    * NOT used — it false-positives on technical research terms. */
   if (memory_filter_check_injection_commands(digest)) {
      char buf[128];
      snprintf(buf, sizeof(buf),
               "Research run #%lld is %s, but its status couldn't be safely shown inline.",
               (long long)run.id, run.status);
      return strdup(buf);
   }
   return strdup(digest);
}

/* --- cancel ---------------------------------------------------------------- */

static char *handle_cancel(struct json_object *details, int user_id) {
   struct json_object *jid = NULL;
   int64_t run_id = (json_object_object_get_ex(details, "run_id", &jid) && jid)
                        ? json_object_get_int64(jid)
                        : 0;
   if (run_id <= 0) {
      return strdup("Error: 'run_id' is required to cancel a research run.");
   }

   research_run_t run;
   if (research_db_run_get(run_id, user_id, &run) != AUTH_DB_SUCCESS) {
      return strdup("No such research run."); /* one answer — see handle_status */
   }
   if (strcmp(run.status, "done") == 0 || strcmp(run.status, "failed") == 0 ||
       strcmp(run.status, "cancelled") == 0) {
      char buf[128];
      snprintf(buf, sizeof(buf), "Research run #%lld already finished (%s).", (long long)run.id,
               run.status);
      return strdup(buf);
   }

   /* ONE mutation path (like job_tool): a RUNNING run is signalled through its
    * session (the controller stops at its next round boundary, §5.6); a still-
    * QUEUED run — spawn is synchronous but job_manager_begin runs inside the
    * detached worker, so a real no-session window exists — is retired directly and
    * marked fired.  Plain job_manager_cancel() cannot retire a queued row, so a
    * cancel landing in that window would be reported "not running" while the
    * worker then claimed and ran it to completion. */
   char status[JOB_STATUS_MAX];
   char buf[160];
   switch (job_manager_cancel_or_retire(run.conversation_id, user_id, status, sizeof(status))) {
      case JOB_CANCEL_SIGNALLED:
         snprintf(buf, sizeof(buf),
                  "Cancelling research run #%lld — it'll stop after the current round.",
                  (long long)run.id);
         return strdup(buf);
      case JOB_CANCEL_RETIRED:
         /* The queued row was retired before it ran; the worker never runs, so
          * mirror the cancel onto the research header (the retire touches only the
          * job row). */
         research_db_run_set_terminal(run.id, "cancelled", "cancelled", time(NULL));
         snprintf(buf, sizeof(buf), "Cancelled research run #%lld before it started.",
                  (long long)run.id);
         return strdup(buf);
      case JOB_CANCEL_ALREADY_TERMINAL:
         /* The job finished between the run-status read above and here. */
         snprintf(buf, sizeof(buf), "Research run #%lld already finished (%s).", (long long)run.id,
                  status);
         return strdup(buf);
      case JOB_CANCEL_FORBIDDEN:
      case JOB_CANCEL_NOT_FOUND:
         break; /* one answer for both — see handle_status */
   }
   /* No default: -Wswitch flags a new enumerator instead of silently mislabeling. */
   return strdup("No such research run.");
}

/* --- dispatch -------------------------------------------------------------- */

static char *deep_research_callback(const char *action, char *value, int *should_respond) {
   if (should_respond != NULL) {
      *should_respond = 1; /* results feed back to the LLM */
   }
   if (action == NULL || action[0] == '\0') {
      return strdup("Error: action is required (start, status, cancel).");
   }

   /* Execution backstop for the runtime master switch: the native schema already
    * hides the tool when disabled (llm_tools_refresh), but this refuses any
    * non-schema entry (a legacy <command> tag, a hallucinated call replayed from
    * history) so a disabled feature can never actually run. */
   if (!g_config.research.enabled) {
      return strdup("Deep research is turned off. Enable it in Settings → Deep Research "
                    "(or set [research] enabled = true in dawn.toml).");
   }

   struct json_object *details = NULL;
   if (value != NULL && value[0] != '\0') {
      details = json_tokener_parse(value);
      if (details == NULL) {
         return strdup("Error: invalid JSON in details parameter.");
      }
   } else {
      details = json_object_new_object();
   }

   /* Caller context: user_id is the requester's (non-overridable); parent conv is
    * the conversation this request came from. */
   int user_id = 1;
   int64_t parent_conv = 0;
   bool caller_is_job = false;
   session_t *ctx = session_get_command_context();
   if (ctx != NULL) {
      if (ctx->metrics.user_id > 0) {
         user_id = ctx->metrics.user_id;
      }
      parent_conv = atomic_load(&ctx->stream_conversation_id);
      caller_is_job = (ctx->type == SESSION_TYPE_JOB);
   }

   /* Only a CONFIRMED start begins work (takes a pool slot + LLM budget).  Require
    * a real JSON boolean true — json_object_get_boolean() would coerce a non-empty
    * string like "false" to true, and a confirmed start spends real budget. */
   bool starts_work = false;
   if (strcmp(action, "start") == 0) {
      struct json_object *jc = NULL;
      starts_work = (json_object_object_get_ex(details, "confirm", &jc) && jc &&
                     json_object_is_type(jc, json_type_boolean) && json_object_get_boolean(jc));
   }

   /* No session context means an unauthenticated caller — an MQTT publish on a
    * broker without auth/TLS would otherwise run as user_id 1.  There is no
    * legitimate unauthenticated deep_research caller (unlike a harmless user-scoped
    * read), and cancel is a mutation (DoS on user 1's runs), so fail EVERY action
    * closed when there is no session — not just the confirmed start. */
   if (ctx == NULL) {
      json_object_put(details);
      OLOG_WARNING("deep_research: refused '%s' from a caller with no session context", action);
      return strdup("Error: deep research can only be used from a user session.");
   }

   /* A background-job/research worker runs headless and must never start research
    * runs (unbounded fan-out).  The tool is already hidden from a research
    * session's schema (allowlist) and from a job session's; this backstops any
    * non-schema path (e.g. a legacy <command> tag). */
   if (caller_is_job && starts_work) {
      json_object_put(details);
      return strdup("Error: a background task can't start a deep-research run.");
   }

   char *result = NULL;
   if (strcmp(action, "start") == 0) {
      result = handle_start(details, user_id, parent_conv);
   } else if (strcmp(action, "status") == 0) {
      result = handle_status(details, user_id);
   } else if (strcmp(action, "cancel") == 0) {
      result = handle_cancel(details, user_id);
   } else {
      char buf[160];
      snprintf(buf, sizeof(buf), "Error: unknown action '%s'. Valid: start, status, cancel.",
               action);
      result = strdup(buf);
   }

   json_object_put(details);
   return result;
}

/* =============================================================================
 * Metadata + registration
 * ============================================================================= */

static const treg_param_t deep_research_params[] = {
   {
       .name = "action",
       .description = "The action: 'start' (propose/begin a research run), 'status' (check a run's "
                      "progress / find its report), 'cancel' (stop a run).",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "start", "status", "cancel" },
       .enum_count = 3,
   },
   {
       .name = "details",
       .description =
           "JSON object with action-specific fields.\n"
           "start: {brief (required — the question/topic to research), mode ('web' (default), "
           "'private', or 'both'; private/both are not available yet and run web-only), deliver_to "
           "(optional messaging channel display_name for the completion notice), confirm (boolean, "
           "default false)}. Call start with confirm=false FIRST to get the plan + cost envelope, "
           "show it to the user, and ONLY after they agree call start again with confirm=true.\n"
           "status: {run_id (the number from start)}.\n"
           "cancel: {run_id (required)}.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

/* Runtime master switch: the tool is compiled in (DAWN_ENABLE_DEEP_RESEARCH_TOOL)
 * but refuses to advertise/execute until [research] enabled = true (default off,
 * §9) — a research run costs real time/tokens, so it is opt-in. */
static bool deep_research_is_available(void) {
   return g_config.research.enabled;
}

static const tool_metadata_t deep_research_metadata = {
   .name = "deep_research",
   .device_string = "deep_research",
   .topic = "dawn",
   .aliases = { "research", "deep_dive" },
   .alias_count = 2,

   .description =
       "Kick off a THOROUGH background research investigation that READS FULL SOURCE PAGES across "
       "many rounds, tracks coverage per sub-question, and saves a persistent CITED REPORT to the "
       "user's notes.  This is not the same as answering with a few 'search' calls yourself: it "
       "goes much DEEPER (it fetches and reads whole pages, not just search snippets, over many "
       "more rounds than you would do inline) and it runs in the BACKGROUND so the user isn't left "
       "waiting.\n\n"
       "Prefer deep_research over answering inline when ANY of these hold: the user asks you to "
       "'research', 'look into', 'dig into', 'investigate', or 'do a deep dive on' a topic; the "
       "user wants a written brief/report or a thorough comparison; the topic is broad or "
       "comparison-heavy enough that a handful of searches would only skim it; or the user wants "
       "it "
       "done in the background while they do something else.  If a couple of 'search' calls would "
       "genuinely answer the question well and the user clearly wants the answer right now, just "
       "do "
       "that instead — do not force a background run on a quick question.  For a single background "
       "task that is not multi-source research, use the 'job' tool.\n\n"
       "start is CONFIRMATION-GATED, so reaching for it is low-risk: call it with confirm=false "
       "FIRST to get the plan + cost envelope, show that to the user, and only start the run "
       "(confirm=true) after they say yes.  The run works on its own; on completion the user is "
       "notified, a cited report is saved to their notes, and a summary lands back in this "
       "conversation.  Check progress with status.",

   .params = deep_research_params,
   .param_count = 2,
   .capabilities = TOOL_CAP_NETWORK,
   .default_local = true,
   .default_remote = true,
   .callback = deep_research_callback,
   .is_available = deep_research_is_available,
};

int deep_research_tool_register(void) {
   return tool_registry_register(&deep_research_metadata);
}
