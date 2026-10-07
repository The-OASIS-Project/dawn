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
 * Trash and archive over IMAP, several folders' messages on one login, and
 * moving them back (undo).  A move touches only the messages asked for, after
 * checking they exist: UID MOVE when the server has it, else UID COPY + UID
 * STORE \Deleted + UID EXPUNGE of those UIDs.  A bare EXPUNGE, which purges
 * every message marked \Deleted in the folder (another client's kept mail
 * included), is never sent.  Where each message landed comes from COPYUID
 * (RFC 4315), read only while a MOVE or COPY is in flight.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"
#include "tools/email_client.h"
#include "tools/email_client_internal.h"
#include "tools/email_imap_roles.h"
#include "tools/email_imap_state.h"
#include "tools/email_parse.h"
#include "tools/email_transfer.h"

/* The UIDs of @p uids whose @p mask entry is true, as a set; 0 when none. */
static size_t masked_set(const uint32_t *uids,
                         const bool *mask,
                         int n,
                         uint32_t *tmp,
                         char *out,
                         size_t out_size) {
   int k = 0;
   for (int i = 0; i < n; i++) {
      if (mask[i])
         tmp[k++] = uids[i];
   }
   return k ? email_imap_uid_set(tmp, k, out, out_size) : 0;
}

/* COPYUID mapped against only the UIDs actually sent (@p mask), the result
 * scattered back to @p n positions (0 for those not sent). */
static email_copyuid_rc_t map_sent(const email_copyuid_t *cap,
                                   const uint32_t *uids,
                                   const bool *mask,
                                   int n,
                                   uint32_t *dest,
                                   uint32_t *dest_v) {
   uint32_t sent[EMAIL_IMAP_MOVE_MAX], landed[EMAIL_IMAP_MOVE_MAX];
   int k = 0;
   for (int i = 0; i < n; i++) {
      if (mask[i])
         sent[k++] = uids[i];
   }
   const email_copyuid_rc_t m = email_imap_copyuid_map(cap, sent, k, landed, dest_v);
   for (int i = 0, j = 0; i < n; i++)
      dest[i] = mask[i] ? landed[j++] : 0;
   return m;
}

/* The longest UID set one command carries: EMAIL_IMAP_MOVE_MAX ten-digit UIDs. */
#define MOVE_SET_MAX (EMAIL_IMAP_MOVE_MAX * 11 + 1)

/* One command whose reply only says whether it worked.  *first is true while
 * the next command will log in again (a refused or failed one closes libcurl's
 * connection): that one goes through the instrumented perform. */
static CURLcode command(CURL *curl,
                        email_instrument_ctx_t *dctx,
                        const email_conn_t *conn,
                        const char *cmd,
                        bool *first) {
   curl_buffer_t buf;
   const CURLcode res = email_imap_run_command(curl, dctx, conn, "move", cmd, *first, &buf);
   curl_buffer_free(&buf);
   *first = res != CURLE_OK;
   return res;
}

/* Which of @p uids the selected mailbox holds: UID FETCH (UID), read-only.  A
 * UID MOVE or COPY of a UID that doesn't exist is still OK on the wire, so
 * without this a stale id would report a move that never happened.
 * @return CURLE_OK, or why it failed (*gone: the mailbox itself isn't there) */
static CURLcode present_uids(CURL *curl,
                             email_instrument_ctx_t *dctx,
                             const email_conn_t *conn,
                             const char *set,
                             const uint32_t *uids,
                             int n,
                             bool *present,
                             bool *gone,
                             bool *first) {
   char cmd[MOVE_SET_MAX + 32];
   snprintf(cmd, sizeof(cmd), "UID FETCH %s (UID)", set);
   const int logins_before = dctx->login_seen;
   curl_buffer_t buf;
   const CURLcode res = email_imap_run_command(curl, dctx, conn, "move", cmd, *first, &buf);
   *first = res != CURLE_OK;
   *gone = false;
   if (res != CURLE_OK) {
      /* A folder removed or renamed elsewhere, or a mailbox rebuilt since the ids
       * were issued (its pinned SELECT fails): none of these are there. */
      *gone = res == CURLE_REMOTE_FILE_NOT_FOUND ||
              email_imap_select_failed(res == CURLE_LOGIN_DENIED, logins_before, dctx->login_seen);
   } else {
      for (int i = 0; i < n; i++)
         present[i] = buf.data && email_imap_fetch_has_uid(buf.data, uids[i]);
   }
   curl_buffer_free(&buf);
   return res;
}

