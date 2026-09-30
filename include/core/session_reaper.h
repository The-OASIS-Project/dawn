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
 * The session reaper: finishes destroying sessions session_destroy() has
 * ended.  One thread: it joins a session's compaction worker, waits (without
 * blocking on any one session) for its last reference, then hands it to
 * session_manager_finalize().  Part of the session unit (session_manager.c).
 */

#ifndef SESSION_REAPER_H
#define SESSION_REAPER_H

#include <stdbool.h>

#include "core/session_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A destroyed session's last reference is waited for this long; a hold past
 * it means a wedged worker, and the session is leaked (recoverable) rather
 * than freed under that worker (a use-after-free). */
#define SESSION_DESTROY_REF_WAIT_MAX_SEC 30

/* How often the reaper looks at sessions still waiting for their last
 * reference (a release to zero also wakes it at once). */
#define SESSION_REAPER_POLL_MS 250

/* Shutdown waits this long for destroyed sessions to finish while the
 * database is still open. */
#define SESSION_REAPER_SHUTDOWN_DRAIN_MS 3000

/**
 * @brief Start the reaper thread (session_manager_init, which fails without
 *        it: a destroy would otherwise wait on its caller's thread).
 * @return 0, or 1 if the thread couldn't start.
 */
int session_reaper_start(void);

/**
 * @brief Hand an ended session to the reaper.  The session is already out of
 *        the active list; the reaper owns it from here.  Without a running
 *        reaper (not started, or stopped) it is finished on the caller's
 *        thread, waiting as session_destroy() used to.
 */
void session_reaper_enqueue(session_t *session);

/** @brief Wake the reaper (a destroyed session's last reference was released). */
void session_reaper_wake(void);

/**
 * @brief Wait up to @p timeout_ms for every ended session to be finished.
 *        Shutdown calls it before closing the database, so final metrics and
 *        memory extraction still reach it.
 * @return true if none is left.
 */
bool session_reaper_drain(int timeout_ms);

/**
 * @brief Stop and join the reaper, then finish what it holds: a session
 *        nothing references is finalized, one still referenced is leaked with
 *        an error (shutdown; session_manager_cleanup).
 */
void session_reaper_stop(void);

/**
 * @brief Finish nothing more until session_reaper_stop() (shutdown, once the
 *        drain is done): the database and memory subsystems close next, and
 *        a finish racing that teardown could reach them half-closed.  Ended
 *        sessions keep collecting; stop finishes them without the database.
 */
void session_reaper_hold(void);

/** @brief Sessions ended and not yet finished (tests, diagnostics). */
int session_reaper_pending(void);

/**
 * @brief Finish an ended session nothing references any more: final metrics,
 *        memory extraction, queued WebUI frames purged, freed.
 *        Defined in session_manager.c.
 */
void session_manager_finalize(session_t *session);

#ifdef __cplusplus
}
#endif

#endif /* SESSION_REAPER_H */
