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
 * Relation-predicate canonicalization for the two-tier vocabulary.
 * See memory_predicate_dedup.h for the resolver contract.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include "memory/memory_predicate_dedup.h"

#include <ctype.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db_internal.h"
#include "dawn_error.h"
#include "logging.h"

/* =============================================================================
 * Standard vocabulary — Schema.org Person properties + ConceptNet abstractions.
 *
 * See dawn/docs/PHASE_0_EXTRACTION_PROMPT_DRAFT.md §"Two-tier vocabulary"
 * for the mapping table from each entry to its Schema.org / ConceptNet
 * source.  Predicates in this list have semantic guarantees in the
 * memory subsystem:
 *
 *   - Some are one-at-a-time (married_to, born_in, ...) or identifying
 *     (email_is, works_at, ...) — see SPECIAL_RELATIONS[] in
 *     memory_db_relations.c.  Adding one requires updating that list too.
 *   - Contradiction pairs (likes/dislikes, enjoys/hates, can/cannot,
 *     is/is_not) are in CONTRADICTORY_PAIRS[] in memory_db_relations.c.  This
 *     list intentionally still includes both halves of each pair so
 *     the resolver doesn't accidentally collapse `dislikes` onto
 *     `likes`.
 *
 * `is_not` is intentionally omitted (unused in observed extraction
 * data; negation handled via `is` with negated object or via
 * dislikes/cannot analogs).  If the prompt change post-Phase 0 starts
 * emitting `is_not` again, add it back here.
 * ============================================================================= */
static const char *const STANDARD_RELATIONS[] = {
   /* Schema.org Person properties */
   "lives_in",         /* schema:homeLocation */
   "works_at",         /* schema:worksFor */
   "attends_school",   /* schema:alumniOf / memberOf */
   "born_in",          /* schema:birthPlace */
   "born_on",          /* schema:birthDate */
   "married_to",       /* schema:spouse */
   "parent_of",        /* schema:parent (inverse) */
   "child_of",         /* schema:children (inverse) */
   "sibling_of",       /* schema:sibling */
   "friend_of",        /* foaf:knows */
   "has_pet",          /* schema:owns (Pet) */
   "owns_vehicle",     /* schema:owns (Vehicle) */
   "member_of",        /* schema:memberOf */
   "primary_language", /* schema:knowsLanguage */
   "email_is",         /* schema:email */
   "phone_number_is",  /* schema:telephone */
   "nationality",      /* schema:nationality */
   /* Sentiment / activity */
   "likes",
   "dislikes",
   "enjoys",
   "hates",
   /* ConceptNet commonsense */
   "can",    /* ConceptNet:CapableOf */
   "cannot", /* ConceptNet:NotCapableOf */
   "is_a",   /* ConceptNet:IsA — type/category */
   "is",     /* ConceptNet:HasProperty — attribute (color, size, mood) */
   /* High-frequency specialized properties */
   "favorite_color",
   "favorite_food",
};
static const int STANDARD_RELATIONS_COUNT = (int)(sizeof(STANDARD_RELATIONS) /
                                                  sizeof(STANDARD_RELATIONS[0]));

/* =============================================================================
 * Helpers
 * ============================================================================= */

/* Lowercase + trim leading/trailing whitespace into a fixed-size buffer.
 * Returns the written length (0 on empty input). */
static size_t normalize_predicate(const char *src, char *out, size_t out_cap) {
   if (out_cap == 0)
      return 0;
   out[0] = '\0';
   if (!src)
      return 0;

   /* Skip leading whitespace */
   while (*src && isspace((unsigned char)*src))
      src++;
   size_t src_len = strlen(src);
   /* Trim trailing whitespace */
   while (src_len > 0 && isspace((unsigned char)src[src_len - 1]))
      src_len--;
   if (src_len >= out_cap)
      src_len = out_cap - 1;
   for (size_t i = 0; i < src_len; i++) {
      out[i] = (char)tolower((unsigned char)src[i]);
   }
   out[src_len] = '\0';
   return src_len;
}

/* Token-Jaccard on snake_case predicates.  Split on underscore.
 * "has_child" tokens = ["has", "child"]; "has_children" = ["has", "children"].
 * Strict equality on tokens (no stemming) — keeps the comparator fast and
 * predictable.  The Jaccard threshold (0.66) absorbs single-token variation. */
#define MAX_TOKENS 16
#define MAX_TOKEN_LEN 32

