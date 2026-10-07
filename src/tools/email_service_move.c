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
 * Trash and archive across backends, several messages per call, and their
 * undo.  The single place a move or an undo tells the WebUI what changed
 * (email_changed_notify), so the panel's moves and the tool's are each pushed
 * once.  Gmail calls are paced per account; IMAP holds the account's lease.
 */

#include <pthread.h>
#include <sodium.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "logging.h"
#include "tools/email_client.h"
#include "tools/email_imap_roles.h"
#include "tools/email_parse.h"
#include "tools/email_service.h"
#include "tools/email_service_internal.h"
#include "tools/email_transfer.h"
#include "tools/email_undo.h"
#include "tools/gmail_client.h"
#include "tools/gmail_client_internal.h"
#include "tools/oauth_client.h"

_Static_assert(EMAIL_MOVE_MAX_IDS <= EMAIL_IMAP_GROUP_MAX_IDS &&
                   EMAIL_MOVE_MAX_IDS <= EMAIL_IMAP_MOVE_MAX,
               "a move's ids must fit one IMAP group");

/* =============================================================================
 * The change hook
 * ============================================================================= */

/* Weak no-op: a build without the WebUI has no one to tell. */
__attribute__((weak)) void email_changed_notify(int user_id,
                                                int64_t account_id,
                                                email_move_kind_t kind,
                                                bool undo,
                                                const email_summary_t *created,
                                                int nc,
                                                const char *const *destroyed,
                                                int nd,
                                                bool refresh) {
   (void)user_id;
   (void)account_id;
   (void)kind;
   (void)undo;
   (void)created;
   (void)nc;
   (void)destroyed;
   (void)nd;
   (void)refresh;
}

/* =============================================================================
 * Gmail pacing: at most GMAIL_PACE_PER_SEC calls a second per account, the
 * panel's and the tool's together, so a 50-message move doesn't burst past the
 * per-user rate.  A slot reserved here is a time to go at; the wait happens
 * with no lock held.
 * ============================================================================= */

#define GMAIL_PACE_PER_SEC 40
#define GMAIL_PACE_SLOTS 32

typedef struct {
   int64_t account_id;
   uint64_t next_ns;
} pace_slot_t;

