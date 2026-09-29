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
 * Admin-socket memory-maintenance handlers (dawn-admin memory *).  Five
 * synchronous handlers backed by the memory subsystem's admin layer:
 * recategorize, cleanup_meta_facts, summarize_missing, reextract,
 * reextract_status.  Split out of admin_socket.c so that file can stay
 * under the size limits in CLAUDE.md.  Sibling of
 * admin_socket_memory_entity.c (which owns the entity-merge handlers).
 * Dispatched from handle_client() in admin_socket.c via declarations in
 * include/auth/admin_socket_internal.h.
 */

#define _GNU_SOURCE
#define ADMIN_SOCKET_INTERNAL_ALLOWED

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "auth/admin_socket_internal.h"
#include "auth/auth_db.h"
#include "dawn_error.h"
#include "memory/memory_db.h"
#include "memory/memory_db_admin.h"
#include "memory/memory_extraction.h"
#include "memory/memory_note_bridge.h"
#include "memory/memory_recategorize.h"
#include "memory/memory_summarize_missing.h"
#include "tools/document_db.h"

int handle_memory_recategorize(int client_fd, const char *payload, uint16_t payload_len) {
   if (payload_len == 0 || payload_len >= AUTH_USERNAME_MAX) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid username");
   }

   char username[AUTH_USERNAME_MAX];
   memcpy(username, payload, payload_len);
   username[payload_len] = '\0';

   auth_user_t user;
   if (auth_db_get_user(username, &user) != AUTH_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "User not found");
   }

   if (memory_recategorize_is_running()) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Recategorization already running");
   }

   int count = 0;
   if (memory_db_fact_count_general(user.id, &count) != MEMORY_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR,
                                "Failed to query general facts");
   }
   if (count == 0) {
      return send_text_response(client_fd, ADMIN_RESP_SUCCESS, "No general facts to recategorize");
   }

   if (memory_recategorize_start(user.id) != 0) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR,
                                "Failed to start recategorization thread");
   }

   char msg[256];
   snprintf(msg, sizeof(msg), "Recategorization started for user %s (%d general facts)", username,
            count);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
}

/* =============================================================================
 * Memory cleanup_meta_facts — bulk-delete interaction-event "meta-facts"
 * pre-existing in the DB before the May 2026 prompt fix that blocks them
 * at extraction time.
 *
 * Payload wire format:
 *   Byte 0:        flags (bit 0 = ADMIN_MEM_CLEANUP_FLAG_DRY_RUN)
 *   Byte 1..:      username string (length = payload_len - 1)
 *
 * Default LIKE patterns are hardcoded server-side — `User asked%`,
 * `User inquired%`, `User requested%`, `user asked%`, `user inquired%`,
 * `user requested%` (lowercase forms cover historical extraction-prompt
 * variations).  Custom-pattern injection from the operator deferred until
 * the use case appears.
 *
 * ADMIN_MEM_CLEANUP_FLAG_DRY_RUN is the wire-protocol flag bit; defined in
 * include/auth/admin_socket.h alongside the other ADMIN_MSG_MEMORY_* flags.
 * ============================================================================= */

