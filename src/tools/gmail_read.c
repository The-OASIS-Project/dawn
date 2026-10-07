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
 * Reading one Gmail message (gmail_read_message): the API's MIME tree turned
 * into the email_mime part list, so both backends share one reading policy.
 */

#include <ctype.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "logging.h"
#include "tools/email_display.h"
#include "tools/email_mime.h"
#include "tools/email_transfer.h"
#include "tools/gmail_client.h"
#include "tools/gmail_client_internal.h"

/* =============================================================================
 * Reading a message
 *
 * format=full gives the MIME tree with each text part's bytes inline
 * (transfer-decoded, in its own charset) and every attachment behind an
 * attachmentId, so a read never downloads attachment bytes.  The tree becomes
 * the email_mime part list, and email_mime does the rest, as it does for IMAP.
 * ============================================================================= */

/* =============================================================================
 * Public API: Read Message
 * ============================================================================= */

/* One part's bytes from behind its attachmentId: Gmail moves a large body part
 * out of the tree, and the body we read may be one.  NULL with *err set on
 * failure, or NULL with *err EMAIL_ERR_NONE when the part is bigger than
 * GMAIL_TEXT_PART_MAX (its reply passes GMAIL_PART_RESPONSE_MAX): it reads as
 * cut, with none of its text. */
static unsigned char *gmail_fetch_part(CURL *curl,
                                       const char *token,
                                       const char *message_id,
                                       const char *attachment_id,
                                       size_t *len_out,
                                       email_err_t *err) {
   *len_out = 0;
   *err = EMAIL_ERR_FAILED;
   const size_t alen = strlen(attachment_id);
   if (alen == 0 || alen > 1024)
      return NULL;
   for (size_t i = 0; i < alen; i++) {
      if (!gmail_b64url_char((unsigned char)attachment_id[i]) && attachment_id[i] != '=')
         return NULL;
   }
   char url[1400];
   snprintf(url, sizeof(url), GMAIL_API_BASE "/messages/%s/attachments/%s", message_id,
            attachment_id);
   curl_buffer_t resp;
   long http_code = 0;
   CURLcode res = CURLE_OK;
   if (gmail_api_get_capped(curl, token, url, GMAIL_PART_RESPONSE_MAX, &resp, &http_code, &res) !=
       0) {
      /* A reply past the response cap comes back as no reply with no status. */
      *err = res == CURLE_OK && http_code == 0 ? EMAIL_ERR_NONE : gmail_http_err(res, http_code);
      return NULL;
   }
   struct json_object *root = json_tokener_parse(resp.data);
   curl_buffer_free(&resp);
   if (!root)
      return NULL;
   const char *data = gmail_json_str(root, "data");
   unsigned char *bytes = data ? gmail_base64url_decode(data, GMAIL_TEXT_PART_MAX, len_out) : NULL;
   json_object_put(root);
   if (bytes)
      *err = EMAIL_ERR_NONE;
   return bytes;
}

/* What fetch_body_part needs to fetch a part during email_mime_apply. */
typedef struct {
   CURL *curl;
   const char *token;
   const char *message_id;
   gmail_parts_t *w;
   email_err_t err; /* the first fetch that failed, or NONE */
} fetch_ctx_t;

/* email_mime_fetch_fn: one body part from behind its attachmentId, when the
 * reading reaches it.  Fetched once (a part read as text and as HTML). */
static bool fetch_body_part(void *ctx,
                            int index,
                            const char **data_out,
                            size_t *len_out,
                            bool *cut_out) {
   fetch_ctx_t *fc = ctx;
   gmail_parts_t *w = fc->w;
   if (w->owned[index]) {
      *data_out = (const char *)w->owned[index];
      *len_out = w->owned_len[index];
      *cut_out = w->owned_len[index] >= GMAIL_TEXT_PART_MAX;
      return true;
   }
   if (!w->attachment_ids[index] || fc->err != EMAIL_ERR_NONE)
      return false;
   size_t len = 0;
   email_err_t e = EMAIL_ERR_NONE;
   unsigned char *bytes = gmail_fetch_part(fc->curl, fc->token, fc->message_id,
                                           w->attachment_ids[index], &len, &e);
   if (!bytes) {
      if (e == EMAIL_ERR_NONE) {
         /* Too big to take: the part reads as cut, and isn't asked for again. */
         free((void *)w->attachment_ids[index]);
         w->attachment_ids[index] = NULL;
         return false;
      }
      fc->err = e;
      return false;
   }
   w->owned[index] = bytes;
   w->owned_len[index] = len;
   *data_out = (const char *)bytes;
   *len_out = len;
   *cut_out = len >= GMAIL_TEXT_PART_MAX;
   return true;
}

/* Copies the attachment ids out of the JSON tree, so it can be freed.  Every
 * entry ends up a copy or NULL (a lost id reads as a cut part). */
static int take_attachment_ids(gmail_parts_t *w) {
   int rc = 0;
   w->ids_owned = true;
   for (int i = 0; i < w->count; i++) {
      if (w->attachment_ids[i]) {
         w->attachment_ids[i] = strdup(w->attachment_ids[i]);
         if (!w->attachment_ids[i])
            rc = 1;
      }
   }
   return rc;
}

