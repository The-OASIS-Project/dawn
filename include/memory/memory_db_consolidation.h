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
   const char *pre_image;  /* JSON */
   const char *post_image; /* JSON */
   const char *status;     /* NULL: "applied" */
   /* A set-aside candidate's own conversation, when the row quotes it (0:
    * the batch's): that conversation made private or forgotten removes it. */
   int64_t source_conversation_id;
} memory_consolidation_op_t;

/**
 * @brief Record a batch
 * @param id_out Receives the batch id
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_consolidation_batch_add(const memory_consolidation_batch_t *batch, int64_t *id_out);

/** The batch's summary, once stored. */
int memory_db_consolidation_batch_set_summary(int64_t batch_id, int user_id, int64_t summary_id);

/**
 * @brief Record one operation of a batch
 * @param id_out Receives the op id (may be NULL)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
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
   char status[16];
   char *pre_image;  /* heap, may be NULL */
   char *post_image; /* heap, may be NULL */
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
 * @brief In a forget's transaction (lock held): remove the op rows that name
 *        a going fact (the ids @p going_sql selects, bound with ?1 = user),
 *        or a relation of one, and the dropped and deferred candidates of
 *        every batch of a conversation that taught one
 *
 * Call while the going facts and their sources are still there.
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_consolidation_scrub_facts_locked(int user_id, const char *going_sql);

/**
 * @brief In a forget's transaction (lock held): remove the batches (and with
 *        them their ops) of the conversations in @p ids_json (a JSON array)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_consolidation_drop_conversations_locked(int user_id, const char *ids_json);

/**
 * @brief In a forget's transaction (lock held), after its deletes: remove the
 *        linked and proposed rows that name an entity no longer there
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_consolidation_scrub_entities_locked(int user_id);

/** A fact's sources as a reinforce finds them (what it may change). */
typedef struct {
   bool had_conversation;          /* the reinforcing conversation is already a source */
   int64_t source_conversation_id; /* the fact's own (0: none) */
   int64_t msg_id_start;
   int64_t msg_id_end;
} memory_consolidation_fact_source_t;

/**
 * @brief What a reinforce of @p fact_id from @p conversation_id changes about
 *        its sources, before it
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND or MEMORY_DB_FAILURE
 */
int memory_db_consolidation_fact_source(int user_id,
                                        int64_t fact_id,
                                        int64_t conversation_id,
                                        memory_consolidation_fact_source_t *out);

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
