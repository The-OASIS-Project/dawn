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
 * What an extraction reply proposes (its facts, their relations, its entities),
 * read once from the reply's JSON, and the one way a proposed fact is stored:
 * both the prompt path (process_extraction_response) and the consolidator
 * store through memory_store_candidate.
 */

#ifndef MEMORY_CANDIDATES_H
#define MEMORY_CANDIDATES_H

#include <stdbool.h>
#include <stdint.h>

#include "memory/memory_db_consolidation.h"
#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/**
 * @brief Parse a JSON response from an LLM, handling common formatting variants.
 *
 * Tries direct parse, markdown code block extraction, and brace/bracket fallback.
 * Caller owns the returned object and must call json_object_put().
 *
 * @param response Raw LLM response text
 * @return Parsed json_object, or NULL on failure
 */
struct json_object *memory_extraction_parse_json(const char *response);

/** One relation a proposed fact states.  Strings are borrowed from the reply. */
typedef struct {
   const char *subject;
   const char *predicate;           /* as the model wrote it */
   char canon[MEMORY_RELATION_MAX]; /* its canonical form (memory_predicate_canonicalize) */
   const char *object;
   int64_t valid_from; /* 0: open */
   int64_t valid_to;   /* 0: open; an inverted range is read as open on both ends */
   /* "replaces": the old value the user said this replaced, MEMORY_RELATION_REPLACES_UNKNOWN
    * when they couldn't say which, or NULL */
   const char *replaces;
   bool dropped; /* not stored (its subject is the assistant) */
} memory_candidate_rel_t;

/** One proposed fact.  Strings are borrowed from the reply. */
typedef struct {
   const char *text;
   const char *subject; /* NULL when the reply named none */
   bool told;           /* the reply claimed "explicit" (anything else is inferred) */
   float confidence;
   const char *expires_at; /* the reference date as written, or NULL */
   int first_rel;          /* its relations: rels[first_rel .. first_rel + n_rels) */
   int n_rels;
   struct json_object *json; /* the fact as the reply wrote it */
} memory_candidate_t;

/** One proposed entity.  Strings are borrowed from the reply. */
typedef struct {
   const char *name;
   const char *type; /* person, pet, place, org or thing (anything else is thing) */
} memory_candidate_entity_t;

/** What one or more extraction replies propose.  Heap arrays; the replies'
 *  JSON must outlive it. */
typedef struct {
   memory_candidate_t *items;
   int count;
   memory_candidate_rel_t *rels;
   int n_rels;
   memory_candidate_entity_t *entities;
   int n_entities;
   int cap, rel_cap, entity_cap;
} memory_candidates_t;

/**
 * @brief Read the facts, their relations and the entities of an extraction
 *        reply's JSON into @p out (appended to what it holds)
 *
 * A fact without text, a relation missing its subject, predicate or object,
 * and an entity missing its name or type are left out.  Each relation's
 * predicate is canonicalized against @p user_id's vocabulary.
 *
 * @return SUCCESS, or FAILURE when out of memory (@p out keeps what it read)
 */
int memory_candidates_parse(struct json_object *root, int user_id, memory_candidates_t *out);

/**
 * @brief Drop what states the assistant as its subject: a fact whose subject
 *        is the assistant (by @p ai_name, or "assistant"), and any relation
 *        whose subject is.  "dawn" stays (DAWN the project is a subject its
 *        developer talks about).
 * @return How many facts and relations were dropped
 */
int memory_candidates_drop_assistant(memory_candidates_t *c, const char *ai_name);

/** Whether candidate @p idx's subject is the assistant (as
 *  memory_candidates_drop_assistant decides). */
bool memory_candidates_about_assistant(const memory_candidates_t *c, int idx, const char *ai_name);

/** Release @p c's arrays (not the JSON they borrow from). */
void memory_candidates_free(memory_candidates_t *c);

/** A batch's store: its user, conversation, provenance and the entities it
 *  resolved so far (opaque; memory_store_open). */
typedef struct memory_store memory_store_t;

