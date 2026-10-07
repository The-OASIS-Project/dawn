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
 * IMAP read state: the INBOX's unread count, marking messages read or unread,
 * and a read's FLAGS check and \Seen restore.  Only \Seen is ever changed.
 *
 * Every command here is built from a parsed UID (uint32) and a constant; a
 * folder only ever travels in the percent-encoded URL path (its SELECT).
 */

#include <stdio.h>
#include <string.h>

#include "logging.h"
#include "tools/email_client_internal.h"
#include "tools/email_imap_state.h"
#include "tools/email_transfer.h"

/* A restore's own timeout: short, because the read it follows may already
 * have used the operation's. */
#define EMAIL_IMAP_RESTORE_TIMEOUT_SEC 10L

/* The longest UID set one STORE carries: 50 ten-digit UIDs and commas. */
#define EMAIL_IMAP_UID_SET_MAX 600

CURLcode email_imap_status_inbox_unseen(CURL *curl,
                                        email_instrument_ctx_t *dctx,
                                        const email_conn_t *conn,
                                        bool first,
                                        int *unseen) {
   *unseen = -1;
   curl_easy_setopt(curl, CURLOPT_URL, conn->imap_url);
   curl_buffer_t buf;
   CURLcode res = email_imap_run_command(curl, dctx, conn, "status", "STATUS INBOX (UNSEEN)", first,
                                         &buf);
   if (res == CURLE_OK && !(buf.data && email_imap_status_unseen(buf.data, unseen)))
      *unseen = -1;
   curl_buffer_free(&buf);
   return res;
}

int email_imap_inbox_unseen(const email_conn_t *conn, int *unseen, email_err_t *err) {
   email_err_t err_local;
   if (!err)
      err = &err_local;
   *err = EMAIL_ERR_FAILED;
   if (!conn || !unseen)
      return 1;
   *unseen = -1;
   CURL *curl = email_imap_handle_create(conn);
   if (!curl)
      return 1;
   email_instrument_ctx_t dctx;
   email_instrument_attach(curl, &dctx);
   const CURLcode res = email_imap_status_inbox_unseen(curl, &dctx, conn, true, unseen);
   if (res == CURLE_QUOTE_ERROR)
      email_instrument_note_status_refused(conn->imap_url, conn->username);
   email_instrument_op_done(conn->username, "status", curl, &dctx);
   curl_easy_cleanup(curl);
   if (res != CURLE_OK) {
      *err = email_err_from_curl(res);
      return 1;
   }
   if (*unseen < 0)
      return 1; /* answered, but without a count */
   *err = EMAIL_ERR_NONE;
   return 0;
}

CURLcode email_imap_seen_state(CURL *curl,
                               email_instrument_ctx_t *dctx,
                               const email_conn_t *conn,
                               const char *folder_url,
                               uint32_t uid,
                               bool first,
                               int *state) {
   *state = EMAIL_IMAP_UID_ABSENT;
   curl_easy_setopt(curl, CURLOPT_URL, folder_url);
   char cmd[64];
   snprintf(cmd, sizeof(cmd), "UID FETCH %u (UID FLAGS)", uid);
   curl_buffer_t buf;
   const CURLcode res = email_imap_run_command(curl, dctx, conn, "read", cmd, first, &buf);
   if (res == CURLE_OK && buf.data)
      *state = email_imap_fetch_seen(buf.data, uid);
   curl_buffer_free(&buf);
   return res;
}

bool email_imap_restore_unseen(CURL *curl,
                               email_instrument_ctx_t *dctx,
                               const email_conn_t *conn,
                               const char *folder_url,
                               uint32_t uid) {
   /* Must happen even when the read was stopped: drop the cancel, give it a
    * short timeout of its own. */
   email_transfer_clear_cancel(curl);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, EMAIL_IMAP_RESTORE_TIMEOUT_SEC);
   curl_easy_setopt(curl, CURLOPT_URL, folder_url);
   char cmd[64];
   snprintf(cmd, sizeof(cmd), "UID STORE %u -FLAGS.SILENT (\\Seen)", uid);
   curl_buffer_t buf;
   const CURLcode res = email_imap_run_command(curl, dctx, conn, "read", cmd, true, &buf);
   curl_buffer_free(&buf);
   if (res != CURLE_OK) {
      OLOG_WARNING("email: couldn't mark UID %u unread again after reading it: %s", uid,
                   curl_easy_strerror(res));
      return false;
   }
   return true;
}

/* One folder: STORE, then the FETCH that says which UIDs are there in the
 * state asked for.  The URL switch makes curl SELECT it on the same connection,
 * which is already logged in (email_imap_set_seen's first command). */
