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
 * auth_db accessor layer for the SAGE proactive-attention subsystem:
 * attention_rules (per-user watch definitions) CRUD + attention_log (event
 * outcome ledger) inserts.  Cold path (a few writes/hour), so statements are
 * prepared ad-hoc per call (the auth_db_list_session_metrics style) rather than
 * cached in s_db — this also avoids the boot-time prepare-before-table coupling.
 */

#ifndef AUTH_DB_ATTENTION_H
#define AUTH_DB_ATTENTION_H

#include <stdbool.h>
#include <stdint.h>

#include "core/attention/attention.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Insert a watch row.  @out_id optional.  Thresholds must already be concrete. */
int auth_db_attention_rule_insert(const sage_watch_t *w, int64_t *out_id);

/** Update the editable fields of a watch (user-scoped by @user_id + @id). */
int auth_db_attention_rule_update(int user_id, int64_t id, const sage_watch_t *w);

/** Enable/disable a watch (user-scoped). */
int auth_db_attention_rule_set_enabled(int user_id, int64_t id, bool enabled);

/** Delete a watch (user-scoped). */
int auth_db_attention_rule_delete(int user_id, int64_t id);

/**
 * Load one user's watches into @out (up to @max); @out_count receives the number
 * loaded.  @user_id must be > 0 (AUTH_DB_INVALID otherwise).
 */
int auth_db_attention_rule_list(int user_id, sage_watch_t *out, int max, int *out_count);

/** Load every user's watches (the in-process cache load only). */
int auth_db_attention_rule_list_all(sage_watch_t *out, int max, int *out_count);

/** Count watches for @user_id (for the per-user cap). */
int auth_db_attention_rule_count(int user_id, int *out_count);

/**
 * Append one attention_log row.  judge_score/urgency/outcome columns are left
 * NULL in P0.  @delivered_at_ms == 0 records a NULL delivered_at (dropped/expired).
 */
int auth_db_attention_log_insert(int user_id,
                                 const char *event_key,
                                 const char *category,
                                 const char *source,
                                 const char *summary,
                                 const char *gate_rule,
                                 const char *privacy,
                                 const char *surface,
                                 int64_t delivered_at_ms);

#ifdef __cplusplus
}
#endif

#endif /* AUTH_DB_ATTENTION_H */
