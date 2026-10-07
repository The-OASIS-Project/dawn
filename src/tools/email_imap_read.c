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
 * IMAP: reading one message (email_read_message, email_client.h): a bounded
 * fetch into the shared MIME reader, and the read state left as asked.
 */

#include <curl/curl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/curl_buffer.h"
#include "logging.h"
#include "tools/email_client.h"
#include "tools/email_client_internal.h"
#include "tools/email_display.h"
#include "tools/email_imap_state.h"
#include "tools/email_instrument.h"
#include "tools/email_mime.h"
#include "tools/email_parse.h"
#include "tools/email_transfer.h"

/* =============================================================================
 * Public API: Read Message
 * ============================================================================= */

/* A read's sink: keeps at most `cap` bytes, then stops the transfer (a short
 * write makes curl end it with CURLE_WRITE_ERROR), so a message larger than
 * the read wants is never downloaded whole. */
typedef struct {
   curl_buffer_t buf;
   size_t cap;
   bool hit_cap;
} read_sink_t;

static size_t read_sink_write(void *data, size_t size, size_t nmemb, void *userp) {
   read_sink_t *s = userp;
   const size_t n = size * nmemb;
   const size_t room = s->cap > s->buf.size ? s->cap - s->buf.size : 0;
   if (n > room) {
      if (room > 0)
         curl_buffer_write_callback(data, 1, room, &s->buf);
      s->hit_cap = true;
      return 0;
   }
   return curl_buffer_write_callback(data, size, nmemb, &s->buf);
}

/* Subject and From without reading the body: FETCH (ENVELOPE) answers inline
 * (no literal for libcurl to drop) and, unlike a body fetch, leaves the
 * message unread. */
static int read_headers_only(CURL *curl,
                             email_instrument_ctx_t *dctx,
                             const email_conn_t *conn,
                             const char *folder_url,
                             uint32_t uid,
                             email_message_t *out,
                             email_err_t *err) {
   curl_easy_setopt(curl, CURLOPT_URL, folder_url);
   char cmd[64];
   snprintf(cmd, sizeof(cmd), "UID FETCH %u (ENVELOPE)", uid);
   curl_buffer_t buf;
   const CURLcode res = email_imap_run_command(curl, dctx, conn, "read", cmd, true, &buf);
   if (res != CURLE_OK || !buf.data) {
      *err = res == CURLE_OK ? EMAIL_ERR_NOT_FOUND : email_err_from_curl(res);
      curl_buffer_free(&buf);
      return 1;
   }
   char subject[512] = "";
   char from_name[256] = "";
   char from_addr[256] = "";
   const bool ok = email_parse_envelope(buf.data, subject, sizeof(subject), from_name,
                                        sizeof(from_name), from_addr, sizeof(from_addr));
   curl_buffer_free(&buf);
   if (!ok) {
      *err = EMAIL_ERR_NOT_FOUND;
      return 1;
   }
   email_mime_header_text(subject, out->subject, sizeof(out->subject));
   email_mime_header_text(from_name, out->from_name, sizeof(out->from_name));
   email_display_sanitize(from_addr, strlen(from_addr), out->from_addr, sizeof(out->from_addr), 0);
   return 0;
}