int handle_memory_cleanup_meta_facts(int client_fd, const char *payload, uint16_t payload_len) {
   if (payload_len < 2 || payload_len >= AUTH_USERNAME_MAX + 1) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid cleanup payload");
   }
   uint8_t flags = (uint8_t)payload[0];
   bool dry_run = (flags & ADMIN_MEM_CLEANUP_FLAG_DRY_RUN) != 0;

   uint16_t uname_len = payload_len - 1;
   if (uname_len == 0 || uname_len >= AUTH_USERNAME_MAX) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid username");
   }
   char username[AUTH_USERNAME_MAX];
   memcpy(username, payload + 1, uname_len);
   username[uname_len] = '\0';

   auth_user_t user;
   if (auth_db_get_user(username, &user) != AUTH_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "User not found");
   }

   /* Default meta-fact pattern set.  Mixed-case variants cover historical
    * extraction-prompt drift (the prompt has flipped between "User" and
    * "user" leading-cap over months of edits). */
   static const char *const k_default_patterns[] = {
      "User asked%",     "User inquired%", "User requested%", "User wanted to know%",
      "User looked up%", "user asked%",    "user inquired%",  "user requested%",
   };
   const int n_patterns = (int)(sizeof(k_default_patterns) / sizeof(k_default_patterns[0]));

   int matched = 0;
   /* not-a-removal: an operator's cleanup of extraction noise, not the user forgetting */
   int rc = memory_db_facts_delete_by_patterns(user.id, k_default_patterns, n_patterns, dry_run,
                                               &matched);
   if (rc != MEMORY_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR,
                                "cleanup_meta_facts query failed");
   }

   char msg[512];
   if (dry_run) {
      snprintf(msg, sizeof(msg),
               "Dry run: %d meta-fact rows match for user '%s'.\n"
               "Patterns: User asked%%, User inquired%%, User requested%%, User wanted to "
               "know%%, User looked up%%, plus lowercase variants.\n"
               "Re-run without --dry-run (--confirm-delete) to delete.",
               matched, username);
   } else {
      snprintf(msg, sizeof(msg), "Deleted %d meta-fact rows for user '%s'.", matched, username);
   }
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
}

/* =============================================================================
 * Memory summarize-missing — backfill summary rows for conversations whose
 * extraction never produced one.  Mirrors the recategorize pattern: dry-run
 * preview returns the pending count, --confirm spawns a background worker
 * that loops over the missing-summary set, runs the canonical extraction
 * prompt per conversation, and stores summary+topics only.
 *
 * Payload wire format (matches the recategorize/cleanup pattern):
 *   Byte 0:        flags (bit 0 = ADMIN_MEM_SUMMARIZE_FLAG_DRY_RUN)
 *   Byte 1..4:     max_count (uint32_t little-endian; 0 = unlimited)
 *   Byte 5..:      username string (length = payload_len - 5)
 * ============================================================================= */

int handle_memory_summarize_missing(int client_fd, const char *payload, uint16_t payload_len) {
   /* Minimum: 1 (flags) + 4 (max_count) + 1 (username char) = 6 bytes. */
   if (payload_len < 6) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid summarize-missing payload");
   }
   uint8_t flags = (uint8_t)payload[0];
   bool dry_run = (flags & ADMIN_MEM_SUMMARIZE_FLAG_DRY_RUN) != 0;

   uint32_t max_count = 0;
   memcpy(&max_count, payload + 1, sizeof(max_count));

   uint16_t uname_len = payload_len - 5;
   if (uname_len == 0 || uname_len >= AUTH_USERNAME_MAX) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid username");
   }
   char username[AUTH_USERNAME_MAX];
   memcpy(username, payload + 5, uname_len);
   username[uname_len] = '\0';

   auth_user_t user;
   if (auth_db_get_user(username, &user) != AUTH_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "User not found");
   }

   int pending = 0;
   if (memory_summarize_missing_count(user.id, &pending) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR,
                                "Failed to count missing summaries");
   }
   if (pending == 0) {
      return send_text_response(client_fd, ADMIN_RESP_SUCCESS,
                                "No conversations missing summaries");
   }

   if (dry_run) {
      char msg[256];
      if (max_count > 0) {
         snprintf(msg, sizeof(msg),
                  "Dry run: %d conversations missing summaries for user '%s' "
                  "(would process up to %u).  Re-run with --confirm to start.",
                  pending, username, max_count);
      } else {
         snprintf(msg, sizeof(msg),
                  "Dry run: %d conversations missing summaries for user '%s'.  "
                  "Re-run with --confirm to start.",
                  pending, username);
      }
      return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
   }

   if (memory_summarize_missing_is_running()) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Summarize-missing already running");
   }

   if (memory_summarize_missing_start(user.id, max_count) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR,
                                "Failed to start summarize-missing worker");
   }

   char msg[256];
   if (max_count > 0) {
      snprintf(msg, sizeof(msg),
               "Summarize-missing started for user '%s': %d pending, processing up to %u "
               "(progress in daemon logs).",
               username, pending, max_count);
   } else {
      snprintf(msg, sizeof(msg),
               "Summarize-missing started for user '%s': %d pending "
               "(progress in daemon logs).",
               username, pending);
   }
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
}

