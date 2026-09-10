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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Gmail REST API client — fetches and sends email via Gmail API v1.
 *
 * Security notes:
 * - Message IDs validated as hex-only before URL interpolation (path traversal prevention)
 * - Authorization header wiped with sodium_memzero() after each curl call
 * - HTTPS-only with certificate verification enforced
 * - Response size capped at GMAIL_MAX_RESPONSE_SIZE (4 MB)
 * - Header injection prevention via sanitize_header_value() on all user fields
 * - Base64url decode: text/plain capped at max_body_chars, text/html capped at 2 MB
 * - Gmail search params quoted to prevent query injection
 * - Batch API used for metadata fetches (2 HTTP calls instead of N+1)
 */

#define _GNU_SOURCE /* strcasestr */

#include "tools/gmail_client.h"

#include <ctype.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <sodium.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/curl_buffer.h"
#include "logging.h"
#include "tools/html_parser.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define GMAIL_API_BASE "https://gmail.googleapis.com/gmail/v1/users/me"
#define GMAIL_BATCH_URL "https://gmail.googleapis.com/batch/gmail/v1"
#define GMAIL_BATCH_BOUNDARY "dawn_gmail_batch"
#define GMAIL_MAX_RESPONSE_SIZE (4 * 1024 * 1024) /* 4 MB */
#define GMAIL_CURL_TIMEOUT 30
#define GMAIL_MIME_MAX_DEPTH 10

/* =============================================================================
 * Response Buffer
 *
 * Uses the shared curl_buffer_t (include/core/curl_buffer.h) with a 4 MB
 * cap on Gmail REST responses (large mailbox listings, full message
 * bodies).  The shared helper truncates with a flag instead of aborting
 * mid-transfer — gmail_api_get/post check `resp->truncated` after
 * curl_easy_perform and reject the response.
 * ============================================================================= */

/* =============================================================================
 * Message ID Validation
 *
 * Gmail message IDs are hex strings (e.g., "18e4a2b3c4d5e6f7").
 * Reject anything with non-hex chars to prevent path traversal.
 * ============================================================================= */

static bool is_valid_gmail_id(const char *id) {
   if (!id || !id[0])
      return false;
   for (const char *p = id; *p; p++) {
      if (!isxdigit((unsigned char)*p))
         return false;
   }
   return true;
}

/* =============================================================================
 * HTTP Helpers
 *
 * Reuse a CURL handle for HTTP keepalive (single TLS handshake per operation).
 * Authorization header is wiped after each call.
 * ============================================================================= */

static CURL *gmail_create_curl(void) {
   CURL *curl = curl_easy_init();
   if (!curl)
      return NULL;

   curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)GMAIL_CURL_TIMEOUT);
   DAWN_CURL_SET_PROTOCOLS(curl, "https");
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);

   return curl;
}

/* GET a Gmail API URL.  When @p http_code_out is non-NULL it receives the HTTP
 * status (0 if the transfer never completed), so callers can distinguish a 404
 * (message/resource not found) from a transport/network failure. */
static int gmail_api_get(CURL *curl,
                         const char *token,
                         const char *url,
                         curl_buffer_t *resp,
                         long *http_code_out) {
   if (http_code_out)
      *http_code_out = 0;
   curl_buffer_init_with_max(resp, GMAIL_MAX_RESPONSE_SIZE);

   char auth_header[2112];
   snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", token);

   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, auth_header);

   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
   curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);

   CURLcode res = curl_easy_perform(curl);

   sodium_memzero(auth_header, sizeof(auth_header));
   curl_slist_free_all(headers);

   if (res != CURLE_OK) {
      OLOG_ERROR("gmail: API request failed: %s", curl_easy_strerror(res));
      curl_buffer_free(resp);
      return 1;
   }

   if (resp->truncated) {
      OLOG_ERROR("gmail: API response exceeded %d byte cap; rejecting", GMAIL_MAX_RESPONSE_SIZE);
      curl_buffer_free(resp);
      return 1;
   }

   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
   if (http_code_out)
      *http_code_out = http_code;
   if (http_code < 200 || http_code >= 300) {
      OLOG_ERROR("gmail: API request returned HTTP %ld", http_code);
      curl_buffer_free(resp);
      return 1;
   }

   return 0;
}

static int gmail_api_post(CURL *curl,
                          const char *token,
                          const char *url,
                          const char *content_type,
                          const char *body,
                          curl_buffer_t *resp) {
   curl_buffer_init_with_max(resp, GMAIL_MAX_RESPONSE_SIZE);

   char auth_header[2112];
   snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", token);

   char ct_header[256];
   snprintf(ct_header, sizeof(ct_header), "Content-Type: %s", content_type);

   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, auth_header);
   headers = curl_slist_append(headers, ct_header);

   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);

   CURLcode res = curl_easy_perform(curl);

   sodium_memzero(auth_header, sizeof(auth_header));
   curl_slist_free_all(headers);

   if (res != CURLE_OK) {
      OLOG_ERROR("gmail: API POST failed: %s", curl_easy_strerror(res));
      curl_buffer_free(resp);
      return 1;
   }

   if (resp->truncated) {
      OLOG_ERROR("gmail: API POST response exceeded %d byte cap; rejecting",
                 GMAIL_MAX_RESPONSE_SIZE);
      curl_buffer_free(resp);
      return 1;
   }

   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
   if (http_code < 200 || http_code >= 300) {
      OLOG_ERROR("gmail: API POST returned HTTP %ld", http_code);
      curl_buffer_free(resp);
      return 1;
   }

   return 0;
}

/* =============================================================================
 * Base64url Codec
 *
 * Gmail encodes message bodies in base64url (RFC 4648 §5): A-Za-z0-9-_,
 * no padding. We need decode (for reading) and encode (for sending).
 * ============================================================================= */

