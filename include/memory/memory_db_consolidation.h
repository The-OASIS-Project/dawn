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
 * The record of what each consolidation batch did to memory
 * (memory_consolidation_batches, memory_consolidation_ops): one row per batch,
 * one per operation, each with its item's text and what it changed (pre- and
 * post-image), so the user can see and undo it.  The record holds fact text,
 * so a forget removes the rows that name what it takes.
 */

#ifndef MEMORY_DB_CONSOLIDATION_H
#define MEMORY_DB_CONSOLIDATION_H

#include <stdbool.h>
#include <stdint.h>

#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Longest reason an op row keeps, in bytes (cut on a character boundary). */
#define MEMORY_CONSOLIDATE_REASON_MAX 200

/** Ops one batch records at most; one past it is not recorded (logged once
 *  per batch).  Every reader (a batch's listing, a whole-batch undo) takes
 *  them all. */
#define MEMORY_CONSOLIDATION_OPS_MAX 256

/** memory_db_consolidation_op_add(): the batch holds MEMORY_CONSOLIDATION_OPS_MAX
 *  ops already; nothing written. */
#define MEMORY_CONSOLIDATION_FULL 10

/** Days a deferred candidate stays open for a later batch to take up. */
#define MEMORY_CONSOLIDATE_DEFERRED_DAYS 30

/** A batch, as recorded. */
typedef struct {
   int user_id;
   int64_t conversation_id; /* 0: none */
   int64_t msg_id_start;    /* the first saved row the batch read (0: none) */
   int64_t msg_id_end;      /* the last row its provenance covers (0: none) */
   const char *path;        /* "agent" | "agent_partial" */
   const char *stop;        /* finished | triage | repair_failed | cancelled | tokens */
   int outside;             /* outside_kind_t the batch's history carried */
   int rounds;              /* model replies after the extraction */
   int input_tokens;        /* every call's input, cached included */
} memory_consolidation_batch_t;

/** One operation, as recorded.  Strings may be NULL; text and target_text are
 *  cut to MEMORY_FACT_TEXT_MAX bytes, reason to MEMORY_CONSOLIDATE_REASON_MAX. */
typedef struct {
   int64_t batch_id;
   int user_id;
   int seq;        /* order applied */
   const char *op; /* added | reinforced | merged | replaced | closed | linked | proposed |
                      pref_set | dropped | deferred | suggested_forget */
   int64_t fact_id;
   int64_t other_fact_id; /* the keeper or the newer value */
   int64_t relation_id;
   int64_t entity_id;
   int64_t other_entity_id;
   int64_t link_id;
   int outside;
   const char *text;
   const char *target_text;
   const char *reason;
   const char *reason_by;  /* "model" or "dawn": who wrote reason (NULL: none) */
   const char *pre_image;  /* JSON */
   const char *post_image; /* JSON */
   const char *status;     /* NULL: "applied" */
   /* A set-aside candidate's own conversation, when the row quotes it (0:
    * the batch's): that conversation made private or forgotten removes it.
    * Made private already, the row isn't written (MEMORY_DB_NOT_FOUND). */
   int64_t source_conversation_id;
   /* A deferred candidate this row takes up (0: none): claimed (consumed)
    * and the row written in one step, or neither (MEMORY_DB_NOT_FOUND) when
    * it is no longer open or its conversation can't be learned from. */
   int64_t claim_op_id;
} memory_consolidation_op_t;

/**
 * @brief Record a batch
 * @param id_out Receives the batch id
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_consolidation_batch_add(const memory_consolidation_batch_t *batch, int64_t *id_out);

/** The batch's path and stop, changed after it was recorded (a batch that
 *  filled its record). */
int memory_db_consolidation_batch_set_stop(int64_t batch_id,
                                           int user_id,
                                           const char *path,
                                           const char *stop);

/** How many more ops batch @p batch_id of @p user_id can record into @p room.
 *  @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND or MEMORY_DB_FAILURE */
int memory_db_consolidation_batch_room(int user_id, int64_t batch_id, int *room);

/** The batch's summary, once stored. */
int memory_db_consolidation_batch_set_summary(int64_t batch_id, int user_id, int64_t summary_id);

/**
 * @brief Record one operation of a batch
 * @param id_out Receives the op id (may be NULL)
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND (not written: see
 *         source_conversation_id and claim_op_id), MEMORY_CONSOLIDATION_FULL
 *         or MEMORY_DB_FAILURE
 */
int memory_db_consolidation_op_add(const memory_consolidation_op_t *op, int64_t *id_out);

