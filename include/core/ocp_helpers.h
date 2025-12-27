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
 *
 * Provides timestamp generation, checksum computation and validation,
 * and secure data handling for OCP-compliant messages.
 */

#ifndef OCP_HELPERS_H
#define OCP_HELPERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* SHA256 hex string length (64 chars + null terminator) */
#define OCP_SHA256_HEX_LEN 65

/**
 * @brief Get current Unix timestamp in milliseconds
 *
 * Uses clock_gettime(CLOCK_REALTIME) for consistency.
 *
 * @return Current time as milliseconds since epoch
 */
int64_t ocp_get_timestamp_ms(void);

/**
 * @brief Compute SHA256 hash of data and return as lowercase hex string
 *
 * Uses OpenSSL EVP API (not deprecated).
 *
 * @param data Data to hash
 * @param len Length of data
 * @param hex_out Output buffer (must be at least OCP_SHA256_HEX_LEN bytes)
 * @return true on success, false on error
 */
bool ocp_sha256_compute(const unsigned char *data, size_t len, char *hex_out);

/**
 * @brief Compute SHA256 hash of a file
 *
 * @param filepath Path to file
 * @param hex_out Output buffer (must be at least OCP_SHA256_HEX_LEN bytes)
 * @return true on success, false on error
 */
bool ocp_sha256_file(const char *filepath, char *hex_out);

/**
 * @brief Decode base64 string to binary data
 *
 * @param input Base64-encoded string
 * @param out_len Output: length of decoded data
 * @return Allocated buffer with decoded data, or NULL on error. Caller must free.
 */
unsigned char *ocp_base64_decode(const char *input, size_t *out_len);

/**
 * @brief Validate OCP checksum for file reference
 *
 * Validates the file path is safe (no path traversal), then computes and
 * compares the checksum using constant-time comparison.
 *
 * @param filepath File path from value field
 * @param expected_checksum Expected SHA256 checksum (hex string), or NULL to skip
 * @param allowed_base_path Base directory files must be under (NULL for no check)
 * @return true if valid, false if mismatch or invalid path
 */
bool ocp_validate_file_checksum(const char *filepath,
                                const char *expected_checksum,
                                const char *allowed_base_path);

/**
 * @brief Validate OCP checksum for inline data
 *
 * @param content The content string
 * @param encoding Encoding type: "base64", "utf8", or "none"
 * @param expected_checksum Expected SHA256 checksum (hex string), or NULL to skip
 * @return true if valid, false if mismatch
 */
bool ocp_validate_inline_checksum(const char *content,
                                  const char *encoding,
                                  const char *expected_checksum);

/**
 * @brief Check if a file path is safe (within allowed directory)
 *
 * Uses realpath() to resolve symlinks and prevent path traversal attacks.
 *
 * @param filepath Path to check
 * @param allowed_base Base directory the file must be within
 * @return true if path is safe, false otherwise
 */
bool ocp_is_path_safe(const char *filepath, const char *allowed_base);

#endif /* OCP_HELPERS_H */
