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
 * The mail panel's wire shapes: a list row, a read message, and the words
 * for an error code.  Pure: no I/O, no locks.
 */

#ifndef EMAIL_WIRE_H
#define EMAIL_WIRE_H

#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tools/email_types.h"

/* The client's request id echoed in a reply, in bytes; printable only (no C0 or
 * DEL), so it escapes to at most twice its length. */
#define EMAIL_WIRE_REQ_MAX 64

#ifdef __cplusplus
extern "C" {
#endif

/* The largest read reply frame: past it, body_html is cut further. */
#define EMAIL_PANEL_FRAME_MAX (1536 * 1024)
/* The panel's body_text cap (the LLM tool's is the account's own). */
#define EMAIL_PANEL_TEXT_MAX (256 * 1024)

/** One list row; starred/important only where the backend knows them (Gmail). */
json_object *email_wire_row(int64_t account_id, const email_summary_t *s, bool flags_known);

/**
 * @brief A read reply's payload: {message, unread}
 *
 * If the whole frame would pass @p frame_max (EMAIL_PANEL_FRAME_MAX), body_html
 * is cut further (at a character boundary) and html_truncated set; if it
 * still doesn't fit, body_html is dropped.
 */
json_object *email_wire_read_payload(int64_t account_id,
                                     const email_message_t *m,
                                     bool unread,
                                     size_t frame_max);

/** A short English sentence for @p err, for "error" next to "error_code". */
const char *email_wire_error_text(email_err_t err);

/** An account's status in accounts[]: "ok", "auth_revoked", "auth_failed" or "unreachable". */
const char *email_wire_account_status(email_err_t err);

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_WIRE_H */
