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
 * Email account database — multi-account IMAP/SMTP storage with encryption.
 */

#ifndef EMAIL_DB_H
#define EMAIL_DB_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "auth/auth_db.h"      /* AUTH_DB_SUCCESS / _NOT_FOUND / _FAILURE */
#include "tools/email_types.h" /* EMAIL_MAX_READ_BODY_LEN */

#define EMAIL_MAX_ACCOUNTS 16

typedef struct {
   int64_t id;
   int user_id;
   char name[128]; /* "Gmail", "Work", etc. */
   char imap_server[256];
   int imap_port; /* 993 default */
   bool imap_ssl;
   char smtp_server[256];
   int smtp_port; /* 465 default */
   bool smtp_ssl;
   char username[128];
   char display_name[64]; /* For From: header */
   uint8_t encrypted_password[384];
   int encrypted_password_len;
   char auth_type[16]; /* "app_password" or "oauth" */
   char oauth_account_key[128];
   bool enabled;
   bool read_only;     /* Blocks send/confirm_send actions */
   int max_recent;     /* Default EMAIL_MAX_RECENT_DEFAULT */
   int max_body_chars; /* Default EMAIL_MAX_READ_BODY_LEN (50000) */
   time_t created_at;
   int digest_depth; /* Digest scan depth, 1..EMAIL_DIGEST_DEPTH_MAX (default 50) */
} email_account_t;

/**
 * @brief Encrypt a password into an email account struct.
 * @return 0 on success, 1 on failure
 */
int email_encrypt_password(const char *plaintext, email_account_t *acct);

/**
 * @brief Decrypt account password.
 * @return 0 on success, 1 on failure
 */
int email_decrypt_password(const email_account_t *acct, char *out, size_t out_len);

/**
 * @brief Create a new email account.
 * @param id_out  Receives the new account ID on success
 * @return 0 on success, 1 on failure
 */
int email_db_account_create(const email_account_t *acct, int64_t *id_out);

/**
 * @brief Get account by ID.
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND (no such account), or AUTH_DB_FAILURE
 *         (the database couldn't answer: try again later)
 */
int email_db_account_get(int64_t id, email_account_t *out);

/**
 * @brief List accounts for a user.
 * @param count_out  Number of accounts written to out
 * @return 0 on success, 1 on failure
 */
int email_db_account_list(int user_id, email_account_t *out, int max_count, int *count_out);

/**
 * @brief Update an existing account.
 * @return 0 on success, 1 on failure
 */
int email_db_account_update(const email_account_t *acct);

/**
 * @brief Delete an account by ID.
 * @return 0 on success, 1 on failure
 */
int email_db_account_delete(int64_t id);

/**
 * @brief Set the read_only flag on an account.
 * @return 0 on success, 1 on failure
 */
int email_db_account_set_read_only(int64_t id, bool read_only);

/**
 * @brief Set the enabled flag on an account.
 * @return 0 on success, 1 on failure
 */
int email_db_account_set_enabled(int64_t id, bool enabled);

#endif /* EMAIL_DB_H */