static const char BASE64URL_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* Lookup table: 256 bytes of .rodata, replaces 5 branches per character */
static const int8_t BASE64URL_LUT[256] = {
   ['A'] = 0,  ['B'] = 1,  ['C'] = 2,  ['D'] = 3,  ['E'] = 4,  ['F'] = 5,  ['G'] = 6,  ['H'] = 7,
   ['I'] = 8,  ['J'] = 9,  ['K'] = 10, ['L'] = 11, ['M'] = 12, ['N'] = 13, ['O'] = 14, ['P'] = 15,
   ['Q'] = 16, ['R'] = 17, ['S'] = 18, ['T'] = 19, ['U'] = 20, ['V'] = 21, ['W'] = 22, ['X'] = 23,
   ['Y'] = 24, ['Z'] = 25, ['a'] = 26, ['b'] = 27, ['c'] = 28, ['d'] = 29, ['e'] = 30, ['f'] = 31,
   ['g'] = 32, ['h'] = 33, ['i'] = 34, ['j'] = 35, ['k'] = 36, ['l'] = 37, ['m'] = 38, ['n'] = 39,
   ['o'] = 40, ['p'] = 41, ['q'] = 42, ['r'] = 43, ['s'] = 44, ['t'] = 45, ['u'] = 46, ['v'] = 47,
   ['w'] = 48, ['x'] = 49, ['y'] = 50, ['z'] = 51, ['0'] = 52, ['1'] = 53, ['2'] = 54, ['3'] = 55,
   ['4'] = 56, ['5'] = 57, ['6'] = 58, ['7'] = 59, ['8'] = 60, ['9'] = 61, ['-'] = 62, ['_'] = 63,
};

/** Check if a character is a valid base64url character */
static bool is_b64url_char(unsigned char c) {
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
          c == '_';
}

/**
 * Decode base64url to heap-allocated buffer. Caller frees.
 * @param max_out  Maximum decoded bytes (0 = no limit)
 * @param out_len  Output: number of decoded bytes
 * @return Heap-allocated buffer, or NULL on failure
 */
static unsigned char *base64url_decode(const char *input, size_t max_out, size_t *out_len) {
   if (!input || !input[0]) {
      *out_len = 0;
      return NULL;
   }

   size_t in_len = strlen(input);
   size_t alloc_size = (in_len * 3) / 4 + 4;
   if (max_out > 0 && alloc_size > max_out + 2)
      alloc_size = max_out + 2; /* +2: room for null termination by callers */

   unsigned char *out = malloc(alloc_size);
   if (!out)
      return NULL;

   size_t pos = 0;
   size_t i = 0;
   while (i < in_len) {
      unsigned char ca = (i < in_len) ? (unsigned char)input[i++] : 0;
      unsigned char cb = (i < in_len) ? (unsigned char)input[i++] : 0;
      unsigned char cc = (i < in_len) ? (unsigned char)input[i++] : 0;
      unsigned char cd = (i < in_len) ? (unsigned char)input[i++] : 0;

      if (!is_b64url_char(ca) || !is_b64url_char(cb))
         break;

      int a = BASE64URL_LUT[ca];
      int b = BASE64URL_LUT[cb];
      bool have_c = is_b64url_char(cc);
      bool have_d = is_b64url_char(cd);

      uint32_t triple = ((uint32_t)a << 18) | ((uint32_t)b << 12);
      if (have_c)
         triple |= ((uint32_t)BASE64URL_LUT[cc] << 6);
      if (have_d)
         triple |= (uint32_t)BASE64URL_LUT[cd];

      if (pos < alloc_size)
         out[pos++] = (triple >> 16) & 0xFF;
      if (have_c && pos < alloc_size)
         out[pos++] = (triple >> 8) & 0xFF;
      if (have_d && pos < alloc_size)
         out[pos++] = triple & 0xFF;

      if (max_out > 0 && pos >= max_out)
         break;
   }

   *out_len = pos;
   return out;
}

static size_t base64url_encode(const unsigned char *data, size_t len, char *out, size_t out_len) {
   size_t needed = ((len * 4) / 3) + 4;
   if (out_len < needed)
      return 0;

   size_t pos = 0;
   size_t i = 0;
   while (i < len) {
      size_t start = i;
      uint32_t octet_a = data[i++];
      uint32_t octet_b = i < len ? data[i++] : 0;
      uint32_t octet_c = i < len ? data[i++] : 0;
      uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

      size_t remaining = len - start;
      out[pos++] = BASE64URL_CHARS[(triple >> 18) & 0x3F];
      out[pos++] = BASE64URL_CHARS[(triple >> 12) & 0x3F];
      if (remaining > 1)
         out[pos++] = BASE64URL_CHARS[(triple >> 6) & 0x3F];
      if (remaining > 2)
         out[pos++] = BASE64URL_CHARS[triple & 0x3F];
   }
   out[pos] = '\0';
   return pos;
}

/* =============================================================================
 * Header Injection Prevention
 * ============================================================================= */

static void sanitize_header_value(const char *src, char *dst, size_t dst_len) {
   size_t j = 0;
   for (size_t i = 0; src[i] && j < dst_len - 1; i++) {
      if (src[i] != '\r' && src[i] != '\n')
         dst[j++] = src[i];
   }
   dst[j] = '\0';
}

/* =============================================================================
 * URL Encoding (for query parameters)
 * ============================================================================= */

static size_t url_encode(const char *str, char *out, size_t out_len) {
   static const char hex[] = "0123456789ABCDEF";
   size_t pos = 0;
   for (const char *p = str; *p && pos + 3 < out_len; p++) {
      unsigned char c = (unsigned char)*p;
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
          c == '_' || c == '.' || c == '~') {
         out[pos++] = c;
      } else {
         out[pos++] = '%';
         out[pos++] = hex[c >> 4];
         out[pos++] = hex[c & 0x0F];
      }
   }
   out[pos] = '\0';
   return pos;
}

/* =============================================================================
 * MIME Body Extraction
 *
 * Gmail API returns message body in a nested MIME parts tree.
 * Prefer text/plain, fall back to text/html with full HTML-to-text conversion
 * via html_parser (strips style/script blocks, entities, whitespace, etc.).
 * ============================================================================= */

