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
 * Memory→note bridge — see include/memory/memory_note_bridge.h for the
 * rationale.  Writes/refreshes/deletes a single self-directing "gloss" fact per
 * note, linked by note_doc_id.  The verbatim note body never enters
 * memory_facts; only the gloss sentence does.
 */

#include "memory/memory_note_bridge.h"

#include <stdio.h>
#include <string.h>

#include "config/dawn_config.h"
#include "core/memory_filter.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_types.h"

/* Longest label echoed into the gloss text.  The full text must fit in
 * MEMORY_FACT_TEXT_MAX (512); the template uses the label twice, so cap it
 * well under half the budget. */
#define GLOSS_LABEL_MAX 120

/* Glosses are pinned high-confidence so they rank well in retrieval and (in
 * concert with the decay exemption) never fade — the gloss must reliably win
 * over any stale fact about the same topic.  Load-bearing, not arbitrary. */
#define MEMORY_NOTE_GLOSS_CONFIDENCE 1.0f

/* Build the self-directing gloss sentence.  It names the note (twice — once for
 * readability, once as the document_read argument) and instructs verbatim
 * retrieval, so EVERY surface that surfaces the fact (memory.search + per-turn
 * focus injection) carries the directive.  Control chars and single quotes in
 * the label are sanitized so they can't break the sentence. */
static void build_gloss_text(char *buf, size_t buflen, const char *label) {
   char clean[GLOSS_LABEL_MAX + 1];
   size_t j = 0;
   for (size_t i = 0; label && label[i] != '\0' && j < GLOSS_LABEL_MAX; i++) {
      unsigned char c = (unsigned char)label[i];
      clean[j++] = (c < 0x20 || c == '\'') ? ' ' : (char)c;
   }
   clean[j] = '\0';

   snprintf(buf, buflen,
            "The user has a saved reference note titled '%s'. Its full text is filed in the "
            "document store — retrieve it verbatim with the document_read tool (name: '%s') before "
            "quoting or summarizing it; do not rely on memory for the exact wording.",
            clean, clean);
}

int memory_note_bridge_upsert_gloss(int user_id, int64_t note_doc_id, const char *label) {
   if (user_id <= 0 || note_doc_id <= 0 || !label || !label[0])
      return FAILURE;
   if (!g_config.memory.enabled)
      return SUCCESS; /* memory off — no facts at all, nothing to bridge */

   /* The gloss text is built from the user-controlled label and is later surfaced
    * into the LLM context (memory search + focus injection).  The bridge writes
    * the fact directly, bypassing the extraction-time injection filter, so gate
    * the label here: a label that trips the blocklist must not become an
    * imperative sentence in the prompt.  The note itself still saved fine. */
   if (memory_filter_check(label)) {
      OLOG_WARNING("memory_note_bridge: refusing gloss for note %lld — label tripped the injection "
                   "filter",
                   (long long)note_doc_id);
      return FAILURE;
   }

   char text[MEMORY_FACT_TEXT_MAX];
   build_gloss_text(text, sizeof(text), label);

   /* Replace any existing gloss for this note (rename/overwrite → fresh text +
    * embedding).  Delete + create, because fact_text is immutable for FTS5
    * contentless maintenance and the embedding must be recomputed. */
   int64_t existing = 0;
   memory_db_fact_find_by_note_doc_id(user_id, note_doc_id, &existing);
   if (existing > 0)
      memory_db_fact_delete(existing, user_id);

   int64_t fact_id = 0;
   if (memory_db_fact_create(user_id, text, MEMORY_NOTE_GLOSS_CONFIDENCE, "explicit", NULL,
                             &fact_id) != MEMORY_DB_SUCCESS ||
       fact_id <= 0) {
      OLOG_WARNING("memory_note_bridge: failed to create gloss for note %lld",
                   (long long)note_doc_id);
      return FAILURE;
   }

   /* Link first, then embed: embed_and_store marks the embedding cache dirty, so
    * note_doc_id must already be on the row when the cache next reloads (the
    * gloss flag is read from there).  If the link is refused (the setter's
    * ownership guard rejects a cross-user note_doc_id), don't leave an unlinked
    * orphan gloss behind — delete it and fail. */
   if (memory_db_fact_set_note_doc_id(fact_id, user_id, note_doc_id) != MEMORY_DB_SUCCESS) {
      OLOG_WARNING(
          "memory_note_bridge: note_doc_id link refused for note %lld — rolling back gloss",
          (long long)note_doc_id);
      memory_db_fact_delete(fact_id, user_id);
      return FAILURE;
   }
   memory_embeddings_embed_and_store(user_id, fact_id, text); /* best-effort */

   OLOG_INFO("memory_note_bridge: gloss fact %lld → note %lld ('%s')", (long long)fact_id,
             (long long)note_doc_id, label);
   return SUCCESS;
}

int memory_note_bridge_delete_gloss(int user_id, int64_t note_doc_id) {
   if (user_id <= 0 || note_doc_id <= 0)
      return FAILURE;
   if (!g_config.memory.enabled)
      return SUCCESS;

   int64_t existing = 0;
   memory_db_fact_find_by_note_doc_id(user_id, note_doc_id, &existing);
   if (existing <= 0)
      return SUCCESS; /* no gloss to remove */

   if (memory_db_fact_delete(existing, user_id) != MEMORY_DB_SUCCESS) {
      OLOG_WARNING("memory_note_bridge: failed to delete gloss %lld for note %lld",
                   (long long)existing, (long long)note_doc_id);
      return FAILURE;
   }
   return SUCCESS;
}
