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
 * Deep-research operator admin opcode handlers (dawn-admin research
 * start/status/cancel) — the HEADLESS benchmark spawn path
 * (DEEP_RESEARCH_DESIGN.md §16).  Sibling of admin_socket_ota.c.
 *
 * `start` deliberately BYPASSES the conversational confirmation gate (§7a): the
 * operator invoking it over the SO_PEERCRED-gated admin socket IS the human
 * authorization the confirm otherwise collects (§16.4).  It reuses the IDENTICAL
 * shared spawn path (research_spawn_run), so the loop / read-only allowlist /
 * injection gates are byte-for-byte the tool's — the only thing dropped is the
 * chat turn.  mode is forced to web (private/both need the unbuilt P2 egress
 * control); --user is REQUIRED and validated against a real account so a
 * benchmark's third-party brief never lands in the primary user's space by
 * default; every spawn is audit-logged.
 */

#define ADMIN_SOCKET_INTERNAL_ALLOWED

#ifdef DAWN_ENABLE_DEEP_RESEARCH_TOOL

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "auth/admin_socket_internal.h"
#include "auth/auth_db.h"
#include "config/dawn_config.h" /* g_config.research.enabled */
#include "core/job_manager.h"   /* job_manager_cancel_or_retire, JOB_CANCEL_*, JOB_STATUS_MAX */
#include "dawn_error.h"
#include "logging.h"
#include "tools/deep_research_tool.h" /* research_spawn_run */
#include "tools/research_run.h"       /* RESEARCH_MAX_LEDGER_QUESTIONS */

/* Little-endian readers — the wire is fixed LE regardless of host byte order. */
static int32_t rd_i32le(const unsigned char *p) {
   return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                    ((uint32_t)p[3] << 24));
}
static int64_t rd_i64le(const unsigned char *p) {
   uint64_t v = 0;
   for (int i = 0; i < 8; i++) {
      v |= (uint64_t)p[i] << (8 * i);
   }
   return (int64_t)v;
}

/* =============================================================================
 * research start — headless spawn.  Payload: [user_id i32][brief bytes].
 * ============================================================================= */

int handle_research_start_cmd(int client_fd, const char *payload, uint16_t payload_len) {
   if (!g_config.research.enabled) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                "Deep research is disabled ([research].enabled = false).");
   }
   /* Need at least user_id (4) + one brief byte. */
   if (!payload || payload_len < 5) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                "Invalid research start payload (need user_id + brief).");
   }
   const unsigned char *p = (const unsigned char *)payload;
   int user_id = (int)rd_i32le(p);
   if (user_id <= 0) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                "A valid --user is required (a benchmark run must not default to "
                                "the primary user).");
   }
   /* Validate the target user exists (§16.4): a benchmark brief is third-party
    * text, so it must not create an orphan conversation under a bogus uid. */
   auth_user_identity_t ident;
   if (auth_db_get_user_identity(user_id, &ident) != AUTH_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_NOT_FOUND, "No such user.");
   }

   uint16_t brief_len = (uint16_t)(payload_len - 4);
   if (brief_len >= RESEARCH_BRIEF_MAX) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Brief too long.");
   }
   char brief[RESEARCH_BRIEF_MAX];
   memcpy(brief, payload + 4, brief_len);
   brief[brief_len] = '\0';

   int64_t run_id = 0, conv_id = 0;
   char err[256];
   if (research_spawn_run(user_id, 0 /* no parent conv */, brief, NULL /* deliver_to */, &run_id,
                          &conv_id, err, sizeof(err)) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, err);
   }

   /* A headless spawn is invisible to the --user it runs as, so record it
    * (§16.4).  Sibling admin handlers log their actions the same way. */
   OLOG_INFO("research: admin spawned run #%lld (conv %lld) for user %d (%u-byte brief)",
             (long long)run_id, (long long)conv_id, user_id, (unsigned)brief_len);

   char msg[96];
   snprintf(msg, sizeof(msg), "run_id=%lld conv_id=%lld", (long long)run_id, (long long)conv_id);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
}

/* =============================================================================
 * research status — machine-readable run line.  Payload: [user_id i32][run_id i64].
 * Reads the ledger directly (NOT the tool's handle_status: its injection gate is
 * for an LLM consumer; this consumer is a script, §16.3).
 * ============================================================================= */