/* A failure after the command went out (its answer was lost or cut short), as
 * opposed to one that kept it from being sent at all: then whether it ran is
 * unknown. */
static bool answer_lost(CURLcode res) {
   switch (res) {
      case CURLE_COULDNT_RESOLVE_HOST:
      case CURLE_COULDNT_CONNECT:
      case CURLE_LOGIN_DENIED:
      case CURLE_SSL_CONNECT_ERROR:
      case CURLE_PEER_FAILED_VERIFICATION:
      case CURLE_OUT_OF_MEMORY:
         return false;
      default:
         return true;
   }
}

/* Errors that hold for the whole account, not just one folder's messages. */
static bool err_covers_account(email_err_t e) {
   return e == EMAIL_ERR_CANCELLED || e == EMAIL_ERR_UNREACHABLE || e == EMAIL_ERR_TIMEOUT ||
          e == EMAIL_ERR_AUTH_FAILED || e == EMAIL_ERR_AUTH_REVOKED;
}

/* The UIDs @p mask marks (of @p n) moved from the selected mailbox to @p quoted_dest:
 * UID MOVE, else COPY + STORE \Deleted + UID EXPUNGE (no UIDPLUS: left flagged).
 * Fills @p moved (where each landed, 0 unknown) and per-UID results; never cancelled
 * part way (the caller has dropped the cancel).  @return a CURLcode-shaped failure
 * of the first command, CURLE_OK otherwise (per-UID results carry the rest). */
