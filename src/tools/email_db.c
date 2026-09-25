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
 * Uses shared auth_db handle via auth_db_internal.h prepared statements.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "tools/email_db.h"

#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "core/crypto_store.h"
#include "dawn_error.h"
#include "logging.h"

/* =============================================================================
 * Password Encryption (mirrors calendar_db.c pattern)
 * ============================================================================= */

int email_encrypt_password(const char *plaintext, email_account_t *acct) {
   if (!plaintext || !plaintext[0]) {
      acct->encrypted_password_len = 0;
      return 0;
   }
   size_t pt_len = strlen(plaintext);
   size_t out_written = 0;
   if (crypto_store_encrypt(plaintext, pt_len, acct->encrypted_password,
                            sizeof(acct->encrypted_password), &out_written) != 0) {
      OLOG_ERROR("email_db: failed to encrypt password");
      return 1;
   }
   acct->encrypted_password_len = (int)out_written;
   return 0;
}

int email_decrypt_password(const email_account_t *acct, char *out, size_t out_len) {
   if (acct->encrypted_password_len <= 0) {
      out[0] = '\0';
      return 0;
   }
   size_t dec_written = 0;
   if (crypto_store_decrypt(acct->encrypted_password, (size_t)acct->encrypted_password_len, out,
                            out_len - 1, &dec_written) != 0) {
      OLOG_ERROR("email_db: failed to decrypt password");
      return 1;
   }
   out[dec_written] = '\0';
   return 0;
}

/* =============================================================================
 * Row Helper
 * ============================================================================= */

/* The auth layer can't include tools headers, so it mirrors these defaults for the
 * email_accounts schema; fail the build rather than let the two drift. */
_Static_assert(EMAIL_DEFAULT_DIGEST_DEPTH == EMAIL_DIGEST_DEPTH_DEFAULT,
               "auth-layer digest_depth default must mirror email_types.h");
_Static_assert(EMAIL_DEFAULT_BODY_CHARS == EMAIL_MAX_READ_BODY_LEN,
               "auth-layer max_body_chars default must mirror email_types.h");

/* Unset (<= 0) means "use the default"; anything above the ceiling is capped so a
 * hand-edited row can't drive an unbounded digest paging loop. */
static int clamp_digest_depth(int depth) {
   if (depth <= 0)
      return EMAIL_DIGEST_DEPTH_DEFAULT;
   if (depth > EMAIL_DIGEST_DEPTH_MAX)
      return EMAIL_DIGEST_DEPTH_MAX;
   return depth;
}

static void row_to_account(sqlite3_stmt *st, email_account_t *out) {
   memset(out, 0, sizeof(*out));
   out->id = sqlite3_column_int64(st, 0);
   out->user_id = sqlite3_column_int(st, 1);

   const char *s;
   s = (const char *)sqlite3_column_text(st, 2);
   if (s)
      snprintf(out->name, sizeof(out->name), "%s", s);

   s = (const char *)sqlite3_column_text(st, 3);
   if (s)
      snprintf(out->imap_server, sizeof(out->imap_server), "%s", s);
   out->imap_port = sqlite3_column_int(st, 4);
   out->imap_ssl = sqlite3_column_int(st, 5) != 0;

   s = (const char *)sqlite3_column_text(st, 6);
   if (s)
      snprintf(out->smtp_server, sizeof(out->smtp_server), "%s", s);
   out->smtp_port = sqlite3_column_int(st, 7);
   out->smtp_ssl = sqlite3_column_int(st, 8) != 0;

   s = (const char *)sqlite3_column_text(st, 9);
   if (s)
      snprintf(out->username, sizeof(out->username), "%s", s);

   s = (const char *)sqlite3_column_text(st, 10);
   if (s)
      snprintf(out->display_name, sizeof(out->display_name), "%s", s);

   /* Encrypted password (BLOB) */
   int blob_len = sqlite3_column_bytes(st, 11);
   if (blob_len > 0 && blob_len <= (int)sizeof(out->encrypted_password)) {
      memcpy(out->encrypted_password, sqlite3_column_blob(st, 11), blob_len);
   }
   out->encrypted_password_len = sqlite3_column_int(st, 12);

   s = (const char *)sqlite3_column_text(st, 13);
   if (s)
      snprintf(out->auth_type, sizeof(out->auth_type), "%s", s);

   s = (const char *)sqlite3_column_text(st, 14);
   if (s)
      snprintf(out->oauth_account_key, sizeof(out->oauth_account_key), "%s", s);

   out->enabled = sqlite3_column_int(st, 15) != 0;
   out->read_only = sqlite3_column_int(st, 16) != 0;
   out->max_recent = sqlite3_column_int(st, 17);
   if (out->max_recent <= 0 || out->max_recent > EMAIL_MAX_FETCH_RESULTS)
      out->max_recent = EMAIL_MAX_RECENT_DEFAULT;
   out->max_body_chars = sqlite3_column_int(st, 18);
   out->created_at = (time_t)sqlite3_column_int64(st, 19);
   out->digest_depth = clamp_digest_depth(sqlite3_column_int(st, 20));
}

/* =============================================================================
 * CRUD Operations
 * ============================================================================= */