/**
 * Recursively extract text body from Gmail MIME payload.
 * @param payload  JSON object (Gmail message payload or part)
 * @param out      Output buffer
 * @param out_len  Size of output buffer
 * @param depth    Recursion depth (capped at GMAIL_MIME_MAX_DEPTH)
 * @return 1 if text/plain found, 2 if text/html found, 0 if nothing found
 */
static int extract_text_body(struct json_object *payload, char *out, size_t out_len, int depth) {
   if (!payload || depth > GMAIL_MIME_MAX_DEPTH)
      return 0;

   /* Check this part's mimeType */
   struct json_object *mime_obj = NULL;
   const char *mime_type = NULL;
   if (json_object_object_get_ex(payload, "mimeType", &mime_obj))
      mime_type = json_object_get_string(mime_obj);

   /* If this is a leaf part with body data */
   struct json_object *body_obj = NULL;
   if (json_object_object_get_ex(payload, "body", &body_obj)) {
      struct json_object *data_obj = NULL;
      if (json_object_object_get_ex(body_obj, "data", &data_obj)) {
         const char *b64data = json_object_get_string(data_obj);
         if (b64data && b64data[0] && mime_type) {
            if (strcasecmp(mime_type, "text/plain") == 0) {
               size_t decoded_len = 0;
               unsigned char *decoded = base64url_decode(b64data, out_len - 1, &decoded_len);
               if (decoded) {
                  decoded[decoded_len] = '\0';
                  /* Some senders (Patreon/Mailgun) embed HTML tags in text/plain parts.
                   * Detect and strip them so tracking URLs don't pollute the body. */
                  bool has_html = false;
                  size_t scan_limit = decoded_len < 256 ? decoded_len : 256;
                  for (size_t si = 0; si < scan_limit; si++) {
                     if (decoded[si] == '<' &&
                         (isalpha((unsigned char)decoded[si + 1]) ||
                          (decoded[si + 1] == '/' && isalpha((unsigned char)decoded[si + 2])))) {
                        has_html = true;
                        break;
                     }
                  }
                  if (has_html) {
                     char *cleaned = NULL;
                     if (html_extract_text_plain((const char *)decoded, decoded_len, &cleaned) ==
                             HTML_PARSE_SUCCESS &&
                         cleaned) {
                        size_t copy_len = strlen(cleaned);
                        if (copy_len >= out_len)
                           copy_len = out_len - 1;
                        memcpy(out, cleaned, copy_len);
                        out[copy_len] = '\0';
                        free(cleaned);
                        free(decoded);
                        return 1;
                     }
                  }
                  size_t copy_len = decoded_len < out_len - 1 ? decoded_len : out_len - 1;
                  memcpy(out, decoded, copy_len);
                  out[copy_len] = '\0';
                  free(decoded);
                  return 1;
               }
            }
            if (strcasecmp(mime_type, "text/html") == 0 && out[0] == '\0') {
               size_t decoded_len = 0;
               /* Decode full HTML — marketing emails can be 30KB+ of HTML that
                * compress to a few hundred chars of text. Cap at 2MB to prevent
                * adversarial emails from causing unbounded allocation. */
               unsigned char *decoded = base64url_decode(b64data, 2 * 1024 * 1024, &decoded_len);
               if (decoded) {
                  decoded[decoded_len] = '\0';
                  char *extracted = NULL;
                  if (html_extract_text_plain((const char *)decoded, decoded_len, &extracted) ==
                          HTML_PARSE_SUCCESS &&
                      extracted) {
                     size_t copy_len = strlen(extracted);
                     if (copy_len >= out_len)
                        copy_len = out_len - 1;
                     memcpy(out, extracted, copy_len);
                     out[copy_len] = '\0';
                     free(extracted);
                  }
                  free(decoded);
                  return 2;
               }
            }
         }
      }
   }

   /* Recurse into parts */
   struct json_object *parts_obj = NULL;
   if (json_object_object_get_ex(payload, "parts", &parts_obj) &&
       json_object_is_type(parts_obj, json_type_array)) {
      int parts_len = json_object_array_length(parts_obj);
      int best = 0;
      for (int i = 0; i < parts_len; i++) {
         struct json_object *part = json_object_array_get_idx(parts_obj, i);
         int result = extract_text_body(part, out, out_len, depth + 1);
         if (result == 1)
            return 1; /* text/plain found — stop immediately */
         if (result > best)
            best = result;
      }
      return best;
   }

   return 0;
}

/** Count attachments in MIME parts (parts with filename in disposition) */
static int count_attachments(struct json_object *payload, int depth) {
   if (!payload || depth > GMAIL_MIME_MAX_DEPTH)
      return 0;

   int count = 0;

   /* Check if this part has a filename */
   struct json_object *filename_obj = NULL;
   if (json_object_object_get_ex(payload, "filename", &filename_obj)) {
      const char *filename = json_object_get_string(filename_obj);
      if (filename && filename[0])
         count++;
   }

   /* Recurse into parts */
   struct json_object *parts_obj = NULL;
   if (json_object_object_get_ex(payload, "parts", &parts_obj) &&
       json_object_is_type(parts_obj, json_type_array)) {
      int parts_len = json_object_array_length(parts_obj);
      for (int i = 0; i < parts_len; i++) {
         count += count_attachments(json_object_array_get_idx(parts_obj, i), depth + 1);
      }
   }

   return count;
}

/* =============================================================================
 * Header Parsing
 *
 * Extract headers from Gmail API metadata format (payload.headers array).
 * ============================================================================= */

static const char *find_gmail_header(struct json_object *headers_arr, const char *name) {
   if (!headers_arr || !json_object_is_type(headers_arr, json_type_array))
      return NULL;

   int len = json_object_array_length(headers_arr);
   for (int i = 0; i < len; i++) {
      struct json_object *header = json_object_array_get_idx(headers_arr, i);
      struct json_object *name_obj = NULL;
      if (json_object_object_get_ex(header, "name", &name_obj)) {
         if (strcasecmp(json_object_get_string(name_obj), name) == 0) {
            struct json_object *val_obj = NULL;
            if (json_object_object_get_ex(header, "value", &val_obj))
               return json_object_get_string(val_obj);
         }
      }
   }
   return NULL;
}

