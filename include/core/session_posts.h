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
 * Telling a turn what DAWN posted into its conversation from elsewhere (part
 * of the session unit; see session_posts.c).
 */

#ifndef SESSION_POSTS_H
#define SESSION_POSTS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct session;

#define SESSION_POSTS_MAX 5        /* posts one turn tells: the newest (earlier: said left out) */
#define SESSION_POST_TEXT_MAX 2000 /* bytes of one post told */

/**
 * @brief What DAWN posted into the turn's conversation that the session hasn't
 *        told yet, as a piece of the turn's context; NULL when nothing.
 *
 * Posts after the previous question (or what an earlier turn already told),
 * not in this history already, at most SESSION_POSTS_MAX, each cut to
 * SESSION_POST_TEXT_MAX, DAWN's markers defused.  Marks them told by the
 * running turn (taken back with it, session_posts_untell_locked()).  Reads the
 * database with no session lock held.  Call once per turn, before its prompt
 * is built.
 *
 * @param session The session whose turn is starting.
 * @param user_id The turn's user (0, a guest: nothing).
 * @return The text (caller frees), or NULL.
 */
char *session_posts_take(struct session *session, int user_id);

/**
 * @brief Whether the turn @p turn_token was told post @p msg_id of conversation
 *        @p conv_id (session_posts_take).  Takes history_mutex.
 */
bool session_posts_told_by_turn(struct session *session,
                                uint64_t turn_token,
                                int64_t conv_id,
                                int64_t msg_id);

/**
 * @brief Caller holds history_mutex.  The running turn is taken back: what it
 *        told is told again by the next one.
 */
void session_posts_untell_locked(struct session *session);

#ifdef __cplusplus
}
#endif

#endif /* SESSION_POSTS_H */