int email_db_account_create(const email_account_t *acct, int64_t *id_out) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_email_acct_create;
   sqlite3_reset(st);
   sqlite3_bind_int(st, 1, acct->user_id);
   sqlite3_bind_text(st, 2, acct->name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, acct->imap_server, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(st, 4, acct->imap_port);
   sqlite3_bind_int(st, 5, acct->imap_ssl ? 1 : 0);
   sqlite3_bind_text(st, 6, acct->smtp_server, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(st, 7, acct->smtp_port);
   sqlite3_bind_int(st, 8, acct->smtp_ssl ? 1 : 0);
   sqlite3_bind_text(st, 9, acct->username, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 10, acct->display_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_blob(st, 11, acct->encrypted_password, acct->encrypted_password_len,
                     SQLITE_TRANSIENT);
   sqlite3_bind_int(st, 12, acct->encrypted_password_len);
   sqlite3_bind_text(st, 13, acct->auth_type, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 14, acct->oauth_account_key, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(st, 15, acct->enabled ? 1 : 0);
   sqlite3_bind_int(st, 16, acct->read_only ? 1 : 0);
   sqlite3_bind_int(st, 17, acct->max_recent > 0 ? acct->max_recent : EMAIL_MAX_RECENT_DEFAULT);
   sqlite3_bind_int(st, 18,
                    acct->max_body_chars > 0 ? acct->max_body_chars : EMAIL_MAX_READ_BODY_LEN);
   sqlite3_bind_int64(st, 19, (int64_t)time(NULL));
   sqlite3_bind_int(st, 20, clamp_digest_depth(acct->digest_depth));

   int result = FAILURE;
   if (sqlite3_step(st) == SQLITE_DONE) {
      if (id_out)
         *id_out = sqlite3_last_insert_rowid(s_db.db);
      result = SUCCESS;
   } else {
      OLOG_ERROR("email_db: account create failed: %s", sqlite3_errmsg(s_db.db));
   }
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}

int email_db_account_get(int64_t id, email_account_t *out) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_email_acct_get;
   sqlite3_reset(st);
   sqlite3_bind_int64(st, 1, id);

   int result = 1;
   if (sqlite3_step(st) == SQLITE_ROW) {
      row_to_account(st, out);
      result = 0;
   }
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return result;
}

int email_db_account_list(int user_id, email_account_t *out, int max_count, int *count_out) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_email_acct_list;
   sqlite3_reset(st);
   sqlite3_bind_int(st, 1, user_id);

   int count = 0;
   while (count < max_count && sqlite3_step(st) == SQLITE_ROW) {
      row_to_account(st, &out[count]);
      count++;
   }
   sqlite3_reset(st);

   if (count_out)
      *count_out = count;

   AUTH_DB_UNLOCK();
   return SUCCESS;
}

int email_db_account_update(const email_account_t *acct) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_email_acct_update;
   sqlite3_reset(st);
   sqlite3_bind_text(st, 1, acct->name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, acct->imap_server, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(st, 3, acct->imap_port);
   sqlite3_bind_int(st, 4, acct->imap_ssl ? 1 : 0);
   sqlite3_bind_text(st, 5, acct->smtp_server, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(st, 6, acct->smtp_port);
   sqlite3_bind_int(st, 7, acct->smtp_ssl ? 1 : 0);
   sqlite3_bind_text(st, 8, acct->username, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 9, acct->display_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_blob(st, 10, acct->encrypted_password, acct->encrypted_password_len,
                     SQLITE_TRANSIENT);
   sqlite3_bind_int(st, 11, acct->encrypted_password_len);
   sqlite3_bind_text(st, 12, acct->auth_type, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 13, acct->oauth_account_key, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(st, 14, acct->max_recent);
   sqlite3_bind_int(st, 15, acct->max_body_chars);
   sqlite3_bind_int(st, 16, clamp_digest_depth(acct->digest_depth));
   sqlite3_bind_int64(st, 17, acct->id);

   int rc = sqlite3_step(st);
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return (rc == SQLITE_DONE) ? 0 : 1;
}

int email_db_account_delete(int64_t id) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_email_acct_delete;
   sqlite3_reset(st);
   sqlite3_bind_int64(st, 1, id);

   int rc = sqlite3_step(st);
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return (rc == SQLITE_DONE) ? 0 : 1;
}

int email_db_account_set_read_only(int64_t id, bool read_only) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_email_acct_set_read_only;
   sqlite3_reset(st);
   sqlite3_bind_int(st, 1, read_only ? 1 : 0);
   sqlite3_bind_int64(st, 2, id);

   int rc = sqlite3_step(st);
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return (rc == SQLITE_DONE) ? 0 : 1;
}

int email_db_account_set_enabled(int64_t id, bool enabled) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_email_acct_set_enabled;
   sqlite3_reset(st);
   sqlite3_bind_int(st, 1, enabled ? 1 : 0);
   sqlite3_bind_int64(st, 2, id);

   int rc = sqlite3_step(st);
   sqlite3_reset(st);

   AUTH_DB_UNLOCK();
   return (rc == SQLITE_DONE) ? 0 : 1;
}