static void gmail_message_headers(struct json_object *headers, email_message_t *out) {
   gmail_header_fields(headers, out->from_name, sizeof(out->from_name), out->from_addr,
                       sizeof(out->from_addr), out->subject, sizeof(out->subject), out->date_str,
                       sizeof(out->date_str));
   email_mime_addr_first(gmail_find_header(headers, "Reply-To"), &out->reply_to);
}

int gmail_read_message(const char *token,
                       const char *message_id,
                       const email_read_opts_t *opts,
                       email_message_t *out,
                       email_err_t *err) {
   email_err_t err_local;
   if (!err)
      err = &err_local;
   *err = EMAIL_ERR_FAILED;
   memset(out, 0, sizeof(*out));
   if (!token || !token[0] || !message_id || !opts)
      return 1;
   if (!gmail_message_id_valid(message_id)) {
      /* Not echoed: it can come from a client. */
      OLOG_ERROR("gmail: invalid message ID (%zu bytes)", strlen(message_id));
      return 1;
   }
   snprintf(out->message_id, sizeof(out->message_id), "%s", message_id);

   CURL *curl = gmail_create_curl();
   if (!curl)
      return 1;
   email_transfer_set_cancel(curl, opts->cancel);
   char url[512];
   if (opts->headers_only)
      snprintf(url, sizeof(url),
               GMAIL_API_BASE "/messages/%s?format=metadata&metadataHeaders=From"
                              "&metadataHeaders=Subject&metadataHeaders=Date",
               message_id);
   else
      snprintf(url, sizeof(url), GMAIL_API_BASE "/messages/%s?format=full", message_id);

   curl_buffer_t resp;
   long http_code = 0;
   CURLcode res = CURLE_OK;
   if (gmail_api_get_ex(curl, token, url, &resp, &http_code, &res) != 0) {
      curl_easy_cleanup(curl);
      *err = gmail_http_err(res, http_code);
      return 1;
   }
   struct json_object *root = json_tokener_parse(resp.data);
   curl_buffer_free(&resp);
   if (!root) {
      curl_easy_cleanup(curl);
      return 1;
   }

   const char *thread = gmail_json_str(root, "threadId");
   if (thread)
      snprintf(out->thread_id, sizeof(out->thread_id), "%s", thread);
   const char *idate = gmail_json_str(root, "internalDate"); /* ms since the epoch, as a string */
   if (idate)
      out->internal_date = (time_t)(strtoll(idate, NULL, 10) / 1000);
   struct json_object *labels = NULL;
   if (json_object_object_get_ex(root, "labelIds", &labels) &&
       json_object_is_type(labels, json_type_array)) {
      for (size_t i = 0; i < json_object_array_length(labels); i++) {
         const char *l = json_object_get_string(json_object_array_get_idx(labels, i));
         if (l && strcmp(l, "UNREAD") == 0)
            out->unread_before = true;
      }
   }
   struct json_object *payload = NULL;
   struct json_object *headers = NULL;
   if (json_object_object_get_ex(root, "payload", &payload))
      json_object_object_get_ex(payload, "headers", &headers);
   gmail_message_headers(headers, out);

   int rc = 0;
   if (!opts->headers_only) {
      rc = 1;
      gmail_parts_t w;
      if (gmail_parts_from_payload(payload, &w) == 0 &&
          email_mime_addr_list(gmail_find_header(headers, "To"), EMAIL_MAX_ADDRS, &out->to_list,
                               &out->to_count, &out->to_total) == 0 &&
          email_mime_addr_list(gmail_find_header(headers, "Cc"), EMAIL_MAX_ADDRS, &out->cc_list,
                               &out->cc_count, &out->cc_total) == 0) {
         /* The attachment ids point into the JSON; copy them so the tree (a few
          * MB at worst) can go before decoding. */
         fetch_ctx_t fc = { .curl = curl, .token = token, .message_id = message_id, .w = &w };
         if (take_attachment_ids(&w) == 0) {
            json_object_put(root);
            root = NULL;
            rc = email_mime_apply(w.parts, w.count, w.cut, opts, fetch_body_part, &fc, out);
            if (fc.err != EMAIL_ERR_NONE) {
               email_message_free(out);
               *err = fc.err;
               rc = 1;
            }
         }
      }
      gmail_parts_free(&w);
   }
   if (root)
      json_object_put(root);
   if (rc == 0 && !opts->headers_only && opts->mark != EMAIL_MARK_AS_BACKEND) {
      /* A Gmail read changes nothing; marking it read is a call of its own. */
      bool unread = out->unread_before;
      if (opts->mark == EMAIL_MARK_READ && unread) {
         email_err_t merr = EMAIL_ERR_NONE;
         if (gmail_mark_read(curl, token, message_id, &merr) == 0)
            unread = false;
         else
            OLOG_WARNING("gmail: couldn't mark a message read (%s)", email_error_name(merr));
      }
      out->unread_known = true;
      out->unread_after = unread;
   }
   curl_easy_cleanup(curl);
   if (rc != 0) {
      email_message_free(out);
      if (*err == EMAIL_ERR_NONE)
         *err = EMAIL_ERR_FAILED;
      return 1;
   }
   *err = EMAIL_ERR_NONE;
   return 0;
}