/* =============================================================================
 * Memory reextract — drop derived memory tables + reset extraction
 * cursors + spawn recovery worker.  See memory_db_admin.{c,h}.
 * ============================================================================= */

/* Decode the binary reextract payload into stack-resident outputs.
 * Returns SUCCESS on a well-formed payload, FAILURE otherwise. */
static int decode_reextract_payload(
    const char *payload,
    uint16_t payload_len,
    uint8_t *out_flags,
    char *out_backup_path, /* size >= ADMIN_REEXTRACT_BACKUP_PATH_MAX + 1 */
    uint16_t *out_backup_path_len,
    uint32_t *out_max_cost_micros,
    char *out_username, /* size >= ADMIN_REEXTRACT_USERNAME_MAX + 1 */
    uint8_t *out_username_len) {
   /* Minimum payload: flags (1) + backup_path_len (2) + max_cost (4) + uname_len (1)
    * + 1 byte username = 9 bytes. */
   if (payload_len < 9)
      return FAILURE;

   size_t off = 0;
   *out_flags = (uint8_t)payload[off++];

   uint16_t bplen;
   memcpy(&bplen, payload + off, sizeof(bplen));
   off += sizeof(bplen);
   if (bplen > ADMIN_REEXTRACT_BACKUP_PATH_MAX)
      return FAILURE;
   if (payload_len < off + bplen + 4 + 1)
      return FAILURE;
   if (bplen > 0) {
      memcpy(out_backup_path, payload + off, bplen);
      off += bplen;
   }
   out_backup_path[bplen] = '\0';
   *out_backup_path_len = bplen;

   uint32_t cap;
   memcpy(&cap, payload + off, sizeof(cap));
   off += sizeof(cap);
   *out_max_cost_micros = cap;

   uint8_t ulen = (uint8_t)payload[off++];
   if (ulen == 0 || ulen > ADMIN_REEXTRACT_USERNAME_MAX)
      return FAILURE;
   if (payload_len < off + ulen)
      return FAILURE;
   memcpy(out_username, payload + off, ulen);
   out_username[ulen] = '\0';
   *out_username_len = ulen;

   /* Reject embedded NUL — username travels as raw bytes on the wire,
    * but server-side it must be a valid C-string. */
   for (uint8_t i = 0; i < ulen; i++) {
      if (out_username[i] == '\0')
         return FAILURE;
   }

   return SUCCESS;
}

/* Synthesize the default backup path when the client passed an empty one.
 * /var/lib/dawn/auth.db.reextract.YYYYMMDD-HHMMSSZ — fits inside
 * ADMIN_REEXTRACT_BACKUP_PATH_MAX with margin.  UTC instead of local
 * time so DST fall-back can't produce identical filenames an hour
 * apart, and so the timestamp matches audit-log conventions. */
static void default_backup_path(char *out, size_t out_size) {
   time_t now = time(NULL);
   struct tm tm_buf;
   gmtime_r(&now, &tm_buf);
   snprintf(out, out_size, "/var/lib/dawn/auth.db.reextract.%04d%02d%02d-%02d%02d%02dZ",
            tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min,
            tm_buf.tm_sec);
}

