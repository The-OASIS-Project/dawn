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
   /** This turn's context: [system_time], retrieved items, and per-turn notes,
    *  sent in front of the question. */
   char *volatile_block;
   /** What DAWN knows about the user (preferences, recent conversations), sent
    *  in front of the question when it changed since the conversation last
    *  had it. */
   char *memory_body;
   /** The standing directions of the surface the turn arrived on (voice,
    *  channel, room, headless job, tool availability); "" for none.  Appended
    *  when they differ from what the conversation last had. */
   char *directives;
   /** Every registered tool's neutral definition, a JSON array in registry
    *  order (llm_tools_definitions): what a conversation starting now freezes,
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
   free(p->volatile_block);
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
   p->volatile_block = NULL;
   p->memory_body = NULL;
   p->directives = NULL;
}

#ifdef __cplusplus
}
#endif

#endif /* CORE_PROMPT_PARTS_H */
