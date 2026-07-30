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
 * Authentication Database - Conversation History Module
 *
 * Handles user conversation storage and retrieval:
 * - Create, get, list, rename, delete conversations
 * - Add and retrieve messages
 * - Search by title and content
 * - Conversation continuation (compaction)
 * - Context token tracking
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "logging.h"

/* =============================================================================
 * Helper Functions
 * ============================================================================= */

/**
 * @brief Build a LIKE pattern with escaped wildcards
 *
 * Escapes SQL LIKE wildcards (%, _, \) in the input and wraps with %...%
 * Uses backslash as the escape character.
 *
 * @param query Input search query
 * @param pattern Output buffer for escaped pattern
 * @param pattern_size Size of output buffer
 */
static void build_like_pattern(const char *query, char *pattern, size_t pattern_size) {
   if (!query || !pattern || pattern_size < 4) {
      if (pattern && pattern_size > 0) {
         pattern[0] = '\0';
      }
      return;
   }

   size_t j = 0;
   pattern[j++] = '%'; /* Leading wildcard */

   for (size_t i = 0; query[i] && j < pattern_size - 2; i++) {
      char c = query[i];
      /* Escape LIKE special characters: % _ \ */
      if (c == '%' || c == '_' || c == '\\') {
         if (j < pattern_size - 3) {
            pattern[j++] = '\\';
            pattern[j++] = c;
         }
      } else {
         pattern[j++] = c;
      }
   }

   if (j < pattern_size - 1) {
      pattern[j++] = '%'; /* Trailing wildcard */
   }
   pattern[j] = '\0';
}

/* =============================================================================
 * Conversation CRUD Operations
 * ============================================================================= */

