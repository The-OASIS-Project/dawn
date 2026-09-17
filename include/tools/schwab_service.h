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
 * Charles Schwab service layer — quotes + portfolio orchestration/formatting.
 */

#ifndef SCHWAB_SERVICE_H
#define SCHWAB_SERVICE_H

#include <stdbool.h>

/**
 * Build an LLM-facing quote summary for one or more comma/space-separated
 * symbols. Returns an owned string (caller frees); failure paths are prefixed
 * with TOOL_RESULT_ERROR_MARK. @p user_id owns the Schwab OAuth link.
 */
char *schwab_service_quote(int user_id, const char *symbols_csv);

/**
 * Build an LLM-facing portfolio summary across all of the user's Schwab
 * accounts (positions + balances + combined total). When @p accounts_only is
 * true, balances only (no per-position detail). Returns an owned string;
 * failures prefixed with TOOL_RESULT_ERROR_MARK.
 */
char *schwab_service_portfolio(int user_id, bool accounts_only);

#endif /* SCHWAB_SERVICE_H */
