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
 * Entity-graph candidate retrieval — Phase 1A (fact-linked relations).
 * See memory_graph_retrieval.h for the pipeline contract.
 */

#include "memory/memory_graph_retrieval.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "config/dawn_config.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_db_entities.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_types.h"

/* Question-starter words.  When the first sentence-initial capitalized
 * token is one of these, we skip it — it's never a proper noun in
 * question context.  Other capitalized tokens later in the query are
 * always considered (a proper noun can follow a question-starter, e.g.
 * "What did John say?"). */
static const char *const QUESTION_STARTERS[] = {
   "What", "Why", "How",  "Who", "When", "Where", "Which", "Would", "Could", "Should", "Is",
   "Are",  "Was", "Were", "Did", "Does", "Do",    "Has",   "Have",  "Had",   "Will",   NULL,
};

/* Minimum proper-noun length to count as a candidate.  3 covers common
 * names ("Tim", "Sam") without admitting noise like "U.S.". */
#define MIN_CANDIDATE_LEN 3

/* Per-candidate scratch buffer.  MEMORY_ENTITY_NAME_MAX covers any
 * canonical_name in memory_entities; the candidate itself can't be
 * longer than that or it wouldn't resolve. */
#define CANDIDATE_BUF_LEN MEMORY_ENTITY_NAME_MAX

static bool token_is_question_starter(const char *tok, size_t len) {
   for (int i = 0; QUESTION_STARTERS[i] != NULL; i++) {
      if (strlen(QUESTION_STARTERS[i]) == len && strncmp(QUESTION_STARTERS[i], tok, len) == 0) {
         return true;
      }
   }
   return false;
}

/* Inside-token character predicate: letters, digits, apostrophe, hyphen.
 * Apostrophe + hyphen admit compound names like "O'Brien" and
 * "Mary-Anne".  We strip a trailing apostrophe-s ("John's") at
 * normalize time so possessives resolve. */
static bool is_token_char(unsigned char c) {
   return isalnum(c) || c == '\'' || c == '-';
}

/* Lowercase + possessive strip in one pass.  Writes to @p out (capacity
 * @p out_cap, must be > len + 1).  Returns the written length.
 *
 * "John's" → "john"
 * "Mary-Anne's" → "mary-anne"
 * "O'Brien" → "o'brien"
 */
static size_t normalize_candidate(const char *src, size_t len, char *out, size_t out_cap) {
   if (out_cap == 0)
      return 0;
   /* Strip trailing 's" — case-insensitive match for either apostrophe.
    * We check before the lower-case pass so we don't have to deal with
    * unicode curly quotes here (LoCoMo is ASCII). */
   if (len >= 2 && src[len - 1] == 's' && src[len - 2] == '\'') {
      len -= 2;
   }
   if (len >= out_cap)
      len = out_cap - 1;
   for (size_t i = 0; i < len; i++) {
      out[i] = (char)tolower((unsigned char)src[i]);
   }
   out[len] = '\0';
   return len;
}

static bool seed_set_contains(const int64_t *set, int set_count, int64_t id) {
   for (int i = 0; i < set_count; i++) {
      if (set[i] == id)
         return true;
   }
   return false;
}

int memory_graph_extract_seed_entities(int user_id,
                                       const char *query,
                                       int64_t *out_entity_ids,
                                       int max,
                                       int *out_count) {
   if (out_count != NULL)
      *out_count = 0;
   if (out_entity_ids == NULL || max <= 0)
      return FAILURE;
   if (query == NULL || query[0] == '\0')
      return SUCCESS;

   int seed_count = 0;

   /* Entities the query names by any of their names ("Dawn" names "Dawn
    * Smith"; an alias names its entity), multi-word names included. */
   memory_entity_match_t named[MEMORY_GRAPH_MAX_SEED_CANDIDATES];
   int n_named = 0;
   const int named_cap = max < MEMORY_GRAPH_MAX_SEED_CANDIDATES ? max
                                                                : MEMORY_GRAPH_MAX_SEED_CANDIDATES;
   if (memory_embeddings_entity_matches(user_id, query, NULL, 0, 0.0f, named, named_cap,
                                        &n_named) == SUCCESS) {
      for (int i = 0; i < n_named && seed_count < max; i++) {
         if (named[i].named && !seed_set_contains(out_entity_ids, seed_count, named[i].id)) {
            out_entity_ids[seed_count++] = named[i].id;
         }
      }
   }

   bool first_token = true;
   const char *p = query;
   const char *end = query + strlen(query);

   while (p < end && seed_count < max) {
      /* Skip non-token chars */
      while (p < end && !is_token_char((unsigned char)*p))
         p++;
      if (p >= end)
         break;
      const char *tok_start = p;
      while (p < end && is_token_char((unsigned char)*p))
         p++;
      size_t tok_len = (size_t)(p - tok_start);
      bool was_first = first_token;
      first_token = false;

      /* Filter: capitalized AND length >= MIN_CANDIDATE_LEN.  The
       * apostrophe-s suffix counts toward length here — "Tim's" passes
       * the length gate at 5 even though we'll strip to 3 when
       * normalizing.  That's deliberate (we want to match "Tim"). */
      if (tok_len < MIN_CANDIDATE_LEN)
         continue;
      if (!isupper((unsigned char)tok_start[0]))
         continue;
      if (was_first && token_is_question_starter(tok_start, tok_len)) {
         /* Sentence-initial question-starter — skip as candidate. */
         continue;
      }

      char canon[CANDIDATE_BUF_LEN];
      size_t canon_len = normalize_candidate(tok_start, tok_len, canon, sizeof(canon));
      if (canon_len < MIN_CANDIDATE_LEN)
         continue;

      /* Exact canonical-name match, as its class (an alias seeds its
       * canonical: the expansion reads every name in the class, so the
       * alias itself would be the same seed twice). */
      int64_t root = 0;
      if (memory_db_entity_root_by_name(user_id, canon, &root) != MEMORY_DB_SUCCESS || root <= 0)
         continue;
      if (seed_set_contains(out_entity_ids, seed_count, root))
         continue;
      out_entity_ids[seed_count++] = root;
   }

   if (out_count != NULL)
      *out_count = seed_count;
   return SUCCESS;
}

