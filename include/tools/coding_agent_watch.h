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
 * The coding-agent watcher: one thread that follows the agent gateway's
 * events (task_events, a long poll) and carries them onto DAWN's coding jobs:
 * the agent's steps into the job's events, "needs you" to the user, and the
 * task's end to the job's.
 */

#ifndef CODING_AGENT_WATCH_H
#define CODING_AGENT_WATCH_H

#include <json-c/json.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the watcher for the declared gateway (none: nothing).
 *
 * First reads the events missed since the saved cursor, then checks DAWN's
 * coding jobs still open against the gateway, then follows its events.
 * @return SUCCESS or FAILURE (the thread didn't start).
 */
int coding_watch_start(void);

/** Stop the watcher and wait for it (its long poll is cut short). */
void coding_watch_stop(void);

/**
 * @brief Carry one gateway event onto its job.  The watcher's step, exposed
 *        for tests.
 *
 * Checked against the job row: a coding job whose owner is the event's user.
 * Steps, requests and ends become CONV_EVENT_AGENT_* events; an end ends the
 * job (once).  An event for a job DAWN has ended, or no longer has, asks the
 * gateway to stop the task.
 */
void coding_watch_apply(struct json_object *event);

#ifdef __cplusplus
}
#endif

#endif /* CODING_AGENT_WATCH_H */
