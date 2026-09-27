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
 * Forget what memory learned from a conversation.
 */

#include "memory/memory_forget.h"

#include <string.h>

#include "auth/auth_db.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_extraction.h"

/* The conversation plus its continuations.  MEMORY_FORGET_TOO_LONG past
 * CONV_CHAIN_MAX; FAILURE when none is the user's. */
static int load_chain(int user_id, int64_t conv_id, int64_t *ids, int *n) {
   const int rc = conv_db_continuation_chain(conv_id, user_id, ids, CONV_CHAIN_MAX, n);
   if (rc == AUTH_DB_LIMIT_EXCEEDED) {
      OLOG_WARNING("memory_forget: conversation %lld has more than %d continuations",
                   (long long)conv_id, CONV_CHAIN_MAX);
      return MEMORY_FORGET_TOO_LONG;
   }
   if (rc != AUTH_DB_SUCCESS || *n <= 0) {
      return FAILURE;
   }
   return SUCCESS;
}

int memory_conversation_learned(int user_id, int64_t conv_id, memory_conv_learned_t *out) {
   if (!out) {
      return FAILURE;
   }
   memset(out, 0, sizeof(*out));
   int64_t ids[CONV_CHAIN_MAX];
   int n = 0;
   const int chain_rc = load_chain(user_id, conv_id, ids, &n);
   if (chain_rc != SUCCESS) {
      return chain_rc;
   }
   /* A count that misses rows still being written would undersell what a forget
    * removes, so let an in-flight extraction finish first (no hold: counting
    * needn't keep a new one from starting).  If the wait runs out, count what
    * is there. */
   (void)memory_extraction_wait_idle(user_id, MEMORY_FORGET_WAIT_SEC);
   return memory_db_conversations_learned_count(user_id, ids, n, out) == MEMORY_DB_SUCCESS
              ? SUCCESS
              : FAILURE;
}

int memory_forget_conversation(int user_id, int64_t conv_id, memory_conv_learned_t *deleted_out) {
   memory_conv_learned_t d;
   memset(&d, 0, sizeof(d));
   if (deleted_out) {
      *deleted_out = d;
   }
   int64_t ids[CONV_CHAIN_MAX];
   int n = 0;
   const int chain_rc = load_chain(user_id, conv_id, ids, &n);
   if (chain_rc != SUCCESS) {
      return chain_rc;
   }
   if (!memory_extraction_hold_user(user_id, MEMORY_FORGET_WAIT_SEC)) {
      OLOG_WARNING("memory_forget: user %d conversation %lld: extraction still running; not "
                   "forgetting yet",
                   user_id, (long long)conv_id);
      return MEMORY_FORGET_BUSY;
   }
   int rc = memory_db_conversations_forget(user_id, ids, n, &d);
   memory_extraction_release_user(user_id);
   if (rc != MEMORY_DB_SUCCESS) {
      return FAILURE;
   }

   OLOG_INFO("memory_forget: user %d conversation %lld (+%d continuation(s)): removed %d facts, "
             "%d summaries, %d relations, %d preferences, %d entities",
             user_id, (long long)conv_id, n - 1, d.facts, d.summaries, d.relations, d.preferences,
             d.entities);
   if (deleted_out) {
      *deleted_out = d;
   }
   return SUCCESS;
}
