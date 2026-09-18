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
 * STAT network-telemetry renderer — pure formatting of a stat_snapshot_t's
 * network fields into an LLM-facing string. Split from stat_tool.c so the
 * interpretation rules (min-metric primary path, kind-based cellular detection,
 * up/carrier link state, fail_streak>=2 = down, cellular-bearer honesty) are
 * unit-testable without the tool-registry/config plumbing.
 */

#ifndef STAT_RENDER_H
#define STAT_RENDER_H

#include <stddef.h>

#include "core/stat_service.h" /* stat_snapshot_t + net structs */

/**
 * @brief Append an LLM-facing network summary to @p buf (bounded by @p sz).
 *
 * Appends (does not overwrite) using the strlen(buf) offset, so callers can
 * compose it after other status text. snprintf-bounded: truncates gracefully,
 * never overflows. When @p s has no network telemetry it appends a short notice.
 */
void stat_render_network(const stat_snapshot_t *s, char *buf, size_t sz);

#endif /* STAT_RENDER_H */
