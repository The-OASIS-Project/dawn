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
 * API key tags.  See llm_key_tag.h.
 */

#include "llm/llm_key_tag.h"

#include <pthread.h>
#include <sodium.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "core/crypto_store.h"
#include "llm/llm_turn_blocks.h"
#include "logging.h"

/* The purpose label for the key store's digest (crypto_kdf: 8 characters). */
#define KEY_TAG_CONTEXT "llm-ktag"

#define KEY_TAG_BYTES 8
_Static_assert(2 * KEY_TAG_BYTES + 1 == LLM_KEY_TAG_MAX, "the tag is the digest in hex");

static unsigned char s_process_key[crypto_generichash_KEYBYTES];
static pthread_once_t s_process_once = PTHREAD_ONCE_INIT;

static void process_key_init(void) {
   OLOG_WARNING("LLM key tags: key store unavailable; stored reasoning won't replay after a "
                "restart");
   if (sodium_init() >= 0) {
      randombytes_buf(s_process_key, sizeof(s_process_key));
      return;
   }
   /* No randomness source either: still a key of this process's own (its id,
    * the time, an address), never a constant another install shares, so a
    * copied database replays nothing here. */
   struct {
      pid_t pid;
      struct timespec now;
      const void *where;
   } seed;
   memset(&seed, 0, sizeof(seed)); /* no stray padding bytes in the hash */
   seed.pid = getpid();
   seed.where = (const void *)&seed;
   clock_gettime(CLOCK_REALTIME, &seed.now);
   crypto_generichash(s_process_key, sizeof(s_process_key), (const unsigned char *)&seed,
                      sizeof(seed), NULL, 0);
}

void llm_key_tag(const char *api_key, char *out, size_t out_len) {
   if (!out || out_len == 0) {
      return;
   }
   const char *key = api_key ? api_key : "";
   unsigned char digest[crypto_generichash_BYTES_MIN] = { 0 };
   if (crypto_store_keyed_digest(KEY_TAG_CONTEXT, key, strlen(key), digest, sizeof(digest)) != 0) {
      pthread_once(&s_process_once, process_key_init);
      if (crypto_generichash(digest, sizeof(digest), (const unsigned char *)key, strlen(key),
                             s_process_key, sizeof(s_process_key)) != 0) {
         /* No tag to give (unreachable with these sizes): every key would share
          * it, so it is logged and never stored with this process's reasoning
          * beyond a restart. */
         OLOG_ERROR("LLM key tags: digest failed; keys can't be told apart");
         snprintf(out, out_len, "none");
         return;
      }
   }
   char hex[2 * KEY_TAG_BYTES + 1];
   for (int i = 0; i < KEY_TAG_BYTES; i++) {
      snprintf(hex + 2 * i, 3, "%02x", digest[i]);
   }
   snprintf(out, out_len, "%s", hex);
   sodium_memzero(digest, sizeof(digest));
}

void llm_request_carrier(const char *base_url, const char *api_key, char *out, size_t out_len) {
   char tag[LLM_KEY_TAG_MAX];
   llm_key_tag(api_key, tag, sizeof(tag));
   llm_turn_blocks_carrier(base_url, tag, out, out_len);
}
