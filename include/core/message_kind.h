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
 * Message kinds: rows of a conversation that are request context rather than
 * messages someone sees.  A model reads them, in place, on every later
 * request; no client, search, export or memory extraction does.  They are
 * appended and never edited, so a request's prefix stays the same turn after
 * turn and after a reload.
 *
 * On a history message the kind is the internal key MESSAGE_KIND_KEY (never
 * sent on the wire); in the database it is messages.kind (NULL = an ordinary
 * message).  Dependency-free: shared by the database, session and LLM layers.
 */

#ifndef MESSAGE_KIND_H
#define MESSAGE_KIND_H

#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/** History message key carrying a message's kind (absent = ordinary). */
#define MESSAGE_KIND_KEY "_kind"

typedef enum {
   MESSAGE_KIND_NONE = 0,     /**< an ordinary message */
   MESSAGE_KIND_TURN_CONTEXT, /**< a turn's context: time, retrievals, notices (user) */
   MESSAGE_KIND_MEMORY,       /**< what DAWN knows about the user (user) */
   MESSAGE_KIND_ENVELOPE,     /**< input DAWN wrote for a turn it started itself (user) */
   MESSAGE_KIND_LOOP_NOTE,    /**< a tool-loop hint or closing message (user/assistant) */
   MESSAGE_KIND_DIRECTIVE,    /**< the surface's standing directions (system) */
   MESSAGE_KIND_INSTRUCTION,  /**< a change to the frozen instructions (system) */
   MESSAGE_KIND_PREFIX,       /**< the frozen system prompt; in memory only, never a row */
   MESSAGE_KIND_SUMMARY,      /**< a compaction's summary: a part of the first kept question;
                                   in memory only (the conversation holds it), never a row */
} message_kind_t;

/** The kind's stored name (NULL for MESSAGE_KIND_NONE). */
static inline const char *message_kind_name(message_kind_t kind) {
   switch (kind) {
      case MESSAGE_KIND_TURN_CONTEXT:
         return "turn_context";
      case MESSAGE_KIND_MEMORY:
         return "memory";
      case MESSAGE_KIND_ENVELOPE:
         return "envelope";
      case MESSAGE_KIND_LOOP_NOTE:
         return "loop_note";
      case MESSAGE_KIND_DIRECTIVE:
         return "directive";
      case MESSAGE_KIND_INSTRUCTION:
         return "instruction";
      case MESSAGE_KIND_PREFIX:
         return "prefix";
      case MESSAGE_KIND_SUMMARY:
         return "summary";
      case MESSAGE_KIND_NONE:
      default:
         return NULL;
   }
}

/** The kind a stored name names; MESSAGE_KIND_NONE for NULL, "" or unknown. */
static inline message_kind_t message_kind_parse(const char *name) {
   if (!name || !*name) {
      return MESSAGE_KIND_NONE;
   }
   for (int k = MESSAGE_KIND_TURN_CONTEXT; k <= MESSAGE_KIND_SUMMARY; k++) {
      if (strcmp(name, message_kind_name((message_kind_t)k)) == 0) {
         return (message_kind_t)k;
      }
   }
   return MESSAGE_KIND_NONE;
}

/** Whether @p kind is held by the conversation, never saved as a row. */
static inline bool message_kind_in_memory_only(message_kind_t kind) {
   return kind == MESSAGE_KIND_PREFIX || kind == MESSAGE_KIND_SUMMARY;
}

/** Whether a row of @p kind may have @p role (mirrors the schema's CHECK). */
static inline bool message_kind_role_ok(message_kind_t kind, const char *role) {
   if (!role) {
      return false;
   }
   switch (kind) {
      case MESSAGE_KIND_NONE:
         return true;
      case MESSAGE_KIND_TURN_CONTEXT:
      case MESSAGE_KIND_MEMORY:
      case MESSAGE_KIND_ENVELOPE:
         return strcmp(role, "user") == 0;
      case MESSAGE_KIND_LOOP_NOTE:
         return strcmp(role, "user") == 0 || strcmp(role, "assistant") == 0;
      case MESSAGE_KIND_DIRECTIVE:
      case MESSAGE_KIND_INSTRUCTION:
         return strcmp(role, "system") == 0;
      case MESSAGE_KIND_PREFIX:
      case MESSAGE_KIND_SUMMARY:
      default:
         return false; /* in memory only: the conversation holds these, never a row */
   }
}

#ifdef __cplusplus
}
#endif

#endif /* MESSAGE_KIND_H */