/* Fetch fact @p fid into out_facts[*produced] unless it is already there,
 * gone, or expired.  Whether it was added. */
static bool add_fact(int user_id,
                     int64_t fid,
                     float score,
                     memory_fact_t *out_facts,
                     float *out_scores,
                     int *produced) {
   if (fid <= 0)
      return false;
   for (int j = 0; j < *produced; j++) {
      if (out_facts[j].id == fid)
         return false;
   }
   /* memory_db_fact_get is user-scoped at the SQL layer (CWE-639
    * defense-in-depth): a row pointing at another user's fact is NOT_FOUND. */
   memory_fact_t fact = { 0 };
   if (memory_db_fact_get(fid, user_id, &fact) != MEMORY_DB_SUCCESS)
      return false;
   /* Expiry retrieval guard (v58): this by-id retrieval feeds the LLM like
    * the vector-only path in memory_fact_search.c, so expired facts (a
    * scheduled event that hasn't happened) are dropped here. */
   if (memory_db_fact_expiry_hidden(fact.expires_at))
      return false;
   /* A replaced or merged fact isn't current (a relation can still point at
    * it), and the rescore that would drop it can be off. */
   if (fact.superseded_by > 0)
      return false;
   out_facts[*produced] = fact;
   out_scores[*produced] = score;
   (*produced)++;
   return true;
}

int memory_graph_expand_fact_linked(int user_id,
                                    const int64_t *seed_entity_ids,
                                    int seed_count,
                                    memory_fact_t *out_facts,
                                    float *out_scores,
                                    int max,
                                    int *out_count) {
   if (out_count != NULL)
      *out_count = 0;
   if (out_facts == NULL || out_scores == NULL || max <= 0)
      return FAILURE;
   if (seed_count <= 0 || seed_entity_ids == NULL)
      return SUCCESS;

   const float bonus = g_config.memory.graph_retrieval.entity_grounding_bonus;

   /* Per-seed fan-out budget: what is left, shared by the seeds still to go,
    * so a seed that finds little hands its share on.  Caller passes the global
    * cap (typically graph_retrieval.max_facts_per_query, default 30) so we
    * don't explode for high-degree entities like John/Tim/Caroline. */
   int produced = 0;
   int64_t about_ids[64];  /* Per seed: facts about the entity. */
   int64_t linked_ids[64]; /* Per seed: facts its relations came from. */
   const int buf_cap = (int)(sizeof(about_ids) / sizeof(about_ids[0]));

   for (int s = 0; s < seed_count && produced < max; s++) {
      int seed_cap = (max - produced) / (seed_count - s);
      if (seed_cap < 1)
         seed_cap = 1;
      if (seed_cap > buf_cap)
         seed_cap = buf_cap;

      int n_about = 0;
      int n_linked = 0;
      if (memory_db_fact_ids_for_subject(user_id, seed_entity_ids[s], about_ids, seed_cap,
                                         &n_about) != MEMORY_DB_SUCCESS)
         n_about = 0;
      if (memory_db_relation_fact_ids_for_entity(user_id, seed_entity_ids[s], linked_ids, seed_cap,
                                                 &n_linked) != MEMORY_DB_SUCCESS)
         n_linked = 0;

      /* Take the two in turn, so neither crowds out the other. */
      int taken = 0;
      for (int k = 0; (k < n_about || k < n_linked) && taken < seed_cap && produced < max; k++) {
         if (k < n_about &&
             add_fact(user_id, about_ids[k], bonus, out_facts, out_scores, &produced)) {
            taken++;
         }
         if (k < n_linked && taken < seed_cap && produced < max &&
             add_fact(user_id, linked_ids[k], bonus, out_facts, out_scores, &produced)) {
            taken++;
         }
      }
   }

   if (out_count != NULL)
      *out_count = produced;
   return SUCCESS;
}

/* Set-membership helper for the expanded seed table. */
