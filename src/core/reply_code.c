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
 * Reply codes (see core/reply_code.h).
 */

#include "core/reply_code.h"

#include <ctype.h>
#include <pthread.h>
#include <sodium.h>
#include <stdio.h>
#include <string.h>

static unsigned char s_key[crypto_generichash_KEYBYTES];
static pthread_once_t s_key_once = PTHREAD_ONCE_INIT;

static void key_init(void) {
   randombytes_buf(s_key, sizeof(s_key));
}

void reply_code_new(char out[REPLY_CODE_LEN]) {
   snprintf(out, REPLY_CODE_LEN, "%06u", (unsigned)randombytes_uniform(1000000));
}

void reply_code_digest(const char *code, char out[REPLY_CODE_DIGEST_HEX]) {
   pthread_once(&s_key_once, key_init);
   unsigned char digest[crypto_generichash_BYTES];
   crypto_generichash(digest, sizeof(digest), (const unsigned char *)(code ? code : ""),
                      code ? strlen(code) : 0, s_key, sizeof(s_key));
   sodium_bin2hex(out, REPLY_CODE_DIGEST_HEX, digest, sizeof(digest));
   sodium_memzero(digest, sizeof(digest));
}

bool reply_code_digest_equal(const char *a, const char *b) {
   if (!a || !b) {
      return false;
   }
   const size_t len = strlen(a);
   return len > 0 && len == strlen(b) && sodium_memcmp(a, b, len) == 0;
}

bool reply_code_in_text(const char *body, char out[REPLY_CODE_LEN]) {
   if (out) {
      out[0] = '\0';
   }
   if (!body) {
      return false;
   }
   while (*body && isspace((unsigned char)*body)) {
      body++;
   }
   size_t n = 0;
   while (isdigit((unsigned char)body[n])) {
      n++;
   }
   /* A full stop or exclamation mark after it (a phone's keyboard adds
    * them) is still the code. */
   size_t end = n;
   while (body[end] == '.' || body[end] == '!') {
      end++;
   }
   if (n != REPLY_CODE_DIGITS || (body[end] && !isspace((unsigned char)body[end]))) {
      return false;
   }
   if (out) {
      memcpy(out, body, REPLY_CODE_DIGITS);
      out[REPLY_CODE_DIGITS] = '\0';
   }
   return true;
}
