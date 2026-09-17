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
 * Charles Schwab REST client — thin authenticated JSON GET over libcurl.
 */

#include "tools/schwab_client.h"

#include <curl/curl.h>
#include <sodium.h>
#include <stdio.h>
#include <string.h>

#include "core/curl_buffer.h"
#include "logging.h"
#include "tools/oauth_client.h" /* OAUTH_TOKEN_BUF_SIZE */

/* Quotes and account/position payloads are small; cap defensively. */
#define SCHWAB_MAX_RESPONSE (4 * 1024 * 1024)
#define SCHWAB_HTTP_TIMEOUT 20L

schwab_rc_t schwab_client_get_json(const char *bearer,
                                   const char *url,
                                   struct json_object **out_root,
                                   long *http_code_out) {
   if (out_root) {
      *out_root = NULL;
   }
   if (http_code_out) {
      *http_code_out = 0;
   }
   if (!bearer || !url || !out_root) {
      return SCHWAB_RC_ERROR;
   }

   CURL *curl = curl_easy_init();
   if (!curl) {
      return SCHWAB_RC_ERROR;
   }

   char auth_header[OAUTH_TOKEN_BUF_SIZE + 32];
   snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", bearer);
   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, auth_header);
   headers = curl_slist_append(headers, "Accept: application/json");

   curl_buffer_t resp;
   curl_buffer_init_with_max(&resp, SCHWAB_MAX_RESPONSE);
   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, SCHWAB_HTTP_TIMEOUT);
   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
   DAWN_CURL_SET_PROTOCOLS(curl, "https");

   CURLcode cres = curl_easy_perform(curl);
   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
   curl_slist_free_all(headers);
   curl_easy_cleanup(curl);
   /* Do NOT log the URL (may carry symbols) at ERROR by default; never log the
    * response body or bearer. */
   sodium_memzero(auth_header, sizeof(auth_header));

   if (http_code_out) {
      *http_code_out = http_code;
   }

   schwab_rc_t rc;
   if (cres != CURLE_OK) {
      OLOG_ERROR("schwab: HTTP request failed (curl %d)", cres);
      rc = SCHWAB_RC_HTTP;
   } else if (http_code == 401) {
      rc = SCHWAB_RC_AUTH;
   } else if (http_code == 429) {
      rc = SCHWAB_RC_RATE_LIMITED;
   } else if (http_code == 200 && resp.truncated) {
      /* Succeeded but the body blew past SCHWAB_MAX_RESPONSE — distinct from a
       * transport error so callers can tell the user to narrow the window rather
       * than reporting a network failure. */
      OLOG_ERROR("schwab: response exceeded %d bytes (truncated)", SCHWAB_MAX_RESPONSE);
      rc = SCHWAB_RC_TOO_LARGE;
   } else if (http_code != 200) {
      OLOG_ERROR("schwab: unexpected HTTP %ld", http_code);
      rc = SCHWAB_RC_HTTP;
   } else if (!resp.data) {
      /* Empty 200 body — curl_buffer_t.data stays NULL when the write callback
       * never fires; json_tokener_parse(NULL) would dereference NULL. */
      OLOG_ERROR("schwab: empty response body on HTTP 200");
      rc = SCHWAB_RC_HTTP;
   } else {
      struct json_object *root = json_tokener_parse(resp.data);
      if (!root) {
         OLOG_ERROR("schwab: failed to parse response JSON");
         rc = SCHWAB_RC_ERROR;
      } else {
         *out_root = root;
         rc = SCHWAB_RC_OK;
      }
   }

   /* The portfolio response body carries unmasked account numbers/balances —
    * scrub it from freed heap (masking happens downstream, so nothing unmasked
    * egresses, but don't leave cleartext PII lingering). */
   if (resp.data) {
      sodium_memzero(resp.data, resp.size);
   }
   curl_buffer_free(&resp);
   return rc;
}