int conv_db_create(int user_id, const char *title, int64_t *conv_id_out) {
   if (user_id <= 0 || !conv_id_out) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Check conversation limit per user */
   if (CONV_MAX_PER_USER > 0) {
      sqlite3_reset(s_db.stmt_conv_count);
      sqlite3_bind_int(s_db.stmt_conv_count, 1, user_id);
      if (sqlite3_step(s_db.stmt_conv_count) == SQLITE_ROW) {
         int count = sqlite3_column_int(s_db.stmt_conv_count, 0);
         if (count >= CONV_MAX_PER_USER) {
            sqlite3_reset(s_db.stmt_conv_count);
            AUTH_DB_UNLOCK();
            return AUTH_DB_LIMIT_EXCEEDED;
         }
      }
      sqlite3_reset(s_db.stmt_conv_count);
   }

   time_t now = time(NULL);

   /* Use default title if none provided, truncate if too long */
   char safe_title[CONV_TITLE_MAX];
   if (title && title[0] != '\0') {
      strncpy(safe_title, title, CONV_TITLE_MAX - 1);
      safe_title[CONV_TITLE_MAX - 1] = '\0';
   } else {
      strcpy(safe_title, "New Conversation");
   }

   sqlite3_reset(s_db.stmt_conv_create);
   sqlite3_bind_int(s_db.stmt_conv_create, 1, user_id);
   sqlite3_bind_text(s_db.stmt_conv_create, 2, safe_title, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(s_db.stmt_conv_create, 3, (int64_t)now);
   sqlite3_bind_int64(s_db.stmt_conv_create, 4, (int64_t)now);
   sqlite3_bind_int64(s_db.stmt_conv_create, 5, (int64_t)now); /* anchor_date = now (v42) */

   int rc = sqlite3_step(s_db.stmt_conv_create);
   sqlite3_reset(s_db.stmt_conv_create);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("conv_db_create: insert failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   *conv_id_out = sqlite3_last_insert_rowid(s_db.db);

   AUTH_DB_UNLOCK();

   OLOG_INFO("Created conversation %lld for user %d", (long long)*conv_id_out, user_id);
   return AUTH_DB_SUCCESS;
}

int conv_db_create_with_origin(int user_id,
                               const char *title,
                               const char *origin,
                               int64_t *conv_id_out) {
   if (user_id <= 0 || !conv_id_out) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Check conversation limit per user */
   if (CONV_MAX_PER_USER > 0) {
      sqlite3_reset(s_db.stmt_conv_count);
      sqlite3_bind_int(s_db.stmt_conv_count, 1, user_id);
      if (sqlite3_step(s_db.stmt_conv_count) == SQLITE_ROW) {
         int count = sqlite3_column_int(s_db.stmt_conv_count, 0);
         if (count >= CONV_MAX_PER_USER) {
            sqlite3_reset(s_db.stmt_conv_count);
            AUTH_DB_UNLOCK();
            return AUTH_DB_LIMIT_EXCEEDED;
         }
      }
      sqlite3_reset(s_db.stmt_conv_count);
   }

   time_t now = time(NULL);

   /* Use default title if none provided, truncate if too long */
   char safe_title[CONV_TITLE_MAX];
   if (title && title[0] != '\0') {
      strncpy(safe_title, title, CONV_TITLE_MAX - 1);
      safe_title[CONV_TITLE_MAX - 1] = '\0';
   } else {
      strcpy(safe_title, "Voice Conversation");
   }

   /* Use 'webui' as default origin if not specified */
   const char *safe_origin = (origin && origin[0] != '\0') ? origin : "webui";

   /* Insert with origin field using prepared statement */
   sqlite3_reset(s_db.stmt_conv_create_origin);
   sqlite3_bind_int(s_db.stmt_conv_create_origin, 1, user_id);
   sqlite3_bind_text(s_db.stmt_conv_create_origin, 2, safe_title, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(s_db.stmt_conv_create_origin, 3, (int64_t)now);
   sqlite3_bind_int64(s_db.stmt_conv_create_origin, 4, (int64_t)now);
   sqlite3_bind_text(s_db.stmt_conv_create_origin, 5, safe_origin, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(s_db.stmt_conv_create_origin, 6, (int64_t)now); /* anchor_date = now (v42) */

   int rc = sqlite3_step(s_db.stmt_conv_create_origin);
   sqlite3_reset(s_db.stmt_conv_create_origin);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("conv_db_create_with_origin: insert failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   *conv_id_out = sqlite3_last_insert_rowid(s_db.db);

   AUTH_DB_UNLOCK();

   OLOG_INFO("Created %s conversation %lld for user %d", safe_origin, (long long)*conv_id_out,
             user_id);
   return AUTH_DB_SUCCESS;
}

int conv_db_reassign(int64_t conv_id, int new_user_id) {
   if (conv_id <= 0 || new_user_id <= 0) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Verify target user exists before reassigning */
   const char *check_sql = "SELECT 1 FROM users WHERE id = ?";
   sqlite3_stmt *check_stmt;
   int rc = sqlite3_prepare_v2(s_db.db, check_sql, -1, &check_stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("conv_db_reassign: user check prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int(check_stmt, 1, new_user_id);
   rc = sqlite3_step(check_stmt);
   sqlite3_finalize(check_stmt);
   if (rc != SQLITE_ROW) {
      OLOG_WARNING("conv_db_reassign: target user %d does not exist", new_user_id);
      AUTH_DB_UNLOCK();
      return AUTH_DB_INVALID;
   }

   /* Update conversation to new user */
   sqlite3_reset(s_db.stmt_conv_reassign);
   sqlite3_bind_int(s_db.stmt_conv_reassign, 1, new_user_id);
   sqlite3_bind_int64(s_db.stmt_conv_reassign, 2, conv_id);

   rc = sqlite3_step(s_db.stmt_conv_reassign);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(s_db.stmt_conv_reassign);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("conv_db_reassign: update failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   if (changes == 0) {
      return AUTH_DB_NOT_FOUND;
   }

   OLOG_INFO("Reassigned conversation %lld to user %d", (long long)conv_id, new_user_id);
   return AUTH_DB_SUCCESS;
}

int conv_db_get(int64_t conv_id, int user_id, conversation_t *conv_out) {
   if (conv_id <= 0 || !conv_out) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_conv_get);
   sqlite3_bind_int64(s_db.stmt_conv_get, 1, conv_id);

   int rc = sqlite3_step(s_db.stmt_conv_get);
   if (rc != SQLITE_ROW) {
      sqlite3_reset(s_db.stmt_conv_get);
      AUTH_DB_UNLOCK();
      return (rc == SQLITE_DONE) ? AUTH_DB_NOT_FOUND : AUTH_DB_FAILURE;
   }

   /* Check ownership */
   int owner_id = sqlite3_column_int(s_db.stmt_conv_get, 1);
   if (owner_id != user_id) {
      sqlite3_reset(s_db.stmt_conv_get);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FORBIDDEN;
   }

   memset(conv_out, 0, sizeof(*conv_out));
   conv_out->id = sqlite3_column_int64(s_db.stmt_conv_get, 0);
   conv_out->user_id = owner_id;

   const char *title = (const char *)sqlite3_column_text(s_db.stmt_conv_get, 2);
   if (title) {
      strncpy(conv_out->title, title, CONV_TITLE_MAX - 1);
      conv_out->title[CONV_TITLE_MAX - 1] = '\0';
   }

   conv_out->created_at = (time_t)sqlite3_column_int64(s_db.stmt_conv_get, 3);
   conv_out->updated_at = (time_t)sqlite3_column_int64(s_db.stmt_conv_get, 4);
   conv_out->message_count = sqlite3_column_int(s_db.stmt_conv_get, 5);
   conv_out->is_archived = sqlite3_column_int(s_db.stmt_conv_get, 6) != 0;
   conv_out->context_tokens = sqlite3_column_int(s_db.stmt_conv_get, 7);
   conv_out->context_max = sqlite3_column_int(s_db.stmt_conv_get, 8);

   /* Continuation fields (schema v7+) */
   conv_out->continued_from = sqlite3_column_int64(s_db.stmt_conv_get, 9);
   const char *summary = (const char *)sqlite3_column_text(s_db.stmt_conv_get, 10);
   conv_out->compaction_summary = summary ? strdup(summary) : NULL;

   /* Per-conversation LLM settings (schema v11+) */
   const char *llm_type = (const char *)sqlite3_column_text(s_db.stmt_conv_get, 11);
   if (llm_type) {
      strncpy(conv_out->llm_type, llm_type, sizeof(conv_out->llm_type) - 1);
      conv_out->llm_type[sizeof(conv_out->llm_type) - 1] = '\0';
   }
   const char *cloud_provider = (const char *)sqlite3_column_text(s_db.stmt_conv_get, 12);
   if (cloud_provider) {
      strncpy(conv_out->cloud_provider, cloud_provider, sizeof(conv_out->cloud_provider) - 1);
      conv_out->cloud_provider[sizeof(conv_out->cloud_provider) - 1] = '\0';
   }
   const char *model = (const char *)sqlite3_column_text(s_db.stmt_conv_get, 13);
   if (model) {
      strncpy(conv_out->model, model, sizeof(conv_out->model) - 1);
      conv_out->model[sizeof(conv_out->model) - 1] = '\0';
   }
   const char *tools_mode = (const char *)sqlite3_column_text(s_db.stmt_conv_get, 14);
   if (tools_mode) {
      strncpy(conv_out->tools_mode, tools_mode, sizeof(conv_out->tools_mode) - 1);
      conv_out->tools_mode[sizeof(conv_out->tools_mode) - 1] = '\0';
   }
   const char *thinking_mode = (const char *)sqlite3_column_text(s_db.stmt_conv_get, 15);
   if (thinking_mode) {
      strncpy(conv_out->thinking_mode, thinking_mode, sizeof(conv_out->thinking_mode) - 1);
      conv_out->thinking_mode[sizeof(conv_out->thinking_mode) - 1] = '\0';
   }

   /* Privacy flag (schema v16+) */
   conv_out->is_private = sqlite3_column_int(s_db.stmt_conv_get, 16) != 0;

   /* Origin field (schema v17+) */
   const char *origin = (const char *)sqlite3_column_text(s_db.stmt_conv_get, 17);
   if (origin) {
      strncpy(conv_out->origin, origin, sizeof(conv_out->origin) - 1);
      conv_out->origin[sizeof(conv_out->origin) - 1] = '\0';
   } else {
      strcpy(conv_out->origin, "webui"); /* Default for old conversations */
   }

   /* Reasoning effort (schema v36+) */
   const char *reasoning_effort = (const char *)sqlite3_column_text(s_db.stmt_conv_get, 18);
   if (reasoning_effort) {
      strncpy(conv_out->reasoning_effort, reasoning_effort, sizeof(conv_out->reasoning_effort) - 1);
      conv_out->reasoning_effort[sizeof(conv_out->reasoning_effort) - 1] = '\0';
   }

   /* Compaction watermark (schema v67+) */
   conv_out->context_watermark_msg_id = sqlite3_column_int64(s_db.stmt_conv_get, 19);

   /* Pinned flag (schema v69+) */
   conv_out->is_pinned = sqlite3_column_int(s_db.stmt_conv_get, 20) != 0;

   sqlite3_reset(s_db.stmt_conv_get);
   AUTH_DB_UNLOCK();

   return AUTH_DB_SUCCESS;
}

void conv_free(conversation_t *conv) {
   if (!conv)
      return;
   if (conv->compaction_summary) {
      free(conv->compaction_summary);
      conv->compaction_summary = NULL;
   }
}

int conv_db_create_continuation(int user_id,
                                int64_t parent_id,
                                const char *compaction_summary,
                                int64_t *conv_id_out) {
   if (user_id <= 0 || parent_id <= 0 || !conv_id_out) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Verify parent exists and belongs to user, then archive it */
   const char *sql_archive = "UPDATE conversations SET is_archived = 1, updated_at = ? "
                             "WHERE id = ? AND user_id = ?";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql_archive, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("conv_db_create_continuation: prepare archive failed: %s",
                 sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   time_t now = time(NULL);
   sqlite3_bind_int64(stmt, 1, (int64_t)now);
   sqlite3_bind_int64(stmt, 2, parent_id);
   sqlite3_bind_int(stmt, 3, user_id);

   rc = sqlite3_step(stmt);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("conv_db_create_continuation: archive failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   int changes = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);

   if (changes == 0) {
      /* Parent not found or doesn't belong to user */
      AUTH_DB_UNLOCK();
      return AUTH_DB_NOT_FOUND;
   }

   /* Get parent title and LLM settings for the continuation */
   const char *sql_get_parent =
       "SELECT title, llm_type, cloud_provider, model, tools_mode, thinking_mode, reasoning_effort "
       "FROM conversations WHERE id = ?";
   rc = sqlite3_prepare_v2(s_db.db, sql_get_parent, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(stmt, 1, parent_id);

   char parent_title[CONV_TITLE_MAX] = "Continued";
   char parent_llm_type[16] = "";
   char parent_cloud_provider[16] = "";
   char parent_model[64] = "";
   char parent_tools_mode[16] = "";
   char parent_thinking_mode[16] = "";
   char parent_reasoning_effort[16] = "";

   if (sqlite3_step(stmt) == SQLITE_ROW) {
      const char *title = (const char *)sqlite3_column_text(stmt, 0);
      if (title) {
         snprintf(parent_title, sizeof(parent_title), "%s (cont.)", title);
      }
      /* Copy parent LLM settings (explicit null termination for safety) */
      const char *val;
      if ((val = (const char *)sqlite3_column_text(stmt, 1)) != NULL) {
         strncpy(parent_llm_type, val, sizeof(parent_llm_type) - 1);
         parent_llm_type[sizeof(parent_llm_type) - 1] = '\0';
      }
      if ((val = (const char *)sqlite3_column_text(stmt, 2)) != NULL) {
         strncpy(parent_cloud_provider, val, sizeof(parent_cloud_provider) - 1);
         parent_cloud_provider[sizeof(parent_cloud_provider) - 1] = '\0';
      }
      if ((val = (const char *)sqlite3_column_text(stmt, 3)) != NULL) {
         strncpy(parent_model, val, sizeof(parent_model) - 1);
         parent_model[sizeof(parent_model) - 1] = '\0';
      }
      if ((val = (const char *)sqlite3_column_text(stmt, 4)) != NULL) {
         strncpy(parent_tools_mode, val, sizeof(parent_tools_mode) - 1);
         parent_tools_mode[sizeof(parent_tools_mode) - 1] = '\0';
      }
      if ((val = (const char *)sqlite3_column_text(stmt, 5)) != NULL) {
         strncpy(parent_thinking_mode, val, sizeof(parent_thinking_mode) - 1);
         parent_thinking_mode[sizeof(parent_thinking_mode) - 1] = '\0';
      }
      if ((val = (const char *)sqlite3_column_text(stmt, 6)) != NULL) {
         strncpy(parent_reasoning_effort, val, sizeof(parent_reasoning_effort) - 1);
         parent_reasoning_effort[sizeof(parent_reasoning_effort) - 1] = '\0';
      }
   }
   sqlite3_finalize(stmt);

   /* Create continuation conversation with inherited LLM settings.
    * anchor_date (v42) is set to now() rather than inherited — relative phrases
    * in the continuation should resolve against the continuation's start, not
    * the parent's.  In the dominant case (compaction-driven continuation in the
    * same session) the choice is functionally equivalent to inheriting because
    * both anchors fall on the same calendar day; the divergence matters only
    * for forks resumed days+ later, where time(NULL) is unambiguously correct. */
   const char *sql_create =
       "INSERT INTO conversations (user_id, title, created_at, updated_at, continued_from, "
       "compaction_summary, llm_type, cloud_provider, model, tools_mode, thinking_mode, "
       "reasoning_effort, anchor_date) "
       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
   rc = sqlite3_prepare_v2(s_db.db, sql_create, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("conv_db_create_continuation: prepare insert failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, parent_title, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 3, (int64_t)now);
   sqlite3_bind_int64(stmt, 4, (int64_t)now);
   sqlite3_bind_int64(stmt, 5, parent_id);
   if (compaction_summary) {
      sqlite3_bind_text(stmt, 6, compaction_summary, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(stmt, 6);
   }
   /* Bind inherited LLM settings */
   sqlite3_bind_text(stmt, 7, parent_llm_type, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 8, parent_cloud_provider, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 9, parent_model, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 10, parent_tools_mode, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 11, parent_thinking_mode, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 12, parent_reasoning_effort, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 13, (int64_t)now);

   rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("conv_db_create_continuation: insert failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   *conv_id_out = sqlite3_last_insert_rowid(s_db.db);

   AUTH_DB_UNLOCK();

   OLOG_INFO("Created continuation conversation %lld from parent %lld for user %d",
             (long long)*conv_id_out, (long long)parent_id, user_id);
   return AUTH_DB_SUCCESS;
}

/* =============================================================================
 * Conversation Listing Operations
 * ============================================================================= */

int conv_db_list(int user_id,
                 bool include_archived,
                 const conv_pagination_t *pagination,
                 conversation_callback_t callback,
                 void *ctx) {
   if (user_id <= 0 || !callback) {
      return AUTH_DB_INVALID;
   }

   int limit = CONV_LIST_DEFAULT_LIMIT;
   int offset = 0;
   if (pagination) {
      limit = (pagination->limit > 0 && pagination->limit <= CONV_LIST_MAX_LIMIT)
                  ? pagination->limit
                  : CONV_LIST_DEFAULT_LIMIT;
      offset = (pagination->offset >= 0) ? pagination->offset : 0;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_conv_list);
   sqlite3_bind_int(s_db.stmt_conv_list, 1, user_id);
   sqlite3_bind_int(s_db.stmt_conv_list, 2, include_archived ? 1 : 0);
   sqlite3_bind_int(s_db.stmt_conv_list, 3, limit);
   sqlite3_bind_int(s_db.stmt_conv_list, 4, offset);

   int rc;
   while ((rc = sqlite3_step(s_db.stmt_conv_list)) == SQLITE_ROW) {
      conversation_t conv = { 0 };

      conv.id = sqlite3_column_int64(s_db.stmt_conv_list, 0);
      conv.user_id = sqlite3_column_int(s_db.stmt_conv_list, 1);

      const char *title = (const char *)sqlite3_column_text(s_db.stmt_conv_list, 2);
      if (title) {
         strncpy(conv.title, title, CONV_TITLE_MAX - 1);
         conv.title[CONV_TITLE_MAX - 1] = '\0';
      }

      conv.created_at = (time_t)sqlite3_column_int64(s_db.stmt_conv_list, 3);
      conv.updated_at = (time_t)sqlite3_column_int64(s_db.stmt_conv_list, 4);
      conv.message_count = sqlite3_column_int(s_db.stmt_conv_list, 5);
      conv.is_archived = sqlite3_column_int(s_db.stmt_conv_list, 6) != 0;
      conv.context_tokens = sqlite3_column_int(s_db.stmt_conv_list, 7);
      conv.context_max = sqlite3_column_int(s_db.stmt_conv_list, 8);

      /* Continuation fields - only load continued_from for list view (chain indicator) */
      conv.continued_from = sqlite3_column_int64(s_db.stmt_conv_list, 9);
      conv.compaction_summary = NULL; /* Load on demand via conv_db_get */

      /* Privacy flag (schema v16+) */
      conv.is_private = sqlite3_column_int(s_db.stmt_conv_list, 11) != 0;

      /* Origin field (schema v17+) */
      const char *origin = (const char *)sqlite3_column_text(s_db.stmt_conv_list, 12);
      if (origin) {
         strncpy(conv.origin, origin, sizeof(conv.origin) - 1);
         conv.origin[sizeof(conv.origin) - 1] = '\0';
      } else {
         strcpy(conv.origin, "webui");
      }

      /* Pinned flag (schema v69+) */
      conv.is_pinned = sqlite3_column_int(s_db.stmt_conv_list, 13) != 0;

      if (callback(&conv, ctx) != 0) {
         break; /* Callback requested stop */
      }
   }

   sqlite3_reset(s_db.stmt_conv_list);
   AUTH_DB_UNLOCK();

   return AUTH_DB_SUCCESS;
}

int conv_db_list_all(bool include_archived,
                     const conv_pagination_t *pagination,
                     conversation_all_callback_t callback,
                     void *ctx) {
   if (!callback) {
      return AUTH_DB_INVALID;
   }

   int limit = CONV_LIST_DEFAULT_LIMIT;
   int offset = 0;
   if (pagination) {
      limit = (pagination->limit > 0 && pagination->limit <= CONV_LIST_MAX_LIMIT)
                  ? pagination->limit
                  : CONV_LIST_DEFAULT_LIMIT;
      offset = (pagination->offset >= 0) ? pagination->offset : 0;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_conv_list_all);
   sqlite3_bind_int(s_db.stmt_conv_list_all, 1, include_archived ? 1 : 0);
   sqlite3_bind_int(s_db.stmt_conv_list_all, 2, limit);
   sqlite3_bind_int(s_db.stmt_conv_list_all, 3, offset);

   int rc;
   while ((rc = sqlite3_step(s_db.stmt_conv_list_all)) == SQLITE_ROW) {
      conversation_t conv = { 0 };
      char username[AUTH_USERNAME_MAX] = { 0 };

      conv.id = sqlite3_column_int64(s_db.stmt_conv_list_all, 0);
      conv.user_id = sqlite3_column_int(s_db.stmt_conv_list_all, 1);

      const char *title = (const char *)sqlite3_column_text(s_db.stmt_conv_list_all, 2);
      if (title) {
         strncpy(conv.title, title, CONV_TITLE_MAX - 1);
         conv.title[CONV_TITLE_MAX - 1] = '\0';
      }

      conv.created_at = (time_t)sqlite3_column_int64(s_db.stmt_conv_list_all, 3);
      conv.updated_at = (time_t)sqlite3_column_int64(s_db.stmt_conv_list_all, 4);
      conv.message_count = sqlite3_column_int(s_db.stmt_conv_list_all, 5);
      conv.is_archived = sqlite3_column_int(s_db.stmt_conv_list_all, 6) != 0;
      conv.context_tokens = sqlite3_column_int(s_db.stmt_conv_list_all, 7);
      conv.context_max = sqlite3_column_int(s_db.stmt_conv_list_all, 8);
      conv.continued_from = sqlite3_column_int64(s_db.stmt_conv_list_all, 9);
      conv.compaction_summary = NULL;

      /* Privacy flag (schema v16+) */
      conv.is_private = sqlite3_column_int(s_db.stmt_conv_list_all, 11) != 0;

      /* Origin field (schema v17+) */
      const char *origin = (const char *)sqlite3_column_text(s_db.stmt_conv_list_all, 12);
      if (origin) {
         strncpy(conv.origin, origin, sizeof(conv.origin) - 1);
         conv.origin[sizeof(conv.origin) - 1] = '\0';
      } else {
         strcpy(conv.origin, "webui");
      }

      const char *uname = (const char *)sqlite3_column_text(s_db.stmt_conv_list_all, 13);
      if (uname) {
         strncpy(username, uname, AUTH_USERNAME_MAX - 1);
         username[AUTH_USERNAME_MAX - 1] = '\0';
      }

      /* Pinned flag (schema v69+) */
      conv.is_pinned = sqlite3_column_int(s_db.stmt_conv_list_all, 14) != 0;

      if (callback(&conv, username, ctx) != 0) {
         break;
      }
   }

   sqlite3_reset(s_db.stmt_conv_list_all);
   AUTH_DB_UNLOCK();

   return AUTH_DB_SUCCESS;
}

int conv_db_rename(int64_t conv_id, int user_id, const char *new_title) {
   if (conv_id <= 0 || !new_title || strlen(new_title) == 0) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_conv_rename);
   sqlite3_bind_text(s_db.stmt_conv_rename, 1, new_title, -1, SQLITE_STATIC);
   sqlite3_bind_int64(s_db.stmt_conv_rename, 2, conv_id);
   sqlite3_bind_int(s_db.stmt_conv_rename, 3, user_id);

   int rc = sqlite3_step(s_db.stmt_conv_rename);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(s_db.stmt_conv_rename);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return AUTH_DB_FAILURE;
   }

   /* No rows updated means either not found or forbidden */
   return (changes > 0) ? AUTH_DB_SUCCESS : AUTH_DB_NOT_FOUND;
}

int conv_db_auto_title(int64_t conv_id, int user_id, const char *title) {
   if (conv_id <= 0 || !title || title[0] == '\0') {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_conv_auto_title);
   sqlite3_bind_text(s_db.stmt_conv_auto_title, 1, title, -1, SQLITE_STATIC);
   sqlite3_bind_int64(s_db.stmt_conv_auto_title, 2, (int64_t)time(NULL));
   sqlite3_bind_int64(s_db.stmt_conv_auto_title, 3, conv_id);
   sqlite3_bind_int(s_db.stmt_conv_auto_title, 4, user_id);

   int rc = sqlite3_step(s_db.stmt_conv_auto_title);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(s_db.stmt_conv_auto_title);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return AUTH_DB_FAILURE;
   }

   return (changes > 0) ? AUTH_DB_SUCCESS : AUTH_DB_NOT_FOUND;
}

int conv_db_set_title_locked(int64_t conv_id, int user_id, int locked) {
   if (conv_id <= 0 || user_id <= 0) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_conv_set_title_locked);
   sqlite3_bind_int(s_db.stmt_conv_set_title_locked, 1, locked);
   sqlite3_bind_int64(s_db.stmt_conv_set_title_locked, 2, conv_id);
   sqlite3_bind_int(s_db.stmt_conv_set_title_locked, 3, user_id);

   int rc = sqlite3_step(s_db.stmt_conv_set_title_locked);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(s_db.stmt_conv_set_title_locked);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return AUTH_DB_FAILURE;
   }

   return (changes > 0) ? AUTH_DB_SUCCESS : AUTH_DB_NOT_FOUND;
}

int conv_db_set_private(int64_t conv_id, int user_id, bool is_private) {
   if (conv_id <= 0 || user_id <= 0) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_conv_set_private);
   sqlite3_bind_int(s_db.stmt_conv_set_private, 1, is_private ? 1 : 0);
   sqlite3_bind_int64(s_db.stmt_conv_set_private, 2, conv_id);
   sqlite3_bind_int(s_db.stmt_conv_set_private, 3, user_id);

   int rc = sqlite3_step(s_db.stmt_conv_set_private);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(s_db.stmt_conv_set_private);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("conv_db_set_private: update failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   if (changes > 0) {
      OLOG_INFO("Conversation %lld privacy set to %s", (long long)conv_id,
                is_private ? "private" : "public");
   }

   /* No rows updated means either not found or forbidden */
   return (changes > 0) ? AUTH_DB_SUCCESS : AUTH_DB_NOT_FOUND;
}

int conv_db_set_pinned(int64_t conv_id, int user_id, bool is_pinned) {
   if (conv_id <= 0 || user_id <= 0) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_conv_set_pinned);
   sqlite3_bind_int(s_db.stmt_conv_set_pinned, 1, is_pinned ? 1 : 0);
   sqlite3_bind_int64(s_db.stmt_conv_set_pinned, 2, conv_id);
   sqlite3_bind_int(s_db.stmt_conv_set_pinned, 3, user_id);

   int rc = sqlite3_step(s_db.stmt_conv_set_pinned);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(s_db.stmt_conv_set_pinned);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("conv_db_set_pinned: update failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   if (changes > 0) {
      OLOG_INFO("Conversation %lld %s", (long long)conv_id, is_pinned ? "pinned" : "unpinned");
   }

   /* No rows updated means either not found or forbidden */
   return (changes > 0) ? AUTH_DB_SUCCESS : AUTH_DB_NOT_FOUND;
}

int conv_db_is_private(int64_t conv_id, int user_id, bool *is_private_out) {
   if (conv_id <= 0 || !is_private_out) {
      return AUTH_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Use a direct query for efficiency (no prepared statement needed for rare calls) */
   const char *sql = "SELECT is_private FROM conversations WHERE id = ? AND user_id = ?";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, conv_id);
   sqlite3_bind_int(stmt, 2, user_id);

   rc = sqlite3_step(stmt);
   if (rc == SQLITE_ROW) {
      *is_private_out = (sqlite3_column_int(stmt, 0) != 0);
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();
      return AUTH_DB_SUCCESS;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   return AUTH_DB_NOT_FOUND;
}

int conv_db_get_anchor_date(int64_t conv_id, int64_t *anchor_out) {
   if (conv_id <= 0 || !anchor_out) {
      return AUTH_DB_FAILURE;
   }

   *anchor_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   const char *sql = "SELECT anchor_date FROM conversations WHERE id = ?";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, conv_id);

   rc = sqlite3_step(stmt);
   if (rc == SQLITE_ROW) {
      *anchor_out = sqlite3_column_int64(stmt, 0);
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();
      return AUTH_DB_SUCCESS;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return AUTH_DB_NOT_FOUND;
}

int conv_db_get_created_at(int64_t conv_id, int64_t *created_at_out) {
   if (conv_id <= 0 || !created_at_out) {
      return AUTH_DB_FAILURE;
   }

   *created_at_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   const char *sql = "SELECT created_at FROM conversations WHERE id = ?";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, conv_id);

   rc = sqlite3_step(stmt);
   if (rc == SQLITE_ROW) {
      *created_at_out = sqlite3_column_int64(stmt, 0);
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();
      return AUTH_DB_SUCCESS;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return AUTH_DB_NOT_FOUND;
}

int conv_db_force_anchor_date_unsafe(int64_t conv_id, int user_id, int64_t anchor) {
   if (conv_id <= 0 || user_id <= 0 || anchor < 0) {
      return AUTH_DB_FAILURE;
   }

   /* Surface accidental production calls in the log.  Bench is the only
    * legitimate caller; if this fires from anywhere else, the convention has
    * been violated and extracted facts may desync from the new anchor. */
   OLOG_WARNING("conv_db_force_anchor_date_unsafe: bench-only API invoked for conv %lld user %d",
                (long long)conv_id, user_id);

   AUTH_DB_LOCK_OR_FAIL();

   const char *sql = "UPDATE conversations SET anchor_date = ? WHERE id = ? AND user_id = ?";
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, anchor);
   sqlite3_bind_int64(stmt, 2, conv_id);
   sqlite3_bind_int(stmt, 3, user_id);

   rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return AUTH_DB_FAILURE;
   }
   if (changes == 0) {
      /* Either conv doesn't exist or user doesn't own it. */
      return AUTH_DB_NOT_FOUND;
   }
   return AUTH_DB_SUCCESS;
}

/* Null the provenance back-pointers on any memory row that referenced a
 * conversation just deleted.  The memory tables intentionally have NO foreign
 * key to conversations — a fact outlives the chat it was learned from — so
 * nothing nulls source_conversation_id automatically; without this, every
 * conversation delete leaves a dangling link behind (they had accumulated into
 * the thousands before this).  The rows (the knowledge) are kept; only the stale
 * pointer + its now-meaningless message range are cleared.  Caller holds the
 * auth_db lock.  A structural FK ON DELETE SET NULL would be cleaner but means
 * rebuilding memory_facts and its FTS shadow tables — deferred as too risky. */
static void clear_memory_provenance(int64_t conv_id) {
   static const char *const TABLES[] = { "memory_facts", "memory_preferences", "memory_summaries",
                                         "memory_relations" };
   for (size_t i = 0; i < sizeof(TABLES) / sizeof(TABLES[0]); i++) {
      char sql[192];
      snprintf(sql, sizeof(sql),
               "UPDATE %s SET source_conversation_id=NULL, source_msg_id_start=NULL, "
               "source_msg_id_end=NULL WHERE source_conversation_id=?",
               TABLES[i]); /* table name is a fixed allowlist literal, never input */
      sqlite3_stmt *st = NULL;
      if (sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL) != SQLITE_OK) {
         OLOG_WARNING("conv_db_delete: provenance-clear prepare for %s failed: %s", TABLES[i],
                      sqlite3_errmsg(s_db.db));
         continue;
      }
      sqlite3_bind_int64(st, 1, conv_id);
      if (sqlite3_step(st) != SQLITE_DONE) {
         OLOG_WARNING("conv_db_delete: provenance-clear on %s failed: %s", TABLES[i],
                      sqlite3_errmsg(s_db.db));
      }
      sqlite3_finalize(st);
   }
}

int conv_db_delete(int64_t conv_id, int user_id) {
   if (conv_id <= 0) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Messages are deleted automatically via CASCADE */
   sqlite3_reset(s_db.stmt_conv_delete);
   sqlite3_bind_int64(s_db.stmt_conv_delete, 1, conv_id);
   sqlite3_bind_int(s_db.stmt_conv_delete, 2, user_id);

   int rc = sqlite3_step(s_db.stmt_conv_delete);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(s_db.stmt_conv_delete);

   /* Only clear provenance if we actually deleted the row (owned + existed) —
    * otherwise the facts belong to whoever really owns conv_id. */
   if (rc == SQLITE_DONE && changes > 0) {
      clear_memory_provenance(conv_id);
   }

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return AUTH_DB_FAILURE;
   }

   if (changes > 0) {
      OLOG_INFO("Deleted conversation %lld for user %d", (long long)conv_id, user_id);
      return AUTH_DB_SUCCESS;
   }

   return AUTH_DB_NOT_FOUND;
}

int conv_db_delete_admin(int64_t conv_id) {
   if (conv_id <= 0) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_conv_delete_admin);
   sqlite3_bind_int64(s_db.stmt_conv_delete_admin, 1, conv_id);

   int rc = sqlite3_step(s_db.stmt_conv_delete_admin);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(s_db.stmt_conv_delete_admin);

   if (rc == SQLITE_DONE && changes > 0) {
      clear_memory_provenance(conv_id);
   }

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return AUTH_DB_FAILURE;
   }

   if (changes > 0) {
      OLOG_INFO("Admin deleted conversation %lld", (long long)conv_id);
      return AUTH_DB_SUCCESS;
   }

   return AUTH_DB_NOT_FOUND;
}

/* =============================================================================
 * Search Operations
 * ============================================================================= */

int conv_db_search(int user_id,
                   const char *query,
                   const conv_pagination_t *pagination,
                   conversation_callback_t callback,
                   void *ctx) {
   if (user_id <= 0 || !query || !callback) {
      return AUTH_DB_INVALID;
   }

   int limit = CONV_LIST_DEFAULT_LIMIT;
   int offset = 0;
   if (pagination) {
      limit = (pagination->limit > 0 && pagination->limit <= CONV_LIST_MAX_LIMIT)
                  ? pagination->limit
                  : CONV_LIST_DEFAULT_LIMIT;
      offset = (pagination->offset >= 0) ? pagination->offset : 0;
   }

   /* Build escaped LIKE pattern: %query% with wildcards escaped */
   char pattern[CONV_TITLE_MAX * 2 + 3]; /* Worst case: every char escaped + %...% */
   build_like_pattern(query, pattern, sizeof(pattern));

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_conv_search);
   sqlite3_bind_int(s_db.stmt_conv_search, 1, user_id);
   sqlite3_bind_text(s_db.stmt_conv_search, 2, pattern, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(s_db.stmt_conv_search, 3, limit);
   sqlite3_bind_int(s_db.stmt_conv_search, 4, offset);

   int rc;
   while ((rc = sqlite3_step(s_db.stmt_conv_search)) == SQLITE_ROW) {
      conversation_t conv = { 0 };

      conv.id = sqlite3_column_int64(s_db.stmt_conv_search, 0);
      conv.user_id = sqlite3_column_int(s_db.stmt_conv_search, 1);

      const char *title = (const char *)sqlite3_column_text(s_db.stmt_conv_search, 2);
      if (title) {
         strncpy(conv.title, title, CONV_TITLE_MAX - 1);
         conv.title[CONV_TITLE_MAX - 1] = '\0';
      }

      conv.created_at = (time_t)sqlite3_column_int64(s_db.stmt_conv_search, 3);
      conv.updated_at = (time_t)sqlite3_column_int64(s_db.stmt_conv_search, 4);
      conv.message_count = sqlite3_column_int(s_db.stmt_conv_search, 5);
      conv.is_archived = sqlite3_column_int(s_db.stmt_conv_search, 6) != 0;
      conv.context_tokens = sqlite3_column_int(s_db.stmt_conv_search, 7);
      conv.context_max = sqlite3_column_int(s_db.stmt_conv_search, 8);

      /* Continuation fields - only load continued_from for search results (chain indicator) */
      conv.continued_from = sqlite3_column_int64(s_db.stmt_conv_search, 9);
      conv.compaction_summary = NULL; /* Load on demand via conv_db_get */

      /* Privacy flag (schema v16+) */
      conv.is_private = sqlite3_column_int(s_db.stmt_conv_search, 11) != 0;

      /* Origin field (schema v17+) */
      const char *origin = (const char *)sqlite3_column_text(s_db.stmt_conv_search, 12);
      if (origin) {
         strncpy(conv.origin, origin, sizeof(conv.origin) - 1);
         conv.origin[sizeof(conv.origin) - 1] = '\0';
      } else {
         strcpy(conv.origin, "webui");
      }

      if (callback(&conv, ctx) != 0) {
         break;
      }
   }

   sqlite3_reset(s_db.stmt_conv_search);
   AUTH_DB_UNLOCK();

   return AUTH_DB_SUCCESS;
}

int conv_db_search_content(int user_id,
                           const char *query,
                           const conv_pagination_t *pagination,
                           conversation_callback_t callback,
                           void *ctx) {
   if (user_id <= 0 || !query || !callback) {
      return AUTH_DB_INVALID;
   }

   int limit = CONV_LIST_DEFAULT_LIMIT;
   int offset = 0;
   if (pagination) {
      limit = (pagination->limit > 0 && pagination->limit <= CONV_LIST_MAX_LIMIT)
                  ? pagination->limit
                  : CONV_LIST_DEFAULT_LIMIT;
      offset = (pagination->offset >= 0) ? pagination->offset : 0;
   }

   /* Build escaped LIKE pattern: %query% with wildcards escaped */
   char pattern[CONV_TITLE_MAX * 2 + 3]; /* Worst case: every char escaped + %...% */
   build_like_pattern(query, pattern, sizeof(pattern));

   AUTH_DB_LOCK_OR_FAIL();

   /* Use pre-prepared statement for content search */
   sqlite3_reset(s_db.stmt_conv_search_content);
   sqlite3_bind_int(s_db.stmt_conv_search_content, 1, user_id);
   sqlite3_bind_text(s_db.stmt_conv_search_content, 2, pattern, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(s_db.stmt_conv_search_content, 3, limit);
   sqlite3_bind_int(s_db.stmt_conv_search_content, 4, offset);

   int rc;
   while ((rc = sqlite3_step(s_db.stmt_conv_search_content)) == SQLITE_ROW) {
      conversation_t conv = { 0 };

      conv.id = sqlite3_column_int64(s_db.stmt_conv_search_content, 0);
      conv.user_id = sqlite3_column_int(s_db.stmt_conv_search_content, 1);

      const char *title = (const char *)sqlite3_column_text(s_db.stmt_conv_search_content, 2);
      if (title) {
         strncpy(conv.title, title, CONV_TITLE_MAX - 1);
         conv.title[CONV_TITLE_MAX - 1] = '\0';
      }

      conv.created_at = (time_t)sqlite3_column_int64(s_db.stmt_conv_search_content, 3);
      conv.updated_at = (time_t)sqlite3_column_int64(s_db.stmt_conv_search_content, 4);
      conv.message_count = sqlite3_column_int(s_db.stmt_conv_search_content, 5);
      conv.is_archived = sqlite3_column_int(s_db.stmt_conv_search_content, 6) != 0;
      conv.context_tokens = sqlite3_column_int(s_db.stmt_conv_search_content, 7);
      conv.context_max = sqlite3_column_int(s_db.stmt_conv_search_content, 8);

      /* Continuation fields - only load continued_from for search results (chain indicator) */
      conv.continued_from = sqlite3_column_int64(s_db.stmt_conv_search_content, 9);
      conv.compaction_summary = NULL; /* Load on demand via conv_db_get */

      /* Privacy flag (schema v16+) */
      conv.is_private = sqlite3_column_int(s_db.stmt_conv_search_content, 11) != 0;

      /* Origin field (schema v17+) */
      const char *origin = (const char *)sqlite3_column_text(s_db.stmt_conv_search_content, 12);
      if (origin) {
         strncpy(conv.origin, origin, sizeof(conv.origin) - 1);
         conv.origin[sizeof(conv.origin) - 1] = '\0';
      } else {
         strcpy(conv.origin, "webui");
      }

      if (callback(&conv, ctx) != 0) {
         break;
      }
   }

   sqlite3_reset(s_db.stmt_conv_search_content);
   AUTH_DB_UNLOCK();

   return AUTH_DB_SUCCESS;
}

/* =============================================================================
 * Context and Metadata Operations
 * ============================================================================= */

int conv_db_update_context(int64_t conv_id, int user_id, int context_tokens, int context_max) {
   if (conv_id <= 0) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Use prepared statement with ownership check in WHERE clause */
   sqlite3_reset(s_db.stmt_conv_update_context);
   sqlite3_bind_int(s_db.stmt_conv_update_context, 1, context_tokens);
   sqlite3_bind_int(s_db.stmt_conv_update_context, 2, context_max);
   sqlite3_bind_int64(s_db.stmt_conv_update_context, 3, conv_id);
   sqlite3_bind_int(s_db.stmt_conv_update_context, 4, user_id);

   int rc = sqlite3_step(s_db.stmt_conv_update_context);
   sqlite3_reset(s_db.stmt_conv_update_context);

   if (rc != SQLITE_DONE) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   int changes = sqlite3_changes(s_db.db);
   AUTH_DB_UNLOCK();

   /* No rows updated = conversation not found or wrong owner */
   return (changes > 0) ? AUTH_DB_SUCCESS : AUTH_DB_NOT_FOUND;
}

int conv_db_set_compaction_watermark(int64_t conv_id,
                                     int user_id,
                                     const char *summary,
                                     int64_t watermark_msg_id) {
   if (conv_id <= 0 || watermark_msg_id <= 0) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Single atomic monotonic UPDATE on the same conversation row — no archive,
    * no continuation.  The WHERE guard `? >= context_watermark_msg_id` makes a
    * stale async compaction a harmless no-op rather than a watermark rewind. */
   sqlite3_reset(s_db.stmt_conv_set_watermark);
   if (summary) {
      sqlite3_bind_text(s_db.stmt_conv_set_watermark, 1, summary, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(s_db.stmt_conv_set_watermark, 1);
   }
   sqlite3_bind_int64(s_db.stmt_conv_set_watermark, 2, watermark_msg_id);
   sqlite3_bind_int64(s_db.stmt_conv_set_watermark, 3, conv_id);
   sqlite3_bind_int(s_db.stmt_conv_set_watermark, 4, user_id);
   sqlite3_bind_int64(s_db.stmt_conv_set_watermark, 5, watermark_msg_id);

   int rc = sqlite3_step(s_db.stmt_conv_set_watermark);
   sqlite3_reset(s_db.stmt_conv_set_watermark);

   if (rc != SQLITE_DONE) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   /* 0 changes = not found / wrong owner / stale (guard rejected the rewind).
    * Stale is benign, so treat 0 changes as success for the caller's purposes. */
   AUTH_DB_UNLOCK();
   return AUTH_DB_SUCCESS;
}

void conv_db_format_compaction_context(int64_t conv_id,
                                       const char *summary,
                                       char *out,
                                       size_t out_len) {
   if (!out || out_len == 0) {
      return;
   }
   out[0] = '\0';
   if (!summary || !summary[0]) {
      return;
   }

   /* Reconstruct a [COMPACTED conv=N msgs=X-Y node=Z depth=D] marker from the
    * latest summary node so a RELOADED session keeps a context_expand handle to
    * the compacted originals (the live in-memory marker is built in
    * llm_context.c; keep the two formats recognizable to the same tool/parser).
    * Falls back to a plain summary line when no node metadata exists. */
   summary_node_t node = { 0 };
   if (summary_node_get_latest(conv_id, &node) == AUTH_DB_SUCCESS && node.msg_id_start > 0 &&
       node.msg_id_end > 0) {
      snprintf(out, out_len,
               "[COMPACTED conv=%lld msgs=%lld-%lld node=%lld depth=%d] "
               "Previous conversation context (summarized): %s",
               (long long)conv_id, (long long)node.msg_id_start, (long long)node.msg_id_end,
               (long long)node.id, node.depth, summary);
   } else {
      snprintf(out, out_len, "Previous conversation context (summarized): %s", summary);
   }
   summary_node_free(&node);
}

int conv_db_lock_llm_settings(int64_t conv_id,
                              int user_id,
                              const char *llm_type,
                              const char *cloud_provider,
                              const char *model,
                              const char *tools_mode,
                              const char *thinking_mode,
                              const char *reasoning_effort) {
   if (conv_id <= 0) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Update LLM settings only if message_count is 0 (prevents race conditions) */
   const char *sql = "UPDATE conversations SET "
                     "llm_type = ?, cloud_provider = ?, model = ?, "
                     "tools_mode = ?, thinking_mode = ?, reasoning_effort = ? "
                     "WHERE id = ? AND user_id = ? AND message_count = 0";

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare lock_llm_settings failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   /* Bind parameters - use empty string if NULL to avoid storing NULL */
   sqlite3_bind_text(stmt, 1, llm_type ? llm_type : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 2, cloud_provider ? cloud_provider : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, model ? model : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 4, tools_mode ? tools_mode : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 5, thinking_mode ? thinking_mode : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 6, reasoning_effort ? reasoning_effort : "", -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 7, conv_id);
   sqlite3_bind_int(stmt, 8, user_id);

   rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("auth_db: lock_llm_settings step failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   int changes = sqlite3_changes(s_db.db);
   AUTH_DB_UNLOCK();

   /* No rows updated = conversation not found, wrong owner, or already has messages */
   return (changes > 0) ? AUTH_DB_SUCCESS : AUTH_DB_NOT_FOUND;
}

int conv_db_fill_llm_settings_if_empty(int64_t conv_id,
                                       int user_id,
                                       const char *llm_type,
                                       const char *cloud_provider,
                                       const char *model,
                                       const char *tools_mode,
                                       const char *thinking_mode,
                                       const char *reasoning_effort) {
   if (conv_id <= 0) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Fill ONLY the columns currently NULL/empty. Never overwrites a value the user
    * chose or a continuation inherited from its parent, and has NO message_count
    * gate — so it's race-proof against the first-turn worker persisting the user
    * message (which would otherwise flip message_count to 1 and defeat the
    * lock-style guard). */
   const char *sql =
       "UPDATE conversations SET "
       "llm_type = CASE WHEN llm_type IS NULL OR llm_type = '' THEN ?1 ELSE llm_type END, "
       "cloud_provider = CASE WHEN cloud_provider IS NULL OR cloud_provider = '' "
       "THEN ?2 ELSE cloud_provider END, "
       "model = CASE WHEN model IS NULL OR model = '' THEN ?3 ELSE model END, "
       "tools_mode = CASE WHEN tools_mode IS NULL OR tools_mode = '' THEN ?4 ELSE tools_mode END, "
       "thinking_mode = CASE WHEN thinking_mode IS NULL OR thinking_mode = '' "
       "THEN ?5 ELSE thinking_mode END, "
       "reasoning_effort = CASE WHEN reasoning_effort IS NULL OR reasoning_effort = '' "
       "THEN ?6 ELSE reasoning_effort END "
       "WHERE id = ?7 AND user_id = ?8";

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare fill_llm_settings failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_text(stmt, 1, llm_type ? llm_type : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 2, cloud_provider ? cloud_provider : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, model ? model : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 4, tools_mode ? tools_mode : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 5, thinking_mode ? thinking_mode : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 6, reasoning_effort ? reasoning_effort : "", -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 7, conv_id);
   sqlite3_bind_int(stmt, 8, user_id);

   rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("auth_db: fill_llm_settings step failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   int changes = sqlite3_changes(s_db.db);
   AUTH_DB_UNLOCK();

   /* changes == 0 only when the id/user_id predicate matched no row. */
   return (changes > 0) ? AUTH_DB_SUCCESS : AUTH_DB_NOT_FOUND;
}

int conv_db_update_llm_settings(int64_t conv_id,
                                int user_id,
                                const char *llm_type,
                                const char *cloud_provider,
                                const char *model,
                                const char *tools_mode,
                                const char *thinking_mode,
                                const char *reasoning_effort) {
   if (conv_id <= 0) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   const char *sql = "UPDATE conversations SET "
                     "llm_type = ?, cloud_provider = ?, model = ?, "
                     "tools_mode = ?, thinking_mode = ?, reasoning_effort = ? "
                     "WHERE id = ? AND user_id = ?";

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare update_llm_settings failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_text(stmt, 1, llm_type ? llm_type : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 2, cloud_provider ? cloud_provider : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, model ? model : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 4, tools_mode ? tools_mode : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 5, thinking_mode ? thinking_mode : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 6, reasoning_effort ? reasoning_effort : "", -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 7, conv_id);
   sqlite3_bind_int(stmt, 8, user_id);

   rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("auth_db: update_llm_settings step failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   int changes = sqlite3_changes(s_db.db);
   AUTH_DB_UNLOCK();

   return (changes > 0) ? AUTH_DB_SUCCESS : AUTH_DB_NOT_FOUND;
}

/* =============================================================================
 * Message Operations
 * ============================================================================= */

int conv_db_add_message_with_tools(int64_t conv_id,
                                   int user_id,
                                   const char *role,
                                   const char *content,
                                   const char *tool_calls,
                                   const char *tool_call_id,
                                   const char *reasoning,
                                   int64_t *msg_id_out) {
   if (msg_id_out)
      *msg_id_out = 0;

   if (conv_id <= 0 || !role || !content) {
      return AUTH_DB_INVALID;
   }

   /* Validate role */
   if (strcmp(role, "system") != 0 && strcmp(role, "user") != 0 && strcmp(role, "assistant") != 0 &&
       strcmp(role, "tool") != 0) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   time_t now = time(NULL);

   /* Insert message with ownership check in single query */
   sqlite3_reset(s_db.stmt_msg_add);
   sqlite3_bind_int64(s_db.stmt_msg_add, 1, conv_id);
   sqlite3_bind_text(s_db.stmt_msg_add, 2, role, -1, SQLITE_STATIC);
   sqlite3_bind_text(s_db.stmt_msg_add, 3, content, -1, SQLITE_STATIC);
   if (tool_calls)
      sqlite3_bind_text(s_db.stmt_msg_add, 4, tool_calls, -1, SQLITE_STATIC);
   else
      sqlite3_bind_null(s_db.stmt_msg_add, 4);
   if (tool_call_id)
      sqlite3_bind_text(s_db.stmt_msg_add, 5, tool_call_id, -1, SQLITE_STATIC);
   else
      sqlite3_bind_null(s_db.stmt_msg_add, 5);
   if (reasoning)
      sqlite3_bind_text(s_db.stmt_msg_add, 6, reasoning, -1, SQLITE_STATIC);
   else
      sqlite3_bind_null(s_db.stmt_msg_add, 6);
   sqlite3_bind_int64(s_db.stmt_msg_add, 7, (int64_t)now);
   sqlite3_bind_int64(s_db.stmt_msg_add, 8, conv_id); /* For ownership check */
   sqlite3_bind_int(s_db.stmt_msg_add, 9, user_id);   /* For ownership check */

   int rc = sqlite3_step(s_db.stmt_msg_add);
   sqlite3_reset(s_db.stmt_msg_add);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("conv_db_add_message: insert failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   /* Check if message was actually inserted (ownership verification) */
   int changes = sqlite3_changes(s_db.db);
   if (changes == 0) {
      /* Distinguish between not found and forbidden:
       * Check if conversation exists but belongs to different user */
      const char *check_sql = "SELECT user_id FROM conversations WHERE id = ?";
      sqlite3_stmt *check_stmt;
      int check_rc = sqlite3_prepare_v2(s_db.db, check_sql, -1, &check_stmt, NULL);
      int result = AUTH_DB_NOT_FOUND;

      if (check_rc == SQLITE_OK) {
         sqlite3_bind_int64(check_stmt, 1, conv_id);
         if (sqlite3_step(check_stmt) == SQLITE_ROW) {
            /* Conversation exists but user doesn't own it */
            result = AUTH_DB_FORBIDDEN;
         }
         sqlite3_finalize(check_stmt);
      }

      AUTH_DB_UNLOCK();
      return result;
   }

   if (msg_id_out)
      *msg_id_out = sqlite3_last_insert_rowid(s_db.db);

   /* Update conversation metadata */
   sqlite3_reset(s_db.stmt_conv_update_meta);
   sqlite3_bind_int64(s_db.stmt_conv_update_meta, 1, (int64_t)now);
   sqlite3_bind_int64(s_db.stmt_conv_update_meta, 2, conv_id);

   rc = sqlite3_step(s_db.stmt_conv_update_meta);
   sqlite3_reset(s_db.stmt_conv_update_meta);

   AUTH_DB_UNLOCK();

   return (rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int conv_db_add_message_ex(int64_t conv_id,
                           int user_id,
                           const char *role,
                           const char *content,
                           int64_t *msg_id_out) {
   return conv_db_add_message_with_tools(conv_id, user_id, role, content, NULL, NULL, NULL,
                                         msg_id_out);
}

int conv_db_add_message(int64_t conv_id, int user_id, const char *role, const char *content) {
   return conv_db_add_message_ex(conv_id, user_id, role, content, NULL);
}

/* Read the content + tool/reasoning columns + created_at from a message-SELECT row whose
 * projection is (id, conversation_id, role, content, tool_calls, tool_call_id, reasoning,
 * created_at).  All pointers are borrowed (valid only during the callback). */
static void msg_read_columns(conversation_message_t *msg, sqlite3_stmt *stmt) {
   msg->content = (char *)sqlite3_column_text(stmt, 3);
   msg->tool_calls = (char *)sqlite3_column_text(stmt, 4);
   msg->tool_call_id = (char *)sqlite3_column_text(stmt, 5);
   msg->reasoning = (char *)sqlite3_column_text(stmt, 6);
   msg->created_at = (time_t)sqlite3_column_int64(stmt, 7);
}

int conv_db_get_messages(int64_t conv_id, int user_id, message_callback_t callback, void *ctx) {
   if (conv_id <= 0 || !callback) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Single query with ownership check via JOIN */
   sqlite3_reset(s_db.stmt_msg_get);
   sqlite3_bind_int64(s_db.stmt_msg_get, 1, conv_id);
   sqlite3_bind_int(s_db.stmt_msg_get, 2, user_id);

   int rc;
   while ((rc = sqlite3_step(s_db.stmt_msg_get)) == SQLITE_ROW) {
      conversation_message_t msg = { 0 };

      msg.id = sqlite3_column_int64(s_db.stmt_msg_get, 0);
      msg.conversation_id = sqlite3_column_int64(s_db.stmt_msg_get, 1);

      const char *role = (const char *)sqlite3_column_text(s_db.stmt_msg_get, 2);
      if (role) {
         strncpy(msg.role, role, CONV_ROLE_MAX - 1);
         msg.role[CONV_ROLE_MAX - 1] = '\0';
      }

      /* Column pointers are only valid during the callback */
      msg_read_columns(&msg, s_db.stmt_msg_get);

      if (callback(&msg, ctx) != 0) {
         break;
      }
   }

   sqlite3_reset(s_db.stmt_msg_get);
   AUTH_DB_UNLOCK();

   return AUTH_DB_SUCCESS;
}

int conv_db_get_messages_after(int64_t conv_id,
                               int user_id,
                               int64_t after_id,
                               message_callback_t callback,
                               void *ctx) {
   if (conv_id <= 0 || !callback) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Ownership check via JOIN; bounded to messages after the compaction watermark. */
   sqlite3_reset(s_db.stmt_msg_get_after);
   sqlite3_bind_int64(s_db.stmt_msg_get_after, 1, conv_id);
   sqlite3_bind_int(s_db.stmt_msg_get_after, 2, user_id);
   sqlite3_bind_int64(s_db.stmt_msg_get_after, 3, after_id);

   int rc;
   while ((rc = sqlite3_step(s_db.stmt_msg_get_after)) == SQLITE_ROW) {
      conversation_message_t msg = { 0 };

      msg.id = sqlite3_column_int64(s_db.stmt_msg_get_after, 0);
      msg.conversation_id = sqlite3_column_int64(s_db.stmt_msg_get_after, 1);

      const char *role = (const char *)sqlite3_column_text(s_db.stmt_msg_get_after, 2);
      if (role) {
         strncpy(msg.role, role, CONV_ROLE_MAX - 1);
         msg.role[CONV_ROLE_MAX - 1] = '\0';
      }

      /* Column pointers are only valid during the callback */
      msg_read_columns(&msg, s_db.stmt_msg_get_after);

      if (callback(&msg, ctx) != 0) {
         break;
      }
   }

   sqlite3_reset(s_db.stmt_msg_get_after);
   AUTH_DB_UNLOCK();

   return AUTH_DB_SUCCESS;
}

int conv_db_get_messages_admin(int64_t conv_id, message_callback_t callback, void *ctx) {
   if (conv_id <= 0 || !callback) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_msg_get_admin);
   sqlite3_bind_int64(s_db.stmt_msg_get_admin, 1, conv_id);

   int rc;
   while ((rc = sqlite3_step(s_db.stmt_msg_get_admin)) == SQLITE_ROW) {
      conversation_message_t msg = { 0 };

      msg.id = sqlite3_column_int64(s_db.stmt_msg_get_admin, 0);
      msg.conversation_id = sqlite3_column_int64(s_db.stmt_msg_get_admin, 1);

      const char *role = (const char *)sqlite3_column_text(s_db.stmt_msg_get_admin, 2);
      if (role) {
         strncpy(msg.role, role, CONV_ROLE_MAX - 1);
         msg.role[CONV_ROLE_MAX - 1] = '\0';
      }

      msg_read_columns(&msg, s_db.stmt_msg_get_admin);

      if (callback(&msg, ctx) != 0) {
         break;
      }
   }

   sqlite3_reset(s_db.stmt_msg_get_admin);
   AUTH_DB_UNLOCK();

   return AUTH_DB_SUCCESS;
}

/* =============================================================================
 * Utility Operations
 * ============================================================================= */

int conv_db_count(int user_id, int *count_out) {
   if (user_id <= 0 || !count_out) {
      return AUTH_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_conv_count);
   sqlite3_bind_int(s_db.stmt_conv_count, 1, user_id);

   if (sqlite3_step(s_db.stmt_conv_count) == SQLITE_ROW) {
      *count_out = sqlite3_column_int(s_db.stmt_conv_count, 0);
   } else {
      sqlite3_reset(s_db.stmt_conv_count);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_conv_count);
   AUTH_DB_UNLOCK();

   return AUTH_DB_SUCCESS;
}

int conv_db_find_continuation(int64_t parent_id, int user_id, int64_t *continuation_id_out) {
   if (parent_id <= 0 || user_id <= 0 || !continuation_id_out) {
      return AUTH_DB_FAILURE;
   }

   *continuation_id_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   /* Find conversation where continued_from = parent_id and user_id matches */
   const char *sql = "SELECT id FROM conversations "
                     "WHERE continued_from = ? AND user_id = ? "
                     "ORDER BY created_at DESC LIMIT 1";

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare find_continuation failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, parent_id);
   sqlite3_bind_int(stmt, 2, user_id);

   int result = AUTH_DB_NOT_FOUND;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      *continuation_id_out = sqlite3_column_int64(stmt, 0);
      result = AUTH_DB_SUCCESS;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   return result;
}

void conv_generate_title(const char *content, char *title_out, size_t max_len) {
   if (!content || !title_out || max_len == 0) {
      if (title_out && max_len > 0) {
         title_out[0] = '\0';
      }
      return;
   }

   /* Skip leading whitespace */
   while (*content && (*content == ' ' || *content == '\t' || *content == '\n')) {
      content++;
   }

   /* Target ~50 chars, but truncate at word boundary */
   size_t target_len = 50;
   if (target_len >= max_len) {
      target_len = max_len - 4; /* Leave room for "..." */
   }

   size_t content_len = strlen(content);
   if (content_len <= target_len) {
      /* Content fits entirely */
      strncpy(title_out, content, max_len - 1);
      title_out[max_len - 1] = '\0';

      /* Remove trailing newlines */
      size_t len = strlen(title_out);
      while (len > 0 && (title_out[len - 1] == '\n' || title_out[len - 1] == '\r')) {
         title_out[--len] = '\0';
      }
      return;
   }

   /* Find last word boundary before target_len */
   size_t cut_pos = target_len;
   while (cut_pos > 0 && content[cut_pos] != ' ' && content[cut_pos] != '\t') {
      cut_pos--;
   }

   /* If no word boundary found, just cut at target_len */
   if (cut_pos == 0) {
      cut_pos = target_len;
   }

   /* Copy and add ellipsis */
   strncpy(title_out, content, cut_pos);
   title_out[cut_pos] = '\0';

   /* Trim trailing whitespace before ellipsis */
   while (cut_pos > 0 && (title_out[cut_pos - 1] == ' ' || title_out[cut_pos - 1] == '\t')) {
      title_out[--cut_pos] = '\0';
   }

   /* Add ellipsis if there's room - use direct assignment instead of strcat */
   if (cut_pos + 3 < max_len) {
      title_out[cut_pos] = '.';
      title_out[cut_pos + 1] = '.';
      title_out[cut_pos + 2] = '.';
      title_out[cut_pos + 3] = '\0';
   }
}

/* =============================================================================
 * LCM Phase 3 — Message ID Resolution and Range Retrieval
 * ============================================================================= */

int conv_db_get_message_ids(int64_t conv_id, int user_id, int64_t **ids_out, int *count_out) {
   if (conv_id <= 0 || !ids_out || !count_out)
      return AUTH_DB_INVALID;

   *ids_out = NULL;
   *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   const char *sql = "SELECT m.id FROM messages m "
                     "INNER JOIN conversations c ON m.conversation_id = c.id "
                     "WHERE m.conversation_id = ? AND c.user_id = ? ORDER BY m.id ASC";

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("conv_db_get_message_ids: prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, conv_id);
   sqlite3_bind_int(stmt, 2, user_id);

   int capacity = 64;
   int count = 0;
   int64_t *ids = malloc(capacity * sizeof(int64_t));
   if (!ids) {
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   while (sqlite3_step(stmt) == SQLITE_ROW) {
      if (count >= capacity) {
         capacity *= 2;
         int64_t *tmp = realloc(ids, capacity * sizeof(int64_t));
         if (!tmp) {
            free(ids);
            sqlite3_finalize(stmt);
            AUTH_DB_UNLOCK();
            return AUTH_DB_FAILURE;
         }
         ids = tmp;
      }
      ids[count++] = sqlite3_column_int64(stmt, 0);
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   *ids_out = ids;
   *count_out = count;
   return AUTH_DB_SUCCESS;
}

int conv_db_get_messages_by_range(int64_t conv_id,
                                  int user_id,
                                  int64_t start_id,
                                  int64_t end_id,
                                  bool include_private,
                                  message_callback_t callback,
                                  void *ctx) {
   if (conv_id <= 0 || !callback || start_id <= 0 || end_id <= 0)
      return AUTH_DB_INVALID;

   AUTH_DB_LOCK_OR_FAIL();

   /* Explicit ownership check — return FORBIDDEN rather than silent empty result */
   sqlite3_reset(s_db.stmt_conv_get);
   sqlite3_bind_int64(s_db.stmt_conv_get, 1, conv_id);
   sqlite3_bind_int(s_db.stmt_conv_get, 2, user_id);
   int rc = sqlite3_step(s_db.stmt_conv_get);
   sqlite3_reset(s_db.stmt_conv_get);
   if (rc != SQLITE_ROW) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FORBIDDEN;
   }

   /* Defense-in-depth: when `include_private = false` (the memory/provenance
    * default), suppress rows from private conversations in SQL itself.  Even
    * if an upstream caller fails to filter, private content cannot leak. */
   const char *sql = include_private
                         ? "SELECT m.id, m.conversation_id, m.role, m.content, m.created_at "
                           "FROM messages m "
                           "INNER JOIN conversations c ON m.conversation_id = c.id "
                           "WHERE m.conversation_id = ? AND c.user_id = ? "
                           "AND m.id BETWEEN ? AND ? ORDER BY m.id ASC"
                         : "SELECT m.id, m.conversation_id, m.role, m.content, m.created_at "
                           "FROM messages m "
                           "INNER JOIN conversations c ON m.conversation_id = c.id "
                           "WHERE m.conversation_id = ? AND c.user_id = ? AND c.is_private = 0 "
                           "AND m.id BETWEEN ? AND ? ORDER BY m.id ASC";

   sqlite3_stmt *stmt = NULL;
   rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("conv_db_get_messages_by_range: prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, conv_id);
   sqlite3_bind_int(stmt, 2, user_id);
   sqlite3_bind_int64(stmt, 3, start_id);
   sqlite3_bind_int64(stmt, 4, end_id);

   while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
      conversation_message_t msg = { 0 };
      msg.id = sqlite3_column_int64(stmt, 0);
      msg.conversation_id = sqlite3_column_int64(stmt, 1);

      const char *role = (const char *)sqlite3_column_text(stmt, 2);
      if (role) {
         strncpy(msg.role, role, CONV_ROLE_MAX - 1);
         msg.role[CONV_ROLE_MAX - 1] = '\0';
      }

      msg.content = (char *)sqlite3_column_text(stmt, 3);
      msg.created_at = (time_t)sqlite3_column_int64(stmt, 4);

      if (callback(&msg, ctx) != 0)
         break;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   return AUTH_DB_SUCCESS;
}

int conv_db_get_max_msg_id(int64_t conv_id, int user_id, int64_t *max_id_out) {
   if (conv_id <= 0 || !max_id_out)
      return AUTH_DB_FAILURE;
   *max_id_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   /* Ownership check */
   sqlite3_reset(s_db.stmt_conv_get);
   sqlite3_bind_int64(s_db.stmt_conv_get, 1, conv_id);
   sqlite3_bind_int(s_db.stmt_conv_get, 2, user_id);
   int rc = sqlite3_step(s_db.stmt_conv_get);
   sqlite3_reset(s_db.stmt_conv_get);
   if (rc != SQLITE_ROW) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FORBIDDEN;
   }

   const char *sql = "SELECT COALESCE(MAX(id), 0) FROM messages WHERE conversation_id = ?";
   sqlite3_stmt *stmt = NULL;
   rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(stmt, 1, conv_id);
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      *max_id_out = sqlite3_column_int64(stmt, 0);
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return AUTH_DB_SUCCESS;
}

/* =============================================================================
 * Summary Nodes (LCM Phase 4 — hierarchical summaries)
 * ============================================================================= */

void summary_node_free(summary_node_t *node) {
   if (node) {
      free(node->summary_text);
      node->summary_text = NULL;
   }
}

int summary_node_create(const summary_node_t *node, int64_t *node_id_out) {
   if (!node || !node_id_out || !node->summary_text)
      return AUTH_DB_INVALID;

   AUTH_DB_LOCK_OR_FAIL();

   const char *sql = "INSERT INTO summary_nodes "
                     "(conversation_id, prior_node_id, depth, msg_id_start, msg_id_end, "
                     "level, summary_text, token_count, created_at) "
                     "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("summary_node_create: prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, node->conversation_id);
   if (node->prior_node_id > 0)
      sqlite3_bind_int64(stmt, 2, node->prior_node_id);
   else
      sqlite3_bind_null(stmt, 2);
   sqlite3_bind_int(stmt, 3, node->depth);
   sqlite3_bind_int64(stmt, 4, node->msg_id_start);
   sqlite3_bind_int64(stmt, 5, node->msg_id_end);
   sqlite3_bind_int(stmt, 6, node->level);
   sqlite3_bind_text(stmt, 7, node->summary_text, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 8, node->token_count);
   sqlite3_bind_int64(stmt, 9, (int64_t)time(NULL));

   rc = sqlite3_step(stmt);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("summary_node_create: insert failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   *node_id_out = sqlite3_last_insert_rowid(s_db.db);
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   OLOG_INFO("summary_node: created node %lld (conv=%lld, depth=%d, msgs=%lld-%lld)",
             (long long)*node_id_out, (long long)node->conversation_id, node->depth,
             (long long)node->msg_id_start, (long long)node->msg_id_end);

   return AUTH_DB_SUCCESS;
}

static void summary_node_from_row(sqlite3_stmt *stmt, summary_node_t *node) {
   node->id = sqlite3_column_int64(stmt, 0);
   node->conversation_id = sqlite3_column_int64(stmt, 1);
   node->prior_node_id = sqlite3_column_type(stmt, 2) == SQLITE_NULL
                             ? 0
                             : sqlite3_column_int64(stmt, 2);
   node->depth = sqlite3_column_int(stmt, 3);
   node->msg_id_start = sqlite3_column_int64(stmt, 4);
   node->msg_id_end = sqlite3_column_int64(stmt, 5);
   node->level = sqlite3_column_int(stmt, 6);
   const char *text = (const char *)sqlite3_column_text(stmt, 7);
   node->summary_text = text ? strdup(text) : NULL;
   node->token_count = sqlite3_column_int(stmt, 8);
   node->created_at = (time_t)sqlite3_column_int64(stmt, 9);
}

int summary_node_get(int64_t node_id, summary_node_t *node_out) {
   if (node_id <= 0 || !node_out)
      return AUTH_DB_INVALID;

   memset(node_out, 0, sizeof(*node_out));
   AUTH_DB_LOCK_OR_FAIL();

   const char *sql = "SELECT id, conversation_id, prior_node_id, depth, "
                     "msg_id_start, msg_id_end, level, summary_text, token_count, created_at "
                     "FROM summary_nodes WHERE id = ?";

   sqlite3_stmt *s = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &s, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_int64(s, 1, node_id);
   rc = sqlite3_step(s);

   if (rc == SQLITE_ROW) {
      summary_node_from_row(s, node_out);
      sqlite3_finalize(s);
      AUTH_DB_UNLOCK();
      return AUTH_DB_SUCCESS;
   }

   sqlite3_finalize(s);
   AUTH_DB_UNLOCK();
   return AUTH_DB_NOT_FOUND;
}

int summary_node_get_latest(int64_t conv_id, summary_node_t *node_out) {
   if (conv_id <= 0 || !node_out)
      return AUTH_DB_INVALID;

   memset(node_out, 0, sizeof(*node_out));
   AUTH_DB_LOCK_OR_FAIL();

   const char *sql = "SELECT id, conversation_id, prior_node_id, depth, "
                     "msg_id_start, msg_id_end, level, summary_text, token_count, created_at "
                     "FROM summary_nodes WHERE conversation_id = ? "
                     "ORDER BY id DESC LIMIT 1";

   sqlite3_stmt *s = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &s, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_bind_int64(s, 1, conv_id);
   rc = sqlite3_step(s);

   if (rc == SQLITE_ROW) {
      summary_node_from_row(s, node_out);
      sqlite3_finalize(s);
      AUTH_DB_UNLOCK();
      return AUTH_DB_SUCCESS;
   }

   sqlite3_finalize(s);
   AUTH_DB_UNLOCK();
   return AUTH_DB_NOT_FOUND;
}
