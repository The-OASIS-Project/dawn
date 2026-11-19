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
 * Range extraction (memory_extraction_range.h): its checks, the counts its dry
 * run reports, and the memory_extract_ranges record.  The run itself is the
 * extraction every conversation takes (memory_extraction.c), given the range.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#define MEMORY_EXTRACTION_INTERNAL_ALLOWED

#include "memory/memory_extraction_range.h"

#include <json-c/json.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "core/outside_kind.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_extraction.h"
#include "memory/memory_extraction_internal.h"
#include "memory/memory_history_loader.h"

/* =============================================================================
 * The record
 * ============================================================================= */

int extraction_range_record(int64_t conversation_id, int64_t from, int64_t to) {
   AUTH_DB_LOCK_OR_RETURN(FAILURE);
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "INSERT INTO memory_extract_ranges (conversation_id, msg_id_start, "
                               "msg_id_end, created_at) SELECT ?1, ?2, ?3, ?4 WHERE NOT EXISTS "
                               "(SELECT 1 FROM memory_extract_ranges WHERE conversation_id = ?1 "
                               "AND msg_id_start <= ?3 AND msg_id_end >= ?2)",
                               -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, conversation_id);
      sqlite3_bind_int64(st, 2, from);
      sqlite3_bind_int64(st, 3, to);
      sqlite3_bind_int64(st, 4, (int64_t)time(NULL));
      rc = sqlite3_step(st);
   }
   sqlite3_finalize(st);
   const int changed = rc == SQLITE_DONE ? sqlite3_changes(s_db.db) : 0;
   AUTH_DB_UNLOCK();
   if (rc != SQLITE_DONE) {
      return FAILURE;
   }
   return changed > 0 ? SUCCESS : MEMORY_EXTRACT_RANGE_OVERLAP;
}

void extraction_range_unrecord(int64_t conversation_id, int64_t from, int64_t to) {
   AUTH_DB_LOCK_OR_RETURN_VOID();
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "DELETE FROM memory_extract_ranges WHERE conversation_id = ?1 AND "
                          "msg_id_start = ?2 AND msg_id_end = ?3",
                          -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, conversation_id);
      sqlite3_bind_int64(st, 2, from);
      sqlite3_bind_int64(st, 3, to);
      if (sqlite3_step(st) != SQLITE_DONE) {
         OLOG_WARNING("memory_extraction: conv %lld: range record not removed",
                      (long long)conversation_id);
      }
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
}

/* =============================================================================
 * What a range holds
 * ============================================================================= */

/* The range's rows by role, and whether a recorded range overlaps. */
static int count_range(int64_t conversation_id,
                       int64_t from,
                       int64_t to,
                       memory_extract_range_report_t *r) {
   AUTH_DB_LOCK_OR_RETURN(FAILURE);
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT role, kind IS NOT NULL, COUNT(*) FROM messages WHERE "
                               "conversation_id = ?1 AND id BETWEEN ?2 AND ?3 GROUP BY 1, 2",
                               -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, conversation_id);
      sqlite3_bind_int64(st, 2, from);
      sqlite3_bind_int64(st, 3, to);
      while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
         const char *role = (const char *)sqlite3_column_text(st, 0);
         const bool context = sqlite3_column_int(st, 1) != 0; /* DAWN's request context */
         const int n = sqlite3_column_int(st, 2);
         int *slot = &r->other;
         if (!context && role && strcmp(role, "user") == 0) {
            slot = &r->user;
         } else if (!context && role && strcmp(role, "assistant") == 0) {
            slot = &r->assistant;
         } else if (!context && role && strcmp(role, "tool") == 0) {
            slot = &r->tool;
         }
         *slot += n;
      }
   }
   sqlite3_finalize(st);
   st = NULL;
   if (rc == SQLITE_DONE) {
      rc = sqlite3_prepare_v2(s_db.db,
                              "SELECT EXISTS (SELECT 1 FROM memory_extract_ranges WHERE "
                              "conversation_id = ?1 AND msg_id_start <= ?3 AND msg_id_end >= ?2)",
                              -1, &st, NULL);
      if (rc == SQLITE_OK) {
         sqlite3_bind_int64(st, 1, conversation_id);
         sqlite3_bind_int64(st, 2, from);
         sqlite3_bind_int64(st, 3, to);
         rc = sqlite3_step(st);
         r->overlap = rc == SQLITE_ROW && sqlite3_column_int(st, 0) != 0;
      }
      sqlite3_finalize(st);
   }
   AUTH_DB_UNLOCK();
   return rc == SQLITE_ROW ? SUCCESS : FAILURE;
}

