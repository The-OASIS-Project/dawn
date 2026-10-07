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
 * A message's read state on IMAP, as the server reports it: pure parsing of
 * FETCH (FLAGS) and STATUS (UNSEEN) replies, and the UID sets commands carry.
 */

#ifndef EMAIL_IMAP_STATE_H
#define EMAIL_IMAP_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* email_imap_fetch_seen results */
#define EMAIL_IMAP_UID_ABSENT (-1) /* no FETCH line with that UID and its FLAGS */
#define EMAIL_IMAP_UID_UNSEEN 0
#define EMAIL_IMAP_UID_SEEN 1

/**
 * @brief Message @p uid's read state in a reply to a FETCH (UID FLAGS) or a
 *        STORE without .SILENT: "* n FETCH (UID <uid> FLAGS (... \Seen ...))"
 * @return EMAIL_IMAP_UID_SEEN, EMAIL_IMAP_UID_UNSEEN, or EMAIL_IMAP_UID_ABSENT
 *         when the reply has no line for it (no such message)
 */
int email_imap_fetch_seen(const char *response, uint32_t uid);

/**
 * @brief The UNSEEN count from a "* STATUS <mailbox> (... UNSEEN <n> ...)" reply
 * @return true and sets @p unseen when the reply has one
 */
bool email_imap_status_unseen(const char *response, int *unseen);

/**
 * @brief @p uids as an IMAP sequence set ("12,15,40")
 * @return the length written, or 0 when there are none or it doesn't fit
 */
size_t email_imap_uid_set(const uint32_t *uids, int n, char *out, size_t out_size);

/**
 * @brief Whether a perform that failed with libcurl's login-denied code was a
 *        refused SELECT (the folder) rather than a refused login
 *
 * libcurl reports both the same way.  On an already logged-in connection no
 * LOGIN goes out, so an unchanged login count means the SELECT was refused.
 *
 * @param login_denied  the perform returned CURLE_LOGIN_DENIED
 * @param logins_before on-wire LOGIN/AUTHENTICATE count before the perform
 * @param logins_after  the same count after it
 */
bool email_imap_select_failed(bool login_denied, int logins_before, int logins_after);

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_IMAP_STATE_H */
