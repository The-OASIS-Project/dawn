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
 * Markers for synthetic user-role messages DAWN injects into a conversation's
 * LLM context on its own behalf (not typed or spoken by the user).
 */

#ifndef AUTOMATED_EVENT_H
#define AUTOMATED_EVENT_H

/* First line of the envelope a finished background job's results are delivered
 * in (job_reinvoke).  It sits in the parent conversation's in-memory history as a
 * user-role message so the model reacts to it, but it is job output, not
 * something the user said, so memory extraction skips it. */
#define AUTOMATED_EVENT_JOB_UPDATE "[automated background-job update]"

#endif /* AUTOMATED_EVENT_H */
