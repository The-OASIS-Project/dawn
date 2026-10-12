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
 * Phase 6.5 entity-merge admin handlers (dawn-admin memory entity *).  Six
 * synchronous handlers backed by memory_db_alias.c, plus shared payload
 * decode and username→user_id resolution helpers.  Split out of
 * admin_socket.c on 2026-05-12 because that file exceeded the 2500-line
 * hard limit; Phase 2 will add an auto-merge gate that lands here.
 *
 * Opcode range: 0x83-0x88 (ADMIN_MSG_MEMORY_ENTITY_*).  Dispatched from
 * handle_client() in admin_socket.c via declarations in
 * include/auth/admin_socket_internal.h.
 */

#define _GNU_SOURCE
#define ADMIN_SOCKET_INTERNAL_ALLOWED

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/admin_socket_internal.h"
#include "auth/auth_db.h"
#include "dawn_error.h"
#include "memory/memory_db.h"
#include "memory/memory_db_aliases.h"
#include "memory/memory_db_entities.h"

/* Decode the shared entity payload.  Returns SUCCESS on a well-formed
 * payload (out_username + out_reason are NUL-terminated copies that fit in
 * caller-supplied buffers); FAILURE on truncation or invalid lengths. */
static int decode_entity_payload(const char *payload,
                                 uint16_t payload_len,
                                 uint8_t *out_flags,
                                 int64_t *out_arg1,
                                 int64_t *out_arg2,
                                 char *out_username,
                                 size_t username_buf_size,
                                 char *out_reason,
                                 size_t reason_buf_size) {
   if (!payload || payload_len < sizeof(admin_memory_entity_payload_t)) {
      return FAILURE;
   }
   const admin_memory_entity_payload_t *pl = (const admin_memory_entity_payload_t *)payload;
   if (pl->username_len == 0 || pl->username_len > ADMIN_MEM_ENTITY_USERNAME_MAX ||
       pl->reason_len > ADMIN_MEM_ENTITY_REASON_MAX) {
      return FAILURE;
   }
   uint16_t expected = (uint16_t)(sizeof(admin_memory_entity_payload_t) + pl->username_len +
                                  pl->reason_len);
   if (payload_len < expected) {
      return FAILURE;
   }
   if (out_flags)
      *out_flags = pl->flags;
   if (out_arg1)
      *out_arg1 = pl->arg1;
   if (out_arg2)
      *out_arg2 = pl->arg2;

   const char *u_start = payload + sizeof(admin_memory_entity_payload_t);
   if (out_username && username_buf_size > 0) {
      size_t copy = pl->username_len < (username_buf_size - 1) ? pl->username_len
                                                               : (username_buf_size - 1);
      memcpy(out_username, u_start, copy);
      out_username[copy] = '\0';
   }
   const char *r_start = u_start + pl->username_len;
   if (out_reason && reason_buf_size > 0) {
      size_t copy = pl->reason_len < (reason_buf_size - 1) ? pl->reason_len : (reason_buf_size - 1);
      memcpy(out_reason, r_start, copy);
      out_reason[copy] = '\0';
   }
   return SUCCESS;
}

/* Resolve username → user_id; sends NOT_FOUND on miss.  Returns 0 on
 * SUCCESS (out_user populated), 1 on failure (response already sent). */
static int resolve_username_to_user(int client_fd, const char *username, auth_user_t *out_user) {
   if (!username || !*username) {
      send_text_response(client_fd, ADMIN_RESP_FAILURE, "Username required");
      return 1;
   }
   if (auth_db_get_user(username, out_user) != AUTH_DB_SUCCESS) {
      send_text_response(client_fd, ADMIN_RESP_NOT_FOUND, "User not found");
      return 1;
   }
   return 0;
}