/** Set an op's status (consumed, expired, ...); the owner is checked. */
int memory_db_consolidation_op_set_status(int64_t op_id, int user_id, const char *status);

/**
 * @brief Take up deferred candidate @p op_id: marked consumed only while it is
 *        still open and its batch's conversation can still be learned from
 *        (not private, not deleted).  In one statement, so a privacy change
 *        either comes first (nothing taken) or after (it was taken in time).
 * @return MEMORY_DB_SUCCESS when taken, MEMORY_DB_NOT_FOUND when not (gone,
 *         taken, or its conversation made private), MEMORY_DB_FAILURE
 */
int memory_db_consolidation_deferred_claim(int64_t op_id, int user_id);

/** Whether deferred candidate @p op_id is still open and its conversation can
 *  still be learned from (as the claim checks, without taking it up).
 *  @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE */
int memory_db_consolidation_deferred_open(int64_t op_id, int user_id);

/** A deferred candidate a later batch may still take up. */
typedef struct {
   int64_t op_id;
   int64_t batch_id;
   int outside;
   char *candidate;         /* its candidate JSON (heap) */
   int64_t conversation_id; /* the batch that set it aside: its conversation (0: none) */
   int64_t msg_id_start;    /* and the rows it read */
   int64_t msg_id_end;
   int64_t created_at;
} memory_consolidation_deferred_t;

/**
 * @brief The user's open deferred candidates, newest first: status applied,
 *        no older than @p since (epoch s)
 * @param out Caller array of @p max; each candidate freed with
 *            memory_db_consolidation_deferred_free
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE (none returned)
 */
int memory_db_consolidation_deferred_list(int user_id,
                                          int64_t since,
                                          memory_consolidation_deferred_t *out,
                                          int max,
                                          int *count_out);

/** Free what memory_db_consolidation_deferred_list allocated in @p rows. */
void memory_db_consolidation_deferred_free(memory_consolidation_deferred_t *rows, int n);

/** A batch, as read back. */
typedef struct {
   int64_t id;
   int64_t conversation_id;
   int64_t msg_id_start;
   int64_t msg_id_end;
   int64_t summary_id;
   char path[16];
   char stop[16];
   int outside;
   int rounds;
   int input_tokens;
   int64_t created_at;
} memory_consolidation_batch_row_t;

/**
 * @brief The user's batches, newest first: of @p conversation_id (0: all),
 *        with ids below @p before_batch_id (0: from the newest)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_consolidation_batches_list(int user_id,
                                         int64_t conversation_id,
                                         int64_t before_batch_id,
                                         memory_consolidation_batch_row_t *out,
                                         int max,
                                         int *count_out);

/** An op, as read back. */
typedef struct {
   int64_t id;
   int64_t batch_id;
   int seq;
   char op[24];
   int64_t fact_id;
   int64_t other_fact_id;
   int64_t relation_id;
   int64_t entity_id;
   int64_t other_entity_id;
   int64_t link_id;
   int outside;
   char text[MEMORY_FACT_TEXT_MAX];
   char target_text[MEMORY_FACT_TEXT_MAX];
   char reason[MEMORY_CONSOLIDATE_REASON_MAX + 1];
   char reason_by[8]; /* "model", "dawn", or "" (no reason, or recorded before it was kept) */
   char status[16];
   char *pre_image;                /* heap, may be NULL */
   char *post_image;               /* heap, may be NULL */
   int64_t source_conversation_id; /* the conversation it quotes, when not the batch's (0) */
   /* The counterpart (other_fact_id or other_entity_id) as it is now:
    * "current", "superseded" or "gone"; "" when the op has none. */
   char target_status[12];
} memory_consolidation_op_row_t;

/**
 * @brief A batch's ops in the order applied (the owner is checked)
 * @param out Caller array of @p max; images freed with memory_db_consolidation_ops_free
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_consolidation_ops_list(int user_id,
                                     int64_t batch_id,
                                     memory_consolidation_op_row_t *out,
                                     int max,
                                     int *count_out);

/** Free the images memory_db_consolidation_ops_list allocated in @p rows. */
void memory_db_consolidation_ops_free(memory_consolidation_op_row_t *rows, int n);

/**
 * @brief A batch was recorded, undone, changed by a Keep or by a forget:
 *        told to the user's surfaces.  Weak, a no-op unless the WebUI is
 *        built (its strong override sends memory_batch to every tab of the
 *        user).
 */
void memory_consolidation_batch_notify(int user_id, int64_t conversation_id, int64_t batch_id);

/** Batches a forget's scrub changed, told to the user's tabs: the newest
 *  this many, each once (a tab showing an older one keeps the forgotten text
 *  until it reloads). */
