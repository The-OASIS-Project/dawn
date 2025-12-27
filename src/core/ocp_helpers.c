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
 * OCP (OASIS Communications Protocol) v1.1 Helper Functions
 */

#include "core/ocp_helpers.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* OpenSSL */
#include <openssl/buffer.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>

#include "logging.h"

/* Hex nibble lookup table for efficient conversion */
static const char hex_table[] = "0123456789abcdef";

/* =============================================================================
 * Timestamp
 * ============================================================================= */

int64_t ocp_get_timestamp_ms(void) {
   struct timespec ts;
   if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
      /* Fallback - should never happen on Linux */
      return 0;
   }
   return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* =============================================================================
 * SHA256 Computation (using EVP API - not deprecated)
 * ============================================================================= */

bool ocp_sha256_compute(const unsigned char *data, size_t len, char *hex_out) {
   if (!data || len == 0 || !hex_out) {
      return false;
   }

   EVP_MD_CTX *ctx = EVP_MD_CTX_new();
   if (!ctx) {
      LOG_ERROR("ocp_sha256_compute: Failed to create EVP context");
      return false;
   }

   unsigned char hash[EVP_MAX_MD_SIZE];
   unsigned int hash_len = 0;

   if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1 || EVP_DigestUpdate(ctx, data, len) != 1 ||
       EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
      LOG_ERROR("ocp_sha256_compute: SHA256 computation failed");
      EVP_MD_CTX_free(ctx);
      return false;
   }

   EVP_MD_CTX_free(ctx);

   /* Convert to lowercase hex string using lookup table */
   for (unsigned int i = 0; i < hash_len; i++) {
      hex_out[i * 2] = hex_table[(hash[i] >> 4) & 0x0F];
      hex_out[i * 2 + 1] = hex_table[hash[i] & 0x0F];
   }
   hex_out[hash_len * 2] = '\0';

   return true;
}

bool ocp_sha256_file(const char *filepath, char *hex_out) {
   if (!filepath || !hex_out) {
      return false;
   }

   FILE *f = fopen(filepath, "rb");
   if (!f) {
      LOG_WARNING("ocp_sha256_file: Cannot open file: %s", filepath);
      return false;
   }

   EVP_MD_CTX *ctx = EVP_MD_CTX_new();
   if (!ctx) {
      LOG_ERROR("ocp_sha256_file: Failed to create EVP context");
      fclose(f);
      return false;
   }

   if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
      LOG_ERROR("ocp_sha256_file: Failed to initialize SHA256");
      EVP_MD_CTX_free(ctx);
      fclose(f);
      return false;
   }

   unsigned char buffer[8192];
   size_t bytes_read;
   while ((bytes_read = fread(buffer, 1, sizeof(buffer), f)) > 0) {
      if (EVP_DigestUpdate(ctx, buffer, bytes_read) != 1) {
         LOG_ERROR("ocp_sha256_file: Failed to update hash");
         EVP_MD_CTX_free(ctx);
         fclose(f);
         return false;
      }
   }
   fclose(f);

   unsigned char hash[EVP_MAX_MD_SIZE];
   unsigned int hash_len = 0;

   if (EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
      LOG_ERROR("ocp_sha256_file: Failed to finalize hash");
      EVP_MD_CTX_free(ctx);
      return false;
   }

   EVP_MD_CTX_free(ctx);

   /* Convert to lowercase hex string using lookup table */
   for (unsigned int i = 0; i < hash_len; i++) {
      hex_out[i * 2] = hex_table[(hash[i] >> 4) & 0x0F];
      hex_out[i * 2 + 1] = hex_table[hash[i] & 0x0F];
   }
   hex_out[hash_len * 2] = '\0';

   return true;
}

/* =============================================================================
 * Base64 Decoding
 * ============================================================================= */

unsigned char *ocp_base64_decode(const char *input, size_t *out_len) {
   if (!input || !out_len) {
      return NULL;
   }

   size_t input_len = strlen(input);
   if (input_len == 0) {
      *out_len = 0;
      return NULL;
   }

   /* Integer overflow protection: ensure multiplication doesn't overflow */
   if (input_len > SIZE_MAX / 3) {
      LOG_ERROR("ocp_base64_decode: Input too large, would overflow");
      return NULL;
   }

   /* Allocate output buffer (decoded is smaller than encoded) */
   size_t max_decoded_len = (input_len * 3) / 4 + 1;
   unsigned char *buffer = malloc(max_decoded_len);
   if (!buffer) {
      LOG_ERROR("ocp_base64_decode: Memory allocation failed");
      return NULL;
   }

   /* Create BIO chain for base64 decoding */
   BIO *b64 = BIO_new(BIO_f_base64());
   if (!b64) {
      free(buffer);
      return NULL;
   }

   /* Check input_len fits in int for BIO_new_mem_buf */
   if (input_len > INT_MAX) {
      LOG_ERROR("ocp_base64_decode: Input too large for BIO");
      BIO_free(b64);
      free(buffer);
      return NULL;
   }

   BIO *bio = BIO_new_mem_buf(input, (int)input_len);
   if (!bio) {
      BIO_free(b64);
      free(buffer);
      return NULL;
   }

   bio = BIO_push(b64, bio);
   BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);

   int decoded_len = BIO_read(bio, buffer, (int)max_decoded_len);
   BIO_free_all(bio);

   if (decoded_len < 0) {
      free(buffer);
      return NULL;
   }

   *out_len = (size_t)decoded_len;
   return buffer;
}

