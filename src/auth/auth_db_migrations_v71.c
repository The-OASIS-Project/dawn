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
 * Schema migration v71: SAGE proactive-attention tables.
 *
 *  - attention_rules: per-user "watch" definitions, created/edited/muted at
 *    runtime by the `attention` LLM tool and the WebUI Watches panel (never
 *    static config — the JARVIS conversational-control principle).  enum fields
 *    (rule_type/direction/notify) are stored as INTEGER; thresholds are concrete
 *    REALs (catalog defaults are resolved before insert).
 *  - attention_log: the event outcome ledger.  judge_score/urgency/outcome
 *    columns are present-but-nullable so the P2 judge and P1 outcome-capture add
 *    no migration.  delivered_at NULL == dropped/expired.
 *
 * Both are IF NOT EXISTS (idempotent, fresh installs run the ladder from 0), and
 * their indexes live in this migration per the base-vs-migration ordering
 * invariant.  A transient failure returns FAILURE so the ladder holds the
 * version at 70 and retries next boot rather than advertising v71 without the
 * tables (which would hard-fail nothing at boot since attention CRUD prepares
 * ad-hoc, but the subsystem would be non-functional).
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

int auth_db_migrations_v71(sqlite3 *db) {
   if (db == NULL) {
      return AUTH_DB_FAILURE;
   }

   static const char *const ddl =
       "CREATE TABLE IF NOT EXISTS attention_rules ("
       "  id INTEGER PRIMARY KEY,"
       "  user_id INTEGER NOT NULL,"
       "  name TEXT,"
       "  metric TEXT NOT NULL,"
       "  rule_type INTEGER NOT NULL,"
       "  direction INTEGER NOT NULL,"
       "  threshold REAL,"
       "  hysteresis REAL,"
       "  slope_per_min REAL,"
       "  slope_window_sec INTEGER,"
       "  absence_after_sec INTEGER,"
       "  notify INTEGER NOT NULL,"
       "  ttl_min INTEGER,"
       "  enabled INTEGER NOT NULL DEFAULT 1,"
       "  named INTEGER NOT NULL DEFAULT 0,"
       "  muted_until INTEGER,"
       "  source TEXT,"
       "  created_at INTEGER,"
       "  updated_at INTEGER"
       ");"
       "CREATE INDEX IF NOT EXISTS idx_attention_rules_user ON attention_rules(user_id, enabled);"
       "CREATE TABLE IF NOT EXISTS attention_log ("
       "  id INTEGER PRIMARY KEY,"
       "  user_id INTEGER NOT NULL,"
       "  event_key TEXT NOT NULL,"
       "  category TEXT,"
       "  source TEXT,"
       "  summary TEXT,"
       "  gate_rule TEXT,"
       "  judge_score REAL,"
       "  urgency TEXT,"
       "  privacy TEXT,"
       "  surface TEXT,"
       "  delivered_at INTEGER,"
       "  outcome TEXT,"
       "  outcome_at INTEGER,"
       "  fired_within_window INTEGER,"
       "  logged_at INTEGER NOT NULL"
       ");"
       "CREATE INDEX IF NOT EXISTS idx_attention_log_user ON attention_log(user_id, logged_at);"
       "CREATE INDEX IF NOT EXISTS idx_attention_log_event_key ON attention_log(event_key);"
       "CREATE INDEX IF NOT EXISTS idx_attention_log_delivered ON attention_log(delivered_at);";

   char *errmsg = NULL;
   int rc = sqlite3_exec(db, ddl, NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: v71 migration (attention_rules/attention_log) failed: %s",
                 errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }

   return AUTH_DB_SUCCESS;
}
