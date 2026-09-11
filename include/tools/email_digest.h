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
 * Email daily-briefing digest — aggregates recent inbox mail across every
 * enabled account into one categorized, briefing-ready summary.  Read-only;
 * built for scheduled briefings but usable interactively.  See email_digest.c.
 */

#ifndef EMAIL_DIGEST_H
#define EMAIL_DIGEST_H

#include <stdbool.h>

/* Options for a digest run.  window_seconds is a true rolling window (rows newer
 * than now - window_seconds are kept); max caps the rendered rows. */
typedef struct {
   int window_seconds;
   int max;
   bool unread_only;
} email_digest_opts_t;

/* Default rolling window (24h) and the ceiling the tool clamps to (7d). */
#define EMAIL_DIGEST_DEFAULT_WINDOW_SEC (24 * 3600)
#define EMAIL_DIGEST_MAX_WINDOW_SEC (7 * 24 * 3600)
#define EMAIL_DIGEST_DEFAULT_MAX 50
/* Hard ceiling on rendered rows — enforced inside email_digest_build so every
 * caller (tool or direct) inherits it. */
#define EMAIL_DIGEST_MAX_ROWS 200

/**
 * @brief Build a categorized digest of recent inbox mail across all enabled
 *        accounts for @p user_id.
 *
 * Iterates enabled accounts through the email_service layer (so each row carries
 * its account label + address), keeps messages inside the rolling window, merges
 * and date-sorts across accounts, and renders sections (Important / Primary /
 * Other) with display-only E-NN labels plus the real [ID] for follow-up actions.
 * Reply status is left UNKNOWN here (filled by the Phase-2b enrichment pass).
 *
 * @return A heap-allocated tool-result string the caller must free().  Never
 *         NULL; on error returns a TOOL_RESULT_ERROR_MARK-prefixed message.
 */
char *email_digest_build(int user_id, const email_digest_opts_t *opts);

#endif /* EMAIL_DIGEST_H */
