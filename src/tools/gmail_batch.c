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
 * The Gmail batch metadata fetch, without the network: the request body for
 * some of a listing's messages, the reply's parts matched back to those
 * messages by Content-ID, a message's row from its JSON, and search terms
 * cleaned for Gmail's query syntax.
 */

#include <ctype.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "tools/gmail_client.h"
#include "tools/gmail_client_internal.h"

/* Gmail ids are hex: anything else (a path, a query) is refused. */
bool gmail_message_id_valid(const char *id) {
   if (!id || !id[0])
      return false;
   for (const char *p = id; *p; p++) {
      if (!isxdigit((unsigned char)*p))
         return false;
   }
   return true;
}

/* Each part asks for the row's headers only. */
#define BATCH_PART_FMT                                                       \
   "--%s\r\n"                                                                \
   "Content-Type: application/http\r\n"                                      \
   "Content-ID: <%d>\r\n"                                                    \
   "\r\n"                                                                    \
   "GET /gmail/v1/users/me/messages/%s?format=metadata&metadataHeaders=From" \
   "&metadataHeaders=Subject&metadataHeaders=Date HTTP/1.1\r\n"              \
   "\r\n"

char *gmail_batch_body(const gmail_msg_id_t *ids, const int *which, int n) {
   size_t size = 64;
   for (int k = 0; k < n; k++)
      size += sizeof(BATCH_PART_FMT) + strlen(GMAIL_BATCH_BOUNDARY) + 16 + strlen(ids[which[k]].id);
   char *body = malloc(size);
   if (!body)
      return NULL;
   size_t pos = 0;
   for (int k = 0; k < n; k++) {
      const int i = which[k];
      if (!gmail_message_id_valid(ids[i].id))
         continue;
      pos += (size_t)snprintf(body + pos, size - pos, BATCH_PART_FMT, GMAIL_BATCH_BOUNDARY, i,
                              ids[i].id);
   }
   snprintf(body + pos, size - pos, "--%s--\r\n", GMAIL_BATCH_BOUNDARY);
   return body;
}

/* The listing index a reply part answers: its "Content-ID: <response-N>". */
static int part_index(const char *headers, const char *end) {
   static const char key[] = "Content-ID: <response-";
   const size_t klen = sizeof(key) - 1;
   const char *p = NULL;
   for (const char *q = headers; q + klen <= end; q++) {
      if (strncasecmp(q, key, klen) == 0) { /* header names ignore case */
         p = q;
         break;
      }
   }
   if (!p)
      return -1;
   p += sizeof(key) - 1;
   if (p >= end || !isdigit((unsigned char)*p))
      return -1;
   long v = strtol(p, NULL, 10);
   return v >= 0 && v < 1000000 ? (int)v : -1;
}

int gmail_batch_parse(const char *resp,
                      const char *boundary,
                      int n_ids,
                      email_summary_t *rows,
                      unsigned char *state) {
   if (!resp || !boundary || !boundary[0])
      return 0;
   char delim[300];
   const int dl = snprintf(delim, sizeof(delim), "--%s", boundary);
   if (dl <= 0 || (size_t)dl >= sizeof(delim))
      return 0;
   int done = 0;
   const char *pos = resp;
   while ((pos = strstr(pos, delim)) != NULL) {
      pos += dl;
      if (pos[0] == '-' && pos[1] == '-')
         break;
      const char *next = strstr(pos, delim);
      const char *end = next ? next : pos + strlen(pos);
      const char *outer_end = strstr(pos, "\r\n\r\n");
      if (!outer_end)
         break; /* no later part has headers either */
      if (outer_end >= end)
         continue;
      const int i = part_index(pos, outer_end);
      if (i < 0 || i >= n_ids || state[i] != GMAIL_BATCH_PENDING)
         continue;
      const char *http = outer_end + 4;
      int code = 0;
      if (sscanf(http, "HTTP/1.1 %d", &code) != 1)
         continue;
      /* Refused as too many requests (429, or 403 rateLimitExceeded), a passing
       * server error, or a lapsed token: asked for again, and counted missing
       * if never answered, so a row is never dropped without a word. */
      if (code == 429 || code == 403 || code == 401 || code >= 500)
         continue;
      if (code != 200) {
         state[i] = GMAIL_BATCH_GONE; /* deleted since the listing, and the like */
         continue;
      }
      const char *json = strstr(http, "\r\n\r\n");
      if (!json || json >= end)
         continue;
      json += 4;
      struct json_tokener *tok = json_tokener_new();
      struct json_object *msg = tok ? json_tokener_parse_ex(tok, json, (int)(end - json)) : NULL;
      json_tokener_free(tok);
      if (msg && gmail_summary_from_json(msg, &rows[i]) == 0) {
         state[i] = GMAIL_BATCH_DONE;
         done++;
      }
      json_object_put(msg);
   }
   return done;
}

