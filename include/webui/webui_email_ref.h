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
 * An email the user attached to a text turn (the frame's email_refs): checked
 * on the lws thread, carried with the turn, saved on its question row
 * (messages.email_ref) and sent back on every frame that delivers that row.
 */

#ifndef WEBUI_EMAIL_REF_H
#define WEBUI_EMAIL_REF_H

#include <json-c/json.h>
#include <stdbool.h>
#include <stdint.h>

#include "auth/auth_db_messages.h"
#include "core/session_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Longest from or subject kept for the chip (bytes, cut on a character).
 *  Both at this length, escaped (a quote or backslash doubles), with the
 *  longest id, still fit CONV_EMAIL_REF_MAX. */
#define WEBUI_EMAIL_REF_TEXT_MAX 160

/** Refusal codes (an error frame; the turn doesn't run, nothing is saved). */
#define WEBUI_ERR_EMAIL_REF_LIMIT "EMAIL_REF_LIMIT"
#define WEBUI_ERR_EMAIL_UNAVAILABLE "EMAIL_UNAVAILABLE"

/** A checked email reference.  Fixed buffers: it travels inside a turn's work item. */
typedef struct {
   bool present;
   char account[SESSION_ATTACH_ACCOUNT_MAX + 1];       /* the email tool's account name */
   char message_id[SESSION_ATTACH_MESSAGE_ID_MAX + 1]; /* the email tool's message id */
   char stored[CONV_EMAIL_REF_MAX + 1]; /* messages.email_ref: {account_id, message_id, from,
                                           subject} */
} webui_email_ref_t;

/**
 * @brief Read a text frame's email_refs for @p user_id
 *
 * Absent or empty: @p out->present is false.  More than one: EMAIL_REF_LIMIT.
 * Anything else wrong (not a list of objects, an id that isn't one of the
 * account's kind, an account that isn't the user's or is disabled, a name the
 * email tool wouldn't resolve back to it, email off): EMAIL_UNAVAILABLE.
 * The account is looked up in the database only; nothing is fetched.  from and
 * subject are the client's copy, kept for display only (cut, cleaned, never
 * sent to the model).
 *
 * @param code_out, message_out The refusal, for an error frame
 * @return true when the frame may go on (with or without an email)
 */
bool webui_email_ref_from_payload(int user_id,
                                  struct json_object *payload,
                                  webui_email_ref_t *out,
                                  const char **code_out,
                                  const char **message_out);

/**
 * @brief Add a saved row's email_ref to a frame's row object as `email_ref`
 *        (an object), when it is a user row holding a valid one
 */
void webui_row_add_email_ref(struct json_object *row, const char *role, const char *email_ref);

/** The email reference of the text frame this thread is handling, or NULL
 *  (set around the frame's handling, like webui_turn_ref_set). */
void webui_turn_email_set(const webui_email_ref_t *ref);
const webui_email_ref_t *webui_turn_email_get(void);

#ifndef __cplusplus
_Static_assert(4 * WEBUI_EMAIL_REF_TEXT_MAX + SESSION_ATTACH_MESSAGE_ID_MAX + 96 <=
                   CONV_EMAIL_REF_MAX,
               "an email_ref record must always fit its column");
#endif

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_EMAIL_REF_H */
