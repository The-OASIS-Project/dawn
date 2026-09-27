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
 * Bulk fact import: filtering, de-duplication, creation and embedding.
 */

#include "memory/memory_import.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "core/memory_filter.h"
#include "memory/memory_db.h"
#include "memory/memory_embed_backfill.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_similarity.h"
#include "utils/string_utils.h"

/* Closest existing facts checked by find_duplicate_fact(). */
#define IMPORT_DUP_HASH_MATCHES 1
#define IMPORT_DUP_SIMILAR_CANDIDATES 3

/* Exact (normalized-hash) match, then a similarity check against the closest
 * existing facts.  The matching fact's id, or 0. */
static int64_t find_duplicate_fact(int user_id, const char *fact_text) {
   uint32_t hash = memory_normalize_and_hash(fact_text);
   if (hash == 0)
      return 0;

   memory_fact_t existing[IMPORT_DUP_HASH_MATCHES];
   int found = 0;
   memory_db_fact_find_by_hash(user_id, hash, existing, IMPORT_DUP_HASH_MATCHES, &found);
   if (found > 0)
      return existing[0].id;

   memory_fact_t similar[IMPORT_DUP_SIMILAR_CANDIDATES];
   int sim_count = 0;
   memory_db_fact_find_similar(user_id, fact_text, similar, IMPORT_DUP_SIMILAR_CANDIDATES,
                               &sim_count);
   for (int i = 0; i < sim_count; i++) {
      if (memory_is_duplicate(fact_text, similar[i].fact_text, MEMORY_SIMILARITY_THRESHOLD))
         return similar[i].id;
   }
   return 0;
}

float memory_import_confidence(double raw) {
   if (!isfinite(raw)) {
      return MEMORY_IMPORT_DEFAULT_CONFIDENCE;
   }
   return raw < 0.0 ? 0.0f : (raw > 1.0 ? 1.0f : (float)raw);
}

void memory_import_clean_text(char *s) {
   utf8_trim_incomplete(s);
   sanitize_utf8_for_json(s);
}

memory_import_status_t memory_import_fact(int user_id,
                                          const char *text,
                                          float confidence,
                                          bool commit,
                                          char out_text[MEMORY_FACT_TEXT_MAX]) {
   out_text[0] = '\0';
   if (!text || !text[0]) {
      return MEMORY_IMPORT_EMPTY;
   }
   safe_strncpy(out_text, text, MEMORY_FACT_TEXT_MAX);
   memory_import_clean_text(out_text);
   confidence = memory_import_confidence(confidence);

   if (memory_filter_check(out_text)) {
      return MEMORY_IMPORT_BLOCKED;
   }
   const int64_t duplicate = find_duplicate_fact(user_id, out_text);
   if (duplicate > 0) {
      if (commit) {
         /* Imported: the user stated it, whichever conversation also did. */
         memory_db_fact_mark_unsourced(duplicate, user_id);
      }
      return MEMORY_IMPORT_DUPLICATE;
   }
   if (!commit) {
      return MEMORY_IMPORT_ACCEPTED;
   }

   if (memory_db_fact_create(user_id, out_text, confidence, "import", NULL, NULL, NULL) !=
       MEMORY_DB_SUCCESS) {
      return MEMORY_IMPORT_FAILED;
   }
   /* Created without an embedding (embedding hundreds inline would stall the
    * caller); the backfill worker embeds it.  Requests are de-duplicated per
    * user, so one per fact costs a mutex, not a pass. */
   memory_embeddings_request_backfill(user_id);
   return MEMORY_IMPORT_ACCEPTED;
}
