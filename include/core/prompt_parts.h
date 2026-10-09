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
 * The parts of a turn's prompt, as the prompt builder composes them.  A
 * conversation's request is append-only (so the provider's prompt cache holds
 * and a model's earlier reasoning stays valid): the system prompt is frozen by
 * the conversation's first turn, and everything that changes reaches the
 * model as something appended where it became true.  Leaf header: libc only.
 */

#ifndef CORE_PROMPT_PARTS_H
#define CORE_PROMPT_PARTS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Most sections a system prompt is built from. */
#define PROMPT_SECTIONS_MAX 16
/** Longest section name, with its NUL. */
#define PROMPT_SECTION_NAME_MAX 32
/** A tool set's fingerprint: a SHA-256 in hex, with its NUL (DAWN_SHA256_HEX_LEN). */
#define PROMPT_TOOLS_FP_LEN 65

/** Longest section title, with its NUL. */
#define PROMPT_SECTION_TITLE_MAX 64

/** The name of the block a turn's context is framed as ("--- TURN CONTEXT
 *  (<tag>) ---"): the frame's producer (session_prefix.c) and its readers
 *  (focus_incremental.c) share it. */
#define PROMPT_TURN_CONTEXT_NAME "TURN CONTEXT"
/** How a turn context's head line opens: always its first line, so what
 *  follows it is where the turn's items are declared. */
#define PROMPT_TIME_LINE "[system_time]"

struct focus_panel;

/** Longest focus source name, with its NUL. */
#define PROMPT_FOCUS_SOURCE_LEN 32
/** Longest focus item id, with its NUL. */
#define PROMPT_FOCUS_ITEM_ID_LEN 64
/** An item's date as its line shows it (" YYYY-MM-DD"), with its NUL. */
#define PROMPT_FOCUS_DATE_LEN 16

/**
 * One item retrieved for a turn, ranked.  The turn's seam decides whether it
 * is sent (focus_incremental.h): an item the conversation already shows, as it
 * is now, is named rather than sent again.
 */
typedef struct {
   int handle;                             /**< its [M<handle>], stable per conversation;
                                                0 when it has none (sent unnumbered) */
   char source[PROMPT_FOCUS_SOURCE_LEN];   /**< focus source ("memory_fact") */
   char item_id[PROMPT_FOCUS_ITEM_ID_LEN]; /**< opaque key ("fact:12"); "" when none */
   char date[PROMPT_FOCUS_DATE_LEN];       /**< " YYYY-MM-DD" or "" */
   char *text;  /**< one line, DAWN's markers defused; masked at the seam (owned) */
   float score; /**< the ranker's composite score */
} prompt_focus_item_t;

/** One named part of the system prompt (persona, rules, user context, ...). */
typedef struct {
   char name[PROMPT_SECTION_NAME_MAX];   /**< stable key ("persona") */
   char title[PROMPT_SECTION_TITLE_MAX]; /**< how an update names it to the model */
   char *text;                           /**< owned; never empty */
} prompt_section_t;

/**
 * A turn's prompt, in parts.  Any pointer may be NULL (nothing of that part).
 */
typedef struct {
   /** The system prompt's sections, in order.  A change to one reaches a
    *  running conversation as that section alone, appended. */
   prompt_section_t sections[PROMPT_SECTIONS_MAX];
   int n_sections;
   /** The system prompt a conversation starting now would freeze: the
    *  sections joined (prompt_sections_join).  A conversation already running
    *  keeps its own; a change reaches it as an appended instruction change. */
   char *stable_prefix;
   /** The head of this turn's context: the [system_time] line.  The turn's
    *  context is sent in front of its question: head, the retrieved items
    *  the conversation doesn't show yet, then tail. */
   char *context_head;
   /** The items retrieved for this turn, ranked (owned, n_focus_items long). */
   prompt_focus_item_t *focus_items;
   int n_focus_items;
   /** The tail of this turn's context: per-turn notes (a spoken turn's
    *  transcription hint). */
   char *context_tail;
   /** What the client's context panel shows of the retrieval (the
    *  builder's type), and its free; NULL when retrieval didn't run. */
   struct focus_panel *focus_panel;
   void (*focus_panel_free)(struct focus_panel *panel);
   /** What DAWN knows about the user (preferences, recent conversations), sent
    *  in front of the question when it changed since the conversation last
    *  had it. */
   char *memory_body;
   /** The standing directions of the surface the turn arrived on (voice,
    *  channel, room, headless job, tool availability); "" for none.  Appended
    *  when they differ from what the conversation last had. */
   char *directives;
   /** Every registered tool's neutral definition, a JSON array in registry
    *  order (llm_tools_definitions_hashed): what a conversation starting now freezes,
    *  and what a running one's later changes are compared against.  Decided
    *  by registration, not by enable flags: a tool this surface may not use
    *  is refused when called, and the turn's standing directions say which.
    *  NULL when tools are off. */
   char *tool_defs;
   /** Each of tool_defs' canonical hashes by name, a JSON object in the same
    *  order (llm_tools_definitions_hashed, computed once per registry
    *  generation), and the set's fingerprint (the hash of that object, hex).
    *  NULL / "" when not computed: the seam hashes tool_defs itself. */
   char *tool_def_hashes;
   char tool_defs_fp[PROMPT_TOOLS_FP_LEN];
   /** Each registered tool's schema hash as an older build recorded it
    *  (llm_tools_schema_hashes): converting a conversation frozen by name;
    *  NULL when tools are off. */
   char *tool_schemas;
   /** Whether this turn's request sends a tool change in place (the Claude API
    *  itself, a models.toml [inline_tools] model, the beta not rejected for
    *  it): a change is stored inline, else folded with a boundary. */
   bool inline_tools;
   /** When this was built (unix time), and where withdrawals stood then
    *  (conv_db_withdraw_seq): what the user forgot after it is withdrawn from
    *  the turn's rows as they are saved. */
   int64_t built_at;
   int64_t built_seq;
} composed_prompt_t;