/* =============================================================================
 * Path Safety
 * ============================================================================= */

bool ocp_is_path_safe(const char *filepath, const char *allowed_base) {
   if (!filepath || !allowed_base) {
      return false;
   }

   char resolved_path[PATH_MAX];
   char resolved_base[PATH_MAX];

   /* Resolve the base path first */
   if (realpath(allowed_base, resolved_base) == NULL) {
      LOG_WARNING("ocp_is_path_safe: Cannot resolve base path: %s", allowed_base);
      return false;
   }

   /* Resolve the target path */
   if (realpath(filepath, resolved_path) == NULL) {
      /* File doesn't exist - check parent directory */
      char *filepath_copy = strdup(filepath);
      if (!filepath_copy) {
         return false;
      }

      char *last_slash = strrchr(filepath_copy, '/');
      if (last_slash && last_slash != filepath_copy) {
         *last_slash = '\0';
         if (realpath(filepath_copy, resolved_path) == NULL) {
            free(filepath_copy);
            return false;
         }
         free(filepath_copy);
      } else {
         free(filepath_copy);
         return false;
      }
   }

   /* Check that resolved path starts with resolved base */
   size_t base_len = strlen(resolved_base);
   if (strncmp(resolved_path, resolved_base, base_len) != 0) {
      return false;
   }

   /* Ensure there's a path separator after the base (or path equals base) */
   if (resolved_path[base_len] != '\0' && resolved_path[base_len] != '/') {
      return false;
   }

   return true;
}

/* =============================================================================
 * Checksum Validation
 * ============================================================================= */

bool ocp_validate_file_checksum(const char *filepath,
                                const char *expected_checksum,
                                const char *allowed_base_path) {
   /* No checksum provided - skip validation (backward compatibility) */
   if (!expected_checksum || expected_checksum[0] == '\0') {
      return true;
   }

   if (!filepath) {
      LOG_WARNING("OCP: No filepath provided for checksum validation");
      return false; /* Fail-closed: checksum expected but no file */
   }

   /* Path safety check if base path provided */
   if (allowed_base_path && !ocp_is_path_safe(filepath, allowed_base_path)) {
      LOG_WARNING("OCP: Path traversal attempt detected: %s", filepath);
      return false;
   }

   /* Compute actual checksum */
   char actual[OCP_SHA256_HEX_LEN];
   if (!ocp_sha256_file(filepath, actual)) {
      LOG_WARNING("OCP: Could not compute checksum for file: %s", filepath);
      return false; /* Fail-closed: can't validate means reject */
   }

   /* Constant-time comparison to prevent timing attacks */
   size_t expected_len = strlen(expected_checksum);
   size_t actual_len = strlen(actual);

   if (expected_len != actual_len) {
      LOG_WARNING("OCP: Checksum length mismatch for %s", filepath);
      return false;
   }

   /* CRYPTO_memcmp is constant-time */
   if (CRYPTO_memcmp(actual, expected_checksum, actual_len) != 0) {
      LOG_WARNING("OCP: Checksum mismatch for %s (expected: %.16s..., actual: %.16s...)", filepath,
                  expected_checksum, actual);
      return false;
   }

   LOG_INFO("OCP: Checksum validated for %s", filepath);
   return true;
}

bool ocp_validate_inline_checksum(const char *content,
                                  const char *encoding,
                                  const char *expected_checksum) {
   /* No checksum provided - skip validation (backward compatibility) */
   if (!expected_checksum || expected_checksum[0] == '\0') {
      return true;
   }

   if (!content || !encoding) {
      LOG_WARNING("OCP: No content/encoding provided for checksum validation");
      return false; /* Fail-closed */
   }

   char actual[OCP_SHA256_HEX_LEN];
   bool computed = false;

   if (strcmp(encoding, "base64") == 0) {
      /* Decode base64 first, then hash the raw bytes */
      size_t decoded_len = 0;
      unsigned char *decoded = ocp_base64_decode(content, &decoded_len);
      if (decoded && decoded_len > 0) {
         computed = ocp_sha256_compute(decoded, decoded_len, actual);
         free(decoded);
      }
   } else if (strcmp(encoding, "utf8") == 0 || strcmp(encoding, "none") == 0) {
      /* Hash the string bytes directly */
      computed = ocp_sha256_compute((const unsigned char *)content, strlen(content), actual);
   } else {
      LOG_WARNING("OCP: Unknown encoding type: %s", encoding);
      return false;
   }

   if (!computed) {
      LOG_WARNING("OCP: Could not compute checksum for inline data");
      return false; /* Fail-closed */
   }

   /* Constant-time comparison */
   size_t expected_len = strlen(expected_checksum);
   size_t actual_len = strlen(actual);

   if (expected_len != actual_len) {
      LOG_WARNING("OCP: Inline checksum length mismatch");
      return false;
   }

   if (CRYPTO_memcmp(actual, expected_checksum, actual_len) != 0) {
      LOG_WARNING("OCP: Inline data checksum mismatch (expected: %.16s..., actual: %.16s...)",
                  expected_checksum, actual);
      return false;
   }

   LOG_INFO("OCP: Inline data checksum validated");
   return true;
}