/** Parse "Display Name <email@addr>" or bare "email@addr" */
static void parse_from_field(const char *from,
                             char *name_out,
                             size_t name_len,
                             char *addr_out,
                             size_t addr_len) {
   name_out[0] = '\0';
   addr_out[0] = '\0';
   if (!from || !from[0])
      return;

   const char *lt = strchr(from, '<');
   const char *gt = strchr(from, '>');
   if (lt && gt && gt > lt) {
      /* Copy name (trim trailing spaces and quotes) */
      size_t nlen = lt - from;
      while (nlen > 0 && (from[nlen - 1] == ' ' || from[nlen - 1] == '"'))
         nlen--;
      const char *nstart = from;
      while (nlen > 0 && (*nstart == ' ' || *nstart == '"')) {
         nstart++;
         nlen--;
      }
      if (nlen > 0) {
         size_t copy = nlen < name_len - 1 ? nlen : name_len - 1;
         memcpy(name_out, nstart, copy);
         name_out[copy] = '\0';
      }

      /* Copy address */
      size_t alen = gt - lt - 1;
      size_t copy = alen < addr_len - 1 ? alen : addr_len - 1;
      memcpy(addr_out, lt + 1, copy);
      addr_out[copy] = '\0';
   } else {
      snprintf(addr_out, addr_len, "%s", from);
   }
}

/* =============================================================================
 * Search Query Builder
 *
 * Map email_search_params_t to Gmail search syntax.
 * Values are quoted to prevent Gmail query injection.
 * ============================================================================= */

/** Strip double quotes from a string to prevent Gmail query injection */
static void strip_quotes(const char *src, char *dst, size_t dst_len) {
   size_t j = 0;
   for (size_t i = 0; src[i] && j < dst_len - 1; i++) {
      if (src[i] != '"')
         dst[j++] = src[i];
   }
   dst[j] = '\0';
}

static void build_search_query(const email_search_params_t *params,
                               const char *label_query,
                               char *query,
                               size_t query_len) {
   int pos = 0;

   if (label_query && label_query[0])
      pos += snprintf(query + pos, query_len - pos, "%s", label_query);

   if (params->from[0]) {
      char safe[128];
      strip_quotes(params->from, safe, sizeof(safe));
      if (pos > 0)
         query[pos++] = ' ';
      pos += snprintf(query + pos, query_len - pos, "from:\"%s\"", safe);
   }

   if (params->subject[0]) {
      char safe[128];
      strip_quotes(params->subject, safe, sizeof(safe));
      if (pos > 0)
         query[pos++] = ' ';
      pos += snprintf(query + pos, query_len - pos, "subject:\"%s\"", safe);
   }

   if (params->text[0]) {
      char safe[128];
      strip_quotes(params->text, safe, sizeof(safe));
      if (pos > 0)
         query[pos++] = ' ';
      pos += snprintf(query + pos, query_len - pos, "\"%s\"", safe);
   }

   if (params->since[0]) {
      /* Convert YYYY-MM-DD to YYYY/MM/DD for Gmail */
      char date_buf[16];
      snprintf(date_buf, sizeof(date_buf), "%s", params->since);
      for (char *p = date_buf; *p; p++) {
         if (*p == '-')
            *p = '/';
      }
      if (pos > 0)
         query[pos++] = ' ';
      pos += snprintf(query + pos, query_len - pos, "after:%s", date_buf);
   }

   if (params->before[0]) {
      char date_buf[16];
      snprintf(date_buf, sizeof(date_buf), "%s", params->before);
      for (char *p = date_buf; *p; p++) {
         if (*p == '-')
            *p = '/';
      }
      if (pos > 0)
         query[pos++] = ' ';
      pos += snprintf(query + pos, query_len - pos, "before:%s", date_buf);
   }

   if (params->unread_only) {
      if (pos > 0)
         query[pos++] = ' ';
      pos += snprintf(query + pos, query_len - pos, "is:unread");
   }

   if (pos < (int)query_len)
      query[pos] = '\0';
}

/* =============================================================================
 * Message JSON Parser
 *
 * Parse a Gmail message JSON object into email_summary_t.
 * Shared by both batch and single-message fetch paths.
 * ============================================================================= */