/* The conversation's stored rows through @p to (a new array), or NULL. */
static struct json_object *rows_through(int64_t conversation_id, int user_id, int64_t to) {
   struct json_object *all = memory_history_load_from_db(conversation_id, user_id, NULL);
   if (!all) {
      return NULL;
   }
   struct json_object *kept = json_object_new_array();
   for (size_t i = 0; kept && i < json_object_array_length(all); i++) {
      struct json_object *msg = json_object_array_get_idx(all, i), *id = NULL;
      if (json_object_object_get_ex(msg, "id", &id) && json_object_get_int64(id) > to) {
         break; /* in id order */
      }
      if (json_object_array_add(kept, json_object_get(msg)) != 0) {
         json_object_put(msg);
         json_object_put(kept);
         kept = NULL;
      }
   }
   json_object_put(all);
   return kept;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

/* The checks that stop a range before anything is read. */
static int check_conversation(int user_id, int64_t conversation_id, int64_t from, int64_t to) {
   if (from <= 0 || to < from) {
      return MEMORY_EXTRACT_RANGE_BAD_RANGE;
   }
   if (!g_config.memory.enabled) {
      return MEMORY_EXTRACT_RANGE_DISABLED;
   }
   bool is_private = false;
   const int prc = conv_db_is_private(conversation_id, user_id, &is_private);
   if (prc == AUTH_DB_NOT_FOUND) {
      return MEMORY_EXTRACT_RANGE_NOT_FOUND; /* missing and another user's alike */
   }
   if (prc != AUTH_DB_SUCCESS) {
      return FAILURE;
   }
   if (is_private) {
      return MEMORY_EXTRACT_RANGE_PRIVATE;
   }
   job_record_t job;
   const int jrc = conv_db_job_get(conversation_id, user_id, &job);
   if (jrc == AUTH_DB_SUCCESS) {
      return MEMORY_EXTRACT_RANGE_JOB;
   }
   return jrc == AUTH_DB_NOT_FOUND ? SUCCESS : FAILURE;
}

/* Where a forget (of this conversation's memories, or of everything) left the
 * conversation: what was said up to it stays forgotten.  0: never. */
static int forget_floor(int64_t conversation_id, int64_t *floor_out) {
   *floor_out = 0;
   AUTH_DB_LOCK_OR_RETURN(FAILURE);
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT extract_floor_msg_id FROM conversations WHERE id = ?1", -1,
                               &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, conversation_id);
      rc = sqlite3_step(st);
      if (rc == SQLITE_ROW) {
         *floor_out = sqlite3_column_int64(st, 0);
      }
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return rc == SQLITE_ROW ? SUCCESS : FAILURE;
}

int memory_extract_range(int user_id,
                         int64_t conversation_id,
                         int64_t from,
                         int64_t to,
                         bool confirm,
                         memory_extract_range_report_t *out) {
   memory_extract_range_report_t local;
   memory_extract_range_report_t *r = out ? out : &local;
   memset(r, 0, sizeof(*r));
   r->outside = -1;
   const int hard = check_conversation(user_id, conversation_id, from, to);
   if (hard != SUCCESS) {
      return hard;
   }
   int64_t forgotten_to = 0;
   if (memory_db_get_last_extracted_msg_id(conversation_id, &r->cursor) != MEMORY_DB_SUCCESS ||
       forget_floor(conversation_id, &forgotten_to) != SUCCESS ||
       count_range(conversation_id, from, to, r) != SUCCESS) {
      return FAILURE;
   }
   r->counted = true;

   /* The first that applies; a dry run still finds what the batch would
    * read. */
   int refusal = SUCCESS;
   if (to > r->cursor) {
      refusal = MEMORY_EXTRACT_RANGE_PAST_CURSOR;
   } else if (from <= forgotten_to) {
      /* Not rows an extraction skipped: rows the user asked to have forgotten. */
      refusal = MEMORY_EXTRACT_RANGE_FORGOTTEN;
   } else if (r->user == 0) {
      refusal = MEMORY_EXTRACT_RANGE_NO_USER_ROW;
   } else if (r->overlap) {
      refusal = MEMORY_EXTRACT_RANGE_OVERLAP;
   } else if (memory_extraction_in_progress(user_id)) {
      refusal = MEMORY_EXTRACT_RANGE_BUSY;
   }
   if (confirm && refusal != SUCCESS) {
      return refusal;
   }

   struct json_object *history = rows_through(conversation_id, user_id, to);
   if (!history) {
      return FAILURE;
   }
   extraction_range_t range = { .from = from, .to = to, .dry = !confirm, .outside = -1 };
   int rc = extraction_run_range(user_id, conversation_id, history, &range);
   json_object_put(history);
   r->outside = range.outside;
   if (rc == MEMORY_EXTRACTION_BUSY) {
      rc = MEMORY_EXTRACT_RANGE_BUSY;
   }
   if (confirm && rc == SUCCESS) {
      OLOG_INFO("memory_extraction: range run conv %lld rows %lld-%lld started for user %d "
                "(outside %d)",
                (long long)conversation_id, (long long)from, (long long)to, user_id, r->outside);
   }
   return refusal != SUCCESS ? refusal : rc;
}

