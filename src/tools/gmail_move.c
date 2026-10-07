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
 * Gmail trash, archive and their undo, one message per call on the caller's
 * handle: a message's labels read before it moves, the move itself, and its
 * row read back after an undo.  The service sequences and paces them.
 */

#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

#include "logging.h"
#include "tools/gmail_client.h"
#include "tools/gmail_client_internal.h"

/* A Gmail message id is 16 hex digits; anything much longer isn't one. */
#define GMAIL_MOVE_ID_MAX 64
/* Labels read from one message; more are ignored. */
#define GMAIL_MOVE_LABELS_MAX 64

static bool id_ok(const char *id) {
   return gmail_message_id_valid(id) && strnlen(id, GMAIL_MOVE_ID_MAX + 1) <= GMAIL_MOVE_ID_MAX;
}

/* GET /messages/{id}?<query>, parsed; NULL with @p err set. */
static struct json_object *get_message(CURL *curl,
                                       const char *token,
                                       const char *id,
                                       const char *query,
                                       email_err_t *err) {
   char url[sizeof(GMAIL_API_BASE) + GMAIL_MOVE_ID_MAX + 160];
   snprintf(url, sizeof(url), GMAIL_API_BASE "/messages/%s?%s", id, query);
   curl_buffer_t resp;
   long http_code = 0;
   CURLcode res = CURLE_OK;
   if (gmail_api_get_ex(curl, token, url, &resp, &http_code, &res) != 0) {
      *err = gmail_http_err(res, http_code);
      return NULL;
   }
   struct json_object *root = resp.data ? json_tokener_parse(resp.data) : NULL;
   curl_buffer_free(&resp);
   if (!root)
      *err = EMAIL_ERR_FAILED;
   return root;
}

int gmail_move_meta(CURL *curl,
                    const char *token,
                    const char *id,
                    gmail_move_meta_t *meta,
                    email_err_t *err) {
   memset(meta, 0, sizeof(*meta));
   *err = EMAIL_ERR_NOT_FOUND;
   if (!curl || !token || !id_ok(id))
      return 1;
   struct json_object *root = get_message(curl, token, id, "format=minimal", err);
   if (!root)
      return 1;
   struct json_object *arr = NULL;
   const char *labels[GMAIL_MOVE_LABELS_MAX];
   int n = 0;
   if (json_object_object_get_ex(root, "labelIds", &arr) &&
       json_object_is_type(arr, json_type_array)) {
      const size_t len = json_object_array_length(arr);
      for (size_t i = 0; i < len && n < GMAIL_MOVE_LABELS_MAX; i++) {
         const char *l = json_object_get_string(json_object_array_get_idx(arr, i));
         if (!l)
            continue;
         meta->in_inbox |= strcmp(l, "INBOX") == 0;
         meta->in_trash |= strcmp(l, "TRASH") == 0;
         meta->in_spam |= strcmp(l, "SPAM") == 0;
         labels[n++] = l;
      }
   }
   meta->dropped = gmail_labels_pack(labels, n, meta->keep, sizeof(meta->keep));
   json_object_put(root);
   *err = EMAIL_ERR_NONE;
   return 0;
}

int gmail_message_post(CURL *curl,
                       const char *token,
                       const char *id,
                       const char *action,
                       const char *body,
                       long *http_code,
                       email_err_t *err) {
   long code_local = 0;
   if (!http_code)
      http_code = &code_local;
   *http_code = 0;
   *err = EMAIL_ERR_NOT_FOUND;
   if (!curl || !token || !id_ok(id) || !action ||
       (strcmp(action, "trash") != 0 && strcmp(action, "untrash") != 0 &&
        strcmp(action, "modify") != 0))
      return 1;
   char url[sizeof(GMAIL_API_BASE) + GMAIL_MOVE_ID_MAX + 32];
   snprintf(url, sizeof(url), GMAIL_API_BASE "/messages/%s/%s", id, action);
   curl_buffer_t resp;
   CURLcode res = CURLE_OK;
   if (gmail_api_post_ex(curl, token, url, "application/json", body ? body : "{}", &resp, http_code,
                         &res) != 0) {
      *err = gmail_http_err(res, *http_code);
      OLOG_WARNING("gmail: %s of a message failed (HTTP %ld)", action, *http_code);
      return 1;
   }
   curl_buffer_free(&resp);
   *err = EMAIL_ERR_NONE;
   return 0;
}

int gmail_message_row(CURL *curl,
                      const char *token,
                      const char *id,
                      email_summary_t *row,
                      email_err_t *err) {
   *err = EMAIL_ERR_NOT_FOUND;
   if (!curl || !token || !id_ok(id) || !row)
      return 1;
   struct json_object *root = get_message(
       curl, token, id,
       "format=metadata&metadataHeaders=From&metadataHeaders=Subject"
       "&metadataHeaders=Date",
       err);
   if (!root)
      return 1;
   const int rc = gmail_summary_from_json(root, row);
   json_object_put(root);
   *err = rc == 0 ? EMAIL_ERR_NONE : EMAIL_ERR_FAILED;
   return rc;
}