static int parse_message_json(struct json_object *root, email_summary_t *out) {
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

   const char *from = find_gmail_header(headers, "From");
   parse_from_field(from, out->from_name, sizeof(out->from_name), out->from_addr,
                    sizeof(out->from_addr));

   const char *subject = find_gmail_header(headers, "Subject");
   if (subject)
      snprintf(out->subject, sizeof(out->subject), "%s", subject);

   const char *date = find_gmail_header(headers, "Date");
   if (date)
      snprintf(out->date_str, sizeof(out->date_str), "%s", date);

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

/* =============================================================================
 * Message ID List Fetcher
 *
 * Fetch message IDs from Gmail list/search endpoint.
 * ============================================================================= */

typedef struct {
   char id[64];
} gmail_msg_id_t;

static int fetch_message_ids(CURL *curl,
                             const char *token,
                             const char *query,
                             int max_results,
                             const char *page_token,
                             gmail_msg_id_t *ids,
                             int max_ids,
                             int *out_count,
                             char *next_page_token,
                             size_t npt_len) {
   *out_count = 0;
   if (next_page_token && npt_len > 0)
      next_page_token[0] = '\0';

   /* Build URL with query */
   char encoded_query[1024];
   url_encode(query, encoded_query, sizeof(encoded_query));

   char url[2048];
   int url_pos = snprintf(url, sizeof(url), GMAIL_API_BASE "/messages?maxResults=%d&q=%s",
                          max_results > max_ids ? max_ids : max_results, encoded_query);

   /* Append pagination token if provided */
   if (page_token && page_token[0]) {
      char encoded_pt[512];
      url_encode(page_token, encoded_pt, sizeof(encoded_pt));
      snprintf(url + url_pos, sizeof(url) - url_pos, "&pageToken=%s", encoded_pt);
   }

   curl_buffer_t resp;
   if (gmail_api_get(curl, token, url, &resp, NULL) != 0)
      return 1;

   struct json_object *root = json_tokener_parse(resp.data);
   curl_buffer_free(&resp);
   if (!root) {
      OLOG_ERROR("gmail: failed to parse message-list JSON response");
      return 1;
   }

   struct json_object *messages = NULL;
   if (!json_object_object_get_ex(root, "messages", &messages) ||
       !json_object_is_type(messages, json_type_array)) {
      /* No messages — not an error, just empty */
      json_object_put(root);
      return 0;
   }

   int msg_count = json_object_array_length(messages);
   if (msg_count > max_ids)
      msg_count = max_ids;

   for (int i = 0; i < msg_count; i++) {
      struct json_object *msg = json_object_array_get_idx(messages, i);
      struct json_object *id_obj_inner = NULL;
      if (json_object_object_get_ex(msg, "id", &id_obj_inner)) {
         const char *id_str = json_object_get_string(id_obj_inner);
         if (id_str)
            snprintf(ids[i].id, sizeof(ids[i].id), "%s", id_str);
      }
   }

   /* Capture next page token for pagination */
   if (next_page_token && npt_len > 0) {
      struct json_object *npt_obj = NULL;
      if (json_object_object_get_ex(root, "nextPageToken", &npt_obj)) {
         const char *npt = json_object_get_string(npt_obj);
         if (npt)
            snprintf(next_page_token, npt_len, "%s", npt);
      }
   }

   *out_count = msg_count;
   json_object_put(root);
   return 0;
}

/* =============================================================================
 * Batch Metadata Fetcher
 *
 * Fetch metadata for multiple messages in a single HTTP request using the
 * Gmail batch API. Reduces N+1 round-trips to 2 (1 list + 1 batch).
 * ============================================================================= */

/**
 * Build a multipart/mixed batch request body for metadata fetches.
 * Each part requests format=metadata with From/Subject/Date headers.
 * @return Heap-allocated batch body string, or NULL on failure
 */
static char *build_batch_request(const gmail_msg_id_t *ids, int count) {
   /* Estimate: ~256 bytes per message part + boundary overhead */
   size_t alloc_size = (size_t)count * 300 + 128;
   char *body = malloc(alloc_size);
   if (!body)
      return NULL;

   int pos = 0;
   for (int i = 0; i < count; i++) {
      if (!is_valid_gmail_id(ids[i].id))
         continue;

      pos += snprintf(body + pos, alloc_size - pos,
                      "--%s\r\n"
                      "Content-Type: application/http\r\n"
                      "Content-ID: <%d>\r\n"
                      "\r\n"
                      "GET /gmail/v1/users/me/messages/%s"
                      "?format=metadata"
                      "&metadataHeaders=From"
                      "&metadataHeaders=Subject"
                      "&metadataHeaders=Date HTTP/1.1\r\n"
                      "\r\n",
                      GMAIL_BATCH_BOUNDARY, i, ids[i].id);
   }
   pos += snprintf(body + pos, alloc_size - pos, "--%s--\r\n", GMAIL_BATCH_BOUNDARY);

   return body;
}

/**
 * Extract the boundary string from a Content-Type header.
 * @param content_type  e.g. "multipart/mixed; boundary=batch_abc123"
 * @param boundary      Output buffer
 * @param boundary_len  Size of output buffer
 * @return 0 on success, 1 if no boundary found
 */
static int extract_boundary(const char *content_type, char *boundary, size_t boundary_len) {
   if (!content_type)
      return 1;

   const char *bp = strstr(content_type, "boundary=");
   if (!bp)
      return 1;

   bp += 9; /* Skip "boundary=" */

   /* Handle quoted boundary */
   if (*bp == '"') {
      bp++;
      const char *end = strchr(bp, '"');
      if (!end)
         return 1;
      size_t len = end - bp;
      if (len >= boundary_len)
         len = boundary_len - 1;
      memcpy(boundary, bp, len);
      boundary[len] = '\0';
   } else {
      /* Unquoted: read until whitespace, semicolon, or end */
      size_t len = 0;
      while (bp[len] && bp[len] != ' ' && bp[len] != ';' && bp[len] != '\r' && bp[len] != '\n' &&
             len < boundary_len - 1) {
         len++;
      }
      memcpy(boundary, bp, len);
      boundary[len] = '\0';
   }

   return 0;
}

/**
 * Parse batch response and populate email summaries.
 * @param resp_data     Raw batch response body (multipart/mixed)
 * @param boundary      Response boundary string (from Content-Type)
 * @param out           Output array
 * @param max_out       Size of output array
 * @param out_count     Output: number of results
 * @return 0 on success
 */
static int parse_batch_response(const char *resp_data,
                                const char *boundary,
                                email_summary_t *out,
                                int max_out,
                                int *out_count) {
   *out_count = 0;

   /* Build delimiter: "--boundary" (sized for "--" + longest boundary + NUL). */
   char delim[512];
   snprintf(delim, sizeof(delim), "--%s", boundary);
   size_t delim_len = strlen(delim);

   const char *pos = resp_data;
   int fetched = 0;

   while (fetched < max_out) {
      /* Find next boundary */
      pos = strstr(pos, delim);
      if (!pos)
         break;
      pos += delim_len;

      /* Check for closing boundary (--boundary--) */
      if (pos[0] == '-' && pos[1] == '-')
         break;

      /* Skip outer headers: find \r\n\r\n */
      const char *outer_end = strstr(pos, "\r\n\r\n");
      if (!outer_end)
         break;
      pos = outer_end + 4;

      /* Now at inner HTTP response: "HTTP/1.1 200 OK\r\n..." */
      /* Check for 200 status */
      if (strncmp(pos, "HTTP/1.1 200", 12) != 0) {
         /* Non-200 part — skip this message */
         continue;
      }

      /* Find inner headers end: \r\n\r\n */
      const char *inner_end = strstr(pos, "\r\n\r\n");
      if (!inner_end)
         break;
      const char *json_start = inner_end + 4;

      /* Parse JSON body (find end by looking for next boundary or end of data) */
      const char *json_end = strstr(json_start, delim);
      if (!json_end)
         json_end = json_start + strlen(json_start);

      /* Trim trailing whitespace */
      while (json_end > json_start && (json_end[-1] == '\r' || json_end[-1] == '\n'))
         json_end--;

      size_t json_len = json_end - json_start;
      if (json_len == 0)
         continue;

      /* Parse the JSON fragment */
      struct json_tokener *tok = json_tokener_new();
      struct json_object *msg_json = tok ? json_tokener_parse_ex(tok, json_start, json_len) : NULL;
      json_tokener_free(tok);
      if (!msg_json) {
         /* Try with a null-terminated copy */
         char *json_copy = malloc(json_len + 1);
         if (json_copy) {
            memcpy(json_copy, json_start, json_len);
            json_copy[json_len] = '\0';
            msg_json = json_tokener_parse(json_copy);
            free(json_copy);
         }
      }

      if (msg_json) {
         if (parse_message_json(msg_json, &out[fetched]) == 0)
            fetched++;
         json_object_put(msg_json);
      }
   }

   *out_count = fetched;
   return 0;
}

/**
 * Fetch metadata for multiple messages using Gmail batch API.
 * Falls back to individual fetches if batch fails.
 *
 * Stack budget: ~1.6KB for ids array + curl locals. Called from Jetson daemon only.
 */
static int gmail_batch_fetch_metadata(CURL *curl,
                                      const char *token,
                                      const gmail_msg_id_t *ids,
                                      int id_count,
                                      email_summary_t *out,
                                      int max_out,
                                      int *out_count) {
   *out_count = 0;
   if (id_count <= 0)
      return 0;

   /* Build batch request body */
   char *batch_body = build_batch_request(ids, id_count);
   if (!batch_body)
      return 1;

   /* POST to batch endpoint */
   char content_type[128];
   snprintf(content_type, sizeof(content_type), "multipart/mixed; boundary=%s",
            GMAIL_BATCH_BOUNDARY);

   curl_buffer_t resp;
   int rc = gmail_api_post(curl, token, GMAIL_BATCH_URL, content_type, batch_body, &resp);
   free(batch_body);

   if (rc != 0)
      return 1;

   /* Get response Content-Type to extract boundary */
   char *resp_ct = NULL;
   curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &resp_ct);

   char resp_boundary[256];
   if (extract_boundary(resp_ct, resp_boundary, sizeof(resp_boundary)) != 0) {
      OLOG_ERROR("gmail: batch response missing boundary");
      curl_buffer_free(&resp);
      return 1;
   }

   /* Parse batch response */
   rc = parse_batch_response(resp.data, resp_boundary, out, max_out, out_count);
   curl_buffer_free(&resp);
   return rc;
}

