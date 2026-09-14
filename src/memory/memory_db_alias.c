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
 * Memory Database — entity-merge / user-identity-dedup alias surface (v43).
 *
 * Phase 6b source split — this TU holds the pure-function helpers, type
 * filters, composite formula, canonical_priority comparators, and the
 * DB-accessing helpers (load_entity_full / find_user_self_id / overlap
 * signals / user_self_bonus) that ALL three sibling TUs (scorer / cascade
 * / writes) call into.  See memory_db_alias_internal.h for the cross-TU
 * surface and docs/ENTITY_MERGE_DESIGN.md §2 for the design.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_db_alias_internal.h"
#include "memory/memory_db_aliases.h"
#include "memory/memory_db_entities.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_types.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Tokenizer (pure-function helpers shared by Stage 2 and the Jaccard signal)
 *
 * Whitespace + ASCII-punctuation split; tokens shorter than ALIAS_MIN_TOKEN_LEN
 * are dropped (single-letter "a" / "i" tokens swamp the Jaccard denominator
 * with noise without contributing real evidence).  Tokens are case-preserved
 * because canonical_name is already lowercased upstream by
 * memory_make_canonical_name().
 * ============================================================================= */

static bool is_token_separator(unsigned char c) {
   if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
      return true;
   /* ASCII punctuation characters that carve word boundaries. Apostrophes
    * stay attached because "o'brien" / "ai's" etc. shouldn't fragment. */
   switch (c) {
      case '.':
      case ',':
      case ';':
      case ':':
      case '!':
      case '?':
      case '/':
      case '\\':
      case '(':
      case ')':
      case '[':
      case ']':
      case '{':
      case '}':
      case '"':
      case '`':
      case '<':
      case '>':
      case '|':
      case '@':
      case '#':
      case '$':
      case '%':
      case '^':
      case '&':
      case '*':
      case '+':
      case '=':
      case '~':
         return true;
      default:
         return false;
   }
}

void memory_alias_internal_tokenize(const char *s, alias_token_set_t *out) {
   out->count = 0;
   if (!s)
      return;

   const char *p = s;
   while (*p && out->count < ALIAS_MAX_TOKENS) {
      while (*p && is_token_separator((unsigned char)*p)) {
         p++;
      }
      if (!*p)
         break;
      const char *start = p;
      while (*p && !is_token_separator((unsigned char)*p)) {
         p++;
      }
      size_t len = (size_t)(p - start);
      if (len >= (size_t)ALIAS_MIN_TOKEN_LEN && len < ALIAS_TOKEN_MAX) {
         memcpy(out->tokens[out->count], start, len);
         out->tokens[out->count][len] = '\0';
         out->count++;
      }
   }
}

static bool token_set_contains(const alias_token_set_t *set, const char *tok) {
   for (int i = 0; i < set->count; i++) {
      if (strcmp(set->tokens[i], tok) == 0)
         return true;
   }
   return false;
}

/* Directional overlap (Phase 1.5 Ckpt C): |tokens_a ∩ tokens_b| / |tokens_b|.
 *
 * Used at Stage 2 in the synthetic-self path: standard Jaccard penalizes
 * single-token candidates against the verbose synthetic seed (real_name +
 * aliases), e.g. candidate "jon" against a 4-token synthetic gives
 * jaccard = 1/4 = 0.25 which falls below the 0.30 floor and gets dropped.
 * Directional overlap from the candidate's perspective is 1/1 = 1.0 so
 * it survives the floor and reaches Stage 6 scoring.
 *
 * Asymmetric: callers must consistently pass the synthetic side as @p a
 * and the candidate side as @p b.  Standard Jaccard remains the right
 * choice for non-synthetic paths (resolver, extraction-time), where the
 * inbound and existing entities are both real and roughly comparable in
 * token count. */