static void set_seen_folder(CURL *curl,
                            email_instrument_ctx_t *dctx,
                            const email_conn_t *conn,
                            email_imap_seen_batch_t *b,
                            bool seen) {
   for (int i = 0; i < b->n; i++)
      b->updated[i] = false;
   char set[EMAIL_IMAP_UID_SET_MAX];
   if (!b->folder || !b->folder[0] || email_imap_uid_set(b->uids, b->n, set, sizeof(set)) == 0) {
      b->err = EMAIL_ERR_FAILED;
      return;
   }
   char encoded[256];
   email_imap_url_encode_folder(b->folder, encoded, sizeof(encoded));
   char url[sizeof(conn->imap_url) + sizeof(encoded) + 2];
   snprintf(url, sizeof(url), "%s/%s", conn->imap_url, encoded);
   curl_easy_setopt(curl, CURLOPT_URL, url);

   char cmd[sizeof(set) + 48];
   snprintf(cmd, sizeof(cmd), "UID STORE %s %sFLAGS (\\Seen)", set, seen ? "+" : "-");
   curl_buffer_t buf;
   const int logins_before = dctx->login_seen;
   CURLcode res = email_imap_run_command(curl, dctx, conn, "flags", cmd, false, &buf);
   curl_buffer_free(&buf);
   if (res != CURLE_OK) {
      /* A folder deleted or renamed elsewhere: its messages aren't there. */
      b->err = email_imap_select_failed(res == CURLE_LOGIN_DENIED, logins_before, dctx->login_seen)
                   ? EMAIL_ERR_NOT_FOUND
                   : email_err_from_curl(res);
      return;
   }
   /* What the server now holds, for each UID: a missing one isn't there. */
   snprintf(cmd, sizeof(cmd), "UID FETCH %s (UID FLAGS)", set);
   res = email_imap_run_command(curl, dctx, conn, "flags", cmd, false, &buf);
   if (res == CURLE_OK) {
      /* No FETCH lines at all: none of these messages exist. */
      const int want = seen ? EMAIL_IMAP_UID_SEEN : EMAIL_IMAP_UID_UNSEEN;
      for (int i = 0; i < b->n; i++)
         b->updated[i] = buf.data && email_imap_fetch_seen(buf.data, b->uids[i]) == want;
   } else {
      /* The server took the STORE; only the check failed.  Saying "failed"
       * here would contradict what the mailbox now shows. */
      OLOG_WARNING("email: flags were set but couldn't be checked: %s", curl_easy_strerror(res));
      for (int i = 0; i < b->n; i++)
         b->updated[i] = true;
   }
   curl_buffer_free(&buf);
   b->err = EMAIL_ERR_NONE;
}

/* Errors that hold for the whole account, not just one folder. */
static bool err_covers_account(email_err_t e) {
   return e == EMAIL_ERR_CANCELLED || e == EMAIL_ERR_UNREACHABLE || e == EMAIL_ERR_TIMEOUT ||
          e == EMAIL_ERR_AUTH_FAILED || e == EMAIL_ERR_AUTH_REVOKED;
}

int email_imap_set_seen(const email_conn_t *conn,
                        email_imap_seen_batch_t *batches,
                        int nbatches,
                        bool seen) {
   if (!conn || !batches || nbatches <= 0)
      return 1;
   for (int g = 0; g < nbatches; g++)
      batches[g].err = EMAIL_ERR_FAILED;
   CURL *curl = email_imap_handle_create(conn);
   if (!curl)
      return 1;
   email_instrument_ctx_t dctx;
   email_instrument_attach(curl, &dctx);

   /* Log in on its own first, with no folder selected, so a refused login is
    * told apart from a refused SELECT (libcurl reports both the same way). */
   curl_easy_setopt(curl, CURLOPT_URL, conn->imap_url);
   curl_buffer_t buf;
   const CURLcode lres = email_imap_run_command(curl, &dctx, conn, "flags", "NOOP", true, &buf);
   curl_buffer_free(&buf);
   int rc = 0;
   if (lres != CURLE_OK) {
      const email_err_t e = email_err_from_curl(lres);
      for (int g = 0; g < nbatches; g++)
         batches[g].err = e;
      rc = 1;
   } else {
      for (int g = 0; g < nbatches; g++) {
         set_seen_folder(curl, &dctx, conn, &batches[g], seen);
         const email_err_t e = batches[g].err;
         if (e != EMAIL_ERR_NONE)
            rc = 1;
         /* A stop, a lost connection or a refused login holds for the folders
          * after it too: they get the same answer instead of another attempt. */
         if (err_covers_account(e)) {
            for (int r = g + 1; r < nbatches; r++)
               batches[r].err = e;
            break;
         }
      }
   }
   email_instrument_op_done(conn->username, "flags", curl, &dctx);
   curl_easy_cleanup(curl);
   return rc;
}
