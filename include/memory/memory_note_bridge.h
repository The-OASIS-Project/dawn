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
 * Memory→note bridge (Phase 9 of the notes/reference-text store).
 *
 * When a user files reference text as a note, the canonical text lives only in
 * the document store.  The bridge writes a thin "gloss" fact — a self-directing
 * pointer ("the user has a saved note titled X; retrieve it verbatim with
 * document_read") whose `note_doc_id` links it to the note.  The gloss is
 * semantically retrievable, so a fuzzy "what's my bio" surfaces it and steers
 * the model to the exact note, but it is exempt from paraphrase-dedup, prune,
 * decay, and pattern-delete (see the note_doc_id exemptions in the memory DB).
 *
 * The verbatim note body NEVER enters memory_facts — only the gloss sentence.
 *
 * Layer 2.  Save/WebUI callers (Layer 3/4) invoke these AFTER a successful note
 * save/delete, OUTSIDE any DB lock (embedding does I/O).  Both are best-effort
 * from the caller's perspective: a gloss failure must not fail the note op.
 */

#ifndef MEMORY_NOTE_BRIDGE_H
#define MEMORY_NOTE_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create or refresh the bridge gloss for a saved note.
 *
 * Idempotent per note: an existing gloss for @p note_doc_id is replaced
 * (delete + create) so the text and embedding track a renamed label.  No-op
 * (SUCCESS) when memory is disabled.
 *
 * @param user_id Owner user ID
 * @param note_doc_id documents.id of the saved note
 * @param label The note's label (filename); embedded into the gloss text
 * @return SUCCESS or FAILURE (callers treat as best-effort)
 */
int memory_note_bridge_upsert_gloss(int user_id, int64_t note_doc_id, const char *label);

/**
 * @brief memory_note_bridge_upsert_gloss for a note made from outside text
 *        (a research report): the gloss carries @p outside (core/outside_kind.h)
 */
int memory_note_bridge_upsert_gloss_from(int user_id,
                                         int64_t note_doc_id,
                                         const char *label,
                                         int outside);

/**
 * @brief Delete the bridge gloss for a note.
 *
 * Call BEFORE the note row is deleted: the FK `ON DELETE SET NULL` nulls
 * note_doc_id, after which the gloss can no longer be found by it.  No-op
 * (SUCCESS) when no gloss exists or memory is disabled.
 *
 * @param user_id Owner user ID
 * @param note_doc_id documents.id of the note being deleted
 * @return SUCCESS or FAILURE (callers treat as best-effort)
 */
int memory_note_bridge_delete_gloss(int user_id, int64_t note_doc_id);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_NOTE_BRIDGE_H */