/* =============================================================================
 * Public API: Fetch Recent
 * ============================================================================= */

int gmail_fetch_recent(const char *token,
                       const char *label_query,
                       int count,
                       bool unread_only,
                       const char *page_token,
                       email_summary_t *out,
                       int max_out,
                       int *out_count,
                       char *next_page_token,
                       size_t npt_len) {
   *out_count = 0;
   if (next_page_token && npt_len > 0)
      next_page_token[0] = '\0';

   if (!token || !token[0])
      return 1;

   if (count <= 0)
      count = 10;
   if (count > max_out)
      count = max_out;

   CURL *curl = gmail_create_curl();
   if (!curl)
      return 1;

   /* Build query */
   const char *lq = (label_query && label_query[0]) ? label_query : "in:inbox";
   char query[256];
   if (unread_only)
      snprintf(query, sizeof(query), "%s is:unread", lq);
   else
      snprintf(query, sizeof(query), "%s", lq);

   /* Fetch message IDs */
   gmail_msg_id_t ids[EMAIL_MAX_FETCH_RESULTS];
   int id_count = 0;
   int rc = fetch_message_ids(curl, token, query, count, page_token, ids, count, &id_count,
                              next_page_token, npt_len);
   if (rc != 0) {
      curl_easy_cleanup(curl);
      return 1;
   }

   /* Batch fetch metadata (2 HTTP calls total instead of N+1) */
   rc = gmail_batch_fetch_metadata(curl, token, ids, id_count, out, max_out, out_count);

   curl_easy_cleanup(curl);
   return rc;
}

/* =============================================================================
 * Public API: Read Message
 * ============================================================================= */

int gmail_read_message(const char *token,
                       const char *message_id,
                       int max_body_chars,
                       email_message_t *out) {
   memset(out, 0, sizeof(*out));

   if (!token || !token[0] || !message_id)
      return 1;

   if (!is_valid_gmail_id(message_id)) {
      OLOG_ERROR("gmail: invalid message ID '%s'", message_id);
      return 1;
   }

   snprintf(out->message_id, sizeof(out->message_id), "%s", message_id);
   out->uid = 0;

   CURL *curl = gmail_create_curl();
   if (!curl)
      return 1;

   char url[512];
   snprintf(url, sizeof(url), GMAIL_API_BASE "/messages/%s?format=full", message_id);

   curl_buffer_t resp;
   long http_code = 0;
   if (gmail_api_get(curl, token, url, &resp, &http_code) != 0) {
      curl_easy_cleanup(curl);
      return http_code == 404 ? GMAIL_RC_NOT_FOUND : 1;
   }

   curl_easy_cleanup(curl);

   struct json_object *root = json_tokener_parse(resp.data);
   curl_buffer_free(&resp);
   if (!root)
      return 1;

   /* Extract headers */
   struct json_object *payload = NULL;
   struct json_object *headers = NULL;
   if (json_object_object_get_ex(root, "payload", &payload))
      json_object_object_get_ex(payload, "headers", &headers);

   const char *from = find_gmail_header(headers, "From");
   parse_from_field(from, out->from_name, sizeof(out->from_name), out->from_addr,
                    sizeof(out->from_addr));

   const char *to = find_gmail_header(headers, "To");
   if (to)
      snprintf(out->to, sizeof(out->to), "%s", to);

   const char *subject = find_gmail_header(headers, "Subject");
   if (subject)
      snprintf(out->subject, sizeof(out->subject), "%s", subject);

   const char *date = find_gmail_header(headers, "Date");
   if (date)
      snprintf(out->date_str, sizeof(out->date_str), "%s", date);

   /* Extract body (defensive fallback; service layer always supplies a positive cap) */
   if (max_body_chars <= 0)
      max_body_chars = EMAIL_MAX_READ_BODY_LEN;

   char *body_buf = calloc(1, max_body_chars + 1);
   if (body_buf) {
      int result = extract_text_body(payload, body_buf, max_body_chars + 1, 0);
      if (result > 0 && body_buf[0]) {
         out->body = body_buf;
         out->body_len = strlen(body_buf);
         out->truncated = (out->body_len >= max_body_chars);
      } else {
         free(body_buf);
      }
   }

   /* Count attachments */
   out->attachment_count = count_attachments(payload, 0);

   json_object_put(root);
   return 0;
}

