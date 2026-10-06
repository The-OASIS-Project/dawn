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
 * The IMAP client's own helpers, shared by its modules (email_client.c,
 * email_imap_move.c).  Not for use outside the email client.
 */

#ifndef EMAIL_CLIENT_INTERNAL_H
#define EMAIL_CLIENT_INTERNAL_H

#include <curl/curl.h>
#include <stdbool.h>

#include "core/curl_buffer.h"
#include "tools/email_client.h"
#include "tools/email_instrument.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A curl handle set up for @p conn's IMAP server (timeouts, TLS, auth,
 *  IMAP-only protocols).  Caller cleans it up; NULL on failure. */
CURL *email_imap_handle_create(const email_conn_t *conn);

/**
 * @brief One IMAP command on @p curl (its URL already set), reply into @p buf
 * @param first True for the operation's first, auth-bearing command: it goes
 *              through the instrumented perform
 * @param buf   Initialized here; the caller frees it
 */
CURLcode email_imap_run_command(CURL *curl,
                                email_instrument_ctx_t *dctx,
                                const email_conn_t *conn,
                                const char *op,
                                const char *cmd,
                                bool first,
                                curl_buffer_t *buf);

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_CLIENT_INTERNAL_H */