void gmail_query_term(const char *src, char *dst, size_t dst_len) {
   size_t j = 0;
   for (size_t i = 0; src[i] && j + 1 < dst_len; i++) {
      const unsigned char c = (unsigned char)src[i];
      /* A quote would end the phrase it goes into; a currency sign makes Gmail
       * match nothing ("$850,000" finds none of the mail "850,000" finds). */
      if (c == '"' || c == '$')
         continue;
      if (c == 0xE2 && (unsigned char)src[i + 1] == 0x82 && (unsigned char)src[i + 2] == 0xAC) {
         i += 2; /* € */
         continue;
      }
      if (c == 0xC2 && ((unsigned char)src[i + 1] == 0xA3 || (unsigned char)src[i + 1] == 0xA5)) {
         i += 1; /* £ ¥ */
         continue;
      }
      dst[j++] = src[i];
   }
   dst[j] = '\0';
}

int gmail_summary_from_json(struct json_object *root, email_summary_t *out) {
   memset(out, 0, sizeof(*out));

   /* Extract message ID */
   struct json_object *id_obj = NULL;
   if (json_object_object_get_ex(root, "id", &id_obj)) {
      const char *id_str = json_object_get_string(id_obj);
      if (id_str)
         snprintf(out->message_id, sizeof(out->message_id), "%s", id_str);
   }

   /* Extract headers */
   struct json_object *payload = NULL;
   struct json_object *headers = NULL;
   if (json_object_object_get_ex(root, "payload", &payload))
      json_object_object_get_ex(payload, "headers", &headers);

   /* Gmail's API does NOT MIME-decode header values (only the snippet).  The
    * same decoding and display rules as reading a message: the sender's words
    * decoded, invisible and direction-changing characters dropped. */
   gmail_header_fields(headers, out->from_name, sizeof(out->from_name), out->from_addr,
                       sizeof(out->from_addr), out->subject, sizeof(out->subject), out->date_str,
                       sizeof(out->date_str));

   /* Parse snippet as preview */
   struct json_object *snippet_obj = NULL;
   if (json_object_object_get_ex(root, "snippet", &snippet_obj)) {
      const char *snippet = json_object_get_string(snippet_obj);
      if (snippet)
         snprintf(out->preview, sizeof(out->preview), "%s", snippet);
   }

   /* Parse internalDate for sorting (epoch milliseconds) */
   struct json_object *internal_date_obj = NULL;
   if (json_object_object_get_ex(root, "internalDate", &internal_date_obj)) {
      const char *ms_str = json_object_get_string(internal_date_obj);
      if (ms_str)
         out->date = (time_t)(strtoll(ms_str, NULL, 10) / 1000);
   }

   /* Thread id — used by the digest for reply detection + dedup grouping. */
   struct json_object *thread_obj = NULL;
   if (json_object_object_get_ex(root, "threadId", &thread_obj)) {
      const char *tid = json_object_get_string(thread_obj);
      if (tid)
         snprintf(out->thread_id, sizeof(out->thread_id), "%s", tid);
   }

   /* labelIds → read/importance/category flags.  Present in format=metadata and
    * format=minimal responses; absent when a caller uses format=full without
    * asking for labels, in which case the flags stay at their zero defaults. */
   struct json_object *labels = NULL;
   if (json_object_object_get_ex(root, "labelIds", &labels) &&
       json_object_is_type(labels, json_type_array)) {
      size_t n = json_object_array_length(labels);
      for (size_t i = 0; i < n; i++) {
         const char *lbl = json_object_get_string(json_object_array_get_idx(labels, i));
         if (!lbl)
            continue;
         if (strcmp(lbl, "UNREAD") == 0)
            out->unread = true;
         else if (strcmp(lbl, "IMPORTANT") == 0)
            out->important = true;
         else if (strcmp(lbl, "STARRED") == 0)
            out->starred = true;
         else if (strcmp(lbl, "SENT") == 0)
            out->from_me = true;
         else if (strcmp(lbl, "CATEGORY_SOCIAL") == 0)
            out->category = EMAIL_CAT_SOCIAL;
         else if (strcmp(lbl, "CATEGORY_PROMOTIONS") == 0)
            out->category = EMAIL_CAT_PROMOTIONS;
         else if (strcmp(lbl, "CATEGORY_UPDATES") == 0)
            out->category = EMAIL_CAT_UPDATES;
         else if (strcmp(lbl, "CATEGORY_FORUMS") == 0)
            out->category = EMAIL_CAT_FORUMS;
      }
   }

   return 0;
}