static CURLcode move_set(CURL *curl,
                         email_instrument_ctx_t *dctx,
                         const email_conn_t *conn,
                         const email_imap_roles_t *roles,
                         const char *quoted_dest,
                         const uint32_t *uids,
                         const bool *mask,
                         int n,
                         email_copyuid_t *cap,
                         email_move_outcome_t *outcome,
                         email_err_t *errs,
                         uint32_t *dest,
                         uint32_t *dest_v,
                         bool *first) {
   uint32_t tmp[EMAIL_IMAP_MOVE_MAX];
   char set[MOVE_SET_MAX];
   if (masked_set(uids, mask, n, tmp, set, sizeof(set)) == 0)
      return CURLE_OK;
   char cmd[MOVE_SET_MAX + 2 * EMAIL_IMAP_ROLE_FOLDER_MAX + 32];
   memset(cap, 0, sizeof(*cap));
   uint32_t landed[EMAIL_IMAP_MOVE_MAX];
   *dest_v = 0;

   if (roles->move) {
      snprintf(cmd, sizeof(cmd), "UID MOVE %s %s", set, quoted_dest);
      dctx->copyuid = cap;
      const CURLcode res = command(curl, dctx, conn, cmd, first);
      dctx->copyuid = NULL;
      const email_copyuid_rc_t m = map_sent(cap, uids, mask, n, landed, dest_v);
      if (res != CURLE_OK && res != CURLE_QUOTE_ERROR) {
         /* The answer was lost: whether the move ran is unknown.  The caller still
          * sees res for what holds for the whole account. */
         const email_err_t e = answer_lost(res) ? EMAIL_ERR_OUTCOME_UNKNOWN
                                                : email_err_from_curl(res);
         for (int i = 0; i < n; i++) {
            if (mask[i])
               errs[i] = e;
         }
         return res;
      }
      bool still[EMAIL_IMAP_MOVE_MAX] = { false };
      if (res == CURLE_QUOTE_ERROR) {
         /* Refused.  A server that moves message by message may have moved some:
          * whatever is no longer here went. */
         bool gone = false;
         const CURLcode r2 = present_uids(curl, dctx, conn, set, uids, n, still, &gone, first);
         if (r2 != CURLE_OK) {
            for (int i = 0; i < n; i++) {
               if (mask[i])
                  errs[i] = email_err_from_curl(r2);
            }
            return CURLE_OK;
         }
      }
      for (int i = 0; i < n; i++) {
         if (!mask[i])
            continue;
         if (still[i]) {
            errs[i] = EMAIL_ERR_FAILED;
            continue;
         }
         if (res == CURLE_OK && m == EMAIL_COPYUID_MAPPED && landed[i] == 0) {
            /* The server reports what it moved: not this one (it vanished). */
            errs[i] = EMAIL_ERR_NOT_FOUND;
            continue;
         }
         outcome[i] = EMAIL_MOVE_DONE;
         errs[i] = EMAIL_ERR_NONE;
         dest[i] = m == EMAIL_COPYUID_MAPPED ? landed[i] : 0;
      }
      return CURLE_OK;
   }

   snprintf(cmd, sizeof(cmd), "UID COPY %s %s", set, quoted_dest);
   dctx->copyuid = cap;
   CURLcode res = command(curl, dctx, conn, cmd, first);
   dctx->copyuid = NULL;
   if (res != CURLE_OK) {
      for (int i = 0; i < n; i++) {
         if (mask[i])
            errs[i] = res == CURLE_QUOTE_ERROR ? EMAIL_ERR_FAILED : email_err_from_curl(res);
      }
      return res == CURLE_QUOTE_ERROR ? CURLE_OK : res;
   }
   /* Which were copied: with UIDPLUS the server says (only those may be removed),
    * without it every one sent. */
   bool copied[EMAIL_IMAP_MOVE_MAX];
   email_copyuid_rc_t m = EMAIL_COPYUID_NONE;
   if (roles->uidplus) {
      m = map_sent(cap, uids, mask, n, landed, dest_v);
      if (m != EMAIL_COPYUID_MAPPED) {
         /* Can't tell which were copied (UIDPLUS answers every COPY with
          * COPYUID): remove nothing. */
         for (int i = 0; i < n; i++) {
            if (mask[i])
               errs[i] = EMAIL_ERR_NOT_REMOVED;
         }
         return CURLE_OK;
      }
   }
   for (int i = 0; i < n; i++) {
      copied[i] = mask[i] && (!roles->uidplus || landed[i] != 0);
      if (mask[i] && !copied[i])
         errs[i] = EMAIL_ERR_NOT_FOUND; /* vanished before the copy */
   }
   if (masked_set(uids, copied, n, tmp, set, sizeof(set)) == 0)
      return CURLE_OK;
   snprintf(cmd, sizeof(cmd), "UID STORE %s +FLAGS.SILENT (\\Deleted)", set);
   if (command(curl, dctx, conn, cmd, first) != CURLE_OK) {
      for (int i = 0; i < n; i++) {
         if (copied[i])
            errs[i] = EMAIL_ERR_NOT_REMOVED;
      }
      return CURLE_OK;
   }
   if (!roles->uidplus) {
      /* Without UIDPLUS only a bare EXPUNGE would remove them, and that would
       * also purge every other message marked deleted in the folder. */
      OLOG_WARNING("email_imap: server has no MOVE or UIDPLUS; messages are copied and left "
                   "marked deleted");
      for (int i = 0; i < n; i++) {
         if (copied[i]) {
            outcome[i] = EMAIL_MOVE_LEFT_FLAGGED;
            errs[i] = EMAIL_ERR_NONE;
         }
      }
      return CURLE_OK;
   }
   snprintf(cmd, sizeof(cmd), "UID EXPUNGE %s", set);
   res = command(curl, dctx, conn, cmd, first);
   for (int i = 0; i < n; i++) {
      if (!copied[i])
         continue;
      if (res != CURLE_OK) {
         errs[i] = EMAIL_ERR_NOT_REMOVED;
         continue;
      }
      outcome[i] = EMAIL_MOVE_DONE;
      errs[i] = EMAIL_ERR_NONE;
      dest[i] = landed[i];
   }
   return CURLE_OK;
}

