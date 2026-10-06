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
 * Reply codes: a short code DAWN texts to someone and they text back, to
 * show they hold the phone the text went to (a sender's number on a text
 * proves nothing; receiving one does).  Used to verify a number when it is
 * linked, and to approve an action asked for by text.
 */

#ifndef REPLY_CODE_H
#define REPLY_CODE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REPLY_CODE_DIGITS 6
#define REPLY_CODE_LEN (REPLY_CODE_DIGITS + 1) /* with the NUL */
/** A code's digest as hex, with the NUL. */
#define REPLY_CODE_DIGEST_HEX 65

/** A new random code: REPLY_CODE_DIGITS decimal digits. */
void reply_code_new(char out[REPLY_CODE_LEN]);

/**
 * @brief A code's digest, keyed with a secret made at start-up: a stored
 *        digest doesn't give the code away (6 digits are trivial to
 *        brute-force unkeyed).  A restart forgets the key, so a code sent
 *        before it no longer matches.
 */
void reply_code_digest(const char *code, char out[REPLY_CODE_DIGEST_HEX]);

/** Whether two digests are the same (constant time). */
bool reply_code_digest_equal(const char *a, const char *b);

/**
 * @brief Whether a text is a reply code: its first word is exactly
 *        REPLY_CODE_DIGITS digits, maybe followed by '.' or '!' (spaces
 *        around it and anything after it are ignored; a code is never
 *        searched for inside other words)
 *
 * @param body The text
 * @param out  Receives the code (may be NULL)
 */
bool reply_code_in_text(const char *body, char out[REPLY_CODE_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* REPLY_CODE_H */
