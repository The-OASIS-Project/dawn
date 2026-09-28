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
 * Memory Database — entity graph sub-API (Phase S4).
 *
 * Entity CRUD, relation CRUD (including bitemporal valid_from/valid_to
 * variants), entity-photo linkage, entity merging, and entity-embedding
 * storage/load.  Entity embeddings live here (not in memory_db_embeddings.h)
 * because callers operate on entities-with-embeddings as a single record
 * type — per-record-type cohesion beats per-mechanism cohesion when no
 * caller mixes fact and entity vectors in one query.
 *
 * Included transitively via memory_db.h — callers that already
 * `#include "memory/memory_db.h"` see these declarations unchanged.
 */

#ifndef MEMORY_DB_ENTITIES_H
#define MEMORY_DB_ENTITIES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Entity Graph Operations (Phase S4)
 * ============================================================================= */

/**
 * @brief Build a canonical (lowercase ASCII) version of an entity name
 *
 * Lowercases ASCII characters, preserves multibyte UTF-8 as-is,
 * and trims trailing spaces.
 *
 * @param name Input entity name
 * @param out Output buffer
 * @param size Size of output buffer
 */
void memory_make_canonical_name(const char *name, char *out, size_t size);

/**
 * @brief Upsert an entity (insert or increment mention_count)
 *
 * Uses INSERT ... ON CONFLICT ... RETURNING id, mention_count.
 *
 * @param user_id User who owns this entity
 * @param name Display name
 * @param entity_type Entity type (person, pet, place, org, thing)
 * @param canonical_name Lowercased canonical name
 * @param out_created Output: true if this was a new insert (mention_count == 1)
 * @param id_out Output: entity ID on success (may be NULL)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_entity_upsert(int user_id,
                            const char *name,
                            const char *entity_type,
                            const char *canonical_name,
                            bool *out_created,
                            int64_t *id_out);

/**
 * @brief Upsert an entity with explicit `first_seen` / `last_seen` overrides
 *
 * Same contract as memory_db_entity_upsert, plus two timestamp overrides.
 * Used by reextract paths where the conversation's original creation time
 * should be preserved on the entity row instead of "now" — otherwise a
 * full reextract stamps every entity's first_seen into the reextract
 * window, breaking entity-merge `last_seen` priority comparisons and the
 * recompute worker's staleness logic.
 *
 * @param first_seen_override Epoch seconds, or 0 to use time(NULL)
 * @param last_seen_override Epoch seconds, or 0 to use time(NULL).  On
 *        conflict update, this value replaces last_seen (preserves
 *        chronological progress when reextract processes convs in order).
 */
int memory_db_entity_upsert_at(int user_id,
                               const char *name,
                               const char *entity_type,
                               const char *canonical_name,
                               int64_t first_seen_override,
                               int64_t last_seen_override,
                               bool *out_created,
                               int64_t *id_out);

/**
 * @brief Get an entity by exact canonical name
 *
 * @param user_id User ID
 * @param canonical_name Exact canonical name to look up
 * @param out_entity Output: populated entity structure
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_entity_get_by_name(int user_id,
                                 const char *canonical_name,
                                 memory_entity_t *out_entity);

/**
 * @brief Update entity embedding vector
 *
 * @param entity_id Entity ID
 * @param user_id User ID (for ownership check)
 * @param embedding Float array of embedding values
 * @param dims Number of dimensions
 * @param norm Pre-computed L2 norm
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_entity_update_embedding(int64_t entity_id,
                                      int user_id,
                                      const float *embedding,
                                      int dims,
                                      float norm);

/**
 * @brief Create a relation between entities
 *
 * @param user_id User ID
 * @param subject_entity_id Subject entity ID
 * @param relation Relation type (e.g., "is_a", "lives_in")
 * @param object_entity_id Object entity ID (0 for literal)
 * @param object_value Literal value if no object entity
 * @param fact_id Associated fact ID (0 for none)
 * @param confidence Confidence score (0.0-1.0)
 * @param prov Provenance; NULL or conv_id==0 = no provenance
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_relation_create(int user_id,
                              int64_t subject_entity_id,
                              const char *relation,
                              int64_t object_entity_id,
                              const char *object_value,
                              int64_t fact_id,
                              float confidence,
                              int64_t valid_from,
                              int64_t valid_to,
                              const memory_provenance_t *prov);

/**
 * @brief Returns true if the given relation type is in the EXCLUSIVE_RELATIONS
 * list (works_at, lives_in, married_to, ...).  Exposed so the entity-merge
 * alias surface (memory_db_alias.c) can compute exclusive_relation_overlap
 * without re-listing the source-of-truth array.  Single source of truth lives
 * in memory_db.c.
 *
 * @param relation Relation type string (e.g. "works_at"); may be NULL.
 * @return true if exclusive, false otherwise (NULL → false).
 */