int handle_research_status_cmd(int client_fd, const char *payload, uint16_t payload_len) {
   if (!payload || payload_len < 12) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid research status payload.");
   }
   const unsigned char *p = (const unsigned char *)payload;
   int user_id = (int)rd_i32le(p);
   int64_t run_id = rd_i64le(p + 4);
   if (user_id <= 0 || run_id <= 0) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "user_id and run_id are required.");
   }

   research_run_t run;
   if (research_db_run_get(run_id, user_id, &run) != AUTH_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_NOT_FOUND, "No such research run.");
   }

   research_question_t qs[RESEARCH_MAX_LEDGER_QUESTIONS];
   int nq = 0, open = 0, answered = 0, unanswerable = 0;
   if (research_db_question_list(run_id, qs, RESEARCH_MAX_LEDGER_QUESTIONS, &nq) ==
       AUTH_DB_SUCCESS) {
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

   char out[512];
   snprintf(out, sizeof(out),
            "run_id=%lld status=%s stop_reason=%s rounds=%d tool_calls=%d input_tokens=%lld "
            "open=%d answered=%d unanswerable=%d total=%d report_doc_id=%lld",
            (long long)run.id, run.status, run.stop_reason[0] ? run.stop_reason : "-",
            run.rounds_run, run.tool_calls, (long long)run.input_tokens, open, answered,
            unanswerable, nq, (long long)run.report_doc_id);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, out);
}

/* =============================================================================
 * research cancel — stop a run at its next round boundary (timeout recovery for
 * the unattended driver).  Payload: [user_id i32][run_id i64].  Mirrors the
 * tool's handle_cancel: ONE mutation path via job_manager_cancel_or_retire.
 * ============================================================================= */

int handle_research_cancel_cmd(int client_fd, const char *payload, uint16_t payload_len) {
   if (!payload || payload_len < 12) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid research cancel payload.");
   }
   const unsigned char *p = (const unsigned char *)payload;
   int user_id = (int)rd_i32le(p);
   int64_t run_id = rd_i64le(p + 4);
   if (user_id <= 0 || run_id <= 0) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "user_id and run_id are required.");
   }

   research_run_t run;
   if (research_db_run_get(run_id, user_id, &run) != AUTH_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_NOT_FOUND, "No such research run.");
   }
   if (strcmp(run.status, "done") == 0 || strcmp(run.status, "failed") == 0 ||
       strcmp(run.status, "cancelled") == 0) {
      char msg[96];
      snprintf(msg, sizeof(msg), "Run #%lld already finished (%s).", (long long)run.id, run.status);
      return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
   }

   char jstatus[JOB_STATUS_MAX];
   char msg[160];
   switch (job_manager_cancel_or_retire(run.conversation_id, user_id, jstatus, sizeof(jstatus))) {
      case JOB_CANCEL_SIGNALLED:
         OLOG_INFO("research: admin cancelled run #%lld (user %d)", (long long)run.id, user_id);
         snprintf(msg, sizeof(msg), "Cancelling run #%lld — it'll stop after the current round.",
                  (long long)run.id);
         return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
      case JOB_CANCEL_RETIRED:
         /* Queued row retired before it ran; the worker never runs, so mirror the
          * cancel onto the research header (the retire touches only the job row). */
         research_db_run_set_terminal(run.id, "cancelled", "cancelled", time(NULL));
         OLOG_INFO("research: admin retired queued run #%lld (user %d)", (long long)run.id,
                   user_id);
         snprintf(msg, sizeof(msg), "Cancelled run #%lld before it started.", (long long)run.id);
         return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
      case JOB_CANCEL_ALREADY_TERMINAL:
         snprintf(msg, sizeof(msg), "Run #%lld already finished (%s).", (long long)run.id, jstatus);
         return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
      case JOB_CANCEL_FORBIDDEN:
      case JOB_CANCEL_NOT_FOUND:
         break; /* one answer for both — no id oracle */
   }
   /* No default: -Wswitch flags a new enumerator instead of silently mislabeling. */
   return send_text_response(client_fd, ADMIN_RESP_NOT_FOUND, "No such research run.");
}

#endif /* DAWN_ENABLE_DEEP_RESEARCH_TOOL */