int handle_memory_entity_merge(int client_fd, const char *payload, uint16_t payload_len) {
   uint8_t flags = 0;
   int64_t source_id = 0, target_id = 0;
   char username[ADMIN_MEM_ENTITY_USERNAME_MAX + 1] = { 0 };
   char reason[ADMIN_MEM_ENTITY_REASON_MAX + 1] = { 0 };
   if (decode_entity_payload(payload, payload_len, &flags, &source_id, &target_id, username,
                             sizeof(username), reason, sizeof(reason)) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Malformed entity payload");
   }
   if (source_id <= 0 || target_id <= 0 || source_id == target_id) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid source/target ids");
   }

   auth_user_t user;
   if (resolve_username_to_user(client_fd, username, &user) != 0)
      return 1;

   const char *r = (reason[0] ? reason : "operator");
   int64_t link_id = 0;
   int rc = memory_db_entity_alias_link(user.id, source_id, target_id, "soft", r, -1.0f, NULL,
                                        &link_id);
   if (rc != MEMORY_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                "Merge failed (entity not found, has dependents, or self-link)");
   }
   char report[256];
   snprintf(report, sizeof(report), "Linked entity %lld → %lld as soft alias (link_id=%lld).",
            (long long)source_id, (long long)target_id, (long long)link_id);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, report);
}

int handle_memory_entity_split(int client_fd, const char *payload, uint16_t payload_len) {
   uint8_t flags = 0;
   int64_t link_id = 0, unused = 0;
   char username[ADMIN_MEM_ENTITY_USERNAME_MAX + 1] = { 0 };
   char reason[ADMIN_MEM_ENTITY_REASON_MAX + 1] = { 0 };
   if (decode_entity_payload(payload, payload_len, &flags, &link_id, &unused, username,
                             sizeof(username), reason, sizeof(reason)) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Malformed entity payload");
   }
   if (link_id <= 0) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid link_id");
   }
   auth_user_t user;
   if (resolve_username_to_user(client_fd, username, &user) != 0)
      return 1;

   const char *r = (reason[0] ? reason : "split-by-operator");
   int rc = memory_db_entity_alias_unlink(user.id, link_id, r);
   if (rc == MEMORY_DB_NOT_FOUND) {
      return send_text_response(client_fd, ADMIN_RESP_NOT_FOUND,
                                "Link not found or already unlinked");
   }
   if (rc != MEMORY_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                "Split failed (hard merge — use 'dawn-admin memory reextract')");
   }
   char report[160];
   snprintf(report, sizeof(report), "Split link %lld (reason=%s).", (long long)link_id, r);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, report);
}

int handle_memory_entity_aliases(int client_fd, const char *payload, uint16_t payload_len) {
   uint8_t flags = 0;
   int64_t entity_id = 0, unused = 0;
   char username[ADMIN_MEM_ENTITY_USERNAME_MAX + 1] = { 0 };
   if (decode_entity_payload(payload, payload_len, &flags, &entity_id, &unused, username,
                             sizeof(username), NULL, 0) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Malformed entity payload");
   }
   if (entity_id <= 0) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid entity_id");
   }
   auth_user_t user;
   if (resolve_username_to_user(client_fd, username, &user) != 0)
      return 1;

   memory_alias_listing_row_t rows[64];
   int count = 0;
   if (memory_db_entity_alias_list(user.id, entity_id, rows, 64, &count) != MEMORY_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "Alias query failed");
   }

   char report[ADMIN_MSG_CONTENT_MAX + 1];
   int off = snprintf(report, sizeof(report), "Aliases of entity %lld (user '%s'):\n",
                      (long long)entity_id, username);
   for (int i = 0; i < count && off < (int)sizeof(report) - 96; i++) {
      char compbuf[16];
      if (rows[i].composite_score < 0.0f)
         snprintf(compbuf, sizeof(compbuf), "—");
      else
         snprintf(compbuf, sizeof(compbuf), "%.2f", (double)rows[i].composite_score);
      off += snprintf(report + off, sizeof(report) - off,
                      "  [%lld] %s (id=%lld, %s, composite=%s, linked_at=%lld)\n",
                      (long long)rows[i].link_id,
                      rows[i].source_canonical_name[0] ? rows[i].source_canonical_name
                                                       : "(deleted)",
                      (long long)rows[i].source_entity_id, rows[i].link_kind, compbuf,
                      (long long)rows[i].linked_at);
   }
   if (count == 0) {
      snprintf(report, sizeof(report), "No active aliases for entity %lld (user '%s').",
               (long long)entity_id, username);
   }
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, report);
}