int email_imap_move_batch(const email_conn_t *conn,
                          email_move_kind_t kind,
                          email_imap_move_group_t *groups,
                          int ngroups,
                          char *dest_folder,
                          size_t dest_size,
                          email_err_t *err) {
   email_err_t err_local;
   if (!err)
      err = &err_local;
   *err = EMAIL_ERR_FAILED;
   if (dest_folder && dest_size)
      dest_folder[0] = '\0';
   if (!conn || !groups || ngroups <= 0)
      return 1;
   for (int g = 0; g < ngroups; g++) {
      for (int i = 0; i < groups[g].n; i++) {
         groups[g].outcome[i] = EMAIL_MOVE_FAILED;
         groups[g].errs[i] = EMAIL_ERR_FAILED;
         groups[g].dest_uid[i] = 0;
      }
      groups[g].dest_uidvalidity = 0;
   }
   CURL *curl = email_imap_handle_create(conn);
   email_copyuid_t *cap = calloc(1, sizeof(*cap));
   if (!curl || !cap) {
      free(cap);
      if (curl)
         curl_easy_cleanup(curl);
      return 1;
   }
   email_instrument_ctx_t dctx;
   email_instrument_attach(curl, &dctx);
   const atomic_bool *cancel = email_transfer_thread_cancel();

   email_imap_roles_t roles;
   email_err_t e = email_imap_open_roles(curl, &dctx, conn, &roles);
   bool first = false; /* logged in: the next command reuses the connection */
   const char *dest = kind == EMAIL_MOVE_TRASH ? roles.trash : roles.archive;
   char quoted[EMAIL_IMAP_ROLE_FOLDER_MAX * 2 + 3];
   if (e == EMAIL_ERR_NONE && !dest[0]) {
      OLOG_WARNING("email_imap: no %s folder on this account; nothing moved",
                   kind == EMAIL_MOVE_TRASH ? "Trash" : "Archive");
      email_imap_roles_forget(conn); /* the user may create one; look again next time */
      e = kind == EMAIL_MOVE_TRASH ? EMAIL_ERR_NO_TRASH : EMAIL_ERR_FOLDER_MISSING;
   } else if (e == EMAIL_ERR_NONE && !email_imap_quote_folder(dest, quoted, sizeof(quoted))) {
      e = EMAIL_ERR_FAILED;
   }
   if (e != EMAIL_ERR_NONE) {
      for (int g = 0; g < ngroups; g++) {
         for (int i = 0; i < groups[g].n; i++)
            groups[g].errs[i] = e;
      }
      *err = e;
      goto done;
   }
   if (dest_folder && dest_size)
      snprintf(dest_folder, dest_size, "%s", dest);

   for (int g = 0; g < ngroups; g++) {
      email_imap_move_group_t *grp = &groups[g];
      /* Stopping is honoured between folders only: a folder's move, once
       * started, finishes (half of one would leave a copy in both). */
      if (cancel && atomic_load(cancel)) {
         for (int r = g; r < ngroups; r++) {
            for (int i = 0; i < groups[r].n; i++)
               groups[r].errs[i] = EMAIL_ERR_CANCELLED;
         }
         break;
      }
      email_transfer_set_cancel(curl, cancel);
      if (!grp->folder || !grp->folder[0] || grp->n <= 0 || grp->n > EMAIL_IMAP_MOVE_MAX)
         continue;
      if (strcmp(dest, grp->folder) == 0) {
         for (int i = 0; i < grp->n; i++) {
            grp->outcome[i] = EMAIL_MOVE_ALREADY_THERE;
            grp->errs[i] = EMAIL_ERR_NONE;
         }
         continue;
      }
      char url[EMAIL_IMAP_MAILBOX_URL_MAX];
      char set[MOVE_SET_MAX];
      if (!email_imap_mailbox_url(conn, grp->folder, grp->uidvalidity, url, sizeof(url)) ||
          email_imap_uid_set(grp->uids, grp->n, set, sizeof(set)) == 0)
         continue;
      curl_easy_setopt(curl, CURLOPT_URL, url);
      bool present[EMAIL_IMAP_MOVE_MAX] = { false };
      bool gone = false;
      const CURLcode res = present_uids(curl, &dctx, conn, set, grp->uids, grp->n, present, &gone,
                                        &first);
      if (res != CURLE_OK) {
         const email_err_t ge = gone ? EMAIL_ERR_NOT_FOUND : email_err_from_curl(res);
         for (int i = 0; i < grp->n; i++)
            grp->errs[i] = ge;
         if (err_covers_account(ge)) {
            for (int r = g + 1; r < ngroups; r++) {
               for (int i = 0; i < groups[r].n; i++)
                  groups[r].errs[i] = ge;
            }
            *err = ge;
            goto done;
         }
         continue;
      }
      if (dctx.uidvalidity)
         grp->uidvalidity = dctx.uidvalidity; /* the epoch these ids belong to */
      for (int i = 0; i < grp->n; i++) {
         if (!present[i])
            grp->errs[i] = EMAIL_ERR_NOT_FOUND;
      }
      /* From the first changing command on, this folder's move runs to its end. */
      email_transfer_clear_cancel(curl);
      dctx.last_reject[0] = '\0';
      const CURLcode mres = move_set(curl, &dctx, conn, &roles, quoted, grp->uids, present, grp->n,
                                     cap, grp->outcome, grp->errs, grp->dest_uid,
                                     &grp->dest_uidvalidity, &first);
      if (strstr(dctx.last_reject, "TRYCREATE")) {
         /* The Trash or Archive was removed or renamed elsewhere: the cached
          * role names a dead folder, so forget it and say so. */
         email_imap_roles_forget(conn);
         const email_err_t fe = kind == EMAIL_MOVE_TRASH ? EMAIL_ERR_NO_TRASH
                                                         : EMAIL_ERR_FOLDER_MISSING;
         for (int i = 0; i < grp->n; i++) {
            if (present[i] && grp->outcome[i] == EMAIL_MOVE_FAILED)
               grp->errs[i] = fe;
         }
      }
      if (mres != CURLE_OK && err_covers_account(email_err_from_curl(mres))) {
         const email_err_t ge = email_err_from_curl(mres);
         for (int r = g + 1; r < ngroups; r++) {
            for (int i = 0; i < groups[r].n; i++)
               groups[r].errs[i] = ge;
         }
         *err = ge;
         goto done;
      }
   }
   *err = EMAIL_ERR_NONE;

done:
   email_instrument_op_done(conn->username, "move", curl, &dctx);
   curl_easy_cleanup(curl);
   free(cap);
   return *err == EMAIL_ERR_NONE ? 0 : 1;
}