int handle_memory_reextract(int client_fd, const char *payload, uint16_t payload_len) {
   uint8_t flags = 0;
   char backup_path[ADMIN_REEXTRACT_BACKUP_PATH_MAX + 1] = { 0 };
   uint16_t backup_path_len = 0;
   uint32_t max_cost_micros = 0;
   char username[ADMIN_REEXTRACT_USERNAME_MAX + 1] = { 0 };
   uint8_t username_len = 0;

   if (decode_reextract_payload(payload, payload_len, &flags, backup_path, &backup_path_len,
                                &max_cost_micros, username, &username_len) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Malformed reextract payload");
   }

   bool confirm = (flags & ADMIN_REEXTRACT_FLAG_CONFIRM) != 0;
   bool keep_summaries = (flags & ADMIN_REEXTRACT_FLAG_KEEP_SUMMARIES) != 0;

   auth_user_t user;
   if (auth_db_get_user(username, &user) != AUTH_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_NOT_FOUND, "User not found");
   }

   /* Cost estimate doubles as the dry-run report; always run it. */
   memory_db_admin_cost_estimate_t est;
   if (memory_db_admin_estimate_reextract_cost(user.id, &est) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "Cost estimate failed");
   }

   if (!confirm) {
      char report[ADMIN_MSG_CONTENT_MAX + 1];
      int n = snprintf(report, sizeof(report),
                       "Reextract plan for user '%s' (id=%d) — DRY RUN\n"
                       "  Backup path:        %s\n"
                       "  Keep summaries:     %s\n"
                       "  Convs to re-extract: %d (msgs total: %d)\n"
                       "  Provider/model:     %s/%s\n"
                       "  Estimated cost:     %s",
                       username, user.id,
                       backup_path_len > 0 ? backup_path
                                           : "(default /var/lib/dawn/auth.db.reextract.<ts>)",
                       keep_summaries ? "yes" : "no", est.conv_count, est.message_count,
                       est.provider[0] ? est.provider : "(unset)",
                       est.model[0] ? est.model : "(unset)",
                       est.rates_known ? "" : "uncosted (local provider or unknown rates)");
      /* Continuation snprintfs need at least ~80 bytes free to write a
       * useful suffix; guard against the n-fills-buffer case where
       * `sizeof(report) - n == 1` would silently truncate to 0 chars. */
      const int kMinContinuation = 80;
      if (est.rates_known && n > 0 && (int)sizeof(report) - n > kMinContinuation) {
         snprintf(report + n, sizeof(report) - (size_t)n,
                  "$%.4f - $%.4f USD ($%.4f - $%.4f / conv)", est.cost_low_usd, est.cost_high_usd,
                  est.per_conv_cost_low_usd, est.per_conv_cost_high_usd);
      }
      if (max_cost_micros > 0) {
         double cap_usd = (double)max_cost_micros / 1e6;
         size_t cur = strlen(report);
         if (cur + (size_t)kMinContinuation < sizeof(report)) {
            snprintf(report + cur, sizeof(report) - cur, "\n  Max cost cap:       $%.4f USD",
                     cap_usd);
         }
         if (!est.rates_known) {
            /* Security review L1: cap is silently ignored when rates unknown.
             * Make the suppression visible in the dry-run report. */
            cur = strlen(report);
            if (cur + (size_t)kMinContinuation < sizeof(report)) {
               snprintf(report + cur, sizeof(report) - cur,
                        "\n  WARNING: --max-cost-usd is NOT enforced (rates unknown for "
                        "%s/%s)",
                        est.provider[0] ? est.provider : "(unset)",
                        est.model[0] ? est.model : "(unset)");
            }
         }
      }
      /* v43: surface alias-graph state that reset_derived will drop, so the
       * operator sees what they're losing before --confirm.  Only printed
       * when there's something to lose; absent rows render nothing. */
      if (est.active_aliases > 0 || est.pending_proposals > 0) {
         size_t cur = strlen(report);
         if (cur + (size_t)kMinContinuation < sizeof(report)) {
            snprintf(report + cur, sizeof(report) - cur,
                     "\n  Aliases to drop:    %d active soft-link%s + %d pending proposal%s",
                     est.active_aliases, est.active_aliases == 1 ? "" : "s", est.pending_proposals,
                     est.pending_proposals == 1 ? "" : "s");
         }
      }
      return send_text_response(client_fd, ADMIN_RESP_SUCCESS, report);
   }

   /* CONFIRMED PATH — concurrency guards. */
   if (memory_recategorize_is_running()) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                "Recategorization is running; refuse to reset");
   }
   if (memory_extraction_in_progress(user.id)) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                "Extraction is in progress for this user; refuse to reset");
   }
   if (memory_db_admin_reextract_is_running()) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                "Another reextract is already running");
   }

   /* Cost cap. */
   if (max_cost_micros > 0 && est.rates_known) {
      double cap_usd = (double)max_cost_micros / 1e6;
      if (est.cost_high_usd > cap_usd) {
         char msg[256];
         snprintf(msg, sizeof(msg), "Aborted: estimated cost $%.4f exceeds cap $%.4f USD",
                  est.cost_high_usd, cap_usd);
         return send_text_response(client_fd, ADMIN_RESP_FAILURE, msg);
      }
   }

   /* Backup path resolution + execution. */
   char actual_backup[ADMIN_REEXTRACT_BACKUP_PATH_MAX + 1];
   if (backup_path_len > 0) {
      memcpy(actual_backup, backup_path, backup_path_len);
      actual_backup[backup_path_len] = '\0';
   } else {
      default_backup_path(actual_backup, sizeof(actual_backup));
   }
   if (auth_db_backup(actual_backup) != AUTH_DB_SUCCESS) {
      char msg[ADMIN_REEXTRACT_BACKUP_PATH_MAX + 64];
      snprintf(msg, sizeof(msg), "Backup failed (path=%s); reset aborted", actual_backup);
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, msg);
   }

   /* Reset transaction. */
   memory_db_admin_reset_counts_t counts = { 0 };
   /* not-a-removal: derived memory rebuilt by re-extraction, not the user forgetting */
   if (memory_db_admin_reset_derived(user.id, keep_summaries, &counts) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR,
                                "Reset transaction failed; backup preserved");
   }

   /* Spawn recovery worker (detached). */
   int spawn_rc = memory_db_admin_start_reextract_worker();

   auth_db_log_event("MEMORY_REEXTRACT", username, NULL, NULL);

   char report[ADMIN_MSG_CONTENT_MAX + 1];
   snprintf(report, sizeof(report),
            "Reextract executed for user '%s' (id=%d)\n"
            "  Backup written:     %s\n"
            "  Facts deleted:      %d\n"
            "  Preferences:        %d\n"
            "  Summaries:          %d%s\n"
            "  Entities:           %d\n"
            "  Relations:          %d\n"
            "  Conversations reset: %d\n"
            "  Recovery worker:    %s",
            username, user.id, actual_backup, counts.facts_deleted, counts.preferences_deleted,
            counts.summaries_deleted, keep_summaries ? " (kept)" : "", counts.entities_deleted,
            counts.relations_deleted, counts.conversations_reset,
            spawn_rc == SUCCESS ? "started"
                                : "FAILED to start (run dawn-admin reextract-status to retry)");
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, report);
}