#define MEMORY_CONSOLIDATION_TOUCHED_MAX 256

typedef struct {
   int64_t ids[MEMORY_CONSOLIDATION_TOUCHED_MAX];
   int n;
} memory_consolidation_touched_t;

/**
 * @brief In a forget's transaction (lock held): remove the op rows that name
 *        a going fact (the ids @p going_sql selects, bound with ?1 = user),
 *        or a relation of one, the dropped and deferred candidates with its
 *        words, and those of every batch of a conversation that taught one
 *
 * Call while the going facts and their sources are still there.
 * @param touched The batches whose rows went, added to (may be NULL)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_consolidation_scrub_facts_locked(int user_id,
                                               const char *going_sql,
                                               memory_consolidation_touched_t *touched);

/**
 * @brief memory_consolidation_batch_notify for each batch in @p touched.
 *        Never with the auth_db lock held (the notify takes it, and the
 *        WebUI's registry lock).
 */
void memory_consolidation_touched_notify(int user_id,
                                         const memory_consolidation_touched_t *touched);

/**
 * @brief In a forget's transaction (lock held): remove the batches (and with
 *        them their ops) of the conversations in @p ids_json (a JSON array),
 *        and the rows other batches made quoting what they taught
 * @param touched The other batches whose rows changed, added to (may be NULL)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_consolidation_drop_conversations_locked(int user_id,
                                                      const char *ids_json,
                                                      memory_consolidation_touched_t *touched);

/**
 * @brief In a forget's transaction or an entity delete (lock held), after its
 *        deletes: remove the linked and proposed rows that name an entity no
 *        longer there, and the rows about a relation no longer there (a closed
 *        relation's row quotes its subject and object)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_consolidation_scrub_entities_locked(int user_id,
                                                  memory_consolidation_touched_t *touched);

/** A stored fact as a reinforce finds it: what undoing the reinforce puts back. */
typedef struct {
   char text[MEMORY_FACT_TEXT_MAX];
   float confidence;
   char source[MEMORY_SOURCE_MAX];
   int outside;
   int64_t superseded_by;
   int64_t superseded_at;          /* 0: current, or unknown */
   int origin_unsourced;           /* learned outside any conversation */
   int64_t expires_at;             /* 0: none */
   int64_t subject_entity_id;      /* 0: none */
   bool had_conversation;          /* the reinforcing conversation is already a source */
   int64_t source_conversation_id; /* the fact's own (0: none) */
   int64_t msg_id_start;
   int64_t msg_id_end;
} memory_consolidation_fact_state_t;

/**
 * @brief Fact @p fact_id as it is now, and whether @p conversation_id is
 *        already one of its sources
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND or MEMORY_DB_FAILURE
 */
int memory_db_consolidation_fact_state(int user_id,
                                       int64_t fact_id,
                                       int64_t conversation_id,
                                       memory_consolidation_fact_state_t *out);

/** One batch of the user's (@p row filled).
 *  @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND or MEMORY_DB_FAILURE */
int memory_db_consolidation_batch_get(int user_id,
                                      int64_t batch_id,
                                      memory_consolidation_batch_row_t *row);

/** Whether conversation @p conversation_id is the user's.
 *  @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND or MEMORY_DB_FAILURE */
int memory_db_consolidation_conversation_owned(int user_id, int64_t conversation_id);

/* =============================================================================
 * Undo and Keep
 * ============================================================================= */

/** The row an op changed is no longer as the op left it: nothing was done,
 *  and the op is marked stale. */
#define MEMORY_CONSOLIDATION_STALE 7

/** The op has nothing to undo or keep: its kind, its status, or a row
 *  recorded without what that needs. */
#define MEMORY_CONSOLIDATION_NOTHING 8

/** Whether @p o can be undone: applied, of a kind undo reverses, and recorded
 *  with what undoing it needs. */
bool memory_consolidation_op_undoable(const memory_consolidation_op_row_t *o);

/** Whether @p o can be kept: a dropped or deferred candidate (applied, or
 *  deferred and expired) recorded with its candidate. */
bool memory_consolidation_op_keepable(const memory_consolidation_op_row_t *o);

/** What an undo changed beyond the record (set, never cleared): the caches
 *  the caller drops, and with @c memory_bodies (a preference changed) the
 *  USER MEMORY blocks it withdraws from the conversations. */
typedef struct {
   bool facts;
   bool entities;
   bool memory_bodies;
} memory_consolidation_undo_effect_t;

