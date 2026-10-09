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
 * Gmail read state: the UNREAD label on messages, and the INBOX's unread
 * count.  Only UNREAD is ever added or removed.
 */

#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

#include "logging.h"
#include "tools/email_transfer.h"
#include "tools/gmail_client.h"
#include "tools/gmail_client_internal.h"

/* A Gmail message id is 16 hex digits; anything much longer isn't one. */
#define GMAIL_FLAGS_ID_MAX 64
/* {"ids":[...the ids...],"addLabelIds":["UNREAD"]} */
#define GMAIL_FLAGS_BODY_MAX (GMAIL_FLAGS_MAX_IDS * (GMAIL_FLAGS_ID_MAX + 3) + 64)

/* A Gmail message id short enough to go in a URL or a batch body whole. */
static bool id_ok(const char *id) {
   return gmail_message_id_valid(id) && strnlen(id, GMAIL_FLAGS_ID_MAX + 1) <= GMAIL_FLAGS_ID_MAX;
}

/* "addLabelIds" or "removeLabelIds", for UNREAD */
static const char *unread_key(bool unread) {
   return unread ? "addLabelIds" : "removeLabelIds";
}

/* POST one message's modify; 0, or 1 with @p err set. */
static int modify_one(CURL *curl,
                      const char *token,
                      const char *id,
                      bool unread,
                      email_err_t *err) {
   char url[sizeof(GMAIL_API_BASE) + 256];
   snprintf(url, sizeof(url), GMAIL_API_BASE "/messages/%s/modify", id);
   char body[64];
   snprintf(body, sizeof(body), "{\"%s\":[\"UNREAD\"]}", unread_key(unread));
   curl_buffer_t resp;
   long http_code = 0;
   CURLcode res = CURLE_OK;
   if (gmail_api_post_ex(curl, token, url, "application/json", body, &resp, &http_code, &res) !=
       0) {
      *err = gmail_http_err(res, http_code);
      return 1;
   }
   curl_buffer_free(&resp);
   *err = EMAIL_ERR_NONE;
   return 0;
}

int gmail_mark_read(CURL *curl, const char *token, const char *message_id, email_err_t *err) {
   email_err_t err_local;
   if (!err)
      err = &err_local;
   *err = EMAIL_ERR_FAILED;
   if (!curl || !token || !id_ok(message_id))
      return 1;
   return modify_one(curl, token, message_id, false, err);
}

int gmail_set_unread(const char *token,
                     const char *const *ids,
                     int n,
                     bool unread,
                     bool *updated,
                     email_err_t *errs) {
   if (!token || !token[0] || !ids || n <= 0 || n > GMAIL_FLAGS_MAX_IDS || !updated || !errs)
      return 1;
   for (int i = 0; i < n; i++) {
      updated[i] = false;
      errs[i] = id_ok(ids[i]) ? EMAIL_ERR_FAILED : EMAIL_ERR_NOT_FOUND;
   }

   char body[GMAIL_FLAGS_BODY_MAX];
   size_t pos = 0;
   int valid = 0;
   pos += (size_t)snprintf(body, sizeof(body), "{\"ids\":[");
   for (int i = 0; i < n; i++) {
      if (errs[i] == EMAIL_ERR_NOT_FOUND)
         continue;
      const int w = snprintf(body + pos, sizeof(body) - pos, "%s\"%s\"", valid ? "," : "", ids[i]);
      if (w < 0 || (size_t)w >= sizeof(body) - pos)
         return 1;
      pos += (size_t)w;
      valid++;
   }
   const int w = snprintf(body + pos, sizeof(body) - pos, "],\"%s\":[\"UNREAD\"]}",
                          unread_key(unread));
   if (w < 0 || (size_t)w >= sizeof(body) - pos)
      return 1;
   if (valid == 0)
      return 0;

   CURL *curl = gmail_create_curl();
   if (!curl)
      return 1;
   curl_buffer_t resp;
   long http_code = 0;
   CURLcode res = CURLE_OK;
   if (gmail_api_post_ex(curl, token, GMAIL_API_BASE "/messages/batchModify", "application/json",
                         body, &resp, &http_code, &res) == 0) {
      /* batchModify answers for the batch, not per message: an id that isn't in
       * the mailbox doesn't fail it, so it reads as updated here. */
      curl_buffer_free(&resp);
      for (int i = 0; i < n; i++) {
         if (errs[i] != EMAIL_ERR_NOT_FOUND) {
            updated[i] = true;
            errs[i] = EMAIL_ERR_NONE;
         }
      }
      curl_easy_cleanup(curl);
      return 0;
   }
   /* A 400 may be one bad message, so try each.  Anything else (a transfer,
    * auth, scope, rate or server failure) each one would only repeat. */
   const email_err_t batch_err = gmail_http_err(res, http_code);
   if (res != CURLE_OK || http_code != 400) {
      for (int i = 0; i < n; i++) {
         if (errs[i] != EMAIL_ERR_NOT_FOUND)
            errs[i] = batch_err;
      }
      curl_easy_cleanup(curl);
      return 1;
   }
   for (int i = 0; i < n; i++) {
      if (errs[i] == EMAIL_ERR_NOT_FOUND)
         continue;
      updated[i] = modify_one(curl, token, ids[i], unread, &errs[i]) == 0;
   }
   curl_easy_cleanup(curl);
   return 0;
}

int gmail_inbox_unread_on(CURL *curl, const char *token, int *unread, email_err_t *err) {
   email_err_t err_local;
   if (!err)
      err = &err_local;
   *err = EMAIL_ERR_FAILED;
   if (!curl || !token || !token[0] || !unread)
      return 1;
   *unread = -1;
   curl_buffer_t resp;
   long http_code = 0;
   CURLcode res = CURLE_OK;
   if (gmail_api_get_ex(curl, token, GMAIL_API_BASE "/labels/INBOX", &resp, &http_code, &res) !=
       0) {
      *err = gmail_http_err(res, http_code);
      return 1;
   }
   struct json_object *root = json_tokener_parse(resp.data);
   curl_buffer_free(&resp);
   struct json_object *count = NULL;
   if (root && json_object_object_get_ex(root, "messagesUnread", &count) &&
       json_object_is_type(count, json_type_int)) {
      const int64_t v = json_object_get_int64(count);
      if (v >= 0 && v <= 0x7fffffff)
         *unread = (int)v;
   }
   if (root)
      json_object_put(root);
   if (*unread < 0)
      return 1;
   *err = EMAIL_ERR_NONE;
   return 0;
}

int gmail_inbox_unread(const char *token, int *unread, email_err_t *err) {
   if (unread)
      *unread = -1;
   CURL *curl = gmail_create_curl();
   if (!curl) {
      if (err)
         *err = EMAIL_ERR_FAILED;
      return 1;
   }
   const int rc = gmail_inbox_unread_on(curl, token, unread, err);
   curl_easy_cleanup(curl);
   return rc;
}