int email_read_message(const email_conn_t *conn,
                       const char *folder,
                       uint32_t uid,
                       uint32_t uidvalidity,
                       const email_read_opts_t *opts,
                       email_message_t *out,
                       email_err_t *err) {
   email_err_t err_local;
   if (!err)
      err = &err_local;
   *err = EMAIL_ERR_NONE;
   memset(out, 0, sizeof(*out));
   if (!conn || !opts || uid == 0) {
      *err = EMAIL_ERR_FAILED;
      return 1;
   }
   if (!folder || !folder[0])
      folder = "INBOX";

   /* The mailbox, pinned to the epoch the id was issued under: a rebuilt
    * mailbox fails the SELECT (not found) instead of reading another message. */
   char folder_url[EMAIL_IMAP_MAILBOX_URL_MAX];
   if (!email_imap_mailbox_url(conn, folder, uidvalidity, folder_url, sizeof(folder_url))) {
      *err = EMAIL_ERR_FAILED;
      return 1;
   }
   CURL *curl = email_imap_handle_create(conn);
   if (!curl) {
      *err = EMAIL_ERR_FAILED;
      return 1;
   }
   email_instrument_ctx_t dctx;
   email_instrument_attach(curl, &dctx);
   email_transfer_set_cancel(curl, opts->cancel);
   char url[sizeof(folder_url) + 48]; /* the folder URL, then ;UID= and ;PARTIAL= */
   int rc = 1;

   if (opts->headers_only) {
      rc = read_headers_only(curl, &dctx, conn, folder_url, uid, out, err);
      email_instrument_op_done(conn->username, "read", curl, &dctx);
      curl_easy_cleanup(curl);
      if (rc == 0) {
         out->uid = uid;
         out->uidvalidity = dctx.uidvalidity ? dctx.uidvalidity : uidvalidity;
      }
      return rc;
   }

   /* The read state to keep or set: look at it first.  The body fetch below is
    * libcurl's URL fetch, a plain BODY[] (BODY.PEEK isn't reachable through
    * it), so the server marks the message read; with EMAIL_MARK_KEEP an unread
    * message is marked unread again right after (see the restore below). */
   int seen_before = EMAIL_IMAP_UID_ABSENT;
   if (opts->mark != EMAIL_MARK_AS_BACKEND) {
      const CURLcode sres = email_imap_seen_state(curl, &dctx, conn, folder_url, uid, true,
                                                  &seen_before);
      if (sres != CURLE_OK || seen_before == EMAIL_IMAP_UID_ABSENT) {
         *err = sres != CURLE_OK ? email_err_from_curl(sres) : EMAIL_ERR_NOT_FOUND;
         email_instrument_op_done(conn->username, "read", curl, &dctx);
         curl_easy_cleanup(curl);
         return 1;
      }
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, NULL); /* the fetch is the URL's own */
   }
   const bool restore = opts->mark == EMAIL_MARK_KEEP && seen_before == EMAIL_IMAP_UID_UNSEEN;

   /* A bounded fetch (IMAP BODY[]<0.N>, curl ";PARTIAL="): a message bigger than
    * the read wants comes back cut, and is read as truncated.  PARTIAL is core
    * RFC 3501; a server that rejects it gets one plain fetch, whose sink stops
    * the transfer at the same size. */
   const size_t cap = opts->fetch_bytes > 0 ? opts->fetch_bytes : EMAIL_READ_FETCH_TOOL;
   read_sink_t sink;
   bool raw_cut = false;
   bool got = false;
   for (int attempt = 0; attempt < 2 && !got; attempt++) {
      const bool partial = attempt == 0;
      if (partial)
         snprintf(url, sizeof(url), "%s/;UID=%u;PARTIAL=0.%zu", folder_url, uid, cap);
      else
         snprintf(url, sizeof(url), "%s/;UID=%u", folder_url, uid);
      curl_easy_setopt(curl, CURLOPT_URL, url);
      memset(&sink, 0, sizeof(sink));
      dctx.last_reject[0] = '\0'; /* a refused command shows up here */
      curl_buffer_init_with_max(&sink.buf, cap + 1);
      sink.cap = cap;
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, read_sink_write);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);

      const CURLcode res = email_instrument_perform(curl, &dctx, conn->username, "read");
      if (res == CURLE_OK && sink.buf.size > 0) {
         raw_cut = partial && sink.buf.size >= cap;
         got = true;
      } else if (res == CURLE_WRITE_ERROR && sink.hit_cap && sink.buf.size > 0) {
         raw_cut = true;
         got = true;
      } else if (res == CURLE_OK) {
         /* The server answered and had no such message. */
         *err = EMAIL_ERR_NOT_FOUND;
         curl_buffer_free(&sink.buf);
         break;
      } else {
         curl_buffer_free(&sink.buf);
         *err = email_err_from_curl(res);
         /* A server that refused PARTIAL gets the plain fetch.  libcurl reports
          * that refusal as it reports a missing message (no FETCH answer); the
          * server's tagged NO/BAD tells them apart, so a missing message costs
          * one command.  Not a login denial (a second fetch would log in again,
          * the burst the instrumented perform guards against), not a cancel. */
         const bool refused = dctx.last_reject[0] != '\0';
         if (*err == EMAIL_ERR_AUTH_FAILED || *err == EMAIL_ERR_CANCELLED || !partial ||
             (*err == EMAIL_ERR_NOT_FOUND && !refused)) {
            OLOG_ERROR("email: IMAP FETCH uid=%u failed: %s", uid, curl_easy_strerror(res));
            break;
         }
         OLOG_WARNING("email: IMAP partial fetch uid=%u failed (%s); retrying full fetch", uid,
                      curl_easy_strerror(res));
      }
   }
   /* Unread again, whatever became of the fetch (failed, cut, stopped): it may
    * have marked the message read before it ended.  Known side effects: a read
    * another client did in that moment is undone too, and if this fails the
    * message stays read (reported as such). */
   const bool restored = restore && email_imap_restore_unseen(curl, &dctx, conn, folder_url, uid);
   email_instrument_op_done(conn->username, "read", curl, &dctx);
   curl_easy_cleanup(curl);
   if (!got)
      return 1;

   rc = email_mime_parse_raw(sink.buf.data, sink.buf.size, raw_cut, opts, out);
   curl_buffer_free(&sink.buf);
   if (rc != 0) {
      *err = EMAIL_ERR_FAILED;
      return 1;
   }
   if (opts->mark != EMAIL_MARK_AS_BACKEND) {
      out->unread_before = seen_before == EMAIL_IMAP_UID_UNSEEN;
      out->unread_known = true;
      out->unread_after = restored;
   }
   *err = EMAIL_ERR_NONE;
   out->uid = uid;
   out->uidvalidity = dctx.uidvalidity ? dctx.uidvalidity : uidvalidity;
   return 0;
}
