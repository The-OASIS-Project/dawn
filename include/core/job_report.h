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
 * A finished background job reporting back into the conversation it came
 * from: the take written in a turn with no tools, and the post.  Shared by
 * deep research and coding jobs.
 */

#ifndef JOB_REPORT_H
#define JOB_REPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct session;

/** The effective persona, trailing blank lines and spaces trimmed (a report
 *  prompt adds its own break after it). */
void job_report_persona(char *out, size_t out_len);

/**
 * @brief The take: one turn on @p s with tools suppressed, so what it reads
 *        (often someone else's text) can shape words, never actions.
 *
 * @param system_prompt The persona and the report-back instructions.
 * @param ask           What to report on; outside text in it framed by the caller.
 * @return The take (caller frees; cleaned like any reply), or NULL on failure,
 *         an empty reply, or a turn cancelled while it ran (no partial take).
 */
char *job_report_take(struct session *s, const char *system_prompt, const char *ask, int user_id);

/**
 * @brief Post @p text into conversation @p parent_conv as DAWN's own message:
 *        an unprompted assistant row (shown and replayed, never extracted),
 *        and tell its open tabs.
 * @return SUCCESS or FAILURE.
 */
int job_report_post(int64_t parent_conv, int user_id, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* JOB_REPORT_H */
