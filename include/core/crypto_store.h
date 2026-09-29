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
 * Used by CalDAV password encryption, OAuth token storage, and future modules.
 */

#ifndef CRYPTO_STORE_H
#define CRYPTO_STORE_H

#include <stdbool.h>
#include <stddef.h>

/**
 * Initialize the crypto store (load or generate dawn.key via pthread_once).
 * Safe to call multiple times; only the first call has effect.
 * @return 0 on success, 1 on failure
 */
int crypto_store_init(void);

/**
 * Check if the crypto store is ready for use.
 */
bool crypto_store_ready(void);

/**
 * Encrypt plaintext using crypto_secretbox_easy.
 * Output format: [nonce (24 bytes)][ciphertext (pt_len + 16-byte MAC)]
 *
 * @param plaintext   Data to encrypt
 * @param pt_len      Length of plaintext
 * @param out         Output buffer (must be at least pt_len + 40 bytes)
 * @param out_len     Size of output buffer
 * @param out_written Actual bytes written to out (may be NULL)
 * @return 0 on success, 1 on failure
 */
int crypto_store_encrypt(const void *plaintext,
                         size_t pt_len,
                         unsigned char *out,
                         size_t out_len,
                         size_t *out_written);

/**
 * Decrypt data encrypted by crypto_store_encrypt.
 *
 * @param ciphertext  Encrypted data (nonce + ciphertext + MAC)
 * @param ct_len      Length of ciphertext blob
 * @param out         Output buffer for decrypted data
 * @param out_len     Size of output buffer
 * @param out_written Actual bytes written to out (may be NULL)
 * @return 0 on success, 1 on failure (wrong key, corrupt data, buffer too small)
 */
int crypto_store_decrypt(const unsigned char *ciphertext,
                         size_t ct_len,
                         void *out,
                         size_t out_len,
                         size_t *out_written);

/**
 * Keyed digest of @p msg under a subkey derived from the store's key.
 *
 * The subkey is crypto_kdf-derived for @p context (exactly 8 characters, one
 * per use, so digests for one purpose say nothing about another); the digest
 * is BLAKE2b.  The same input gives the same digest across restarts on this
 * install, and one that can't be computed or guessed without the key file.
 *
 * @param context  8-character purpose label
 * @param out_len  crypto_generichash_BYTES_MIN (16) to _MAX (64)
 * @return 0 on success, 1 on failure (store not ready, bad arguments)
 */
int crypto_store_keyed_digest(const char *context,
                              const void *msg,
                              size_t msg_len,
                              unsigned char *out,
                              size_t out_len);

/**
 * Zero the encryption key from memory. Call during shutdown.
 */
void crypto_store_shutdown(void);

#endif /* CRYPTO_STORE_H */
