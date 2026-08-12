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
 * Deep-research P0 budget defaults — the SINGLE source of truth.
 *
 * A pure-constant leaf header (no includes, no code) so BOTH the deterministic
 * core (research_budgets_defaults() in research_run.c) AND the config-default
 * initializer (config_defaults.c, a Layer-0 file that must not pull in the full
 * research_run.h → auth_db.h chain) reference one place.  research_budgets_load()
 * overlays the [research] config over these at run start, and config_defaults.c
 * seeds the config from these same constants — so a bump here propagates to both
 * the compile-time fallback and the runtime default with no drift.
 */

#ifndef RESEARCH_DEFAULTS_H
#define RESEARCH_DEFAULTS_H

#define RESEARCH_DEFAULT_MAX_ROUNDS 6
#define RESEARCH_DEFAULT_MAX_TOOL_CALLS 40
#define RESEARCH_DEFAULT_MAX_INPUT_TOKENS 200000
#define RESEARCH_DEFAULT_MIN_SOURCES 2
#define RESEARCH_DEFAULT_ROUND_DIGEST_MAX_CHARS 6000
#define RESEARCH_DEFAULT_TOP_K_QUESTIONS 8

#endif /* RESEARCH_DEFAULTS_H */
