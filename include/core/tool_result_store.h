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
 * Tool results kept whole while the model is shown a view of them, and who
 * may read one back.
 *
 * A result is stored under a "trs_" handle with its user and the turn's
 * conversation.  It is readable only in that conversation, by that user.  A
 * result stored before its conversation exists (an unsaved voice
 * conversation, or a WebUI turn that creates its conversation) is unbound:
 * readable only by the turn that stored it, or, once that turn ended into the
 * live history, by turns on that history (the session's minted set, kept like
 * the facts a turn saves before it has a conversation).  It is bound to the
 * conversation its history becomes (session_turn_set_conversation,
 * session_bind_created_conversation, the voice save); when that history is
 * discarded instead, no one can read it and it is reclaimed.
 *
 * Parsed trees of JSON results are cached for the turn (a model's follow-up
 * reads parse nothing): results of up to TOOL_RESULT_TREE_CACHE_TEXT_MAX,
 * TOOL_RESULT_TREE_CACHE_PER_SESSION per session, within
 * TOOL_RESULT_TREE_CACHE_MAX_BYTES of estimated tree memory in all, plus one
 * larger tree (up to LLM_TOOL_VIEW_JSON_MAX_BYTES of JSON) in all, replaced
 * when another comes.  A tree in use is pinned and used by its one reader
 * (json-c trees aren't safe to share across threads); the cache lock is held
 * only to find, pin and release.  A large parse is one at a time.
 */

#ifndef TOOL_RESULT_STORE_H
#define TOOL_RESULT_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "auth/auth_db_tool_results.h"

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;
struct session;

/* The largest result kept whole: above it, its head and tail are kept. */
#define TOOL_RESULT_STORE_MAX_BYTES ((size_t)16 * 1024 * 1024)

/* Tree cache (see above).  A json-c tree takes about this many bytes per byte
 * of its text (measured: 4 MB of JSON rows, ~69 MB). */
#define TOOL_RESULT_TREE_CACHE_TEXT_MAX ((size_t)1024 * 1024)
#define TOOL_RESULT_TREE_CACHE_MAX_BYTES ((size_t)64 * 1024 * 1024)
#define TOOL_RESULT_TREE_CACHE_PER_SESSION 2
#define TOOL_RESULT_TREE_BYTES_PER_TEXT_BYTE 17

/* The characters a result_read answer is given. */
#define TOOL_RESULT_READ_BUDGET_CHARS 32000

/* Why a result wasn't stored. */
#define TOOL_RESULT_STORE_OK 0
#define TOOL_RESULT_STORE_FAILED 1
#define TOOL_RESULT_STORE_NO_USER 2 /* a guest's, or no session: never stored */

/* Why one can't be read (every refusal gives the model the same answer). */
#define TOOL_RESULT_OPEN_OK 0
#define TOOL_RESULT_OPEN_REFUSED 1 /* unknown, foreign, another conversation, evicted, guest */
#define TOOL_RESULT_OPEN_FAILED 2  /* the store failed (out of memory, the database) */

/** A stored result, open for reading. */
typedef struct {
   char id[TOOL_RESULTS_ID_LEN];
   tool_results_meta_t meta;
   char *body; /* NULL until tool_result_store_load_body(); NUL-terminated after len */
   size_t len;
} tool_result_doc_t;

/**
 * @brief Store @p text (@p len bytes) from tool @p tool_name, readable in the
 *        caller's conversation (its turn's; 0: not yet, bound later) by the
 *        session's effective user (session_effective_user_id)
 *
 * Contract: called from the storing turn (its thread or a tool thread
 * carrying its token), or with no turn running (a research round), without
 * history_mutex held; @p text is the result as its tool returned it (not
 * neutralized: whatever a read returns is neutralized when that result is
 * finished, like any result; the conversation's tag is masked here, a NUL
 * stored as a space).  A thread that isn't the running turn's stores nothing;
 * a guest (effective user 0) stores nothing (TOOL_RESULT_STORE_NO_USER).
 *
 * @param is_json Whether the view parsed it as JSON
 * @param id_out Its handle
 * @param cut_out Set when it was larger than TOOL_RESULT_STORE_MAX_BYTES and
 *        only its head and tail were kept (may be NULL)
 * @return TOOL_RESULT_STORE_OK, _NO_USER or _FAILED
 */
int tool_result_store_put(struct session *session,
                          const char *tool_name,
                          const char *tool_call_id,
                          const char *text,
                          size_t len,
                          bool is_json,
                          char id_out[TOOL_RESULTS_ID_LEN],
                          bool *cut_out);

/**
 * @brief tool_result_store_put for text that is someone else's: @p frame
 *        (TOOL_FRAME_EMAIL, TOOL_FRAME_WEB, or NULL) is kept with it, and a
 *        read of it is framed the same way (tool_result_store_frame)
 */
int tool_result_store_put_framed(struct session *session,
                                 const char *tool_name,
                                 const char *tool_call_id,
                                 const char *text,
                                 size_t len,
                                 bool is_json,
                                 const char *frame,
                                 char id_out[TOOL_RESULTS_ID_LEN],
                                 bool *cut_out);

/** The frame an open result's text goes in (TOOL_FRAME_*), or NULL. */
const char *tool_result_store_frame(const tool_result_doc_t *doc);

/**
 * @brief Open result @p id for the caller in @p session acting for
 *        @p user_id: allowed when the result is @p user_id's and either in the
 *        caller's conversation (its turn's, else the live history's) or
 *        unbound and stored by the caller's turn (or an ended one of the
 *        history it's on)
 *
 * Fails closed: no session, a user of 0 or less, a malformed id, or a thread
 * that isn't the running turn's is refused.  Reads the row only (see
 * tool_result_store_load_body()).  What is read from the result must reach
 * the model through llm_tools_execute()'s neutralize_result (bodies are
 * masked at rest, not neutralized).
 *
 * @return TOOL_RESULT_OPEN_OK (release with tool_result_store_close),
 *         _REFUSED or _FAILED
 */
int tool_result_store_open(struct session *session,
                           int user_id,
                           const char *id,
                           tool_result_doc_t *doc);

/**
 * @brief Load the body of @p doc (opened: its row only, so a read that a
 *        cached tree answers never copies the body out)
 *
 * The row is checked again: evicted or rebound since it was opened, it is
 * refused.
 *
 * @return TOOL_RESULT_OPEN_OK, _REFUSED or _FAILED
 */
int tool_result_store_load_body(tool_result_doc_t *doc);

void tool_result_store_close(tool_result_doc_t *doc);

/** A parsed tree in use: release it with tool_result_store_tree_release(). */
typedef struct {
   struct json_object *tree;
   void *slot; /* the cache slot pinned for this use, or NULL */
   bool owned; /* a tree parsed for this use alone */
   bool big;   /* too large to cache: the one-at-a-time parse is held */
} tool_result_tree_t;

/**
 * @brief The parsed tree of JSON result @p doc (the view's parse: strict, its
 *        numbers held exactly), from the cache when it is there
 * @return false when it isn't JSON the view parses (read it as text)
 */
bool tool_result_store_tree_acquire(struct session *session,
                                    tool_result_doc_t *doc,
                                    tool_result_tree_t *ref);

void tool_result_store_tree_release(tool_result_tree_t *ref);

/**
 * @brief Cache @p tree (parsed from result @p id's @p text_bytes of text; at
 *        most LLM_TOOL_VIEW_JSON_MAX_BYTES) for
 *        @p session's turn, taking ownership of it (freed when not cached)
 *
 * The caller must not use @p tree afterwards: it may be freed, or shared.
 */
void tool_result_store_tree_seed(struct session *session,
                                 const char *id,
                                 struct json_object *tree,
                                 size_t text_bytes);

/** @brief Drop @p session's cached trees (its turn ended). */
void tool_result_store_drop_trees(struct session *session);

/**
 * @brief Bind unbound results of @p session to conversation @p conv_id (a
 *        row only when its user owns the conversation): those the running
 *        turn @p turn_token stored, and, with @p with_ended, those ended turns
 *        left in the live history (it became that conversation); the caller
 *        holds history_mutex
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE (nothing bound)
 */
int tool_result_store_bind_locked(struct session *session,
                                  int64_t conv_id,
                                  uint64_t turn_token,
                                  bool with_ended);

/**
 * @brief Turn @p turn_token ended without a conversation: its unbound results
 *        become the live history's when it wrote there (@p to_live), else no
 *        one's; the caller holds history_mutex
 */
void tool_result_store_turn_ended_locked(struct session *session,
                                         uint64_t turn_token,
                                         bool to_live);

/**
 * @brief The live history was discarded: forget the unbound results ended
 *        turns left in it (a running turn's stay with it); the caller holds
 *        history_mutex
 *
 * They stay in the store until reclaimed: no one can read them.
 */
void tool_result_store_reset_locked(struct session *session);

/** @brief Free @p session's store state (the session is being freed). */
void tool_result_store_free(struct session *session);

/** @brief The characters a result_read answer gets in @p session's turn. */
size_t tool_result_store_read_budget(struct session *session);

/**
 * @brief Set the characters a result_read answer gets from now on in
 *        @p session (its fair share of the tool loop's batch budget, set
 *        before the batch runs, so an answer usually fits whole; one over its
 *        share is viewed, never stored again); 0 restores the default
 */
void tool_result_store_set_read_budget(struct session *session, size_t chars);

#ifdef __cplusplus
}
#endif

#endif /* TOOL_RESULT_STORE_H */
