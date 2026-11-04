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
 * The bytes DAWN writes into a conversation's messages around outside text,
 * read back both by the live extraction scan (memory_extraction_input.c) and
 * by SQL over stored rows (CONV_ROW_OUTSIDE_KIND_SQL, the v113 fill).  They
 * are stored, so like outside_kind.h they are append-only: a changed marker
 * stops matching every row written before the change.  Plain text with no
 * single quotes (the SQL quotes them as literals); no includes.
 */

#ifndef OUTSIDE_MARKERS_H
#define OUTSIDE_MARKERS_H

/* Opens a document attached to the user's message. */
#define OUTSIDE_MARKER_DOC_OPEN "[ATTACHED DOCUMENT: "

/* First line of a finished background job's result envelope. */
#define OUTSIDE_MARKER_JOB_UPDATE "[automated background-job update]"

/* Opens a device's relayed data (mosquitto_comms.c). */
#define OUTSIDE_MARKER_DEVICE_DATA "[DEVICE DATA]"

/* The kind word a turn-context item from an outside source carries in its
 * header ("[M3 source outside YYYY-MM-DD] text", or "told, outside"). */
#define OUTSIDE_MARKER_ITEM_KIND "outside"

/* What a memory line adds when its batch read outside text: a fact's kind
 * word ("told, outside"), a relation's or summary's mark ("... (outside)"). */
#define OUTSIDE_MARKER_KIND_SUFFIX ", " OUTSIDE_MARKER_ITEM_KIND
#define OUTSIDE_MARKER_MEMORY_MARK " (" OUTSIDE_MARKER_ITEM_KIND ")"

/* What a memory search's source excerpt header starts with ("[source:
 * conv=N msgs=X-Y]", memory_callback.c); it ends with OUTSIDE_MARKER_MEMORY_MARK
 * when that conversation read outside text, and the stored messages quoted
 * under it are then outside text (memory_extraction_input.c). */
#define OUTSIDE_MARKER_SOURCE_HEAD "[source: conv="

/* The source of a document's chunk in a turn context's item header. */
#define OUTSIDE_MARKER_DOC_SOURCE "document_chunk"

/* What a recalled memory's source in an item header starts with.  Its kind
 * word may say "outside" (where it was learned), but the line is DAWN's own
 * rendering of what it stored under the guard, so it never counts as outside
 * text again.  The live scan skips any source with this prefix; the stored-row
 * scan (CONV_TURN_CONTEXT_FOREIGN_SQL) strips the four named ones, memory_fact,
 * memory_entity, memory_relation and memory_summary, so a new memory source
 * joins that list (test_memory_focus_adapters checks it). */
#define OUTSIDE_MARKER_MEMORY_SOURCE "memory_"

#endif /* OUTSIDE_MARKERS_H */
