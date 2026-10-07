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
 * The Gmail client's own helpers, shared by its modules (gmail_client.c,
 * gmail_read.c, gmail_parts.c).  Not for use outside the Gmail client (and
 * its tests).
 */

#ifndef GMAIL_CLIENT_INTERNAL_H
#define GMAIL_CLIENT_INTERNAL_H

#include <curl/curl.h>
#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>

#include "core/curl_buffer.h"
#include "tools/email_mime.h"

#define GMAIL_API_BASE "https://gmail.googleapis.com/gmail/v1/users/me"

/** A curl handle for the Gmail API (timeouts, TLS, HTTPS only).  Caller cleans it up. */
CURL *gmail_create_curl(void);

/**
 * @brief GET a Gmail API URL into @p resp (initialized here; freed on failure)
 * @param http_code_out The HTTP status, 0 when the transfer never completed (may be NULL)
 * @param res_out       The transfer's result (may be NULL)
 * @return 0 on a 2xx, 1 otherwise
 */
int gmail_api_get_ex(CURL *curl,
                     const char *token,
                     const char *url,
                     curl_buffer_t *resp,
                     long *http_code_out,
                     CURLcode *res_out);

/** gmail_api_get_ex with a reply cap of @p max_bytes instead of the default 4 MB. */
int gmail_api_get_capped(CURL *curl,
                         const char *token,
                         const char *url,
                         size_t max_bytes,
                         curl_buffer_t *resp,
                         long *http_code_out,
                         CURLcode *res_out);

/** gmail_api_get_ex without the transfer result. */
int gmail_api_get(CURL *curl,
                  const char *token,
                  const char *url,
                  curl_buffer_t *resp,
                  long *http_code_out);

/**
 * @brief POST @p body to a Gmail API URL, reply into @p resp (freed on failure)
 * @param http_code_out As for gmail_api_get_ex (a 403 rate limit reads as 429)
 * @return 0 on a 2xx, 1 otherwise
 */
int gmail_api_post_ex(CURL *curl,
                      const char *token,
                      const char *url,
                      const char *content_type,
                      const char *body,
                      curl_buffer_t *resp,
                      long *http_code_out,
                      CURLcode *res_out);

/** Remove UNREAD from one message on @p curl's connection (gmail_flags.c). */
int gmail_mark_read(CURL *curl, const char *token, const char *message_id, email_err_t *err);

/** The email_err_t a failed Gmail call stands for (gmail_client.c). */
email_err_t gmail_http_err(CURLcode res, long http_code);

/** gmail_inbox_unread on the caller's handle (no new connection) (gmail_flags.c). */
int gmail_inbox_unread_on(CURL *curl, const char *token, int *unread, email_err_t *err);

/* The most of one text part read from a message's tree. */
#define GMAIL_TEXT_PART_MAX (4 * 1024 * 1024)

/* The reply cap for one part fetched by its attachmentId: the part's base64
 * (4/3 of GMAIL_TEXT_PART_MAX) plus the JSON around it.  A part that fits is
 * read whole; a bigger one reads as cut. */
#define GMAIL_PART_RESPONSE_MAX (GMAIL_TEXT_PART_MAX / 3 * 4 + 64 * 1024)

/** Whether @p c is a base64url character.  gmail_parts.c, as below. */
bool gmail_b64url_char(unsigned char c);

/**
 * @brief Decode base64url into a NUL-terminated heap buffer; caller frees
 * @param max_out At most this many bytes (0 = no limit)
 * @return NULL for empty input or when out of memory
 */
unsigned char *gmail_base64url_decode(const char *input, size_t max_out, size_t *out_len);

/** The value of header @p name in a Gmail payload headers array, or NULL. */
const char *gmail_find_header(struct json_object *headers_arr, const char *name);

/**
 * From, Subject and Date out of a Gmail payload headers array, for a listing
 * and for a read alike.  Gmail doesn't MIME-decode header values, so each is
 * decoded and stripped of invisible and direction-changing characters.  A From
 * with no address shows what it says, as the name.
 */
void gmail_header_fields(struct json_object *headers,
                         char *from_name,
                         size_t from_name_size,
                         char *from_addr,
                         size_t from_addr_size,
                         char *subject,
                         size_t subject_size,
                         char *date,
                         size_t date_size);

/** The string member @p key of @p obj, or NULL. */
const char *gmail_json_str(struct json_object *obj, const char *key);

/* A message's MIME tree (format=full payload) as the email_mime part list. */
typedef struct {
   email_mime_part_t *parts;    /* nodes in document order (email_mime.h) */
   const char **attachment_ids; /* per node: the Gmail attachmentId, or NULL (into the JSON) */
   unsigned char **owned;       /* per node: decoded text bytes, freed by gmail_parts_free */
   size_t *owned_len;           /* their lengths */
   bool ids_owned;              /* attachment_ids were copied out of the JSON (freed too) */
   int count;
   bool cut; /* a depth, part or id limit stopped the walk */
} gmail_parts_t;

/**
 * @brief Walk @p payload into @p out: part ids are IMAP section numbers, text
 *        parts decoded (still in their own charset), others behind their ids
 *
 * @p out's attachment ids point into @p payload until the caller copies them
 * (and sets ids_owned): keep the JSON alive until then.
 * @return 0, or 1 when out of memory; gmail_parts_free either way
 */
int gmail_parts_from_payload(struct json_object *payload, gmail_parts_t *out);

void gmail_parts_free(gmail_parts_t *p);

#endif /* GMAIL_CLIENT_INTERNAL_H */