float memory_alias_internal_directional_overlap(const char *a, const char *b) {
   if (!a || !b || !*a || !*b)
      return 0.0f;

   alias_token_set_t ta, tb;
   memory_alias_internal_tokenize(a, &ta);
   memory_alias_internal_tokenize(b, &tb);
   if (tb.count == 0)
      return 0.0f;

   int intersection = 0;
   for (int i = 0; i < tb.count; i++) {
      if (token_set_contains(&ta, tb.tokens[i]))
         intersection++;
   }
   return (float)intersection / (float)tb.count;
}

float memory_alias_compute_name_jaccard(const char *a, const char *b) {
   if (!a || !b || !*a || !*b)
      return 0.0f;

   alias_token_set_t ta;
   alias_token_set_t tb;
   memory_alias_internal_tokenize(a, &ta);
   memory_alias_internal_tokenize(b, &tb);
   if (ta.count == 0 || tb.count == 0)
      return 0.0f;

   /* |A ∩ B| — count tokens of A that appear in B. */
   int intersection = 0;
   for (int i = 0; i < ta.count; i++) {
      if (token_set_contains(&tb, ta.tokens[i]))
         intersection++;
   }
   /* |A ∪ B| = |A| + |B| - |A ∩ B|.  Both sets dedup their own duplicate
    * tokens implicitly because memory_alias_internal_tokenize doesn't currently dedup
    * within a name, so two repeated tokens in `a` would inflate |A|.  In
    * practice canonical_name comes from memory_make_canonical_name() which
    * is single-name lower-cased text — duplicate tokens within one name
    * (e.g. "Mary Mary Sue") are vanishingly rare and self-cancel symmetrically. */
   int union_size = ta.count + tb.count - intersection;
   if (union_size <= 0)
      return 0.0f;

   return (float)intersection / (float)union_size;
}

bool memory_alias_compute_name_substring(const char *a, const char *b) {
   if (!a || !b || !*a || !*b)
      return false;
   if (strcmp(a, b) == 0)
      return false; /* exact equality is not "substring" */
   return strstr(a, b) != NULL || strstr(b, a) != NULL;
}

/* =============================================================================
 * Type filter helpers — pure functions, no DB
 *
 * Per design §6 Stage 3 + §7: the `thing` carve-out matters for the live DB
 * shape (the LLM extracted "Jon" as `thing` and "Jonathan Smith" as
 * `person` — strict type matching would block an obvious correct merge).
 * ============================================================================= */

static bool type_is_thing(const char *t) {
   return t && strcmp(t, "thing") == 0;
}

bool memory_alias_internal_type_veto_fires(const char *a_type, const char *b_type) {
   /* Veto: both non-thing AND types differ → forced reject. */
   if (!a_type || !b_type)
      return false;
   if (type_is_thing(a_type) || type_is_thing(b_type))
      return false;
   return strcmp(a_type, b_type) != 0;
}

static int type_specificity(const char *t) {
   /* Lower-better is irrelevant here — higher = more specific.  See §9. */
   if (!t)
      return 0;
   if (strcmp(t, "person") == 0)
      return 4;
   if (strcmp(t, "place") == 0 || strcmp(t, "pet") == 0 || strcmp(t, "org") == 0)
      return 3;
   if (strcmp(t, "thing") == 0)
      return 1;
   return 0; /* unknown types rank below thing */
}

float memory_alias_internal_type_match_signal(const char *a_type, const char *b_type) {
   if (!a_type || !b_type)
      return 0.0f;
   if (type_is_thing(a_type) || type_is_thing(b_type))
      return 0.0f; /* thing/anything contributes 0 (not penalty) */
   return strcmp(a_type, b_type) == 0 ? 1.0f : 0.0f;
}

/* =============================================================================
 * Composite formula (pure function — testable without a DB)
 * ============================================================================= */