/**
 * @brief Add a section (a copy of @p text, blank lines around it trimmed).
 *        Empty text adds nothing.
 * @param title How an update of the section names it ("who the user is")
 * @return SUCCESS, or FAILURE when full, the name or title is too long, or on
 *         allocation failure
 */
int prompt_sections_add(composed_prompt_t *cp,
                        const char *name,
                        const char *title,
                        const char *text);

/** The sections joined, a blank line apart (caller frees); NULL on
 *  allocation failure or when there are none. */
char *prompt_sections_join(const composed_prompt_t *cp);

/**
 * @brief The head of a turn's context: its PROMPT_TIME_LINE line for @p now
 *        (local time, human-readable and ISO 8601, with the nudge that makes
 *        the model trust it over a `time` tool call)
 *
 * Never empty: when the time can't be formatted the line still opens with
 * PROMPT_TIME_LINE and says so, so a turn context's first line is always its
 * head.
 *
 * @return Heap text ending in a newline (caller frees), or NULL on allocation
 *         failure
 */
char *prompt_turn_head(time_t now);

/** The pieces of a block DAWN frames, at most. */
#define PROMPT_FRAMED_PIECES 5

/**
 * @brief A block DAWN frames: its @p pieces (NULL or empty for none), in
 *        order, each ending in a newline, between its open and close lines
 *        ("--- <name> (<tag>) ---", "--- END <name> (<tag>) ---"; no
 *        parentheses with no @p tag)
 * @return Heap text (caller frees), or NULL on allocation failure
 */
char *prompt_framed(const char *name,
                    const char *tag,
                    const char *const pieces[PROMPT_FRAMED_PIECES]);

/**
 * @brief Someone else's text (an email, a web page) in its frame @p name
 *        ("EMAIL CONTENT", "WEB CONTENT"), with the line saying it is data
 *        and not an instruction, then @p body
 *
 * @p body must already be neutralized (llm_context_neutralize), so nothing in
 * it can end the frame.  @p tag may be NULL (no conversation tag yet).
 *
 * @return Heap text (caller frees), or NULL on allocation failure
 */
char *prompt_third_party(const char *name, const char *tag, const char *body);

/**
 * @brief Whether @p text holds a third-party frame's open line (as
 *        prompt_third_party writes it): text DAWN framed as someone else's.
 *        An imitation in the text itself is defused by the neutralizer, so a
 *        line found is DAWN's own.
 */
bool prompt_has_third_party(const char *text);

/** Free every part and zero them (the struct is then safe to reuse). NULL-safe. */
static inline void composed_prompt_free(composed_prompt_t *p) {
   if (p == NULL) {
      return;
   }
   for (int i = 0; i < p->n_sections && i < PROMPT_SECTIONS_MAX; i++) {
      free(p->sections[i].text);
      p->sections[i].text = NULL;
      p->sections[i].name[0] = '\0';
      p->sections[i].title[0] = '\0';
   }
   p->n_sections = 0;
   free(p->stable_prefix);
   free(p->context_head);
   free(p->context_tail);
   for (int i = 0; p->focus_items != NULL && i < p->n_focus_items; i++) {
      free(p->focus_items[i].text);
   }
   free(p->focus_items);
   if (p->focus_panel != NULL && p->focus_panel_free != NULL) {
      p->focus_panel_free(p->focus_panel);
   }
   p->focus_panel = NULL;
   p->focus_panel_free = NULL;
   p->focus_items = NULL;
   p->n_focus_items = 0;
   p->context_head = NULL;
   p->context_tail = NULL;
   free(p->memory_body);
   free(p->directives);
   free(p->tool_defs);
   free(p->tool_schemas);
   free(p->tool_def_hashes);
   p->tool_def_hashes = NULL;
   p->tool_defs_fp[0] = '\0';
   p->tool_defs = NULL;
   p->tool_schemas = NULL;
   p->inline_tools = false;
   p->stable_prefix = NULL;
   p->memory_body = NULL;
   p->directives = NULL;
}

#ifdef __cplusplus
}
#endif

#endif /* CORE_PROMPT_PARTS_H */
