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
 * Memory System Type Definitions and DB Return Codes
 *
 * Data structures for the persistent memory system (facts, preferences,
 * summaries, entities, relations, provenance) plus the `MEMORY_DB_*` return
 * codes shared by `memory_db.h` and its split sub-headers
 * (`memory_db_entities.h`, `memory_db_embeddings.h`, `memory_db_provenance.h`).
 * Centralizing the typedef + return codes here lets each sub-header reference
 * them without depending on the umbrella `memory_db.h`.
 */

#ifndef MEMORY_TYPES_H
#define MEMORY_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Memory DB return codes
 *
 * Defined here (rather than in memory_db.h) so that the split sub-headers
 * memory_db_entities.h and memory_db_embeddings.h can reference these codes
 * without pulling in the full memory_db.h umbrella.  Same logic applies to
 * memory_provenance_t below.
 * ============================================================================= */

#define MEMORY_DB_SUCCESS 0
#define MEMORY_DB_FAILURE 1
#define MEMORY_DB_NOT_FOUND 2
#define MEMORY_DB_DUPLICATE 3
/* v43 entity-merge: caller passed an entity that's currently a soft alias
 * to an API that requires a canonical (e.g. memory_db_entity_score_pair).
 * Distinct from NOT_FOUND so callers can render a helpful "pick its
 * canonical instead" message. */
#define MEMORY_DB_INVALID_ALIAS_TARGET 4
/* v44 entity-merge Phase 1.5: link-user-self requires a non-empty
 * users.real_name so the synthetic seed (or the seeded canonical, on
 * commit) has a meaningful name to anchor scoring against.  Distinct
 * from NOT_FOUND so the admin handler can surface a "Configure WebUI
 * Settings → User → Real name" hint instead of a generic error. */
#define MEMORY_DB_REAL_NAME_REQUIRED 5
/* v44 entity-merge Phase 1.5 fold-in: link-user-self could not create the
 * user-self entity because an existing entity already holds the canonicalized
 * real_name AND scored below MEMORY_ALIAS_SELF_PROMOTION_THRESHOLD against
 * the synthetic seed.  Operator action required: either promote the colliding
 * entity manually (`dawn-admin memory entity merge`) or change real_name to
 * disambiguate.  Surfaces from memory_alias_link_user_self_run() commit path. */
#define MEMORY_DB_SELF_NAME_COLLISION 6

/* =============================================================================
 * Provenance
 *
 * Coarse source linkage: every item produced by a single extraction call
 * shares the same (conv_id, msg_id_start, msg_id_end) triple.  conv_id == 0
 * means "no provenance" (voice-only path, import, or explicit remember).
 *
 * Lives here (not in memory_db.h) so the entity / embedding sub-headers and
 * memory_db_provenance.h can declare provenance-aware function prototypes
 * without depending on the memory_db.h umbrella.
 * ============================================================================= */

typedef struct {
   int64_t conv_id; /* 0 = no provenance */
   int64_t msg_id_start;
   int64_t msg_id_end;
} memory_provenance_t;

/* =============================================================================
 * Buffer Size Constants
 * ============================================================================= */

#define MEMORY_FACT_TEXT_MAX 512
/* Canonical size for buffers holding stemmed fact_text + tokenizer working
 * copies across the memory subsystem.  Porter2 only shortens tokens (suffix
 * manipulation) and the tokenizer replaces delimiters 1-to-1 with spaces, so
 * stem-side output is always <= input length.  Sized 256 bytes over
 * MEMORY_FACT_TEXT_MAX (=512) to give the stem framing and tokenizer
 * working copies a comfortable margin against edge cases (e.g., the tokenizer
 * dropping multi-byte UTF-8 punctuation in favor of single spaces) without
 * silent mid-token truncation.  Lives here next to MEMORY_FACT_TEXT_MAX so
 * the stem / fact_search / callback tokenizer call sites share one source
 * of truth — keeping the BM25 ranking pipeline free of size-drift across
 * the token-bookkeeping path. */
#define MEMORY_FACT_STEMS_MAX 768
#define MEMORY_SOURCE_MAX 16
#define MEMORY_CATEGORY_MAX 32
#define MEMORY_PREF_VALUE_MAX 256
#define MEMORY_SUMMARY_MAX 2048
#define MEMORY_TOPICS_MAX 256
#define MEMORY_SESSION_ID_MAX 64
#define MEMORY_SENTIMENT_MAX 16

/* Maximum items for batch operations */
#define MEMORY_MAX_FACTS 50
#define MEMORY_MAX_PREFS 20
#define MEMORY_MAX_SUMMARIES 10

/* =============================================================================
 * Memory Fact Structure
 *
 * Represents a single fact about the user, e.g., "User has a golden retriever
 * named Max" or "User works as a software engineer".
 *
 * Facts can be:
 * - Explicit: User directly stated it ("Remember that I prefer dark mode")
 * - Inferred: Extracted from conversation context
 *
 * Confidence ranges from 0.0 (uncertain) to 1.0 (definite).
 * ============================================================================= */

