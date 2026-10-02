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
 * libFuzzer harness for reading email (email_mime.c): any bytes as a raw
 * RFC 822 message (the IMAP path), as an address header, and, when they parse
 * as JSON, as a Gmail format=full payload (gmail_parts.c).  Mail arrives
 * from anyone, so this is the parser's hostile-input test.  Built only on
 * request: tests/fuzz/build_fuzz_email_mime.sh.
 */

#include <json-c/json.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tools/email_mime.h"
#include "tools/gmail_client_internal.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
   if (size < 1)
      return 0;
   /* The first byte picks the read: cut or whole fetch, panel or tool. */
   const uint8_t mode = data[0];
   data++;
   size--;

   char *raw = malloc(size + 1);
   if (!raw)
      return 0;
   memcpy(raw, data, size);
   raw[size] = '\0';

   email_read_opts_t opts = { .fetch_bytes = 2 * 1024 * 1024,
                              .max_text_chars = (mode & 4) ? 64 : 50000,
                              .max_html_bytes = (mode & 8) ? 32 : 1024 * 1024,
                              .want_html = (mode & 2) != 0,
                              .headers_only = (mode & 16) != 0 };
   email_message_t m;
   memset(&m, 0, sizeof(m));
   if (email_mime_parse_raw(raw, size, (mode & 1) != 0, &opts, &m) == 0) {
      /* The output's promises: NUL-terminated, length matches. */
      if (m.body && (int)strlen(m.body) != m.body_len)
         abort();
      if (m.body_html && strlen(m.body_html) != m.body_html_len)
         abort();
      if (m.to_count > EMAIL_MAX_ADDRS || m.attachment_count > EMAIL_MAX_ATTACHMENTS)
         abort();
      email_message_free(&m);
   }

   email_addr_t *list = NULL;
   int count = 0;
   int total = 0;
   if (email_mime_addr_list(raw, EMAIL_MAX_ADDRS, &list, &count, &total) == 0)
      free(list);
   char text[256];
   email_mime_header_text(raw, text, sizeof(text));

   struct json_object *payload = json_tokener_parse(raw);
   if (payload) {
      gmail_parts_t w;
      if (gmail_parts_from_payload(payload, &w) == 0) {
         memset(&m, 0, sizeof(m));
         if (email_mime_apply(w.parts, w.count, w.cut, &opts, NULL, NULL, &m) == 0)
            email_message_free(&m);
      }
      gmail_parts_free(&w);
      json_object_put(payload);
   }
   free(raw);
   return 0;
}
