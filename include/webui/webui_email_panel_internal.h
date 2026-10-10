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
 * What the mail panel's verb files share (webui_email_panel.c and
 * webui_email_panel_move.c): replies from the lws thread, request checks and
 * the user's accounts.  Internal to those files.
 */

#ifndef WEBUI_EMAIL_PANEL_INTERNAL_H
#define WEBUI_EMAIL_PANEL_INTERNAL_H

#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tools/email_service.h"
#include "webui/webui_email_exec.h"
#include "webui/webui_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A message id the panel got from DAWN fits this (email_summary_t.message_id). */
#define PANEL_MSG_ID_MAX (sizeof(((email_summary_t *)0)->message_id) - 1)

/* The user's enabled accounts, as the panel needs them. */
typedef struct {
   int64_t id;
   bool is_imap; /* takes the account lease; otherwise the Gmail API */
   bool read_only;
} panel_acct_t;

/** Sends {type:"<verb>_response", payload} (success:true unless set; req when given); takes @p
 * payload. */
void email_panel_reply(ws_connection_t *conn,
                       const char *verb,
                       json_object *payload,
                       const char *req);

/** A failure payload for @p code: success false, error_code, error. */
json_object *email_panel_error_payload(email_err_t code);

/** Answers @p verb with @p code. */
void email_panel_reply_error(ws_connection_t *conn,
                             const char *verb,
                             email_err_t code,
                             const char *req);

/** A string of 1..max bytes with no control characters or embedded NUL. */
bool email_panel_text_ok(json_object *v, size_t max);

/** The user's enabled accounts into @p out (EMAIL_MAX_ACCOUNTS); returns the count. */
int email_panel_load_accounts(int user_id, panel_acct_t *out);

/** The account_id member: the user's enabled account, else NULL (@p valid: the member was well
 * formed). */
const panel_acct_t *email_panel_get_account(json_object *payload,
                                            const panel_acct_t *accts,
                                            int n_accts,
                                            bool *valid);

/** The request's session id; @p ok false when the connection has none. */
uint32_t email_panel_session_id_of(ws_connection_t *conn, bool *ok);

/** Submits @p r; a submit that wasn't answered is answered here. */
void email_panel_submit(ws_connection_t *conn, const email_exec_request_t *r);

/** The verb's common start: logged in, email on, the request's req (@p req_out, or NULL). */
bool email_panel_verb_start(ws_connection_t *conn,
                            const char *verb,
                            json_object *payload,
                            char *req,
                            const char **req_out);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_EMAIL_PANEL_INTERNAL_H */