void memory_alias_apply_composite(memory_alias_evidence_t *ev) {
   if (!ev)
      return;

   if (ev->type_veto_fired) {
      ev->composite_score = 0.0f;
      return;
   }

   /* Clamp embedding cosine to [0, 1] — negatives never contribute. */
   float cos_clamped = ev->embedding_cosine;
   if (cos_clamped < 0.0f)
      cos_clamped = 0.0f;
   if (cos_clamped > 1.0f)
      cos_clamped = 1.0f;
   ev->embedding_cosine = cos_clamped;

   float composite = MEMORY_ALIAS_W_NAME_JACCARD * ev->name_jaccard +
                     MEMORY_ALIAS_W_EMBEDDING_COSINE * cos_clamped +
                     MEMORY_ALIAS_W_EXCLUSIVE_RELATION_OVERLAP * ev->exclusive_relation_overlap +
                     MEMORY_ALIAS_W_CONTACT_FIELD_OVERLAP * ev->contact_field_overlap +
                     MEMORY_ALIAS_W_TYPE_MATCH * ev->type_match;

   if (ev->name_substring_bonus_applied)
      composite += MEMORY_ALIAS_BONUS_NAME_SUBSTRING;
   if (ev->user_self_bonus_applied)
      composite += MEMORY_ALIAS_BONUS_USER_SELF;

   if (composite > 1.0f)
      composite = 1.0f;
   if (composite < 0.0f)
      composite = 0.0f;
   ev->composite_score = composite;
}

/* =============================================================================
 * canonical_priority comparators (pure — design §9)
 *
 * Two helpers: the four-axis _compare variant operates on memory_entity_t
 * alone (which doesn't carry the is_user_self column), and the five-axis
 * _compare_self sibling accepts explicit is_user_self flags and short-
 * circuits on them before delegating to the four-axis form.  Cascade call
 * sites that loaded is_user_self from the DB use the sibling; tests / pure
 * callers that don't have is_user_self context use the four-axis form
 * (and accept that two rows differing only in that flag will compare equal).
 *
 * Tuple per design §9 (DESC for is_user_self / type_specificity / mention_count,
 * ASC for first_seen / id):
 *   (is_user_self, type_specificity, mention_count, first_seen, id)
 *
 * Returns:
 *   <  0  → @p a should be canonical over b
 *    0   → fully tied (only when same id and, in _compare_self, equal flags)
 *   >  0  → @p b should be canonical over a
 * ============================================================================= */

int memory_alias_canonical_priority_compare(const memory_entity_t *a, const memory_entity_t *b) {
   if (!a && !b)
      return 0;
   if (!a)
      return 1;
   if (!b)
      return -1;

   /* type_specificity DESC, mention_count DESC, first_seen ASC, id ASC.
    * is_user_self is not present here — see _compare_self for the full tuple. */
   int ts_a = type_specificity(a->entity_type);
   int ts_b = type_specificity(b->entity_type);
   if (ts_a != ts_b)
      return ts_b - ts_a; /* higher type_specificity wins → a < b iff ts_a > ts_b */

   if (a->mention_count != b->mention_count)
      return b->mention_count - a->mention_count;

   if (a->first_seen != b->first_seen)
      return (a->first_seen < b->first_seen) ? -1 : 1; /* older first_seen wins */

   if (a->id != b->id)
      return (a->id < b->id) ? -1 : 1;

   return 0;
}

int memory_alias_canonical_priority_compare_self(const memory_entity_t *a,
                                                 bool a_is_user_self,
                                                 const memory_entity_t *b,
                                                 bool b_is_user_self) {
   if (!a && !b)
      return 0;
   if (!a)
      return 1;
   if (!b)
      return -1;

   /* is_user_self DESC short-circuits the lex tuple. */
   if (a_is_user_self != b_is_user_self)
      return a_is_user_self ? -1 : 1;

   return memory_alias_canonical_priority_compare(a, b);
}

int memory_alias_row_compare_by_composite_desc(const void *a, const void *b) {
   const memory_alias_link_user_self_row_t *ra = (const memory_alias_link_user_self_row_t *)a;
   const memory_alias_link_user_self_row_t *rb = (const memory_alias_link_user_self_row_t *)b;
   /* composite_score DESC */
   if (ra->composite_score > rb->composite_score)
      return -1;
   if (ra->composite_score < rb->composite_score)
      return 1;
   /* entity_id ASC tiebreak — deterministic ordering for equal-score rows. */
   if (ra->entity_id < rb->entity_id)
      return -1;
   if (ra->entity_id > rb->entity_id)
      return 1;
   return 0;
}

