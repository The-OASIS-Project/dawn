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
 * Shared by the email backends' transfers: why one failed (email_err_t, its
 * wire name), and stopping one that is no longer wanted.
 */

#ifndef EMAIL_TRANSFER_H
#define EMAIL_TRANSFER_H

#include <curl/curl.h>
#include <stdatomic.h>

#include "tools/email_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The email_err_t a failed transfer stands for (IMAP and Gmail). */
email_err_t email_err_from_curl(CURLcode res);

/**
 * @brief End @p curl's transfers early once *@p cancel is set
 *
 * Installs a progress callback that aborts the transfer (CURLE_ABORTED_BY_CALLBACK).
 * No-op when @p cancel is NULL.  The flag must outlive the handle.
 */
void email_transfer_set_cancel(CURL *curl, const atomic_bool *cancel);

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_TRANSFER_H */