/**
 * @brief Begin storing a batch
 *
 * @param prov  The batch's provenance (kept: must outlive the store)
 * @param agent The consolidator's store: no paraphrase merge and no relation
 *              closed automatically (the consolidator decides both)
 * @return The store, or NULL when out of memory
 */
memory_store_t *memory_store_open(int user_id,
                                  int64_t conversation_id,
                                  const memory_provenance_t *prov,
                                  bool agent);

/** Upsert @p c's proposed entities (each new one embedded), as the batch's own. */
void memory_store_entities(memory_store_t *st, const memory_candidates_t *c);

/** memory_store_candidate()'s options; zero for the defaults. */
typedef struct {
   const char *text;  /* store this text instead of the candidate's (NULL: its own) */
   bool no_relations; /* store none of its relations */
   /* The fact's provenance (NULL: the batch's): a candidate an earlier batch set
    * aside keeps that batch's conversation, range and outside text. */
   const memory_provenance_t *prov;
   bool keep_exact;  /* a stored fact it says again: not reinforced (the caller does it);
                      * its subject, expiry and relations are stored as for a new one */
   bool paraphrase;  /* a stored fact at paraphrase_dedup_threshold: reinforce it instead */
   const float *vec; /* the text's embedding, when already made (stored with the fact) */
   int dims;
} memory_store_opts_t;

/** Relations one memory_store_candidate() call lists as created. */
#define MEMORY_STORE_NEW_RELATIONS_MAX 16

/** What memory_store_candidate() did. */
typedef struct {
   int64_t fact_id; /* the fact stored or said again (0: none) */
   bool reused;     /* a stored fact said again: reinforced, not created */
   /* A stored fact said again with keep_exact: as it was before this call set
    * its subject, expiry or relations (what an undo puts back). */
   bool has_prior;
   memory_consolidation_fact_state_t prior;
   /* The relations the agent's store created for it (not an open one said
    * again); more than the array holds sets relations_overflow. */
   int64_t new_relation_ids[MEMORY_STORE_NEW_RELATIONS_MAX];
   int n_new_relations;
   bool relations_overflow;
} memory_store_result_t;

/**
 * @brief Store one proposed fact: the fact (or a stored one it repeats,
 *        reinforced), its subject, its expiry and its relations
 *
 * @param idx  Index of the candidate in @p c
 * @param opts NULL for the defaults
 * @return SUCCESS when stored or said again (@p out set), FAILURE when nothing
 *         was stored
 */
int memory_store_candidate(memory_store_t *st,
                           const memory_candidates_t *c,
                           int idx,
                           const memory_store_opts_t *opts,
                           memory_store_result_t *out);

/**
 * @brief A stored fact said again: raise its confidence and add @p prov's
 *        conversation to its sources; @p now_told makes a fact not yet told
 *        told, unless @p prov read outside text
 * @param prov The provenance it was said again with (NULL: the batch's)
 */
void memory_store_reinforce(memory_store_t *st,
                            int64_t fact_id,
                            bool now_told,
                            const memory_provenance_t *prov);

/** The entities the batch created so far (@p ids points into the store). */
int memory_store_fresh_entities(const memory_store_t *st, const int64_t **ids);

/** The time the batch's rows are stored with (its conversation's creation; 0: now). */
int64_t memory_store_created_at(const memory_store_t *st);

/**
 * @brief Finish the batch's store: the prompt path's merge check on the
 *        entities it created, the caches its writes made stale, then free @p st
 */
void memory_store_close(memory_store_t *st);

/**
 * @brief Store a batch's summary (embedded) and the conversation's title from
 *        its extraction reply, as both ways of storing a batch do
 * @param root       The extraction reply's JSON
 * @param created_at What the summary is stored with (0: now)
 * @return The summary's id, or 0 when none was stored
 */
int64_t memory_extraction_store_summary(int user_id,
                                        int64_t conversation_id,
                                        const char *session_id,
                                        struct json_object *root,
                                        int message_count,
                                        int duration_seconds,
                                        const memory_provenance_t *prov,
                                        int64_t created_at);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_CANDIDATES_H */
