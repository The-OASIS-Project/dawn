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
 * Undo tokens for trash and archive: what a move did, kept for a short while
 * under a random token only its user (and account) can redeem, once.
 */

#ifndef EMAIL_UNDO_H
#define EMAIL_UNDO_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "tools/email_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A token: 128 random bits in hex. */
#define EMAIL_UNDO_TOKEN_LEN 32
/** How long a move can be undone. */
#define EMAIL_UNDO_TTL_SEC 60
/** Tokens one user holds (twice the biggest move); their oldest goes first. */
#define EMAIL_UNDO_PER_USER 128
/** Tokens held in all; when full, a move gets no token (no user evicts another). */
#define EMAIL_UNDO_GLOBAL 512

/** The longest list of Gmail labels an undo adds back, packed "A,B,C" with its NUL. */
#define EMAIL_UNDO_LABELS_MAX 384

/** What one move did, enough to move it back. */
typedef struct {
   int64_t account_id;
   uint64_t fingerprint; /* the account's server and login when it moved */
   email_move_kind_t kind;
   bool imap;
   /* IMAP: where it came from and where it is now */
   char src_folder[256];
   char dest_folder[256];
   uint32_t dest_uid;
   uint32_t dest_uidvalidity;
   /* The id it had before the move (Gmail: its id, unchanged by a move) */
   char message_id[192];
   /* Gmail trash: the labels to add back */
   char labels[EMAIL_UNDO_LABELS_MAX];
} email_undo_rec_t;

/**
 * @brief Keep @p rec for @p user_id and hand out its token
 * @param token Out: the token (EMAIL_UNDO_TOKEN_LEN hex digits)
 * @return false when the store is full (the move just has no undo)
 */
bool email_undo_put(int user_id, const email_undo_rec_t *rec, char token[EMAIL_UNDO_TOKEN_LEN + 1]);

/**
 * @brief Take @p token for an undo: it is then in flight (not evicted, and
 *        kept past its window) until email_undo_finish or email_undo_release,
 *        or for at most twice the window if neither ever comes
 * @return true with @p out filled; false for a token that is unknown,
 *         expired, another user's or account's, or already in flight
 *         (EMAIL_ERR_UNDO_EXPIRED for all of them alike)
 */
bool email_undo_claim(int user_id, int64_t account_id, const char *token, email_undo_rec_t *out);

/** The undo ran (or can never run): forget @p token. */
void email_undo_finish(int user_id, const char *token);

/** The undo couldn't run this time (busy, network): @p token may be tried again. */
void email_undo_release(int user_id, const char *token);

/** Tests: the store's clock in seconds (0 = the monotonic one), and an empty store. */
void email_undo_set_clock(time_t now);
void email_undo_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_UNDO_H */