int handle_memory_entity_history(int client_fd, const char *payload, uint16_t payload_len) {
   uint8_t flags = 0;
   int64_t entity_id = 0, unused = 0;
   char username[ADMIN_MEM_ENTITY_USERNAME_MAX + 1] = { 0 };
   if (decode_entity_payload(payload, payload_len, &flags, &entity_id, &unused, username,
                             sizeof(username), NULL, 0) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Malformed entity payload");
   }
   if (entity_id <= 0) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid entity_id");
   }
   auth_user_t user;
   if (resolve_username_to_user(client_fd, username, &user) != 0)
      return 1;

   memory_alias_history_row_t rows[64];
   int count = 0;
   if (memory_db_entity_alias_history(user.id, entity_id, rows, 64, &count) != MEMORY_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "History query failed");
   }

   char report[ADMIN_MSG_CONTENT_MAX + 1];
   int off = snprintf(report, sizeof(report), "Audit timeline for entity %lld (user '%s'):\n",
                      (long long)entity_id, username);
   for (int i = 0; i < count && off < (int)sizeof(report) - 128; i++) {
      if (rows[i].unlinked_at == 0) {
         off += snprintf(report + off, sizeof(report) - off,
                         "  [%lld] %s → %s (%s, %s) at %lld — active\n", (long long)rows[i].link_id,
                         rows[i].source_canonical_name, rows[i].target_canonical_name,
                         rows[i].link_kind, rows[i].reason, (long long)rows[i].linked_at);
      } else {
         off += snprintf(report + off, sizeof(report) - off,
                         "  [%lld] %s → %s (%s, %s) at %lld — UNLINKED %lld (%s)\n",
                         (long long)rows[i].link_id, rows[i].source_canonical_name,
                         rows[i].target_canonical_name, rows[i].link_kind, rows[i].reason,
                         (long long)rows[i].linked_at, (long long)rows[i].unlinked_at,
                         rows[i].unlink_reason[0] ? rows[i].unlink_reason : "?");
      }
   }
   if (count == 0) {
      snprintf(report, sizeof(report), "No alias history for entity %lld (user '%s').",
               (long long)entity_id, username);
   }
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, report);
}

/* Saturating snprintf accumulator.  Plain `off += snprintf(buf+off, cap-off,
 * ...)` is a foot-gun: snprintf returns the *would-have-written* length
 * (which can exceed the supplied size on truncation), so off can drift past
 * `cap`.  The next iteration then computes `cap - off` as size_t, underflows
 * to ~SIZE_MAX, and snprintf happily writes well past the buffer end on the
 * stack.  This helper clamps `*off` to at most `cap - 1` so the buffer-size
 * argument to subsequent snprintf calls is always non-negative.  Returns
 * the new `*off` value as a convenience for callers that prefer a single
 * expression.  `cap` must include the trailing NUL slot. */
static int report_off_advance(int *off, int written, int cap) {
   if (written < 0)
      return *off; /* encoding error — leave unchanged */
   if (*off >= cap - 1)
      return *off; /* already saturated */
   if (written >= cap - *off)
      *off = cap - 1; /* truncation: clamp to leave room for NUL only */
   else
      *off += written;
   return *off;
}

/* Cap matched to the typical DAP user — most graphs sit under a few hundred
 * canonicals.  Heap-allocated (~48 KB at 96 B/row) so we do not trip the
 * admin handler's modest stack budget. */