static int tokenize_predicate(const char *predicate, char tokens[MAX_TOKENS][MAX_TOKEN_LEN]) {
   if (!predicate)
      return 0;
   int n = 0;
   const char *p = predicate;
   while (*p && n < MAX_TOKENS) {
      while (*p == '_')
         p++;
      if (!*p)
         break;
      const char *start = p;
      while (*p && *p != '_')
         p++;
      size_t len = (size_t)(p - start);
      if (len >= MAX_TOKEN_LEN)
         len = MAX_TOKEN_LEN - 1;
      memcpy(tokens[n], start, len);
      tokens[n][len] = '\0';
      n++;
   }
   return n;
}

float memory_predicate_jaccard(const char *a, const char *b) {
   if (!a || !b || !*a || !*b)
      return 0.0f;
   char ta[MAX_TOKENS][MAX_TOKEN_LEN];
   char tb[MAX_TOKENS][MAX_TOKEN_LEN];
   int na = tokenize_predicate(a, ta);
   int nb = tokenize_predicate(b, tb);
   if (na == 0 || nb == 0)
      return 0.0f;
   /* |A ∩ B| */
   int intersect = 0;
   for (int i = 0; i < na; i++) {
      for (int j = 0; j < nb; j++) {
         if (strcmp(ta[i], tb[j]) == 0) {
            intersect++;
            break;
         }
      }
   }
   /* |A ∪ B| = na + nb - intersect */
   int unionsz = na + nb - intersect;
   if (unionsz == 0)
      return 0.0f;
   return (float)intersect / (float)unionsz;
}

bool memory_predicate_is_standard(const char *predicate) {
   if (!predicate)
      return false;
   for (int i = 0; i < STANDARD_RELATIONS_COUNT; i++) {
      if (strcmp(predicate, STANDARD_RELATIONS[i]) == 0)
         return true;
   }
   return false;
}

/* =============================================================================
 * Resolver
 * ============================================================================= */

int memory_predicate_canonicalize(int user_id, const char *candidate, char *out, size_t out_cap) {
   if (!out || out_cap == 0)
      return FAILURE;
   out[0] = '\0';
   if (!candidate || !*candidate)
      return FAILURE;

   /* Step 1: normalize */
   char norm[MEMORY_PREDICATE_NAME_MAX];
   size_t norm_len = normalize_predicate(candidate, norm, sizeof(norm));
   if (norm_len == 0)
      return FAILURE;

   /* Step 2: standard-vocab exact match */
   if (memory_predicate_is_standard(norm)) {
      snprintf(out, out_cap, "%s", norm);
      return SUCCESS;
   }

   /* Step 3: user-vocabulary dedup.  Fetch this user's distinct
    * predicates and find the best Jaccard match.  Ad-hoc prepared
    * statement here (not cached in s_db.stmt_*) because the call
    * frequency is once per relation insert — bounded by extraction
    * fact count, typically <30 per extraction.  Move to cached
    * statement if profiling shows it on a hotter path. */
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT DISTINCT relation FROM memory_relations "
                               "WHERE user_id = ?",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("memory_predicate: prepare user-vocab query failed: %s",
                   sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      /* Step 4: fall through — accept lowercased candidate as custom */
      snprintf(out, out_cap, "%s", norm);
      return SUCCESS;
   }
   sqlite3_bind_int(stmt, 1, user_id);

   char best_match[MEMORY_PREDICATE_NAME_MAX];
   best_match[0] = '\0';
   float best_score = 0.0f;

   while (sqlite3_step(stmt) == SQLITE_ROW) {
      const char *existing = (const char *)sqlite3_column_text(stmt, 0);
      if (!existing || !*existing)
         continue;
      /* Skip if existing equals candidate post-normalize (no canonicalization
       * needed — caller would have stored under the same string). */
      if (strcmp(existing, norm) == 0) {
         best_match[0] = '\0';
         best_score = 1.0f; /* Sentinel: exact match, no canonicalization needed */
         snprintf(best_match, sizeof(best_match), "%s", existing);
         break;
      }
      float score = memory_predicate_jaccard(norm, existing);
      if (score >= MEMORY_PREDICATE_DEDUP_JACCARD_FLOOR && score > best_score) {
         best_score = score;
         snprintf(best_match, sizeof(best_match), "%s", existing);
      }
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   if (best_match[0]) {
      snprintf(out, out_cap, "%s", best_match);
      if (strcmp(best_match, norm) != 0) {
         OLOG_DEBUG("memory_predicate: canonicalized '%s' -> '%s' (jaccard=%.2f, user=%d)", norm,
                    best_match, (double)best_score, user_id);
      }
      return SUCCESS;
   }

   /* Step 4: accept as new custom predicate */
   snprintf(out, out_cap, "%s", norm);
   return SUCCESS;
}
