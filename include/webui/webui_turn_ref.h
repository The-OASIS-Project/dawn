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
 * The client_ref of the text turn a thread is handling: a client tags a
 * `text` frame with one, and that turn's errors and user echo carry it back.
 */

#ifndef WEBUI_TURN_REF_H
#define WEBUI_TURN_REF_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Longest client_ref a text turn may carry (printable ASCII; echoed unchanged). */
#define WEBUI_CLIENT_REF_MAX 64

/**
 * @brief The client_ref of the text turn this thread is handling
 *
 * A client may tag a `text` frame with an opaque client_ref; every error and
 * the user transcript echo for that turn carry it back, so the client knows
 * which of its turns a refusal belongs to.  The ref is per thread: the lws
 * thread holds it while it handles the frame, the turn's worker while the turn
 * runs, and every error or transcript frame built on that thread meanwhile
 * takes it (a queued frame copies it when built).  Set it only around one
 * turn's handling and clear it after.
 *
 * @param ref The ref, or NULL / "" to clear.  Longer than WEBUI_CLIENT_REF_MAX
 *            is truncated (callers validate first).
 */
void webui_turn_ref_set(const char *ref);

/** The calling thread's turn ref, or NULL when it holds none. */
const char *webui_turn_ref_get(void);

/**
 * @brief Whether @p ref is a client_ref a text turn may carry: 1 to
 *        WEBUI_CLIENT_REF_MAX printable ASCII characters (0x20-0x7e).
 */
bool webui_client_ref_valid(const char *ref);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_TURN_REF_H */