static pace_slot_t s_pace[GMAIL_PACE_SLOTS];
static pthread_mutex_t s_pace_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t mono_ns(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void gmail_pace(int64_t account_id) {
   const uint64_t gap = 1000000000ull / GMAIL_PACE_PER_SEC;
   pthread_mutex_lock(&s_pace_mutex);
   const uint64_t now = mono_ns();
   int slot = -1, idle = 0;
   for (int i = 0; i < GMAIL_PACE_SLOTS; i++) {
      if (s_pace[i].account_id == account_id) {
         slot = i;
         break;
      }
      if (s_pace[i].next_ns < s_pace[idle].next_ns)
         idle = i;
   }
   if (slot < 0) {
      /* The slot that has waited longest: an account idle for a while is
       * owed no wait anyway. */
      slot = idle;
      s_pace[slot].account_id = account_id;
      s_pace[slot].next_ns = 0;
   }
   const uint64_t at = s_pace[slot].next_ns > now ? s_pace[slot].next_ns : now;
   s_pace[slot].next_ns = at + gap;
   pthread_mutex_unlock(&s_pace_mutex);
   if (at > now) {
      const uint64_t wait = at - now;
      struct timespec ts = { .tv_sec = (time_t)(wait / 1000000000ull),
                             .tv_nsec = (long)(wait % 1000000000ull) };
      nanosleep(&ts, NULL);
   }
}

/* =============================================================================
 * Shared
 * ============================================================================= */

/* Who the account is on the wire: an undo made under one server or login
 * never runs under another. */
static uint64_t account_fingerprint(const email_account_t *acct) {
   crypto_generichash_state st;
   unsigned char out[8];
   crypto_generichash_init(&st, NULL, 0, sizeof(out));
   const char *parts[] = { acct->auth_type, acct->imap_server, acct->username,
                           acct->oauth_account_key };
   for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
      crypto_generichash_update(&st, (const unsigned char *)parts[i], strlen(parts[i]) + 1);
   crypto_generichash_update(&st, (const unsigned char *)&acct->imap_port, sizeof(acct->imap_port));
   crypto_generichash_final(&st, out, sizeof(out));
   uint64_t fp = 0;
   memcpy(&fp, out, sizeof(fp));
   return fp;
}

/* A message whose move may have happened without its answer arriving: the
 * client can't be told which way, so it reloads. */
static bool outcome_unknown(email_err_t e) {
   return e == EMAIL_ERR_NOT_REMOVED || e == EMAIL_ERR_OUTCOME_UNKNOWN || e == EMAIL_ERR_TIMEOUT ||
          e == EMAIL_ERR_UNREACHABLE;
}

/* Errors that hold for every message of the account, so the rest needn't try. */
static bool err_ends_call(email_err_t e) {
   return e == EMAIL_ERR_AUTH_FAILED || e == EMAIL_ERR_AUTH_REVOKED ||
          e == EMAIL_ERR_RATE_LIMITED || e == EMAIL_ERR_UNREACHABLE || e == EMAIL_ERR_TIMEOUT;
}

/* The writable account a move or undo names; *err says why not. */
static int resolve_writable(int user_id,
                            const email_target_t *target,
                            email_account_t *acct,
                            email_err_t *err) {
   const int rc = email_svc_resolve(user_id, NULL, target, acct);
   if (rc != EMAIL_RC_OK) {
      *err = email_svc_account_err(rc);
      return rc;
   }
   if (acct->read_only) {
      *err = EMAIL_ERR_READ_ONLY;
      return EMAIL_ACCT_RC_READONLY;
   }
   return EMAIL_RC_OK;
}

/* =============================================================================
 * Moves
 * ============================================================================= */

static void keep_undo(int user_id, const email_undo_rec_t *rec, email_move_result_t *r) {
   if (!email_undo_put(user_id, rec, r->undo))
      r->undo[0] = '\0';
}

static void move_imap(int user_id,
                      const email_account_t *acct,
                      const char *const *ids,
                      int n,
                      email_move_kind_t kind,
                      bool want_undo,
                      email_move_result_t *results,
                      email_err_t *call_err) {
   email_imap_group_t *groups = calloc(EMAIL_IMAP_GROUP_MAX_FOLDERS, sizeof(*groups));
   if (!groups) {
      *call_err = EMAIL_ERR_FAILED; /* every result stays FAILED */
      return;
   }
   int at_group[EMAIL_MOVE_MAX_IDS], at_pos[EMAIL_MOVE_MAX_IDS];
   email_err_t errs[EMAIL_MOVE_MAX_IDS];
   for (int i = 0; i < n; i++)
      errs[i] = EMAIL_ERR_FAILED;
   const int ng = email_svc_group_imap_ids(ids, n, groups, at_group, at_pos, errs);

   email_move_outcome_t outcome[EMAIL_IMAP_GROUP_MAX_FOLDERS][EMAIL_MOVE_MAX_IDS];
   email_err_t gerrs[EMAIL_IMAP_GROUP_MAX_FOLDERS][EMAIL_MOVE_MAX_IDS];
   uint32_t dest_uid[EMAIL_IMAP_GROUP_MAX_FOLDERS][EMAIL_MOVE_MAX_IDS];
   email_imap_move_group_t mg[EMAIL_IMAP_GROUP_MAX_FOLDERS];
   char dest_folder[EMAIL_IMAP_ROLE_FOLDER_MAX] = "";
   for (int g = 0; g < ng; g++) {
      mg[g] = (email_imap_move_group_t){ .folder = groups[g].folder,
                                         .uidvalidity = groups[g].uidvalidity,
                                         .uids = groups[g].uids,
                                         .n = groups[g].count,
                                         .outcome = outcome[g],
                                         .errs = gerrs[g],
                                         .dest_uid = dest_uid[g] };
   }
   if (ng > 0) {
      email_conn_t conn;
      const int crc = email_svc_build_conn(acct, &conn);
      if (crc != EMAIL_SVC_CONN_OK) {
         *call_err = email_svc_conn_err(crc);
         for (int g = 0; g < ng; g++) {
            for (int i = 0; i < mg[g].n; i++) {
               outcome[g][i] = EMAIL_MOVE_FAILED;
               gerrs[g][i] = *call_err;
            }
         }
      } else {
         email_imap_move_batch(&conn, kind, mg, ng, dest_folder, sizeof(dest_folder), call_err);
      }
      sodium_memzero(&conn, sizeof(conn));
   }

   bool token_given[EMAIL_IMAP_GROUP_MAX_FOLDERS][EMAIL_MOVE_MAX_IDS] = { { false } };
   const uint64_t fingerprint = account_fingerprint(acct);
   for (int i = 0; i < n; i++) {
      email_move_result_t *r = &results[i];
      if (at_group[i] < 0) {
         r->err = errs[i];
         continue;
      }
      const int g = at_group[i], p = at_pos[i];
      r->outcome = outcome[g][p];
      r->err = gerrs[g][p];
      if (!email_imap_id_format(mg[g].folder, mg[g].uids[p], mg[g].uidvalidity, r->message_id,
                                sizeof(r->message_id)))
         snprintf(r->message_id, sizeof(r->message_id), "%s", ids[i]);
      if (!want_undo || r->outcome != EMAIL_MOVE_DONE || !dest_uid[g][p] ||
          !mg[g].dest_uidvalidity || token_given[g][p])
         continue;
      token_given[g][p] = true;
      email_undo_rec_t rec = { .account_id = acct->id,
                               .fingerprint = fingerprint,
                               .kind = kind,
                               .imap = true,
                               .dest_uid = dest_uid[g][p],
                               .dest_uidvalidity = mg[g].dest_uidvalidity };
      snprintf(rec.src_folder, sizeof(rec.src_folder), "%s", mg[g].folder);
      snprintf(rec.dest_folder, sizeof(rec.dest_folder), "%s", dest_folder);
      snprintf(rec.message_id, sizeof(rec.message_id), "%s", r->message_id);
      keep_undo(user_id, &rec, r);
   }
   free(groups);
}

static void move_gmail(int user_id,
                       const email_account_t *acct,
                       const char *const *ids,
                       int n,
                       email_move_kind_t kind,
                       bool want_undo,
                       email_move_result_t *results,
                       email_err_t *call_err) {
   char token[OAUTH_TOKEN_BUF_SIZE];
   CURL *curl = NULL;
   if (email_svc_gmail_token_err(acct, token, sizeof(token), call_err) != 0 ||
       !(curl = gmail_create_curl())) {
      for (int i = 0; i < n; i++)
         results[i].err = *call_err;
      sodium_memzero(token, sizeof(token));
      return;
   }
   const atomic_bool *cancel = email_transfer_thread_cancel();
   const uint64_t fingerprint = account_fingerprint(acct);
   for (int i = 0; i < n; i++) {
      email_move_result_t *r = &results[i];
      int dup = -1;
      for (int k = 0; k < i && dup < 0; k++) {
         if (strcmp(ids[k], ids[i]) == 0)
            dup = k;
      }
      if (dup >= 0) {
         r->outcome = results[dup].outcome;
         r->err = results[dup].err;
         continue;
      }
      if (cancel && atomic_load(cancel)) {
         for (int k = i; k < n; k++)
            results[k].err = EMAIL_ERR_CANCELLED;
         break;
      }
      email_transfer_set_cancel(curl, cancel);
      gmail_move_meta_t meta;
      memset(&meta, 0, sizeof(meta));
      /* The labels decide whether the message is already where it's going (or,
       * for an archive, in Trash or Spam) and what an undo may add back. */
      gmail_pace(acct->id);
      if (gmail_move_meta(curl, token, ids[i], &meta, &r->err) != 0) {
         if (err_ends_call(r->err)) {
            for (int k = i + 1; k < n; k++)
               results[k].err = r->err;
            *call_err = r->err;
            break;
         }
         continue;
      }
      if (kind == EMAIL_MOVE_ARCHIVE && (meta.in_trash || meta.in_spam)) {
         /* Archiving from Trash or Spam would need restoring it first; say so
          * rather than report it archived. */
         r->err = EMAIL_ERR_IN_TRASH;
         continue;
      }
      if (kind == EMAIL_MOVE_ARCHIVE ? !meta.in_inbox : meta.in_trash) {
         r->outcome = EMAIL_MOVE_ALREADY_THERE;
         r->err = EMAIL_ERR_NONE;
         continue;
      }
      /* Once the move goes out it finishes: a stopped request mustn't leave its
       * outcome unknown. */
      email_transfer_clear_cancel(curl);
      gmail_pace(acct->id);
      const bool trash = kind == EMAIL_MOVE_TRASH;
      if (gmail_message_post(curl, token, ids[i], trash ? "trash" : "modify",
                             trash ? "{}" : "{\"removeLabelIds\":[\"INBOX\"]}", NULL,
                             &r->err) != 0) {
         if (err_ends_call(r->err)) {
            for (int k = i + 1; k < n; k++)
               results[k].err = r->err;
            *call_err = r->err;
            break;
         }
         continue;
      }
      r->outcome = EMAIL_MOVE_DONE;
      r->err = EMAIL_ERR_NONE;
      if (!want_undo)
         continue;
      if (meta.dropped > 0)
         OLOG_WARNING("gmail: %d label(s) of a trashed message won't come back on undo (too many)",
                      meta.dropped);
      email_undo_rec_t rec = { .account_id = acct->id,
                               .fingerprint = fingerprint,
                               .kind = kind,
                               .imap = false };
      snprintf(rec.message_id, sizeof(rec.message_id), "%s", ids[i]);
      if (trash)
         snprintf(rec.labels, sizeof(rec.labels), "%s", meta.keep);
      keep_undo(user_id, &rec, r);
   }
   curl_easy_cleanup(curl);
   sodium_memzero(token, sizeof(token));
}

int email_service_move(int user_id,
                       const email_target_t *target,
                       const char *const *ids,
                       int n,
                       email_move_kind_t kind,
                       bool want_undo,
                       email_move_result_t *results,
                       email_err_t *err) {
   email_err_t err_local;
   if (!err)
      err = &err_local;
   *err = EMAIL_ERR_INVALID_REQUEST;
   if (!target || !ids || !results || n <= 0 || n > EMAIL_MOVE_MAX_IDS)
      return EMAIL_RC_FAILURE;
   for (int i = 0; i < n; i++) {
      results[i] = (email_move_result_t){ .outcome = EMAIL_MOVE_FAILED, .err = EMAIL_ERR_FAILED };
      if (!ids[i])
         return EMAIL_RC_FAILURE;
      snprintf(results[i].message_id, sizeof(results[i].message_id), "%s", ids[i]);
   }

   email_account_t acct;
   int rc = resolve_writable(user_id, target, &acct, err);
   if (rc != EMAIL_RC_OK) {
      sodium_memzero(&acct, sizeof(acct));
      for (int i = 0; i < n; i++)
         results[i].err = *err;
      return rc;
   }
   *err = EMAIL_ERR_NONE;
   if (email_svc_is_gmail_api(&acct)) {
      move_gmail(user_id, &acct, ids, n, kind, want_undo, results, err);
   } else {
      email_svc_lease_t lease;
      if (email_svc_lease_begin(&acct, target, EMAIL_LEASE_WAIT_SEC, &lease, err) != EMAIL_RC_OK) {
         for (int i = 0; i < n; i++)
            results[i].err = *err;
         sodium_memzero(&acct, sizeof(acct));
         return EMAIL_RC_FAILURE;
      }
      move_imap(user_id, &acct, ids, n, kind, want_undo, results, err);
      email_svc_lease_end(&lease);
   }

   /* What left its folder (or was left marked deleted there), and whether
    * anything is now in two places. */
   const char *destroyed[EMAIL_MOVE_MAX_IDS];
   int nd = 0;
   bool refresh = false;
   for (int i = 0; i < n; i++) {
      if (results[i].outcome == EMAIL_MOVE_DONE || results[i].outcome == EMAIL_MOVE_LEFT_FLAGGED)
         destroyed[nd++] = results[i].message_id;
      refresh |= outcome_unknown(results[i].err);
   }
   if (nd > 0 || refresh)
      email_changed_notify(user_id, acct.id, kind, false, NULL, 0, destroyed, nd, refresh);
   sodium_memzero(&acct, sizeof(acct));
   return *err == EMAIL_ERR_NONE ? EMAIL_RC_OK : EMAIL_RC_FAILURE;
}

/* =============================================================================
 * Undo
 * ============================================================================= */

/* An undo that can never succeed: its token should be finished, not released. */
static bool undo_terminal(email_err_t e) {
   return e == EMAIL_ERR_NONE || e == EMAIL_ERR_NOT_FOUND || e == EMAIL_ERR_FOLDER_MISSING ||
          e == EMAIL_ERR_UNDO_EXPIRED || e == EMAIL_ERR_NOT_REMOVED || e == EMAIL_ERR_READ_ONLY ||
          e == EMAIL_ERR_INVALID_REQUEST;
}

static void stamp_row(email_summary_t *row, const email_account_t *acct) {
   snprintf(row->account_name, sizeof(row->account_name), "%s", acct->name);
   snprintf(row->account_addr, sizeof(row->account_addr), "%s", acct->username);
}

/* IMAP undo groups: one per (where they are, its epoch, where they go back to). */
typedef struct {
   int at; /* the first record of the group */
   uint32_t uids[EMAIL_MOVE_MAX_IDS];
   int recs[EMAIL_MOVE_MAX_IDS];
   int n;
} undo_set_t;

static void undo_imap(const email_account_t *acct,
                      const email_undo_rec_t *recs,
                      int n,
                      email_undo_result_t *results,
                      email_err_t *call_err) {
   undo_set_t *sets = calloc(EMAIL_IMAP_GROUP_MAX_FOLDERS, sizeof(*sets));
   email_summary_t *rows = calloc((size_t)n, sizeof(*rows));
   if (!sets || !rows) {
      free(sets);
      free(rows);
      *call_err = EMAIL_ERR_FAILED; /* every result stays FAILED, to be retried */
      return;
   }
   int ns = 0;
   for (int i = 0; i < n; i++) {
      if (results[i].err != EMAIL_ERR_FAILED)
         continue; /* already answered */
      int s = 0;
      for (; s < ns; s++) {
         const email_undo_rec_t *a = &recs[sets[s].at];
         if (a->dest_uidvalidity == recs[i].dest_uidvalidity &&
             strcmp(a->dest_folder, recs[i].dest_folder) == 0 &&
             strcmp(a->src_folder, recs[i].src_folder) == 0)
            break;
      }
      if (s == ns) {
         if (ns == EMAIL_IMAP_GROUP_MAX_FOLDERS)
            continue; /* too spread out: FAILED, may be retried on its own */
         sets[ns++].at = i;
      }
      sets[s].recs[sets[s].n] = i;
      sets[s].uids[sets[s].n++] = recs[i].dest_uid;
   }

   email_move_outcome_t outcome[EMAIL_IMAP_GROUP_MAX_FOLDERS][EMAIL_MOVE_MAX_IDS];
   email_err_t errs[EMAIL_IMAP_GROUP_MAX_FOLDERS][EMAIL_MOVE_MAX_IDS];
   uint32_t new_uid[EMAIL_IMAP_GROUP_MAX_FOLDERS][EMAIL_MOVE_MAX_IDS];
   bool row_ok[EMAIL_IMAP_GROUP_MAX_FOLDERS][EMAIL_MOVE_MAX_IDS];
   email_imap_undo_group_t ug[EMAIL_IMAP_GROUP_MAX_FOLDERS];
   int row_at = 0;
   for (int s = 0; s < ns; s++) {
      const email_undo_rec_t *a = &recs[sets[s].at];
      ug[s] = (email_imap_undo_group_t){ .dest_folder = a->dest_folder,
                                         .dest_uidvalidity = a->dest_uidvalidity,
                                         .src_folder = a->src_folder,
                                         .uids = sets[s].uids,
                                         .n = sets[s].n,
                                         .outcome = outcome[s],
                                         .errs = errs[s],
                                         .new_uid = new_uid[s],
                                         .rows = rows + row_at,
                                         .row_ok = row_ok[s] };
      row_at += sets[s].n;
   }
   if (ns > 0) {
      email_conn_t conn;
      const int crc = email_svc_build_conn(acct, &conn);
      if (crc != EMAIL_SVC_CONN_OK) {
         *call_err = email_svc_conn_err(crc);
         for (int s = 0; s < ns; s++) {
            for (int k = 0; k < ug[s].n; k++) {
               outcome[s][k] = EMAIL_MOVE_FAILED;
               errs[s][k] = *call_err;
               row_ok[s][k] = false;
            }
         }
      } else {
         email_imap_move_back(&conn, ug, ns, call_err);
      }
      sodium_memzero(&conn, sizeof(conn));
   }
   for (int s = 0; s < ns; s++) {
      for (int k = 0; k < ug[s].n; k++) {
         email_undo_result_t *r = &results[sets[s].recs[k]];
         r->err = outcome[s][k] == EMAIL_MOVE_DONE ? EMAIL_ERR_NONE : errs[s][k];
         if (outcome[s][k] != EMAIL_MOVE_DONE || !row_ok[s][k])
            continue;
         r->row = ug[s].rows[k];
         /* The epoch it's back under is the one COPYUID named. */
         r->row_ok = email_imap_id_format(ug[s].src_folder, new_uid[s][k], ug[s].src_uidvalidity,
                                          r->row.message_id, sizeof(r->row.message_id));
      }
   }
   free(sets);
   free(rows);
}

static void undo_gmail(const email_account_t *acct,
                       const email_undo_rec_t *recs,
                       int n,
                       email_undo_result_t *results,
                       email_err_t *call_err) {
   char token[OAUTH_TOKEN_BUF_SIZE];
   CURL *curl = NULL;
   if (email_svc_gmail_token_err(acct, token, sizeof(token), call_err) != 0 ||
       !(curl = gmail_create_curl())) {
      for (int i = 0; i < n; i++) {
         if (results[i].err == EMAIL_ERR_FAILED)
            results[i].err = *call_err;
      }
      sodium_memzero(token, sizeof(token));
      return;
   }
   const atomic_bool *cancel = email_transfer_thread_cancel();
   for (int i = 0; i < n; i++) {
      email_undo_result_t *r = &results[i];
      const email_undo_rec_t *rec = &recs[i];
      if (r->err != EMAIL_ERR_FAILED)
         continue;
      if (cancel && atomic_load(cancel)) {
         for (int k = i; k < n; k++) {
            if (results[k].err == EMAIL_ERR_FAILED)
               results[k].err = EMAIL_ERR_CANCELLED;
         }
         break;
      }
      email_transfer_clear_cancel(curl);
      char body[EMAIL_UNDO_LABELS_MAX * 2 + 64];
      int prc;
      gmail_pace(acct->id);
      if (rec->kind == EMAIL_MOVE_TRASH) {
         prc = gmail_message_post(curl, token, rec->message_id, "untrash", "{}", NULL, &r->err);
         if (prc == 0 && gmail_labels_add_body(rec->labels, false, body, sizeof(body))) {
            long code = 0;
            email_err_t lerr;
            gmail_pace(acct->id);
            if (gmail_message_post(curl, token, rec->message_id, "modify", body, &code, &lerr) !=
                    0 &&
                code == 400 && gmail_labels_add_body(rec->labels, true, body, sizeof(body))) {
               /* A user label deleted since: the system ones still go back. */
               gmail_pace(acct->id);
               gmail_message_post(curl, token, rec->message_id, "modify", body, NULL, &lerr);
            }
         }
      } else {
         prc = gmail_message_post(curl, token, rec->message_id, "modify",
                                  "{\"addLabelIds\":[\"INBOX\"]}", NULL, &r->err);
      }
      if (prc != 0) {
         if (err_ends_call(r->err)) {
            for (int k = i + 1; k < n; k++) {
               if (results[k].err == EMAIL_ERR_FAILED)
                  results[k].err = r->err;
            }
            *call_err = r->err;
            break;
         }
         continue;
      }
      r->err = EMAIL_ERR_NONE;
      email_err_t rerr;
      gmail_pace(acct->id);
      r->row_ok = gmail_message_row(curl, token, rec->message_id, &r->row, &rerr) == 0;
   }
   curl_easy_cleanup(curl);
   sodium_memzero(token, sizeof(token));
}

/* The push for one kind of undo: rows that came back, the ids that left Trash
 * or Archive (a Gmail message keeps its id, so its row just replaces it), and
 * a reload when a row is missing or an outcome is unknown.  @p created and
 * @p gone are scratch (may be NULL: then a reload). */
static void undo_push(int user_id,
                      const email_account_t *acct,
                      email_move_kind_t kind,
                      const email_undo_rec_t *recs,
                      email_undo_result_t *results,
                      int n,
                      email_summary_t *created,
                      char (*gone)[192]) {
   const char *destroyed[EMAIL_MOVE_MAX_IDS];
   int nc = 0, nd = 0;
   bool refresh = !created || !gone, any = false;
   for (int i = 0; i < n; i++) {
      email_undo_result_t *r = &results[i];
      if (recs[i].kind != kind)
         continue;
      any = true;
      if (r->err != EMAIL_ERR_NONE) {
         refresh |= outcome_unknown(r->err);
         continue;
      }
      if (r->row_ok) {
         stamp_row(&r->row, acct);
         if (created)
            created[nc++] = r->row;
      } else {
         refresh = true;
      }
      if (recs[i].imap && gone &&
          email_imap_id_format(recs[i].dest_folder, recs[i].dest_uid, recs[i].dest_uidvalidity,
                               gone[nd], sizeof(gone[nd]))) {
         destroyed[nd] = gone[nd];
         nd++;
      }
   }
   if (any && (nc > 0 || nd > 0 || refresh))
      email_changed_notify(user_id, acct->id, kind, true, created, nc, destroyed, nd, refresh);
}

int email_service_undo(int user_id,
                       const email_target_t *target,
                       const email_undo_rec_t *recs,
                       int n,
                       email_undo_result_t *results,
                       email_err_t *err) {
   email_err_t err_local;
   if (!err)
      err = &err_local;
   *err = EMAIL_ERR_INVALID_REQUEST;
   if (!target || !recs || !results || n <= 0 || n > EMAIL_MOVE_MAX_IDS)
      return EMAIL_RC_FAILURE;
   for (int i = 0; i < n; i++)
      results[i] = (email_undo_result_t){ .err = EMAIL_ERR_FAILED };

   email_account_t acct;
   const int arc = resolve_writable(user_id, target, &acct, err);
   if (arc != EMAIL_RC_OK) {
      sodium_memzero(&acct, sizeof(acct));
      /* A database hiccup may pass (the tokens go back); an account gone,
       * disabled or read-only means these can never run. */
      const bool transient = arc == EMAIL_RC_FAILURE;
      *err = transient ? EMAIL_ERR_FAILED : EMAIL_ERR_UNDO_EXPIRED;
      for (int i = 0; i < n; i++) {
         results[i].err = *err;
         results[i].retry = transient;
      }
      return EMAIL_RC_FAILURE;
   }
   email_svc_lease_t lease;
   if (email_svc_lease_begin(&acct, target, EMAIL_LEASE_WAIT_SEC, &lease, err) != EMAIL_RC_OK) {
      sodium_memzero(&acct, sizeof(acct));
      for (int i = 0; i < n; i++) {
         results[i].err = *err;
         results[i].retry = true;
      }
      return EMAIL_RC_FAILURE;
   }
   /* The wait may have been long: the account as it is now, under the lease. */
   email_account_t now_acct = { 0 };
   const int nrc = resolve_writable(user_id, target, &now_acct, err);
   if (nrc == EMAIL_RC_FAILURE) {
      /* A transient lookup failure: the tokens go back for another try. */
      email_svc_lease_end(&lease);
      sodium_memzero(&acct, sizeof(acct));
      sodium_memzero(&now_acct, sizeof(now_acct));
      *err = EMAIL_ERR_FAILED;
      for (int i = 0; i < n; i++) {
         results[i].err = EMAIL_ERR_FAILED;
         results[i].retry = true;
      }
      return EMAIL_RC_FAILURE;
   }
   const bool same = nrc == EMAIL_RC_OK &&
                     account_fingerprint(&now_acct) == account_fingerprint(&acct);
   const bool imap = !email_svc_is_gmail_api(&now_acct);
   const uint64_t fp = account_fingerprint(&now_acct);
   for (int i = 0; i < n; i++) {
      if (!same || recs[i].account_id != now_acct.id || recs[i].fingerprint != fp ||
          recs[i].imap != imap)
         results[i].err = EMAIL_ERR_UNDO_EXPIRED;
   }
   /* The account changed under them: say so in the call's result too. */
   *err = same ? EMAIL_ERR_NONE : EMAIL_ERR_UNDO_EXPIRED;
   if (same) {
      if (imap)
         undo_imap(&now_acct, recs, n, results, err);
      else
         undo_gmail(&now_acct, recs, n, results, err);
   }
   email_svc_lease_end(&lease);

   for (int i = 0; i < n; i++)
      results[i].retry = !undo_terminal(results[i].err);
   /* What reappeared, what left Trash or Archive, and whether any row is
    * missing: one push per kind of move undone. */
   email_summary_t *created = calloc((size_t)n, sizeof(*created));
   char(*gone)[192] = calloc((size_t)n, sizeof(*gone));
   for (int kk = 0; kk < 2; kk++) {
      const email_move_kind_t kind = kk == 0 ? EMAIL_MOVE_TRASH : EMAIL_MOVE_ARCHIVE;
      undo_push(user_id, &now_acct, kind, recs, results, n, created, gone);
   }
   free(created);
   free(gone);
   sodium_memzero(&acct, sizeof(acct));
   sodium_memzero(&now_acct, sizeof(now_acct));
   return *err == EMAIL_ERR_NONE ? EMAIL_RC_OK : EMAIL_RC_FAILURE;
}