#define ENTITY_LIST_MAX_ROWS 512

int handle_memory_entity_list(int client_fd, const char *payload, uint16_t payload_len) {
   uint8_t flags = 0;
   int64_t unused1 = 0, unused2 = 0;
   char username[ADMIN_MEM_ENTITY_USERNAME_MAX + 1] = { 0 };
   if (decode_entity_payload(payload, payload_len, &flags, &unused1, &unused2, username,
                             sizeof(username), NULL, 0) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Malformed entity payload");
   }
   bool show_aliases = (flags & ADMIN_MEM_ENTITY_FLAG_INCLUDE_ALIASES) != 0;
   auth_user_t user;
   if (resolve_username_to_user(client_fd, username, &user) != 0)
      return 1;

   memory_alias_entity_row_t *rows = calloc(ENTITY_LIST_MAX_ROWS, sizeof(*rows));
   if (!rows) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "List allocation failed");
   }
   int count = 0;
   if (memory_db_entity_list_for_admin(user.id, show_aliases, rows, ENTITY_LIST_MAX_ROWS, &count) !=
       MEMORY_DB_SUCCESS) {
      free(rows);
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "List query failed");
   }

   char report[ADMIN_MSG_CONTENT_MAX + 1];
   int off = 0;
   report_off_advance(&off,
                      snprintf(report, sizeof(report), "Entities for user '%s'%s:\n", username,
                               show_aliases ? " (canonical + aliases)" : " (canonical only)"),
                      (int)sizeof(report));

   /* Aliases live in the contiguous tail (Pass 2 in
    * memory_db_entity_list_for_admin); find the boundary so we can iterate
    * canonicals and inline their aliases without re-querying. */
   int alias_start = count;
   for (int i = 0; i < count; i++) {
      if (rows[i].is_alias) {
         alias_start = i;
         break;
      }
   }

   /* The report buffer is fixed (4 KB).  Reserve ~160 B for the trailing
    * "...truncated" footer so we surface the cut, never silently overflow.
    * Longest possible footer is ~95 B at 4-digit counts ("...truncated:
    * 9999 more canonicals, 9999 more aliases not shown — message buffer
    * full)\n"); 160 leaves comfortable headroom.  All `off` advancement
    * routes through report_off_advance() so a runaway snprintf return
    * cannot push `off` past `sizeof(report) - 1`, which would underflow
    * `sizeof(report) - off` as size_t and overflow the stack buffer. */
   const int kFooterReserve = 160;
   int rendered_canonicals = 0;
   int rendered_aliases = 0;
   bool overflowed = false;

   for (int i = 0; i < alias_start; i++) {
      if (off >= (int)sizeof(report) - kFooterReserve) {
         overflowed = true;
         break;
      }
      report_off_advance(
          &off,
          snprintf(report + off, sizeof(report) - off, "  [%lld] %s (%s, mentions=%d)%s\n",
                   (long long)rows[i].entity_id, rows[i].name[0] ? rows[i].name : "(unnamed)",
                   rows[i].entity_type[0] ? rows[i].entity_type : "?", rows[i].mention_count,
                   rows[i].is_user_self ? " [user-self]" : ""),
          (int)sizeof(report));
      rendered_canonicals++;

      if (show_aliases) {
         /* Aliases are sorted by canonical_id ASC, so all rows for this
          * canonical form a contiguous run within the alias tail. */
         for (int j = alias_start; j < count; j++) {
            if (rows[j].canonical_id != rows[i].entity_id)
               continue;
            if (off >= (int)sizeof(report) - kFooterReserve) {
               overflowed = true;
               break;
            }
            report_off_advance(
                &off,
                snprintf(report + off, sizeof(report) - off,
                         "      ↳ [%lld] %s (%s, mentions=%d) [alias]\n",
                         (long long)rows[j].entity_id, rows[j].name[0] ? rows[j].name : "(unnamed)",
                         rows[j].entity_type[0] ? rows[j].entity_type : "?", rows[j].mention_count),
                (int)sizeof(report));
            rendered_aliases++;
         }
         if (overflowed)
            break;
      }
   }

   /* Orphan aliases — canonical was not in the rendered set (either the
    * canonical lives past max, or its row was overflow-trimmed).  Emit the
    * remaining aliases under a distinct heading so cluster state is visible
    * even on the unhappy path. */
   if (show_aliases && !overflowed) {
      bool emitted_orphan_header = false;
      for (int j = alias_start; j < count; j++) {
         bool found = false;
         for (int i = 0; i < alias_start; i++) {
            if (rows[i].entity_id == rows[j].canonical_id) {
               found = true;
               break;
            }
         }
         if (found)
            continue; /* already rendered nested under the canonical */
         if (off >= (int)sizeof(report) - kFooterReserve) {
            overflowed = true;
            break;
         }
         if (!emitted_orphan_header) {
            report_off_advance(&off,
                               snprintf(report + off, sizeof(report) - off,
                                        "Aliases whose canonical was not in this listing:\n"),
                               (int)sizeof(report));
            emitted_orphan_header = true;
            /* Re-check budget after the header emission — header + alias
             * line together can exceed the reserve in the worst case. */
            if (off >= (int)sizeof(report) - kFooterReserve) {
               overflowed = true;
               break;
            }
         }
         report_off_advance(&off,
                            snprintf(report + off, sizeof(report) - off,
                                     "  [%lld] %s (%s, mentions=%d) → canonical [%lld]\n",
                                     (long long)rows[j].entity_id,
                                     rows[j].name[0] ? rows[j].name : "(unnamed)",
                                     rows[j].entity_type[0] ? rows[j].entity_type : "?",
                                     rows[j].mention_count, (long long)rows[j].canonical_id),
                            (int)sizeof(report));
         rendered_aliases++;
      }
   }

   if (count == 0) {
      snprintf(report, sizeof(report), "No entities for user '%s'.", username);
   } else if (overflowed) {
      int unrendered_canonicals = alias_start - rendered_canonicals;
      int unrendered_aliases = (count - alias_start) - rendered_aliases;
      report_off_advance(
          &off,
          snprintf(report + off, sizeof(report) - off,
                   "  …(truncated: %d more canonical%s, %d more alias%s not shown — message "
                   "buffer full)\n",
                   unrendered_canonicals, unrendered_canonicals == 1 ? "" : "s", unrendered_aliases,
                   unrendered_aliases == 1 ? "" : "es"),
          (int)sizeof(report));
   } else if (count == ENTITY_LIST_MAX_ROWS) {
      report_off_advance(&off,
                         snprintf(report + off, sizeof(report) - off,
                                  "  …(query capped at %d rows — DB has more entities)\n",
                                  ENTITY_LIST_MAX_ROWS),
                         (int)sizeof(report));
   }
   free(rows);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, report);
}

