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
 * Stored tool results (the tool_results table, v97): a result kept whole
 * while the model is shown a view of it.  Rows only: who may read one is
 * decided in core/tool_result_store.c.
 */

#ifndef AUTH_DB_TOOL_RESULTS_H
#define AUTH_DB_TOOL_RESULTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "trs_" + 12 base62 + NUL (the blob-store id shape, its own prefix). */
#define TOOL_RESULTS_ID_LEN 17
#define TOOL_RESULTS_ID_PREFIX "trs_"
#define TOOL_RESULTS_TOOL_NAME_MAX 128

/* Space kept per conversation, per user and in all, oldest evicted first. */
#define TOOL_RESULTS_CONV_MAX_BYTES ((int64_t)256 * 1024 * 1024)
#define TOOL_RESULTS_USER_MAX_BYTES ((int64_t)1024 * 1024 * 1024)
#define TOOL_RESULTS_TOTAL_MAX_BYTES ((int64_t)4 * 1024 * 1024 * 1024)

/* How long a result no conversation holds yet (an unsaved voice
 * conversation's) is kept: far past the longest a voice conversation waits
 * for its idle save. */
#define TOOL_RESULTS_UNBOUND_GRACE_SEC (24 * 60 * 60)

typedef enum {
   TOOL_RESULTS_TEXT = 0,
   TOOL_RESULTS_JSON = 1,
} tool_results_kind_t;

/** A result to store. */
typedef struct {
   const char *id;           /**< TOOL_RESULTS_ID_PREFIX id (the caller mints it) */
   int user_id;              /**< > 0 */
   int64_t conversation_id;  /**< its conversation, or 0 (unbound: an unsaved one) */
   const char *session_key;  /**< the storing session's key while unbound (may be NULL) */
   const char *tool_name;    /**< the tool that produced it */
   const char *tool_call_id; /**< may be NULL */
   tool_results_kind_t kind;
   int64_t chars; /**< characters in body */
   const char *body;
   size_t bytes;
} tool_results_new_t;

/** A stored result's row, less its body. */
typedef struct {
   int user_id;
   int64_t conversation_id; /**< 0: unbound */
   tool_results_kind_t kind;
   int64_t chars;
   int64_t bytes;
   char tool_name[TOOL_RESULTS_TOOL_NAME_MAX];
} tool_results_meta_t;

/** Whether @p id is a well-formed result handle ("trs_" + 12 letters or digits). */
bool tool_results_id_valid(const char *id);

/**
 * @brief Store @p r, first evicting the oldest results (this table only) that
 *        keep it from fitting the conversation, user and total caps
 *
 * One transaction.  Each eviction is logged with its conversation.  Also
 * reclaims unbound rows older than TOOL_RESULTS_UNBOUND_GRACE_SEC.
 *
 * @param evicted_out Results evicted (may be NULL)
 * @return AUTH_DB_SUCCESS, AUTH_DB_INVALID (a bad field, or larger than a
 *         cap), or AUTH_DB_FAILURE
 */
int tool_results_db_add(const tool_results_new_t *r, int *evicted_out);

/**
 * @brief Result @p id's row, and its body when @p body is not NULL (heap,
 *        NUL-terminated after *@p bytes; the caller frees)
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, AUTH_DB_INVALID or AUTH_DB_FAILURE
 */
int tool_results_db_get(const char *id, tool_results_meta_t *meta, char **body, size_t *bytes);

/**
 * @brief Give the unbound results @p ids to conversation @p conv_id (the one
 *        their history became), then keep the conversation's cap
 *
 * Only a result whose user owns the conversation is bound: one already bound,
 * another user's, or gone (evicted) is left alone.
 *
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE (nothing bound)
 */
int tool_results_db_bind(int64_t conv_id, const char (*ids)[TOOL_RESULTS_ID_LEN], int count);

/** @brief Delete result @p id (a store that couldn't be completed). */
int tool_results_db_delete(const char *id);

/**
 * @brief Delete unbound results stored before @p before (unix seconds)
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int tool_results_db_reclaim_unbound(int64_t before, int *deleted_out);

#ifdef __cplusplus
}
#endif

#endif /* AUTH_DB_TOOL_RESULTS_H */
