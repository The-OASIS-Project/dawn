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
 * The report-back on a finished coding job: posted into the conversation the
 * task came from, as deep research does with its completion take.
 */

#ifndef CODING_AGENT_REPORT_H
#define CODING_AGENT_REPORT_H

#include <json-c/json.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Report coding job @p job's end into the conversation it was started
 *        from.  A job with no such conversation gets nothing here: the job's
 *        notice covers it.
 *
 * On its own thread: a completed task gets a take written in a turn with no
 * tools (the agent's report framed as its words), led by a line of DAWN's;
 * a report the injection filter flags, a failed take and any other end get a
 * fixed line.  Callers report once per job, when they move it to its end.
 *
 * @param job     The coding job (its conversation id).
 * @param user_id Its owner.
 * @param gw_kind The gateway's end: completed, failed or cancelled.
 * @param data    That end's facts and the agent's report (copied; may be NULL).
 */
void coding_report_job_end(int64_t job, int user_id, const char *gw_kind, struct json_object *data);

/** No new report-backs from here on (shutdown); a take being written is cut
 *  short (its fixed line goes instead), and those in flight are waited for. */
void coding_report_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* CODING_AGENT_REPORT_H */
