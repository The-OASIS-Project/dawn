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
 * Shared encryption module — libsodium crypto_secretbox with single key file.
 * Extracted from calendar_service.c to serve CalDAV, OAuth, and future modules.
 */

#include "core/crypto_store.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sodium.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "config/dawn_config.h"
#include "core/path_utils.h"
#include "logging.h"

/* =============================================================================
 * State
 * ============================================================================= */

static unsigned char s_crypto_key[crypto_secretbox_KEYBYTES];
static bool s_crypto_ready = false;
static pthread_once_t s_crypto_once = PTHREAD_ONCE_INIT;
static int s_crypto_error = 0;

/* =============================================================================
 * Key File Path
 * ============================================================================= */

/* Resolve the data directory used for key storage. Prefer the startup-resolved
 * value (dawn.c Step 4b); fall back to expanding the raw config value here for
 * consumers that run OUTSIDE daemon startup and so never populate
 * data_dir_resolved (e.g. the standalone crypto_store unit test, which sets
 * paths.data_dir directly); finally /var/lib/dawn when nothing is configured. */
static void crypto_data_dir(char *out, size_t out_len) {
   if (g_config.paths.data_dir_resolved[0]) {
      snprintf(out, out_len, "%s", g_config.paths.data_dir_resolved);
   } else if (g_config.paths.data_dir[0]) {
      path_expand_tilde(g_config.paths.data_dir, out, out_len);
   } else {
      snprintf(out, out_len, "/var/lib/dawn");
   }
}

static void get_key_path(char *out, size_t out_len) {
   char expanded[512];
   crypto_data_dir(expanded, sizeof(expanded));
   snprintf(out, out_len, "%.*s/dawn.key", (int)(out_len > 10 ? out_len - 10 : 0), expanded);
}

/* =============================================================================
 * Key File Migration: caldav.key → dawn.key
 * ============================================================================= */

static void migrate_key_file(const char *new_path) {
   char expanded[512];
   crypto_data_dir(expanded, sizeof(expanded));

   char old_path[PATH_MAX];
   snprintf(old_path, sizeof(old_path), "%s/caldav.key", expanded);

   struct stat st;
   if (stat(old_path, &st) == 0 && stat(new_path, &st) != 0) {
      /* Old key exists, new one doesn't — rename it */
      if (rename(old_path, new_path) == 0) {
         OLOG_INFO("crypto_store: migrated caldav.key → dawn.key");
      } else {
         OLOG_WARNING("crypto_store: failed to rename caldav.key: %s", strerror(errno));
      }
   }
}

/* =============================================================================
 * Init (pthread_once)
 * ============================================================================= */

static void crypto_init_once(void) {
   if (sodium_init() < 0) {
      OLOG_ERROR("crypto_store: sodium_init failed");
      s_crypto_error = 1;
      return;
   }

   char key_path[512];
   get_key_path(key_path, sizeof(key_path));

   /* Try migration from old caldav.key */
   migrate_key_file(key_path);

   FILE *fp = fopen(key_path, "rb");
   if (fp) {
      size_t n = fread(s_crypto_key, 1, sizeof(s_crypto_key), fp);
      fclose(fp);
      if (n == sizeof(s_crypto_key)) {
         sodium_mlock(s_crypto_key, sizeof(s_crypto_key));
         s_crypto_ready = true;
         return;
      }
      OLOG_WARNING("crypto_store: key file truncated, regenerating");
   }

   /* Generate new key */
   randombytes_buf(s_crypto_key, sizeof(s_crypto_key));

   mode_t old_umask = umask(0077);
   fp = fopen(key_path, "wb");
   umask(old_umask);
   if (!fp) {
      OLOG_ERROR("crypto_store: cannot write key file: %s", strerror(errno));
      s_crypto_error = 1;
      return;
   }
   if (fwrite(s_crypto_key, 1, sizeof(s_crypto_key), fp) != sizeof(s_crypto_key)) {
      OLOG_ERROR("crypto_store: short write to key file");
      fclose(fp);
      s_crypto_error = 1;
      return;
   }
   fclose(fp);
   sodium_mlock(s_crypto_key, sizeof(s_crypto_key));
   OLOG_INFO("crypto_store: generated new encryption key");
   s_crypto_ready = true;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int crypto_store_init(void) {
   pthread_once(&s_crypto_once, crypto_init_once);
   return s_crypto_error;
}

bool crypto_store_ready(void) {
   return s_crypto_ready;
}

int crypto_store_encrypt(const void *plaintext,
                         size_t pt_len,
                         unsigned char *out,
                         size_t out_len,
                         size_t *out_written) {
   if (!plaintext || !out)
      return 1;
   if (crypto_store_init() != 0)
      return 1;

   size_t needed = crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES + pt_len;
   if (needed > out_len) {
      OLOG_ERROR("crypto_store: output buffer too small (%zu needed, %zu available)", needed,
                 out_len);
      return 1;
   }

   unsigned char *nonce = out;
   unsigned char *ct = out + crypto_secretbox_NONCEBYTES;

   randombytes_buf(nonce, crypto_secretbox_NONCEBYTES);
   if (crypto_secretbox_easy(ct, (const unsigned char *)plaintext, pt_len, nonce, s_crypto_key) !=
       0) {
      OLOG_ERROR("crypto_store: encryption failed");
      return 1;
   }

   if (out_written)
      *out_written = needed;
   return 0;
}

int crypto_store_decrypt(const unsigned char *ciphertext,
                         size_t ct_len,
                         void *out,
                         size_t out_len,
                         size_t *out_written) {
   if (!ciphertext || !out || out_len == 0)
      return 1;
   if (crypto_store_init() != 0)
      return 1;

   if (ct_len < crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES) {
      OLOG_ERROR("crypto_store: ciphertext too short");
      return 1;
   }

   const unsigned char *nonce = ciphertext;
   const unsigned char *ct = ciphertext + crypto_secretbox_NONCEBYTES;
   size_t ct_data_len = ct_len - crypto_secretbox_NONCEBYTES;
   size_t pt_len = ct_data_len - crypto_secretbox_MACBYTES;

   if (pt_len > out_len) {
      OLOG_ERROR("crypto_store: output buffer too small for decrypted data");
      return 1;
   }

   if (crypto_secretbox_open_easy((unsigned char *)out, ct, ct_data_len, nonce, s_crypto_key) !=
       0) {
      OLOG_ERROR("crypto_store: decryption failed (wrong key or corrupt data)");
      return 1;
   }

   if (out_written)
      *out_written = pt_len;
   return 0;
}

void crypto_store_shutdown(void) {
   sodium_memzero(s_crypto_key, sizeof(s_crypto_key));
   s_crypto_ready = false;
}