int handle_memory_reextract_status(int client_fd, const char *payload, uint16_t payload_len) {
   uint8_t flags = 0;
   char backup_path[ADMIN_REEXTRACT_BACKUP_PATH_MAX + 1] = { 0 };
   uint16_t backup_path_len = 0;
   uint32_t max_cost_micros = 0;
   char username[ADMIN_REEXTRACT_USERNAME_MAX + 1] = { 0 };
   uint8_t username_len = 0;

   if (decode_reextract_payload(payload, payload_len, &flags, backup_path, &backup_path_len,
                                &max_cost_micros, username, &username_len) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Malformed status payload");
   }

   auth_user_t user;
   if (auth_db_get_user(username, &user) != AUTH_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_NOT_FOUND, "User not found");
   }

   memory_db_admin_status_t st;
   if (memory_db_admin_query_status(user.id, &st) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "Status query failed");
   }

   char report[ADMIN_MSG_CONTENT_MAX + 1];
   snprintf(report, sizeof(report),
            "Reextract status for user '%s' (id=%d)\n"
            "  Worker running:    %s\n"
            "  Total convs:       %d\n"
            "  Done:              %d\n"
            "  Pending:           %d\n"
            "  Failed (cap-hit):  %d\n"
            "  Cost spent:        %s",
            username, user.id, st.worker_running ? "yes" : "no", st.total_conversations,
            st.conversations_done, st.conversations_pending, st.conversations_failed,
            st.cost_tracking_available ? "(unimplemented)" : "not tracked (carried debt)");
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, report);
}

