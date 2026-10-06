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
 * Schema migration v101: who may speak for a messaging channel.
 *
 *  - owner_sender: the provider's id of the person who linked the channel
 *    (Telegram from.id, Discord author id, Slack user id).  Only that person's
 *    messages reach the channel's user.  NULL until known: a one-to-one chat
 *    binds it on its first message; a group chat needs a re-link.
 *  - verified_at / verify_code_hash / verify_expires_at / verify_attempts: an
 *    SMS link completes only when the user enters, in the WebUI, a code DAWN
 *    texted to the number (an SMS sender number can be forged; receiving texts
 *    at it can't).  NULL verified_at = waiting for that code.
 *  - verify_sends / verify_window_start: codes texted for the channel in the
 *    current day, which bounds resends (and so guessing).
 *
 * Existing rows are kept working: every row counts as verified (except an
 * SMS link already unlinked, which comes back through a code), and a
 * Telegram private chat (positive id, which Telegram makes equal to the
 * user's id) gets that id as its owner.  Group rows (negative ids) stay
 * unowned.  Finally a unique index allows one enabled, verified row per
 * (provider, address, owner); rows that would break it are disabled (the
 * user can re-link), most recently used kept.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

static int exec_step(sqlite3 *db, const char *what, const char *sql) {
   char *errmsg = NULL;
   if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v101 %s failed: %s", what, errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}

/* @param added  set true when this call added the column. */
static int add_column(sqlite3 *db, const char *col, const char *decl, bool *added) {
   if (auth_db_column_exists(db, "messaging_channels", col)) {
      return AUTH_DB_SUCCESS;
   }
   char sql[160];
   snprintf(sql, sizeof(sql), "ALTER TABLE messaging_channels ADD COLUMN %s %s", col, decl);
   int rc = exec_step(db, "ALTER", sql);
   if (rc == AUTH_DB_SUCCESS && added) {
      *added = true;
   }
   return rc;
}

/* Disable every live row that shares (provider, address, owner) with a more
 * recently used one, logging each, so the unique index can be built.  Two
 * users linked to one group chat before owners existed is the case this
 * meets; each re-links from their own account. */
static int disable_duplicates(sqlite3 *db) {
   static const char *const sql =
       "UPDATE messaging_channels SET is_enabled = 0 WHERE id IN ("
       "SELECT m.id FROM messaging_channels m "
       "WHERE m.is_enabled = 1 AND m.verified_at IS NOT NULL AND EXISTS ("
       "  SELECT 1 FROM messaging_channels o WHERE o.is_enabled = 1 AND "
       "  o.verified_at IS NOT NULL AND o.provider = m.provider AND "
       "  o.provider_address = m.provider_address AND "
       "  COALESCE(o.owner_sender,'') = COALESCE(m.owner_sender,'') AND "
       "  (COALESCE(o.last_used_at,0) > COALESCE(m.last_used_at,0) OR "
       "   (COALESCE(o.last_used_at,0) = COALESCE(m.last_used_at,0) AND o.id > m.id)))) "
       "RETURNING id, user_id, provider, provider_address";
   /* An SMS row comes back only through a new code (as any unlinked SMS
    * row does), so its proof goes with it. */
   static const char *const unprove_sms = "UPDATE messaging_channels SET verified_at = NULL "
                                          "WHERE provider = 'sms' AND is_enabled = 0";
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v101 duplicate disable failed: %s", sqlite3_errmsg(db));
      return AUTH_DB_FAILURE;
   }
   int rc;
   while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
      OLOG_WARNING("auth_db: v101 disabled messaging channel %lld (user %d, %s:%s): another "
                   "user's row has the same chat; re-link it from your own account",
                   (long long)sqlite3_column_int64(st, 0), sqlite3_column_int(st, 1),
                   (const char *)sqlite3_column_text(st, 2),
                   (const char *)sqlite3_column_text(st, 3));
   }
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("auth_db: v101 duplicate disable failed: %s", sqlite3_errmsg(db));
      return AUTH_DB_FAILURE;
   }
   return exec_step(db, "unlinked SMS rows", unprove_sms);
}

int auth_db_migrations_v101(sqlite3 *db) {
   if (exec_step(db, "begin", "BEGIN IMMEDIATE") != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }
   int rc = AUTH_DB_SUCCESS;
   static const char *const columns[][2] = {
      { "owner_sender", "TEXT" },
      { "verified_at", "INTEGER" },
      { "verify_code_hash", "TEXT" },
      { "verify_expires_at", "INTEGER" },
      { "verify_attempts", "INTEGER NOT NULL DEFAULT 0" },
      { "verify_sends", "INTEGER NOT NULL DEFAULT 0" },
      { "verify_window_start", "INTEGER" },
   };
   bool verified_added = false;
   for (size_t i = 0; rc == AUTH_DB_SUCCESS && i < sizeof(columns) / sizeof(columns[0]); i++) {
      rc = add_column(db, columns[i][0], columns[i][1],
                      strcmp(columns[i][0], "verified_at") == 0 ? &verified_added : NULL);
   }
   /* Rows from before verification counted as proven, once: only in the run
    * that adds the column (this step reruns while any later migration step
    * fails, and by then NULL means "waiting for a code").  An SMS link
    * already unlinked isn't carried over: it comes back through a code. */
   if (rc == AUTH_DB_SUCCESS && verified_added) {
      rc = exec_step(db, "verified backfill",
                     "UPDATE messaging_channels SET verified_at = created_at "
                     "WHERE verified_at IS NULL AND (provider <> 'sms' OR is_enabled = 1)");
   }
   if (rc == AUTH_DB_SUCCESS) {
      rc = exec_step(db, "telegram owner backfill",
                     "UPDATE messaging_channels SET owner_sender = provider_address "
                     "WHERE provider = 'telegram' AND owner_sender IS NULL AND "
                     "provider_address GLOB '[1-9]*' AND "
                     "provider_address NOT GLOB '*[^0-9]*'");
   }
   if (rc == AUTH_DB_SUCCESS) {
      rc = disable_duplicates(db);
   }
   if (rc == AUTH_DB_SUCCESS) {
      rc = exec_step(db, "owner index",
                     "CREATE UNIQUE INDEX IF NOT EXISTS idx_messaging_channels_owner "
                     "ON messaging_channels(provider, provider_address, "
                     "COALESCE(owner_sender,'')) "
                     "WHERE is_enabled = 1 AND verified_at IS NOT NULL");
   }
   if (rc == AUTH_DB_SUCCESS) {
      rc = exec_step(db, "commit", "COMMIT");
   }
   if (rc != AUTH_DB_SUCCESS) {
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      OLOG_ERROR("auth_db: v101 rolled back; messaging channels won't answer until it succeeds "
                 "(it retries on the next start)");
   }
   return rc;
}