int email_imap_move_back(const email_conn_t *conn,
                         email_imap_undo_group_t *groups,
                         int ngroups,
                         email_err_t *err) {
   email_err_t err_local;
   if (!err)
      err = &err_local;
   *err = EMAIL_ERR_FAILED;
   if (!conn || !groups || ngroups <= 0)
      return 1;
   for (int g = 0; g < ngroups; g++) {
      for (int i = 0; i < groups[g].n; i++) {
         groups[g].outcome[i] = EMAIL_MOVE_FAILED;
         groups[g].errs[i] = EMAIL_ERR_FAILED;
         groups[g].new_uid[i] = 0;
         groups[g].row_ok[i] = false;
      }
      groups[g].src_uidvalidity = 0;
   }
   CURL *curl = email_imap_handle_create(conn);
   email_copyuid_t *cap = calloc(1, sizeof(*cap));
   email_summary_t *fetched = calloc(EMAIL_IMAP_MOVE_MAX, sizeof(*fetched));
   if (!curl || !cap || !fetched) {
      free(cap);
      free(fetched);
      if (curl)
         curl_easy_cleanup(curl);
      return 1;
   }
   email_instrument_ctx_t dctx;
   email_instrument_attach(curl, &dctx);
   const atomic_bool *cancel = email_transfer_thread_cancel();
   email_imap_roles_t roles;
   const email_err_t se = email_imap_open_roles(curl, &dctx, conn, &roles);
   bool first = false;
   if (se != EMAIL_ERR_NONE) {
      for (int g = 0; g < ngroups; g++) {
         for (int i = 0; i < groups[g].n; i++)
            groups[g].errs[i] = se;
      }
      *err = se;
      goto done;
   }

   for (int g = 0; g < ngroups; g++) {
      email_imap_undo_group_t *grp = &groups[g];
      if (cancel && atomic_load(cancel)) {
         for (int r = g; r < ngroups; r++) {
            for (int i = 0; i < groups[r].n; i++)
               groups[r].errs[i] = EMAIL_ERR_CANCELLED;
         }
         break;
      }
      email_transfer_set_cancel(curl, cancel);
      char url[EMAIL_IMAP_MAILBOX_URL_MAX], set[MOVE_SET_MAX];
      char quoted[EMAIL_IMAP_ROLE_FOLDER_MAX * 2 + 3];
      if (grp->n <= 0 || grp->n > EMAIL_IMAP_MOVE_MAX ||
          !email_imap_mailbox_url(conn, grp->dest_folder, grp->dest_uidvalidity, url,
                                  sizeof(url)) ||
          email_imap_uid_set(grp->uids, grp->n, set, sizeof(set)) == 0 ||
          !email_imap_quote_folder(grp->src_folder, quoted, sizeof(quoted)))
         continue;
      curl_easy_setopt(curl, CURLOPT_URL, url);
      bool present[EMAIL_IMAP_MOVE_MAX] = { false };
      bool gone = false;
      const CURLcode res = present_uids(curl, &dctx, conn, set, grp->uids, grp->n, present, &gone,
                                        &first);
      if (res != CURLE_OK) {
         /* The Trash was emptied or rebuilt: nothing to bring back. */
         const email_err_t ge = gone ? EMAIL_ERR_NOT_FOUND : email_err_from_curl(res);
         for (int i = 0; i < grp->n; i++)
            grp->errs[i] = ge;
         if (err_covers_account(ge)) {
            *err = ge;
            goto done;
         }
         continue;
      }
      for (int i = 0; i < grp->n; i++) {
         if (!present[i])
            grp->errs[i] = EMAIL_ERR_NOT_FOUND;
      }
      email_transfer_clear_cancel(curl);
      dctx.last_reject[0] = '\0';
      uint32_t src_v = 0;
      const CURLcode mres = move_set(curl, &dctx, conn, &roles, quoted, grp->uids, present, grp->n,
                                     cap, grp->outcome, grp->errs, grp->new_uid, &src_v, &first);
      if (strstr(dctx.last_reject, "TRYCREATE")) {
         /* The folder it came from is gone. */
         for (int i = 0; i < grp->n; i++) {
            if (present[i] && grp->outcome[i] == EMAIL_MOVE_FAILED)
               grp->errs[i] = EMAIL_ERR_FOLDER_MISSING;
         }
      }
      grp->src_uidvalidity = src_v;
      if (mres != CURLE_OK && err_covers_account(email_err_from_curl(mres))) {
         *err = email_err_from_curl(mres);
         goto done;
      }
      /* The restored rows, from the folder they're back in.  A new command, so
       * nothing in these (sender-controlled) lines is read as COPYUID. */
      uint32_t back[EMAIL_IMAP_MOVE_MAX];
      int nb = 0;
      for (int i = 0; i < grp->n; i++) {
         if (grp->new_uid[i])
            back[nb++] = grp->new_uid[i];
      }
      int got = 0;
      const bool fetched_ok = nb > 0 &&
                              email_imap_fetch_summaries(curl, conn, grp->src_folder, back, nb,
                                                         fetched, EMAIL_IMAP_MOVE_MAX, &got) == 0;
      first |= nb > 0 && !fetched_ok;
      if (fetched_ok) {
         for (int i = 0; i < grp->n; i++) {
            for (int k = 0; grp->new_uid[i] && k < got; k++) {
               if (fetched[k].uid == grp->new_uid[i]) {
                  grp->rows[i] = fetched[k];
                  grp->row_ok[i] = true;
                  break;
               }
            }
         }
      }
   }
   *err = EMAIL_ERR_NONE;

done:
   email_instrument_op_done(conn->username, "undo", curl, &dctx);
   curl_easy_cleanup(curl);
   free(cap);
   free(fetched);
   return *err == EMAIL_ERR_NONE ? 0 : 1;
}