static const char *outcome_label(int outcome) {
   switch (outcome) {
      case MEMORY_ALIAS_OUTCOME_AUTO_MERGED:
         return "Auto-merged";
      case MEMORY_ALIAS_OUTCOME_PROPOSED:
         return "Queued for review";
      case MEMORY_ALIAS_OUTCOME_REJECTED:
         return "Below threshold";
      default:
         return "(no candidates)";
   }
}

int handle_memory_entity_link_user_self(int client_fd, const char *payload, uint16_t payload_len) {
   uint8_t flags = 0;
   int64_t unused1 = 0, unused2 = 0;
   char username[ADMIN_MEM_ENTITY_USERNAME_MAX + 1] = { 0 };
   if (decode_entity_payload(payload, payload_len, &flags, &unused1, &unused2, username,
                             sizeof(username), NULL, 0) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Malformed entity payload");
   }
   bool dry_run = (flags & ADMIN_MEM_ENTITY_FLAG_DRY_RUN) != 0;
   auth_user_t user;
   if (resolve_username_to_user(client_fd, username, &user) != 0)
      return 1;

   /* Heap-allocate the result struct: at MEMORY_ALIAS_LINK_USER_SELF_MAX_ROWS
    * = 256 the struct is ~33 KB on aarch64; combined with the report buffer
    * a stack frame this size is an order of magnitude bigger than typical
    * admin handlers, and Phase 2 lifting MAX_ROWS would push toward 128 KB.
    * calloc + free keeps the frame small. */
   memory_alias_link_user_self_result_t *result = calloc(1, sizeof(*result));
   if (!result) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR,
                                "link-user-self: out of memory");
   }
   int rc = memory_alias_link_user_self_run(user.id, dry_run, result);
   if (rc == MEMORY_DB_NOT_FOUND) {
      free(result);
      return send_text_response(client_fd, ADMIN_RESP_NOT_FOUND, "User not found");
   }
   if (rc == MEMORY_DB_REAL_NAME_REQUIRED) {
      free(result);
      /* Surface a readable hint instead of leaking the raw error code —
       * the operator's next step is to set real_name in the WebUI. */
      return send_text_response(
          client_fd, ADMIN_RESP_FAILURE,
          "link-user-self requires a real_name set. Configure in WebUI Settings → User → Real "
          "name.");
   }
   if (rc == MEMORY_DB_SELF_NAME_COLLISION) {
      char msg[512];
      snprintf(msg, sizeof(msg),
               "link-user-self could not seed: an entity already exists with canonical_name "
               "matching your real_name '%s', and it scored below the self-promotion threshold "
               "(composite < %.2f).  Either promote it manually with "
               "`dawn-admin memory entity merge --user <u> --source <id> --target <id>` then "
               "set is_user_self via the WebUI, or change real_name in WebUI Settings → User "
               "→ Real name to disambiguate.",
               result->self_canonical_name, (double)MEMORY_ALIAS_SELF_PROMOTION_THRESHOLD);
      free(result);
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, msg);
   }
   if (rc != MEMORY_DB_SUCCESS) {
      free(result);
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR,
                                "link-user-self orchestrator failed");
   }

   /* Format the §8 report. */
   char report[ADMIN_MSG_CONTENT_MAX + 1];
   int off = 0;
   off += snprintf(report + off, sizeof(report) - off, "link-user-self for '%s'%s:\n", username,
                   dry_run ? " — DRY RUN" : "");
   if (result->self_was_promoted) {
      /* Existing entity matched the synthetic strongly enough to be promoted
       * to is_user_self=1 (per design §8 Path B step 1).  In dry-run, no
       * UPDATE was issued; in commit mode, the entity now carries the flag. */
      off += snprintf(report + off, sizeof(report) - off,
                      "  user-self canonical: %s (id=%lld) [%s existing match]\n",
                      result->self_canonical_name, (long long)result->self_entity_id,
                      dry_run ? "would promote" : "promoted");
   } else if (result->self_was_seeded) {
      if (result->self_entity_id > 0) {
         off += snprintf(report + off, sizeof(report) - off,
                         "  user-self canonical: %s (id=%lld) [seeded]\n",
                         result->self_canonical_name, (long long)result->self_entity_id);
      } else {
         off += snprintf(report + off, sizeof(report) - off,
                         "  user-self canonical: %s (would create)\n", result->self_canonical_name);
         /* v44 (Phase 1.5 Ckpt B): the synthetic seed pulls its tokens
          * from users.real_name + users.identity_aliases, not from
          * persona_description.  DB-touching signals (relation overlap,
          * contact overlap, embedding cosine) still forfeit to 0 because
          * the synthetic carries id=0 — so the scores are a lower bound
          * on what commit will actually compute against the materialized
          * self. */
         off += snprintf(report + off, sizeof(report) - off,
                         "  Note: synthetic seed built from real_name + aliases "
                         "(scores below are conservative — DB-touching signals like "
                         "embedding cosine and relation overlap forfeit to 0; commit "
                         "will score higher against the materialized self).\n");
      }
   } else {
      off += snprintf(report + off, sizeof(report) - off, "  user-self canonical: %s (id=%lld)\n",
                      result->self_canonical_name, (long long)result->self_entity_id);
   }
   off += snprintf(report + off, sizeof(report) - off,
                   "  Considered: %d  |  Auto-merged: %d  |  Proposed: %d  |  Rejected: %d\n",
                   result->considered, result->auto_merged, result->proposed, result->rejected);

   /* Sort rows by composite_score DESC (mention_count DESC tiebreak) so the
    * dry-run "Below threshold" section surfaces the closest-match candidates
    * first, instead of high-mention-count generic entities like "user" /
    * "dawn" burying the actual cluster members.  In-place sort is fine —
    * the result struct is rendered immediately after this and freed. */
   qsort(result->rows, result->row_count, sizeof(*result->rows),
         memory_alias_row_compare_by_composite_desc);

   /* Render rows in three sections: auto-merged, proposed, rejected (top 10). */
   for (int section = 0; section < 3 && off < (int)sizeof(report) - 128; section++) {
      int target_outcome = (section == 0)   ? MEMORY_ALIAS_OUTCOME_AUTO_MERGED
                           : (section == 1) ? MEMORY_ALIAS_OUTCOME_PROPOSED
                                            : MEMORY_ALIAS_OUTCOME_REJECTED;
      int section_count = 0;
      int max_rows = (target_outcome == MEMORY_ALIAS_OUTCOME_REJECTED) ? 10 : 20;
      for (int i = 0;
           i < result->row_count && section_count < max_rows && off < (int)sizeof(report) - 96;
           i++) {
         if (result->rows[i].outcome != target_outcome)
            continue;
         if (section_count == 0) {
            off += snprintf(report + off, sizeof(report) - off, "  %s:\n",
                            outcome_label(target_outcome));
         }
         off += snprintf(report + off, sizeof(report) - off, "    [%lld] %s (%s, composite=%.2f)\n",
                         (long long)result->rows[i].entity_id, result->rows[i].canonical_name,
                         result->rows[i].entity_type, result->rows[i].composite_score);
         section_count++;
      }
   }
   int sret = send_text_response(client_fd, ADMIN_RESP_SUCCESS, report);
   free(result);
   return sret;
}