const char *memory_extract_range_refusal_text(int code) {
   switch (code) {
      case SUCCESS:
         return "";
      case MEMORY_EXTRACT_RANGE_NOT_FOUND:
         return "Conversation not found for this user";
      case MEMORY_EXTRACT_RANGE_PRIVATE:
         return "The conversation is private";
      case MEMORY_EXTRACT_RANGE_BAD_RANGE:
         return "--from must be at least 1 and no more than --to";
      case MEMORY_EXTRACT_RANGE_PAST_CURSOR:
         return "--to is past the conversation's extraction cursor; normal extraction reads "
                "those rows";
      case MEMORY_EXTRACT_RANGE_FORGOTTEN:
         return "The range reaches rows from before the user forgot this conversation's "
                "memories, or all of them; they stay forgotten";
      case MEMORY_EXTRACT_RANGE_NO_USER_ROW:
         return "Nothing in the range is the user's";
      case MEMORY_EXTRACT_RANGE_OVERLAP:
         return "An earlier range run already covers part of this range";
      case MEMORY_EXTRACT_RANGE_BUSY:
         return "An extraction for this user is running; try again when it ends";
      case MEMORY_EXTRACT_RANGE_JOB:
         return "The conversation is a background job's";
      case MEMORY_EXTRACT_RANGE_DISABLED:
         return "Memory is turned off";
      default:
         return "Range extraction failed (see the daemon log)";
   }
}

/* An outside kind's name, for the reply. */
static const char *outside_name(int kind) {
   static const char *const k_names[] = { "none",     "web", "email",      "research", "messaging",
                                          "document", "mcp", "other tool", "unknown",  "calendar" };
   _Static_assert(sizeof(k_names) / sizeof(k_names[0]) == OUTSIDE_KIND_MAX + 1,
                  "a name for every outside kind");
   if (kind < 0 || kind > OUTSIDE_KIND_MAX) {
      return "not known";
   }
   return k_names[kind];
}

void memory_extract_range_describe(int code,
                                   bool confirm,
                                   int64_t from,
                                   int64_t to,
                                   const memory_extract_range_report_t *r,
                                   char *buf,
                                   size_t size) {
   int n = 0;
   if (r->counted) {
      n = snprintf(buf, size,
                   "%s rows %lld-%lld (extraction cursor %lld)\n"
                   "  Rows:        user %d, assistant %d, tool %d, other %d\n"
                   "  Outside:     %s\n"
                   "  Earlier run: %s\n",
                   confirm ? "Range extraction:" : "Range extraction (dry run):", (long long)from,
                   (long long)to, (long long)r->cursor, r->user, r->assistant, r->tool, r->other,
                   outside_name(r->outside), r->overlap ? "overlaps" : "none");
   }
   if (n < 0 || (size_t)n >= size) {
      n = 0;
   }
   if (code != SUCCESS) {
      snprintf(buf + n, size - (size_t)n, "%s%s",
               confirm ? "Refused: " : "Would refuse: ", memory_extract_range_refusal_text(code));
   } else {
      snprintf(buf + n, size - (size_t)n, "%s",
               confirm ? "Started (the result is logged when it ends)."
                       : "Would run (a confirmed run can still be refused if the user's memory "
                         "is busy at that moment).  Re-run with --confirm to extract.");
   }
}
