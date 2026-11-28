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
 * Cryptographic utilities for authentication system.
 * Provides Argon2id password hashing, secure token generation,
 * and constant-time comparison functions.
 */

#ifndef AUTH_CRYPTO_H
#define AUTH_CRYPTO_H

#include <sodium.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Password hash output length (libsodium encoded string)
 *
 * Uses crypto_pwhash_STRBYTES which includes the algorithm identifier,
 * parameters, salt, and hash in a single encoded string.
 */
#define AUTH_HASH_LEN crypto_pwhash_STRBYTES

/**
 * @brief Session token length (64 hex characters + null terminator)
 *
 * 256-bit entropy = 32 bytes = 64 hex characters
 */
#define AUTH_TOKEN_LEN 65

/**
 * @brief Maximum concurrent password hash operations
 *
 * Limits memory usage during password hashing. Each hash uses
 * AUTH_MEMLIMIT bytes, so max memory = limit * AUTH_MEMLIMIT.
 */
#define AUTH_CONCURRENT_HASH_LIMIT 3

/**
 * @brief Hash semaphore timeout in seconds
 *
 * If all hash slots are busy, wait up to this long before returning AUTH_BUSY.
 */
#define AUTH_HASH_TIMEOUT_SEC 5

/*
 * Platform-aware Argon2id parameters
 *
 * Jetson (8GB RAM): 16MB memory, 3 iterations
 * Raspberry Pi (512MB-1GB): 8MB memory, 4 iterations
 *
 * OWASP minimum: 15MB memory, 2 iterations
 * We exceed this on Jetson and compensate with more iterations on Pi.
 */
#ifdef PLATFORM_RPI
#define AUTH_MEMLIMIT (8 * 1024 * 1024) /* 8MB for Pi */
#define AUTH_OPSLIMIT 4                 /* More iterations to compensate */
#else
#define AUTH_MEMLIMIT (16 * 1024 * 1024) /* 16MB for Jetson and others */
#define AUTH_OPSLIMIT 3                  /* Standard iterations */
#endif

/*
 * A well-formed hash, with the parameters above, of no known password.  A
 * login or admin check for a user that doesn't exist verifies against it, so
 * the reply takes as long as a real check and doesn't tell which names exist.
 * libsodium refuses a malformed string without hashing, which would undo that:
 * the salt and hash must be valid unpadded base64 of 16 and 32 bytes.
 */
#ifdef PLATFORM_RPI
#define AUTH_DUMMY_PASSWORD_HASH                           \
   "$argon2id$v=19$m=8192,t=4,p=1$AAAAAAAAAAAAAAAAAAAAAA$" \
   "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
#else
#define AUTH_DUMMY_PASSWORD_HASH                            \
   "$argon2id$v=19$m=16384,t=3,p=1$AAAAAAAAAAAAAAAAAAAAAA$" \
   "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
#endif

/* Compile-time validation of OWASP minimums (best-effort) */
#if AUTH_OPSLIMIT < 2
#error "AUTH_OPSLIMIT must be at least 2 per OWASP guidelines"
#endif

/**
 * @brief Authentication crypto error codes
 */
#define AUTH_CRYPTO_SUCCESS 0
#define AUTH_CRYPTO_FAILURE 1
#define AUTH_CRYPTO_BUSY 2    /* Hash semaphore timeout */
#define AUTH_CRYPTO_OOM 3     /* Out of memory during hash */
#define AUTH_CRYPTO_ENTROPY 4 /* Random number generation failed */

/**
 * @brief Initialize crypto subsystem
 *
 * Must be called before any other auth_crypto functions.
 * Initializes libsodium and the hash semaphore.
 *
 * @return AUTH_CRYPTO_SUCCESS on success, AUTH_CRYPTO_FAILURE on error
 */
int auth_crypto_init(void);

/**
 * @brief Shutdown crypto subsystem
 *
 * Cleans up resources. Safe to call multiple times.
 */
void auth_crypto_shutdown(void);