/* =============================================================================
 * Memory→note bridge gloss backfill (Phase 9) — one-time remediation that
 * creates a bridge gloss for every existing note a user filed before the bridge
 * shipped.  Non-destructive + idempotent: notes that already have a gloss are
 * skipped, so re-running is safe.  Payload: username (no flags).
 * ============================================================================= */
int handle_memory_backfill_note_glosses(int client_fd, const char *payload, uint16_t payload_len) {
   if (payload_len == 0 || payload_len >= AUTH_USERNAME_MAX) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid username");
   }
   char username[AUTH_USERNAME_MAX];
   memcpy(username, payload, payload_len);
   username[payload_len] = '\0';

   auth_user_t user;
   if (auth_db_get_user(username, &user) != AUTH_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "User not found");
   }

   const int batch = 64;
   document_t *notes = malloc(sizeof(document_t) * (size_t)batch);
   if (!notes) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "Out of memory");
   }

   int created = 0, existing = 0, failed = 0, offset = 0;
   for (;;) {
      int count = 0;
      if (document_db_list_filtered(user.id, DOC_KIND_NOTES, notes, batch, offset, &count) !=
          SUCCESS) {
         free(notes);
         return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "Note listing failed");
      }
      for (int i = 0; i < count; i++) {
         /* Glosses are per-owner — only backfill the caller's own notes (the
          * listing also returns global notes that may be owned by others). */
         if (notes[i].user_id != user.id) {
            continue;
         }
         int64_t gloss_id = 0;
         memory_db_fact_find_by_note_doc_id(user.id, notes[i].id, &gloss_id);
         if (gloss_id > 0) {
            existing++;
            continue;
         }
         if (memory_note_bridge_upsert_gloss(user.id, notes[i].id, notes[i].filename) == SUCCESS) {
            created++;
         } else {
            failed++; /* the bridge logged the reason (e.g. label tripped the filter) */
         }
      }
      if (count < batch) {
         break;
      }
      offset += count;
   }
   free(notes);
   notes = NULL;

   char msg[256];
   snprintf(msg, sizeof(msg),
            "Backfilled note glosses for user '%s': %d created, %d already present, %d failed.",
            username, created, existing, failed);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
}

/* =============================================================================
 * Rebuild the document_chunks_fts index (v61) — recovery path for a partial
 * migration backfill or FTS orphans.  Global (the FTS index spans all users), so
 * no payload.  Idempotent: clears and re-indexes every live chunk.
 * ============================================================================= */
int handle_memory_rebuild_document_fts(int client_fd, const char *payload, uint16_t payload_len) {
   (void)payload;
   (void)payload_len;

   int indexed = 0;
   if (document_db_rebuild_fts(&indexed) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR,
                                "Document FTS rebuild failed (is the v61 migration applied?)");
   }

   char msg[128];
   snprintf(msg, sizeof(msg), "Rebuilt document search index: %d chunk(s) indexed.", indexed);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
}