/* =============================================================================
 * Public API: Search
 * ============================================================================= */

int gmail_search(const char *token,
                 const email_search_params_t *params,
                 int max_results,
                 email_summary_t *out,
                 int max_out,
                 int *out_count,
                 char *next_page_token,
                 size_t npt_len) {
   *out_count = 0;
   if (next_page_token && npt_len > 0)
      next_page_token[0] = '\0';

   if (!token || !token[0] || !params)
      return 1;

   if (max_results <= 0)
      max_results = 10;
   if (max_results > max_out)
      max_results = max_out;

   CURL *curl = gmail_create_curl();
   if (!curl) {
      OLOG_ERROR("gmail: failed to init curl handle for search");
      return 1;
   }

   /* Build Gmail search query — folder query comes from params->folder via service layer */
   const char *folder_lq = (params->folder[0]) ? params->folder : NULL;
   char query[1024];
   build_search_query(params, folder_lq, query, sizeof(query));

   if (!query[0]) {
      /* No search criteria — default to inbox */
      snprintf(query, sizeof(query), "in:inbox");
   }

   /* Fetch message IDs */
   gmail_msg_id_t ids[EMAIL_MAX_FETCH_RESULTS];
   int id_count = 0;
   const char *pt = params->page_token[0] ? params->page_token : NULL;
   int rc = fetch_message_ids(curl, token, query, max_results, pt, ids, max_results, &id_count,
                              next_page_token, npt_len);
   if (rc != 0) {
      curl_easy_cleanup(curl);
      return 1;
   }

   /* Batch fetch metadata */
   rc = gmail_batch_fetch_metadata(curl, token, ids, id_count, out, max_out, out_count);

   curl_easy_cleanup(curl);
   return rc;
}

/* =============================================================================
 * Public API: Send
 * ============================================================================= */

int gmail_send(const char *token,
               const char *from_addr,
               const char *display_name,
               const char *to_addr,
               const char *to_name,
               const char *subject,
               const char *body) {
   if (!token || !token[0] || !from_addr || !to_addr || !subject || !body)
      return 1;

   /* Sanitize all header-injectable fields (including from_addr for consistency) */
   char safe_from_addr[256];
   char safe_subject[256], safe_to_name[64], safe_display_name[64], safe_to_addr[256];
   sanitize_header_value(from_addr, safe_from_addr, sizeof(safe_from_addr));
   sanitize_header_value(subject, safe_subject, sizeof(safe_subject));
   sanitize_header_value(to_name ? to_name : "", safe_to_name, sizeof(safe_to_name));
   sanitize_header_value(display_name ? display_name : "", safe_display_name,
                         sizeof(safe_display_name));
   sanitize_header_value(to_addr, safe_to_addr, sizeof(safe_to_addr));

   /* Build From header */
   char from_header[512];
   if (safe_display_name[0]) {
      snprintf(from_header, sizeof(from_header), "%s <%s>", safe_display_name, safe_from_addr);
   } else {
      snprintf(from_header, sizeof(from_header), "%s", safe_from_addr);
   }

   /* Build To header */
   char to_header[384];
   if (safe_to_name[0]) {
      snprintf(to_header, sizeof(to_header), "%s <%s>", safe_to_name, safe_to_addr);
   } else {
      snprintf(to_header, sizeof(to_header), "%s", safe_to_addr);
   }

   /* Build Date header */
   char date_str[64];
   time_t now = time(NULL);
   struct tm tm_info;
   localtime_r(&now, &tm_info);
   strftime(date_str, sizeof(date_str), "%a, %d %b %Y %H:%M:%S %z", &tm_info);

   /* Build RFC 2822 message */
   size_t msg_size = strlen(body) + 1024;
   char *message = malloc(msg_size);
   if (!message)
      return 1;

   snprintf(message, msg_size,
            "Date: %s\r\n"
            "From: %s\r\n"
            "To: %s\r\n"
            "Subject: %s\r\n"
            "Content-Type: text/plain; charset=UTF-8\r\n"
            "MIME-Version: 1.0\r\n"
            "\r\n"
            "%s",
            date_str, from_header, to_header, safe_subject, body);

   /* Base64url-encode the message */
   size_t msg_len = strlen(message);
   size_t b64_len = ((msg_len * 4) / 3) + 4;
   char *b64_msg = malloc(b64_len);
   if (!b64_msg) {
      free(message);
      return 1;
   }

   base64url_encode((const unsigned char *)message, msg_len, b64_msg, b64_len);
   free(message);

   /* Build JSON payload */
   struct json_object *json_payload = json_object_new_object();
   json_object_object_add(json_payload, "raw", json_object_new_string(b64_msg));
   const char *json_str = json_object_to_json_string(json_payload);

   CURL *curl = gmail_create_curl();
   if (!curl) {
      json_object_put(json_payload);
      free(b64_msg);
      return 1;
   }

   curl_buffer_t resp;
   int rc = gmail_api_post(curl, token, GMAIL_API_BASE "/messages/send", "application/json",
                           json_str, &resp);

   curl_buffer_free(&resp);
   curl_easy_cleanup(curl);
   json_object_put(json_payload);
   free(b64_msg);

   if (rc != 0) {
      OLOG_ERROR("gmail: send failed");
      return 1;
   }

   OLOG_INFO("gmail: message sent to %s", safe_to_addr);
   return 0;
}