/* Facts one 'memory superseded' lists, at most. */
#define ADMIN_SUPERSEDED_MAX 50
/* `memory superseded` rows: characters of a fact's and its keeper's text
 * shown, the rest of a row (ids, arrow, indent), and room kept for the
 * "... N more" line. */
#define SUPERSEDED_TEXT_SHOWN 160
#define SUPERSEDED_KEEPER_SHOWN 120
#define SUPERSEDED_ROW_EXTRA 96
#define SUPERSEDED_MORE_LINE 64
#define ADMIN_SUPERSEDED_DEFAULT 20

int handle_memory_fact_superseded(int client_fd, const char *payload, uint16_t payload_len) {
   uint8_t flags = 0;
   int64_t limit = 0, unused = 0;
   char username[ADMIN_MEM_ENTITY_USERNAME_MAX + 1] = { 0 };
   char filter[ADMIN_MEM_ENTITY_REASON_MAX + 1] = { 0 };
   if (decode_entity_payload(payload, payload_len, &flags, &limit, &unused, username,
                             sizeof(username), filter, sizeof(filter)) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Malformed payload");
   }
   if (limit <= 0) {
      limit = ADMIN_SUPERSEDED_DEFAULT;
   } else if (limit > ADMIN_SUPERSEDED_MAX) {
      limit = ADMIN_SUPERSEDED_MAX;
   }
   auth_user_t user;
   if (resolve_username_to_user(client_fd, username, &user) != 0)
      return 1;

   memory_superseded_fact_t *rows = calloc((size_t)limit, sizeof(*rows));
   if (!rows) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "Out of memory");
   }
   int count = 0;
   if (memory_db_fact_list_superseded(user.id, 0, filter[0] ? filter : NULL, rows, (int)limit,
                                      &count) != MEMORY_DB_SUCCESS) {
      free(rows);
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "Query failed");
   }
   char report[ADMIN_MSG_CONTENT_MAX + 1];
   int off = 0;
   report_off_advance(&off,
                      snprintf(report, sizeof(report),
                               "Superseded facts for user '%s' (newest first; restore with "
                               "'memory restore --user %s --fact-id <id>'):\n",
                               username, username),
                      (int)sizeof(report));
   int shown = 0;
   for (int i = 0; i < count; i++) {
      /* Room for this row and the "more" line, or stop and say so. */
      const size_t text_len = strlen(rows[i].text);
      const size_t keeper_len = strlen(rows[i].keeper_text);
      const int need = (int)(text_len < SUPERSEDED_TEXT_SHOWN ? text_len : SUPERSEDED_TEXT_SHOWN) +
                       (int)(keeper_len < SUPERSEDED_KEEPER_SHOWN ? keeper_len
                                                                  : SUPERSEDED_KEEPER_SHOWN) +
                       SUPERSEDED_ROW_EXTRA;
      if (off + need > (int)sizeof(report) - SUPERSEDED_MORE_LINE) {
         break;
      }
      shown++;
      report_off_advance(&off,
                         snprintf(report + off, sizeof(report) - (size_t)off,
                                  "  [%lld] %.*s\n      → [%lld] %.*s\n", (long long)rows[i].id,
                                  SUPERSEDED_TEXT_SHOWN, rows[i].text, (long long)rows[i].keeper_id,
                                  SUPERSEDED_KEEPER_SHOWN,
                                  rows[i].keeper_text[0] ? rows[i].keeper_text : "(gone)"),
                         (int)sizeof(report));
   }
   free(rows);
   if (shown < count) {
      report_off_advance(&off,
                         snprintf(report + off, sizeof(report) - (size_t)off,
                                  "  ... %d more not shown: narrow with --grep\n", count - shown),
                         (int)sizeof(report));
   }
   if (count == 0) {
      snprintf(report, sizeof(report), "No superseded facts for user '%s'%s.", username,
               filter[0] ? " matching that text" : "");
   }
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, report);
}

