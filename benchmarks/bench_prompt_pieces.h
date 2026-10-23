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
 * Splits a DAWN_EXTRACTION_PROMPT_FILE template into the literal pieces the
 * extraction joins with its four values, so the file is never a format string.
 */

#ifndef BENCH_PROMPT_PIECES_H
#define BENCH_PROMPT_PIECES_H

#include "memory/memory_extraction.h"

/**
 * Split @p text in place at each %s, turning each %% into %, the way printf
 * reads the compiled template.  @p pieces then point into @p text.
 *
 * @param text   The template's text (rewritten; garbage when refused)
 * @param pieces Out: the text before, between and after the four %s
 * @return 0 when @p text has exactly four %s and no other conversion, 1 otherwise
 */
int bench_prompt_split(char *text, const char *pieces[MEMORY_EXTRACTION_TEMPLATE_PIECES]);

#endif /* BENCH_PROMPT_PIECES_H */