typedef struct {
   int64_t id;
   int user_id;
   char fact_text[MEMORY_FACT_TEXT_MAX];
   float confidence;
   char source[MEMORY_SOURCE_MAX];     /* "explicit" or "inferred" */
   char category[MEMORY_CATEGORY_MAX]; /* category column (v34); no longer assigned */
   time_t created_at;
   time_t last_accessed;
   int access_count;
   int64_t superseded_by; /* ID of fact that replaced this one, or 0 */
   int64_t expires_at;    /* v58: fact-lifecycle expiry, unix sec; 0 = durable (no expiry).
                           * Only populated by the by-id fetch (memory_db_fact_get); the
                           * keyword/list retrieval statements filter expired rows in SQL,
                           * so their results default this to 0. */
} memory_fact_t;

/* =============================================================================
 * Memory Preference Structure
 *
 * Represents a user preference with a category and value.
 * Categories are normalized (e.g., "theme", "units", "communication_style").
 *
 * Unlike facts, preferences use upsert semantics - storing a new value for
 * an existing category updates rather than creates a new record.
 *
 * reinforcement_count tracks how many times a preference has been expressed,
 * used to increase confidence over time.
 * ============================================================================= */

typedef struct {
   int64_t id;
   int user_id;
   char category[MEMORY_CATEGORY_MAX];
   char value[MEMORY_PREF_VALUE_MAX];
   float confidence;
   char source[MEMORY_SOURCE_MAX]; /* "explicit" or "inferred" */
   time_t created_at;
   time_t updated_at;
   int reinforcement_count;
} memory_preference_t;

/* =============================================================================
 * Memory Summary Structure
 *
 * Stores a summary of a conversation session for later recall.
 * Summaries are generated during memory extraction at session end.
 *
 * Topics is a comma-separated list of main discussion topics.
 * Sentiment captures overall emotional tone ("positive", "neutral", "negative").
 * ============================================================================= */

typedef struct {
   int64_t id;
   int user_id;
   char session_id[MEMORY_SESSION_ID_MAX]; /* Links to metrics/conversation */
   char summary[MEMORY_SUMMARY_MAX];
   char topics[MEMORY_TOPICS_MAX];
   char sentiment[MEMORY_SENTIMENT_MAX];
   time_t created_at;
   int message_count;
   int duration_seconds;
   bool consolidated; /* True if rolled up into a larger summary */
} memory_summary_t;

/* =============================================================================
 * Memory Statistics Structure
 *
 * Used by memory_db_get_stats() to return aggregate counts.
 * ============================================================================= */

typedef struct {
   int fact_count;
   int pref_count;
   int summary_count;
   int entity_count;
   time_t oldest_fact;
   time_t newest_fact;
} memory_stats_t;

/* =============================================================================
 * Extraction Request Structure
 *
 * Passed to memory_trigger_extraction() with all data needed to run
 * extraction asynchronously after session ends.
 * ============================================================================= */

typedef struct {
   int user_id;
   int64_t conversation_id;
   char session_id[MEMORY_SESSION_ID_MAX];
   int message_count;
   int duration_seconds;
} memory_extraction_request_t;

/* =============================================================================
 * Entity Graph Structures (Phase S4)
 *
 * Entities represent named people, pets, places, organizations, and things
 * mentioned by the user. Relations capture typed links between entities
 * (e.g., "Bruno" is_a "golden retriever", "Jon" lives_in "Atlanta").
 * ============================================================================= */

#define MEMORY_ENTITY_NAME_MAX 64
#define MEMORY_ENTITY_TYPE_MAX 32
#define MEMORY_RELATION_MAX 64

typedef struct {
   int64_t id;
   int user_id;
   char name[MEMORY_ENTITY_NAME_MAX];
   char entity_type[MEMORY_ENTITY_TYPE_MAX];
   char canonical_name[MEMORY_ENTITY_NAME_MAX];
   int mention_count;
   time_t first_seen;
   time_t last_seen;
} memory_entity_t;

typedef struct {
   int64_t id;
   int64_t subject_entity_id;
   char relation[MEMORY_RELATION_MAX];
   int64_t object_entity_id;                 /* 0 if literal value */
   char object_name[MEMORY_ENTITY_NAME_MAX]; /* Resolved entity name or literal */
   float confidence;
   /* Validity period (v33).  0 = NULL in SQL = open-ended.
    * "currently true" predicate: valid_to == 0 || valid_to > now() */
   int64_t valid_from;
   int64_t valid_to;
   /* Re-witness counter (v49).  Bumped by the upsert in stmt_memory_relation_create
    * when the same (user_id, subject_entity_id, relation, object) tuple is re-emitted
    * while valid_to IS NULL.  >= 1.  See partial UNIQUE index
    * idx_memory_relations_unique_open. */
   int mention_count;
} memory_relation_t;

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_TYPES_H */