/**
 * @brief Undo op @p op_id of batch @p batch_id
 *
 * In one transaction: the row the op changed is checked against what the op
 * left (its post-image) and put back as it was before (its pre-image); the op
 * becomes undone.  Anything else changed it since (a later batch, a merge
 * onto an added fact, another conversation teaching it), and nothing is done:
 * the op is marked stale.  Records no forgotten row.  The caller holds the
 * user's extraction and marks the removal (memory_consolidation_undo.c).
 *
 * @return MEMORY_DB_SUCCESS (undone, now or before), MEMORY_CONSOLIDATION_STALE,
 *         MEMORY_CONSOLIDATION_NOTHING, MEMORY_DB_NOT_FOUND (no such op of that
 *         batch and user) or MEMORY_DB_FAILURE (nothing done)
 */
int memory_db_consolidation_undo_op(int user_id,
                                    int64_t batch_id,
                                    int64_t op_id,
                                    memory_consolidation_undo_effect_t *effect);

/**
 * @brief The ops a whole-batch undo takes, in the order to undo them (the
 *        last applied first): applied and undoable
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND (no such batch of the user)
 *         or MEMORY_DB_FAILURE
 */
int memory_db_consolidation_undo_ids(int user_id,
                                     int64_t batch_id,
                                     int64_t *ids,
                                     int max,
                                     int *count_out);

/** A candidate the user may keep, as recorded. */
typedef struct {
   char *candidate;         /* its candidate JSON (heap) */
   int outside;             /* the taint it was recorded with */
   int64_t conversation_id; /* the conversation it came from (its own, else the batch's) */
   int64_t msg_id_start;    /* the batch's rows, when it came from the batch's conversation */
   int64_t msg_id_end;
} memory_consolidation_keep_t;

/**
 * @brief Op @p op_id of batch @p batch_id, when it can be kept
 * @param out Its candidate freed with memory_db_consolidation_keep_free
 * @return MEMORY_DB_SUCCESS, MEMORY_CONSOLIDATION_NOTHING (not a candidate
 *         that can be kept), MEMORY_DB_NOT_FOUND or MEMORY_DB_FAILURE
 */
int memory_db_consolidation_keep_get(int user_id,
                                     int64_t batch_id,
                                     int64_t op_id,
                                     memory_consolidation_keep_t *out);

/** Free the candidate memory_db_consolidation_keep_get allocated in @p k
 *  (NULL and a second call are fine). */
void memory_db_consolidation_keep_free(memory_consolidation_keep_t *k);

/**
 * @brief In one transaction: op @p op_id marked kept (only while it still
 *        can be) and @p added (its batch, user and seq set here; NULL: none)
 *        appended to the batch
 * @param added_id_out The appended row's id (may be NULL)
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND (no longer keepable: nothing
 *         written) or MEMORY_DB_FAILURE
 */
int memory_db_consolidation_keep_record(int user_id,
                                        int64_t batch_id,
                                        int64_t op_id,
                                        memory_consolidation_op_t *added,
                                        int64_t *added_id_out);

/**
 * @brief Mark every user's deferred candidates set aside before
 *        @p older_than (epoch s) expired: no later batch takes them up, the
 *        user may still keep them until the record is pruned
 * @param count_out How many (may be NULL)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_consolidation_expire_deferred(int64_t older_than, int *count_out);

/**
 * @brief What a merge of @p from into @p into is about to give the fact kept,
 *        as a JSON object (heap): the conversations it gains as sources
 *        ("added_sources") and its origin_unsourced before
 *        ("keeper_origin_unsourced"), so undoing the merge takes them back.  NULL when @p into
 * isn't the user's or on failure.
 */
char *memory_db_consolidation_merge_image(int user_id, int64_t from, int64_t into);

/**
 * @brief The conversations fact @p fact_id is learned from, as a JSON array
 *        (heap); NULL on failure
 */
char *memory_db_consolidation_fact_sources(int user_id, int64_t fact_id);

/**
 * @brief Where preference @p category was learned, as a JSON object (heap):
 *        its source conversation and message range, origin_unsourced,
 *        reinforcement_count, updated_at and its source conversations
 *        ("sources"), so undoing a change to it puts them back with the
 *        value.  NULL when the user has no such preference or on failure.
 */
char *memory_db_consolidation_pref_image(int user_id, const char *category);

/** Days the record keeps a batch and its ops (the nightly prune). */
#define MEMORY_CONSOLIDATE_RECORD_DAYS 30

/**
 * @brief Remove every user's batches (and their ops) recorded before
 *        @p older_than (epoch s)
 * @param count_out Batches removed (may be NULL)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_consolidation_prune(int64_t older_than, int *count_out);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_DB_CONSOLIDATION_H */