/* =============================================================================
 * DB-accessing helpers
 *
 * All take the auth_db lock per call — the alias surface is invoked at
 * extraction time (off the conversational hot path) and from the operator
 * paths (link-user-self, manual merge), so per-call lock acquisition is
 * the right granularity.  No caller holds the lock across helper calls.
 * ============================================================================= */

/* Load a single entity by id, plus its canonical_id and is_user_self flag.
 * Returns MEMORY_DB_SUCCESS / MEMORY_DB_NOT_FOUND / MEMORY_DB_FAILURE.
 * canonical_id_out: 0 = NULL = self is canonical; > 0 = soft alias of that id.
 * is_user_self_out: optional (NULL OK). */
int memory_alias_internal_load_entity_full(int user_id,
                                           int64_t entity_id,
                                           memory_entity_t *out_entity,
                                           int64_t *out_canonical_id,
                                           bool *out_is_user_self) {
   if (!out_entity || entity_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   const char *sql = "SELECT id, user_id, name, entity_type, canonical_name, "
                     "       mention_count, first_seen, COALESCE(last_seen, 0), "
                     "       canonical_id, is_user_self "
                     "FROM memory_entities WHERE id = ? AND user_id = ?";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int64(stmt, 1, entity_id);
   sqlite3_bind_int(stmt, 2, user_id);

   int rc = sqlite3_step(stmt);
   if (rc == SQLITE_DONE) {
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_NOT_FOUND;
   }
   if (rc != SQLITE_ROW) {
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   memset(out_entity, 0, sizeof(*out_entity));
   out_entity->id = sqlite3_column_int64(stmt, 0);
   out_entity->user_id = sqlite3_column_int(stmt, 1);
   const char *name = (const char *)sqlite3_column_text(stmt, 2);
   const char *etype = (const char *)sqlite3_column_text(stmt, 3);
   const char *canon = (const char *)sqlite3_column_text(stmt, 4);
   if (name) {
      safe_strscpy(out_entity->name, name);
   }
   if (etype) {
      safe_strscpy(out_entity->entity_type, etype);
   }
   if (canon) {
      safe_strscpy(out_entity->canonical_name, canon);
   }
   out_entity->mention_count = sqlite3_column_int(stmt, 5);
   out_entity->first_seen = (time_t)sqlite3_column_int64(stmt, 6);
   out_entity->last_seen = (time_t)sqlite3_column_int64(stmt, 7);

   if (out_canonical_id) {
      *out_canonical_id = (sqlite3_column_type(stmt, 8) == SQLITE_NULL)
                              ? 0
                              : sqlite3_column_int64(stmt, 8);
   }
   if (out_is_user_self) {
      *out_is_user_self = sqlite3_column_int(stmt, 9) != 0;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return MEMORY_DB_SUCCESS;
}

/* Look up the user's username for the user_self_bonus signal.  Output is
 * lowercase-canonicalized for substring comparison against canonical_name. */
static int get_user_username_canonical(int user_id, char *out_buf, size_t buf_size) {
   if (!out_buf || buf_size == 0)
      return MEMORY_DB_FAILURE;
   out_buf[0] = '\0';

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, "SELECT username FROM users WHERE id = ?", -1, &stmt, NULL) !=
       SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);

   int rc = sqlite3_step(stmt);
   if (rc == SQLITE_ROW) {
      const char *u = (const char *)sqlite3_column_text(stmt, 0);
      if (u) {
         memory_make_canonical_name(u, out_buf, buf_size);
      }
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   return (out_buf[0] != '\0') ? MEMORY_DB_SUCCESS : MEMORY_DB_NOT_FOUND;
}

/* Find the user-self entity id for @p user_id (rows with is_user_self = 1).
 * Returns id > 0 on SUCCESS; 0 with NOT_FOUND if seeding hasn't run. */
int memory_alias_internal_find_user_self_id(int user_id, int64_t *out_id) {
   if (!out_id)
      return MEMORY_DB_FAILURE;
   *out_id = 0;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT id FROM memory_entities "
                          "WHERE user_id = ? AND is_user_self = 1 LIMIT 1",
                          -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   int rc = sqlite3_step(stmt);
   if (rc == SQLITE_ROW) {
      *out_id = sqlite3_column_int64(stmt, 0);
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   return (*out_id > 0) ? MEMORY_DB_SUCCESS : MEMORY_DB_NOT_FOUND;
}

/* Compute exclusive_relation_overlap signal between two entities.
 *   1.0 if any open exclusive relation shares object_entity_id (or
 *       canonical-equal object_value)
 *   0.5 if any non-exclusive relation shares object
 *   0   otherwise
 * Uses the partial index idx_memory_relations_subject_open. */
float memory_alias_internal_compute_exclusive_relation_overlap(int user_id,
                                                               int64_t a_id,
                                                               int64_t b_id) {
   if (a_id <= 0 || b_id <= 0 || a_id == b_id)
      return 0.0f;

   AUTH_DB_LOCK_OR_RETURN(0.0f);

   /* Single-statement fetch covering both subjects via IN (?, ?) — the
    * Ckpt 5 fold-in for emb-M2.  One prepare + one step loop instead of
    * two bind cycles; the discriminator column (subject_entity_id) routes
    * each row into a_rels[] or b_rels[].
    *
    * LIMIT 32 = 16-per-side × 2 sides.  ORDER BY valid_from DESC so when
    * a subject has more open relations than the per-side cap, we keep the
    * most recently established ones (relation history matters less than
    * "what's true now" for alias detection — fold-in arch-L3). */
   typedef struct {
      char relation[MEMORY_RELATION_MAX];
      int64_t object_entity_id;
      char object_value[MEMORY_ENTITY_NAME_MAX]; /* literal, may be empty */
   } open_rel_t;

   open_rel_t a_rels[16];
   int a_count = 0;
   open_rel_t b_rels[16];
   int b_count = 0;

   const char *sql = "SELECT subject_entity_id, relation, COALESCE(object_entity_id, 0), "
                     "       COALESCE(object_value, '') "
                     "FROM memory_relations "
                     "WHERE user_id = ? AND subject_entity_id IN (?, ?) "
                     "  AND valid_to IS NULL "
                     "ORDER BY COALESCE(valid_from, 0) DESC "
                     "LIMIT 32";
   sqlite3_stmt *stmt = NULL;

   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return 0.0f;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, a_id);
   sqlite3_bind_int64(stmt, 3, b_id);

   while (sqlite3_step(stmt) == SQLITE_ROW) {
      int64_t subj = sqlite3_column_int64(stmt, 0);
      open_rel_t *dest;
      int *dest_count;
      if (subj == a_id) {
         dest = a_rels;
         dest_count = &a_count;
      } else if (subj == b_id) {
         dest = b_rels;
         dest_count = &b_count;
      } else {
         continue; /* defensive — shouldn't happen given the IN filter */
      }
      if (*dest_count >= 16)
         continue;

      const char *rel = (const char *)sqlite3_column_text(stmt, 1);
      if (!rel)
         continue;
      safe_strscpy(dest[*dest_count].relation, rel);
      dest[*dest_count].object_entity_id = sqlite3_column_int64(stmt, 2);
      const char *ov = (const char *)sqlite3_column_text(stmt, 3);
      if (ov) {
         safe_strscpy(dest[*dest_count].object_value, ov);
      } else {
         dest[*dest_count].object_value[0] = '\0';
      }
      (*dest_count)++;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   float best = 0.0f;
   for (int i = 0; i < a_count; i++) {
      for (int j = 0; j < b_count; j++) {
         if (strcmp(a_rels[i].relation, b_rels[j].relation) != 0)
            continue;

         /* Same relation type — now check object equality. */
         bool object_match = false;
         if (a_rels[i].object_entity_id != 0 && b_rels[j].object_entity_id != 0 &&
             a_rels[i].object_entity_id == b_rels[j].object_entity_id) {
            object_match = true;
         } else if (a_rels[i].object_value[0] != '\0' && b_rels[j].object_value[0] != '\0' &&
                    strcasecmp(a_rels[i].object_value, b_rels[j].object_value) == 0) {
            object_match = true;
         }
         if (!object_match)
            continue;

         float weight = memory_db_relation_is_exclusive(a_rels[i].relation) ? 1.0f : 0.5f;
         if (weight > best)
            best = weight;
         if (best >= 1.0f)
            return best;
      }
   }
   return best;
}

/* Compute contact_field_overlap signal: 1.0 if any contact email/phone is
 * shared between the two entities (after normalization), 0 otherwise. */
float memory_alias_internal_compute_contact_field_overlap(int user_id, int64_t a_id, int64_t b_id) {
   if (a_id <= 0 || b_id <= 0 || a_id == b_id)
      return 0.0f;

   AUTH_DB_LOCK_OR_RETURN(0.0f);

   const char *sql = "SELECT COUNT(*) FROM contacts c1 "
                     "JOIN contacts c2 ON c1.field_type = c2.field_type "
                     "                AND lower(c1.value) = lower(c2.value) "
                     "WHERE c1.user_id = ? AND c1.entity_id = ? "
                     "  AND c2.user_id = ? AND c2.entity_id = ? "
                     "  AND c1.field_type IN ('email', 'phone', 'address')";

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return 0.0f;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, a_id);
   sqlite3_bind_int(stmt, 3, user_id);
   sqlite3_bind_int64(stmt, 4, b_id);

   float overlap = 0.0f;
   if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) > 0) {
      overlap = 1.0f;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return overlap;
}

/* Parse identity_aliases (newline-separated, raw user input) into a
 * canonicalized list.  Each non-empty line is trimmed, run through
 * memory_make_canonical_name, deduped case-insensitively against earlier
 * entries, and stored in @p out[i].  Modifies @p raw in place via
 * strtok_r — caller must pass a writable buffer. */
static void parse_canonical_alias_list(char *raw,
                                       char out[][MEMORY_ENTITY_NAME_MAX],
                                       int max,
                                       int *out_count) {
   *out_count = 0;
   if (!raw || !*raw || max <= 0)
      return;

   char *save = NULL;
   for (char *line = strtok_r(raw, "\n", &save); line != NULL && *out_count < max;
        line = strtok_r(NULL, "\n", &save)) {
      while (*line == ' ' || *line == '\t' || *line == '\r')
         line++;
      char *end = line + strlen(line);
      while (end > line && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
         end--;
      *end = '\0';
      if (*line == '\0')
         continue;

      char canon[MEMORY_ENTITY_NAME_MAX];
      memory_make_canonical_name(line, canon, sizeof(canon));
      if (!canon[0])
         continue;

      bool dup = false;
      for (int i = 0; i < *out_count; i++) {
         if (strcmp(out[i], canon) == 0) {
            dup = true;
            break;
         }
      }
      if (dup)
         continue;

      safe_strscpy(out[*out_count], canon);
      (*out_count)++;
   }
}

/* Walk backwards over continuation bytes (0x80-0xBF) and trim if the
 * leader's expected sequence length isn't fully satisfied — prevents
 * downstream tokenizers from seeing a malformed UTF-8 byte at the cutoff
 * when strncpy split a multi-byte character. */
void memory_alias_internal_trim_trailing_partial_utf8(char *buf) {
   if (!buf)
      return;
   size_t len = strlen(buf);
   if (len == 0)
      return;
   size_t i = len;
   while (i > 0 && ((unsigned char)buf[i - 1] & 0xC0) == 0x80) {
      i--;
   }
   if (i == 0)
      return;
   unsigned char lead = (unsigned char)buf[i - 1];
   size_t expected;
   if ((lead & 0x80) == 0) {
      expected = 1; /* ASCII */
   } else if ((lead & 0xE0) == 0xC0) {
      expected = 2;
   } else if ((lead & 0xF0) == 0xE0) {
      expected = 3;
   } else if ((lead & 0xF8) == 0xF0) {
      expected = 4;
   } else {
      buf[i - 1] = '\0'; /* invalid leader — drop it */
      return;
   }
   if ((len - (i - 1)) < expected) {
      buf[i - 1] = '\0'; /* incomplete tail sequence — drop it */
   }
}

/* Determine whether the user_self_bonus applies between two entities.
 * Bonus fires when one side has is_user_self = 1 AND the other side has
 * user-identity signal — any of (cheap-to-expensive):
 *   1. The "other" side is an allow-listed self-reference token (the
 *      "user" canonical entity, flagged at synth_self_allow_list_user
 *      time).  Unconditional; fires regardless of operator config.
 *   2. contact_overlap_fired (email/phone overlap on the contact_t
 *      surface — strong identity signal).
 *   3. The operator's username canonical is a substring of the other
 *      side's canonical_name.  Catches dev=username case directly.
 *   4. One of the operator's seeded identity_aliases (from
 *      users.identity_aliases) is a substring of the other side's
 *      canonical_name.  Catches the realistic Path A case where the
 *      operator's username is generic ("admin") but they've configured
 *      aliases like "Jon" — "Jon" is a substring of "Jon Smith" and
 *      of candidate-name "jon", so the bonus fires for both.
 * Conditions ordered cheapest-first; alias-substring last because it
 * does an extra DB load + parse vs the username path's single load. */
bool memory_alias_internal_user_self_bonus_applies(int user_id,
                                                   bool a_is_user_self,
                                                   bool b_is_user_self,
                                                   const char *a_canonical_name,
                                                   const char *b_canonical_name,
                                                   bool contact_overlap_fired,
                                                   bool other_is_allow_list_token) {
   /* Exactly one side must be user_self.  If neither or both, no bonus. */
   if (a_is_user_self == b_is_user_self)
      return false;

   const char *other_canon = a_is_user_self ? b_canonical_name : a_canonical_name;
   if (!other_canon || !*other_canon)
      return false;

   /* (1) Allow-listed self-reference token (currently just "user"): the
    * Phase 1.5 brief specifies this branch fires regardless of any
    * username/alias substring conditions, so the dev's actual case
    * (username "admin" or "jon" with a "user" entity in the cluster)
    * still receives the bonus. */
   if (other_is_allow_list_token)
      return true;

   /* (2) Email / phone overlap is a strong user-identity signal. */
   if (contact_overlap_fired)
      return true;

   /* (3) Username substring match on canonical_name. */
   char username_canonical[MEMORY_ENTITY_NAME_MAX];
   if (get_user_username_canonical(user_id, username_canonical, sizeof(username_canonical)) ==
           MEMORY_DB_SUCCESS &&
       *username_canonical && strstr(other_canon, username_canonical) != NULL) {
      return true;
   }

   /* (4) Identity-alias substring match.  Loads users.identity_aliases,
    * parses + canonicalizes, checks each non-empty alias as a substring
    * of @p other_canon.  This is the headline Path-A path for clusters
    * like "Jon" / "Jon Smith" / "smithfabrications@example.com" when
    * the operator has configured identity_aliases via WebUI Settings →
    * User → Aliases. */
   auth_user_identity_t identity;
   if (auth_db_get_user_identity(user_id, &identity) == AUTH_DB_SUCCESS &&
       identity.identity_aliases[0] != '\0') {
      memory_alias_internal_trim_trailing_partial_utf8(identity.identity_aliases);
      char aliases[16][MEMORY_ENTITY_NAME_MAX];
      int alias_count = 0;
      parse_canonical_alias_list(identity.identity_aliases, aliases, 16, &alias_count);
      for (int i = 0; i < alias_count; i++) {
         if (aliases[i][0] && strstr(other_canon, aliases[i]) != NULL) {
            return true;
         }
      }
   }

   return false;
}