int handle_memory_fact_restore(int client_fd, const char *payload, uint16_t payload_len) {
   uint8_t flags = 0;
   int64_t fact_id = 0, unused = 0;
   char username[ADMIN_MEM_ENTITY_USERNAME_MAX + 1] = { 0 };
   if (decode_entity_payload(payload, payload_len, &flags, &fact_id, &unused, username,
                             sizeof(username), NULL, 0) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Malformed payload");
   }
   if (fact_id <= 0) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid fact id");
   }
   auth_user_t user;
   if (resolve_username_to_user(client_fd, username, &user) != 0)
      return 1;
   int reopened = 0;
   int kept_closed = 0;
   const int rc = memory_db_fact_restore(user.id, fact_id, &reopened, &kept_closed);
   if (rc == MEMORY_DB_NOT_FOUND) {
      return send_text_response(client_fd, ADMIN_RESP_NOT_FOUND,
                                "No superseded fact with that id for this user");
   }
   if (rc != MEMORY_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "Restore failed");
   }
   char report[256];
   snprintf(report, sizeof(report), "Restored fact %lld for user '%s' (%d relation(s) reopened%s).",
            (long long)fact_id, username, reopened,
            kept_closed > 0 ? "; the value that replaced it is still current too" : "");
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, report);
}