bool memory_db_relation_is_exclusive(const char *relation);

/**
 * @brief Transactional close-and-create: auto-closes any existing open exclusive
 * relation with a different object before inserting the new row.  All work happens
 * under a single BEGIN IMMEDIATE so other workers cannot observe an inconsistent
 * state.  Non-exclusive relations skip the close branch (multiple open rows valid).
 *
 * See EXCLUSIVE_RELATIONS[] and CONTRADICTORY_PAIRS[] in memory_db.c for the
 * full compile-time lists of auto-close relation types.
 *
 * Use this from extraction instead of memory_db_relation_create directly.
 *
 * @param user_id User ID
 * @param subject_entity_id Subject entity ID
 * @param relation Relation type (auto-close enabled if exclusive)
 * @param object_entity_id Object entity ID (0 for literal)
 * @param object_value Literal value if no object entity
 * @param fact_id Associated fact ID (0 for none)
 * @param confidence Confidence (0.0-1.0)
 * @param valid_from Start of validity period (0 = open-ended/NULL)
 * @param valid_to End of validity period (0 = open-ended/NULL = currently true)
 * @param prov Provenance; NULL or conv_id==0 = no provenance
 * @param out_old_fact_id If non-NULL and an existing open relation was closed
 *        (exclusive supersede or contradictory-pair close), receives that old
 *        relation's fact_id (0 if none was linked)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_relation_supersede(int user_id,
                                 int64_t subject_entity_id,
                                 const char *relation,
                                 int64_t object_entity_id,
                                 const char *object_value,
                                 int64_t fact_id,
                                 float confidence,
                                 int64_t valid_from,
                                 int64_t valid_to,
                                 const memory_provenance_t *prov,
                                 int64_t *out_old_fact_id);

/**
 * @brief List relations where entity is subject (outgoing).  Returns ALL
 * relations regardless of validity period — use _list_by_subject_at for
 * temporal filtering.
 *
 * @param count_out Output: number of relations
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_relation_list_by_subject(int user_id,
                                       int64_t subject_entity_id,
                                       memory_relation_t *out,
                                       int max,
                                       int *count_out);

/**
 * @brief List relations valid at a given timestamp (v33).
 *
 * Returns rows where (valid_from IS NULL OR valid_from <= as_of_ts)
 *                AND (valid_to IS NULL OR valid_to > as_of_ts).
 *
 * Pass as_of_ts = 0 for "currently valid" (now()).  Used by the entity-recall
 * block when building the LLM context.
 *
 * @param user_id User ID
 * @param subject_entity_id Subject entity ID
 * @param as_of_ts Timestamp to evaluate validity at (0 = now)
 * @param out Output array
 * @param max Maximum results
 * @param count_out Output: number of relations
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_relation_list_by_subject_at(int user_id,
                                          int64_t subject_entity_id,
                                          int64_t as_of_ts,
                                          memory_relation_t *out,
                                          int max,
                                          int *count_out);

/**
 * @brief List incoming relations where entity is the object
 *
 * Returns relations where the given entity is the target/object.
 * The object_name field contains the subject entity's resolved name.
 *
 * @param user_id User ID
 * @param object_entity_id Entity ID to find incoming relations for
 * @param out Output array
 * @param max Maximum results
 * @param count_out Output: number of relations
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_relation_list_by_object(int user_id,
                                      int64_t object_entity_id,
                                      memory_relation_t *out,
                                      int max,
                                      int *count_out);

/**
 * @brief Graph-retrieval Phase 1A: return DISTINCT fact_ids of fact-linked
 * relations touching @p entity_id as either subject or object.
 *
 * Walks `memory_relations` for rows where (subject_entity_id = @p entity_id
 * OR object_entity_id = @p entity_id) AND fact_id IS NOT NULL.  Returns the
 * linked fact IDs, sorted by relation confidence DESC then created_at DESC
 * so the highest-quality graph anchors come first when the fan-out cap
 * trips.
 *
 * Skips relations with NULL fact_id — those are structured-only graph
 * edges (Phase 1B territory).  ~40% of relations are fact-linked at the
 * dev's current LoCoMo scale (May 2026 profile).
 *
 * Caller is responsible for deduplicating fact_ids across multiple seed
 * entities (e.g., a fact about both "John" and "Tim" will surface once
 * per seed entity that touches it).
 *
 * @param user_id User ID (defense-in-depth scoping)
 * @param entity_id Seed entity ID
 * @param out_fact_ids Caller-allocated array of size @p max
 * @param max Fan-out cap; bound this from the caller side per query
 * @param count_out Receives actual returned count
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_relation_fact_ids_for_entity(int user_id,
                                           int64_t entity_id,
                                           int64_t *out_fact_ids,
                                           int max,
                                           int *count_out);

/**
 * @brief List this user's distinct relation predicates, ordered by frequency.
 *
 * Used by the Phase 0 extraction-prompt builder to surface the user's
 * accumulated predicate vocabulary back to the LLM ("previously used
 * relation types"), bounding the LLM's tendency to invent parallel
 * duplicates (has_child / has_children, is_friend_of / is_friend_with).
 *
 * Frequency-ordered DESC so when @p max truncates, the most-used
 * predicates survive.  Each row is a snake_case predicate string
 * (canonicalized at insert time via memory_predicate_canonicalize).
 *
 * @param user_id User ID
 * @param out 2D array of [max][MEMORY_RELATION_MAX] for predicate strings
 * @param max Capacity of @p out (typically 30 — enough headroom over
 *            observed per-user distinct counts of 15-23)
 * @param count_out Receives actual returned count
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_relation_distinct_predicates(int user_id,
                                           char out[][MEMORY_RELATION_MAX],
                                           int max,
                                           int *count_out);

/**
 * @brief Bulk-load all relations for a user with canonical-root resolution
 *
 * Single query that joins memory_relations against memory_entities on both
 * subject and object sides, returning each relation along with its
 * SUBJECT and OBJECT canonical-root entity ids (or 0 for object-as-literal
 * where memory_relations.object_entity_id is NULL).  Canonical-root = the
 * entity's `canonical_id` if set, else the entity's own `id` — i.e. the
 * head of the row's equivalence class under the single-level alias
 * invariant.  Bundle 2 (May 13) established that equivalence-class
 * aggregation belongs in SQL; this is the read-side complement to the
 * mention_count / first_seen / last_seen aggregation already done there.
 *
 * Callers (build_entities_json_array, memory_export) use the resolved
 * roots to attribute relations to the canonical entity for the class,
 * not the row they happen to be attached to.  Without this, a relation
 * like `(sugar hill, located_in, georgia)` attached to alias entity "sugar
 * hill" would NOT surface on the canonical "sugar hill, georgia" card.
 *
 * Returned sorted by subject canonical-root for predictable iteration.
 *
 * Output arrays are PARALLEL — out[i], subj_roots[i], and obj_roots[i]
 * all describe the same relation row.  All three must be sized >= @p max.
 *
 * @param user_id     User ID
 * @param out         Output array of relations (size >= max)
 * @param subj_roots  Output: subject canonical-root id for each rel
 * @param obj_roots   Output: object canonical-root id, or 0 for literals
 * @param max         Maximum relations to return
 * @param count_out   Output: number of relations actually written
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_relation_list_with_canonical_roots_by_user(int user_id,
                                                         memory_relation_t *out,
                                                         int64_t *subj_roots,
                                                         int64_t *obj_roots,
                                                         int max,
                                                         int *count_out);

/**
 * @brief List all entities for a user, ordered by mention count
 *
 * Used to feed existing entities into the extraction prompt so the
 * LLM reuses canonical names instead of creating variants.
 *
 * @param user_id User ID
 * @param out Output array of entities
 * @param max Maximum entities to return
 * @param offset Starting offset for pagination
 * @param count_out Output: number of entities found
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_entity_list(int user_id, memory_entity_t *out, int max, int offset, int *count_out);

/**
 * @brief Search entities by keyword (LIKE on canonical_name)
 *
 * @param user_id User ID
 * @param keywords Search terms
 * @param out Output array of entities
 * @param max Maximum entities to return
 * @param count_out Output: number of entities found
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_entity_search(int user_id,
                            const char *keywords,
                            memory_entity_t *out,
                            int max,
                            int *count_out);

/**
 * @brief Delete an entity and its relations
 *
 * @param entity_id Entity ID
 * @param user_id User ID (ownership check)
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_entity_delete(int64_t entity_id, int user_id);

/**
 * @brief Set an entity's photo (image store reference).
 *
 * @param user_id User ID (ownership check)
 * @param entity_id Entity ID
 * @param photo_id Image store ID, or NULL to clear
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_entity_set_photo(int user_id, int64_t entity_id, const char *photo_id);

/**
 * @brief Get an entity's photo ID.
 *
 * @param user_id User ID (ownership check)
 * @param entity_id Entity ID
 * @param out_photo_id Output buffer for photo ID
 * @param photo_id_size Size of output buffer
 * @return MEMORY_DB_SUCCESS (photo_id may be empty if none set),
 *         MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_entity_get_photo(int user_id,
                               int64_t entity_id,
                               char *out_photo_id,
                               size_t photo_id_size);

/**
 * @brief Merge source entity into target entity
 *
 * Reassigns all relations and contacts from source to target,
 * adds source mention_count to target, deduplicates self-referencing
 * relations, then deletes the source entity. All within a transaction.
 *
 * @param user_id User ID (ownership check on both entities)
 * @param source_id Entity to merge FROM (will be deleted)
 * @param target_id Entity to merge INTO (will absorb data)
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_entity_merge(int user_id, int64_t source_id, int64_t target_id);

/**
 * @brief Load entities by id (same fields as memory_db_entity_get_by_name)
 *
 * One statement for any number of ids.  Only the user's entities are
 * returned; order follows the database, not @p ids.
 *
 * @param count_out Output: entities written to @p out
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_entities_get_by_ids(int user_id,
                                  const int64_t *ids,
                                  int n,
                                  memory_entity_t *out,
                                  int *count_out);

/**
 * @brief How many canonical entities have an embedding of @p expected_dims
 *
 * What memory_db_entity_get_embeddings() would load with no limit, so a
 * caller can size its buffers once.
 *
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_entity_embedding_count(int user_id, int expected_dims, int *count_out);

/**
 * @brief Load all entity embeddings for a user (for cache).
 *
 * Defaults to canonical-only: rows with `canonical_id IS NOT NULL` (soft
 * aliases of another entity, v43) are excluded so the entity-embedding
 * cache doesn't double-count surface-form
 * variants of the same real-world entity.  Pass @p include_aliases = true
 * for the future Graph-tab "show all rows" view; production retrieval
 * paths always pass false.
 *
 * @param user_id User ID
 * @param include_aliases When false (default in production), filter out rows
 *                        with canonical_id IS NOT NULL.  When true, include
 *                        every embedded entity for the user.
 * @param expected_dims Expected embedding dimensions
 * @param out_ids Output: entity IDs
 * @param out_names Output: display names (the `name` column, not canonical_name)
 * @param out_types Output: entity types
 * @param out_embeddings Output: flat float array
 * @param out_norms Output: norms
 * @param max Maximum entries
 * @param count_out Output: number loaded
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_entity_get_embeddings(int user_id,
                                    bool include_aliases,
                                    int expected_dims,
                                    int64_t *out_ids,
                                    char out_names[][MEMORY_ENTITY_NAME_MAX],
                                    char out_types[][MEMORY_ENTITY_TYPE_MAX],
                                    float *out_embeddings,
                                    float *out_norms,
                                    int max,
                                    int *count_out);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_DB_ENTITIES_H */