/**
 * @brief Hash a password using Argon2id
 *
 * Uses platform-appropriate memory and iteration parameters.
 * Blocks for up to AUTH_HASH_TIMEOUT_SEC if hash slots are busy.
 *
 * @param password Null-terminated password string
 * @param hash_out Buffer to receive encoded hash (AUTH_HASH_LEN bytes)
 * @return AUTH_CRYPTO_SUCCESS, AUTH_CRYPTO_BUSY, AUTH_CRYPTO_OOM, or AUTH_CRYPTO_FAILURE
 *
 * @note hash_out is automatically zeroed on failure
 */
int auth_hash_password(const char *password, char hash_out[AUTH_HASH_LEN]);

/**
 * @brief Verify a password against stored hash
 *
 * Uses constant-time comparison to prevent timing attacks.
 *
 * @param stored_hash Previously computed hash from auth_hash_password()
 * @param password Password to verify
 * @return true if password matches, false otherwise
 */
bool auth_verify_password(const char *stored_hash, const char *password);

/**
 * @brief Generate a cryptographically secure session token
 *
 * Generates 32 random bytes and encodes as 64 hex characters.
 * Uses getrandom() with no fallback - fails closed on entropy failure.
 *
 * @param token_out Buffer to receive hex token (AUTH_TOKEN_LEN bytes)
 * @return AUTH_CRYPTO_SUCCESS or AUTH_CRYPTO_ENTROPY
 *
 * @note token_out is automatically zeroed on failure
 */
int auth_generate_token(char token_out[AUTH_TOKEN_LEN]);

/**
 * @brief Constant-time token comparison
 *
 * Compares two tokens in constant time to prevent timing attacks.
 * Both tokens must be exactly AUTH_TOKEN_LEN-1 characters.
 *
 * @param a First token
 * @param b Second token
 * @return true if tokens match, false otherwise
 */
bool auth_token_compare(const char *a, const char *b);

/**
 * @brief Securely clear sensitive memory
 *
 * Wrapper around sodium_memzero() for consistent interface.
 *
 * @param buf Buffer to clear
 * @param len Length in bytes
 */
void auth_secure_zero(void *buf, size_t len);

/* ============================================================================
 * CSRF Token Functions
 *
 * CSRF tokens are HMAC-signed tokens containing a timestamp.
 * They are single-use (validated once then discarded) and expire after
 * AUTH_CSRF_TIMEOUT_SEC seconds.
 * ============================================================================ */

/**
 * @brief CSRF token length (hex string + null terminator)
 *
 * Format: 8 bytes timestamp + 16 bytes nonce + 32 bytes HMAC = 56 bytes
 * As hex: 112 chars + null = 113
 */
#define AUTH_CSRF_TOKEN_LEN 113

/**
 * @brief CSRF token validity period in seconds
 */
#define AUTH_CSRF_TIMEOUT_SEC 600 /* 10 minutes */

/**
 * @brief Generate a CSRF token
 *
 * Creates an HMAC-signed token containing the current timestamp.
 * Token is valid for AUTH_CSRF_TIMEOUT_SEC seconds.
 *
 * @param token_out Buffer to receive hex token (AUTH_CSRF_TOKEN_LEN bytes)
 * @return AUTH_CRYPTO_SUCCESS or AUTH_CRYPTO_FAILURE
 */
int auth_generate_csrf_token(char token_out[AUTH_CSRF_TOKEN_LEN]);

/**
 * @brief Verify a CSRF token
 *
 * Validates HMAC signature and checks that token hasn't expired.
 * Uses constant-time comparison to prevent timing attacks.
 *
 * @param token The CSRF token to verify
 * @return true if valid and not expired, false otherwise
 */
bool auth_verify_csrf_token(const char *token);

/**
 * @brief CSRF nonce size in bytes (for single-use tracking)
 */
#define AUTH_CSRF_NONCE_SIZE 16

/**
 * @brief Verify a CSRF token and extract nonce for single-use tracking
 *
 * Same as auth_verify_csrf_token but also extracts the nonce for
 * single-use enforcement by the caller.
 *
 * @param token The CSRF token to verify
 * @param nonce_out Buffer to receive 16-byte nonce (NULL to skip extraction)
 * @return true if valid and not expired, false otherwise
 */
bool auth_verify_csrf_token_extract_nonce(const char *token,
                                          unsigned char nonce_out[AUTH_CSRF_NONCE_SIZE]);

#endif /* AUTH_CRYPTO_H */
