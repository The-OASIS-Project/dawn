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
 * Memory citation — internal parse/format helpers shared by memory_citation.c
 * and its unit test.  The pure (no-I/O) half of the capture path lives in
 * memory_citation_parse.c so the <cited> tokenizer is directly testable.
 */
#ifndef MEMORY_CITATION_INTERNAL_H
#define MEMORY_CITATION_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "core/session_manager.h" /* citation_stash_t, tool_cited_set_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Append @p s to a comma-separated CSV buffer, bounded (never overflows). */
void memory_citation_csv_append(char *buf, size_t bufsz, size_t *len, const char *s);

/**
 * @brief Parse every <cited>…</cited> tag out of @p text and resolve the citations.
 *
 * Pure (no I/O) — the unit-testable core of memory_citation_capture().  Two token
 * shapes are recognized inside a tag: `M<n>` / bare `<n>` (a focus ordinal,
 * validated against @p stash) and `ID:<n>` (a tool fact id, validated against
 * @p tool_set).  Fills:
 *   - @p cited_all   : CSV of every distinct cited canonical id (focus + tool) — audit.
 *   - @p cited_focus : CSV of the focus-cited subset only — Aurora broadcast.
 * and the four counts (any out-param may be NULL).  Cross-provenance de-duplicated
 * on the resolved id; a duplicate focus ordinal counts as @p out_dropped, an
 * `ID:` not in @p tool_set counts as @p out_dropped_tool.
 */
void memory_citation_resolve_cited(const char *text,
                                   const citation_stash_t *stash,
                                   const tool_cited_set_t *tool_set,
                                   char *cited_all,
                                   size_t cited_all_sz,
                                   char *cited_focus,
                                   size_t cited_focus_sz,
                                   int *out_focus_count,
                                   int *out_tool_count,
                                   int *out_dropped,
                                   int *out_dropped_tool);

/**
 * @brief Extract the distinct FACT ids from a resolved cited_all CSV.
 *
 * Pure (no I/O), unit-testable.  @p cited_all is the canonical-id CSV produced by
 * memory_citation_resolve_cited ("fact:5889,summary:2566,relation:41,...").  Only
 * "fact:<n>" tokens are emitted (Phase 2 reinforcement applies to
 * memory_facts.confidence only); summary/relation tokens are ignored.  Writes up
 * to @p max ids into @p out_ids and returns the count written.  The input is
 * already de-duplicated by the resolver, so no extra dedup is performed here.
 */
int memory_citation_extract_fact_ids(const char *cited_all, int64_t *out_ids, int max);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_CITATION_INTERNAL_H */
