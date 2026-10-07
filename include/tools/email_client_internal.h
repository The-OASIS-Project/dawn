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

/** @p folder percent-encoded for an IMAP URL path, as every IMAP call addresses it. */
void email_imap_url_encode_folder(const char *folder, char *out, size_t out_len);

/**
 * @brief STATUS INBOX (UNSEEN) on @p curl, before any mailbox is selected on it
 *        (RFC 3501: STATUS shouldn't be asked of the selected mailbox)
 *
 * Sets the handle's URL to the server (no mailbox); the caller sets its own
 * URL afterwards.  email_imap_flags.c
 * @param first As for email_imap_run_command
 * @return CURLE_OK with @p unseen set, or why it failed (@p unseen -1)
 */
CURLcode email_imap_status_inbox_unseen(CURL *curl,
                                        email_instrument_ctx_t *dctx,
                                        const email_conn_t *conn,
                                        bool first,
                                        int *unseen);

/**
 * @brief Message @p uid's read state in the mailbox at @p folder_url
 * @param state EMAIL_IMAP_UID_SEEN/_UNSEEN/_ABSENT (email_imap_state.h)
 * @return CURLE_OK with @p state set, or why the FETCH failed
 */
CURLcode email_imap_seen_state(CURL *curl,
                               email_instrument_ctx_t *dctx,
                               const email_conn_t *conn,
                               const char *folder_url,
                               uint32_t uid,
                               bool first,
                               int *state);

/**
 * @brief Clear \Seen on @p uid again after a read set it, whatever happened to
 *        the read: no cancel (the read's may be set), its own short timeout,
 *        possibly a new login (curl drops the connection after an aborted fetch)
 * @return true when the server took it
 */
bool email_imap_restore_unseen(CURL *curl,
                               email_instrument_ctx_t *dctx,
                               const email_conn_t *conn,
                               const char *folder_url,
                               uint32_t uid);

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_CLIENT_INTERNAL_H */