/* =============================================================================
 * Public API: Test Connection
 * ============================================================================= */

int gmail_test_connection(const char *token, char *email_out, size_t email_len) {
   if (!token || !token[0])
      return 1;

   CURL *curl = gmail_create_curl();
   if (!curl)
      return 1;

   curl_buffer_t resp;
   int rc = gmail_api_get(curl, token, GMAIL_API_BASE "/profile", &resp, NULL);
   curl_easy_cleanup(curl);

   if (rc != 0)
      return 1;

   /* Optionally extract emailAddress */
   if (email_out && email_len > 0) {
      email_out[0] = '\0';
      struct json_object *root = json_tokener_parse(resp.data);
      if (root) {
         struct json_object *email_obj = NULL;
         if (json_object_object_get_ex(root, "emailAddress", &email_obj)) {
            const char *email = json_object_get_string(email_obj);
            if (email)
               snprintf(email_out, email_len, "%s", email);
         }
         json_object_put(root);
      }
   }

   curl_buffer_free(&resp);
   return 0;
}

/* =============================================================================
 * Public API: List Labels
 * ============================================================================= */

/** System labels to filter out of the label list */
static const char *const gmail_hidden_labels[] = {
   "CATEGORY_PERSONAL",
   "CATEGORY_SOCIAL",
   "CATEGORY_PROMOTIONS",
   "CATEGORY_UPDATES",
   "CATEGORY_FORUMS",
   "UNREAD",
   "CHAT",
   "DRAFT",
   "IMPORTANT",
   "STARRED",
   "INBOX",
   "SENT",
   "TRASH",
   "SPAM",
   NULL,
};

static bool is_hidden_label(const char *id) {
   for (int i = 0; gmail_hidden_labels[i]; i++) {
      if (strcmp(id, gmail_hidden_labels[i]) == 0)
         return true;
   }
   return false;
}

int gmail_list_labels(const char *token, char *out, size_t out_len) {
   if (!out || out_len < 2)
      return 1;
   out[0] = '\0';

   if (!token || !token[0])
      return 1;

   CURL *curl = gmail_create_curl();
   if (!curl)
      return 1;

   curl_buffer_t resp;
   int rc = gmail_api_get(curl, token, GMAIL_API_BASE "/labels", &resp, NULL);
   curl_easy_cleanup(curl);

   if (rc != 0)
      return 1;

   struct json_object *root = json_tokener_parse(resp.data);
   curl_buffer_free(&resp);
   if (!root)
      return 1;

   struct json_object *labels_arr = NULL;
   if (!json_object_object_get_ex(root, "labels", &labels_arr)) {
      json_object_put(root);
      return 1;
   }

   int pos = 0;
   int count = 0;

   /* System folders first */
   pos += snprintf(out + pos, out_len - pos,
                   "System: Inbox, Sent, Drafts, Trash, Spam, Starred, Important, All Mail\n");

   /* Custom labels */
   pos += snprintf(out + pos, out_len - pos, "Labels: ");
   int label_start = pos;

   int arr_len = json_object_array_length(labels_arr);
   for (int i = 0; i < arr_len && count < 64; i++) {
      struct json_object *label = json_object_array_get_idx(labels_arr, i);
      if (!label)
         continue;

      struct json_object *type_obj = NULL, *name_obj = NULL, *id_obj = NULL;
      json_object_object_get_ex(label, "type", &type_obj);
      json_object_object_get_ex(label, "name", &name_obj);
      json_object_object_get_ex(label, "id", &id_obj);

      const char *type = type_obj ? json_object_get_string(type_obj) : "";
      const char *name = name_obj ? json_object_get_string(name_obj) : "";
      const char *id = id_obj ? json_object_get_string(id_obj) : "";

      /* Show user labels, skip system labels already listed above */
      if (strcmp(type, "user") != 0)
         continue;
      if (is_hidden_label(id))
         continue;
      if (!name[0])
         continue;

      if (count > 0 && pos < (int)out_len - 2)
         pos += snprintf(out + pos, out_len - pos, ", ");
      pos += snprintf(out + pos, out_len - pos, "%s", name);
      count++;
   }

   if (pos == label_start) {
      pos += snprintf(out + pos, out_len - pos, "(none)");
   }

   json_object_put(root);
   return 0;
}

/* =============================================================================
 * Trash / Archive
 * ============================================================================= */

int gmail_trash_message(const char *token, const char *message_id) {
   if (!token || !token[0] || !is_valid_gmail_id(message_id)) {
      OLOG_ERROR("gmail_trash: invalid token or message_id");
      return 1;
   }

   char url[256];
   snprintf(url, sizeof(url), GMAIL_API_BASE "/messages/%s/trash", message_id);

   CURL *curl = gmail_create_curl();
   if (!curl)
      return 1;

   curl_buffer_t resp;
   int rc = gmail_api_post(curl, token, url, "application/json", "{}", &resp);
   curl_buffer_free(&resp);
   curl_easy_cleanup(curl);

   if (rc == 0)
      OLOG_INFO("gmail: trashed message %s", message_id);
   else
      OLOG_ERROR("gmail: failed to trash message %s", message_id);

   return rc;
}

int gmail_archive_message(const char *token, const char *message_id) {
   if (!token || !token[0] || !is_valid_gmail_id(message_id)) {
      OLOG_ERROR("gmail_archive: invalid token or message_id");
      return 1;
   }

   char url[256];
   snprintf(url, sizeof(url), GMAIL_API_BASE "/messages/%s/modify", message_id);

   CURL *curl = gmail_create_curl();
   if (!curl)
      return 1;

   curl_buffer_t resp;
   int rc = gmail_api_post(curl, token, url, "application/json",
                           "{\"removeLabelIds\": [\"INBOX\"]}", &resp);
   curl_buffer_free(&resp);
   curl_easy_cleanup(curl);

   if (rc == 0)
      OLOG_INFO("gmail: archived message %s", message_id);
   else
      OLOG_ERROR("gmail: failed to archive message %s", message_id);

   return rc;
}
