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
 * An email as the model reads it: its headers, attachments and body as text.
 * Shared by the email tool's read and an email attached to a chat turn.
 */

#ifndef EMAIL_RENDER_H
#define EMAIL_RENDER_H

#include "tools/email_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief @p msg as text: From, To, Cc, Reply-To (when it differs), Subject,
 *        Date, its attachments, then the body ("[Message truncated]" when cut)
 *
 * Every field is the sender's text, already made safe to show by the reader
 * (email_display_*); the caller frames the whole as third-party content.
 *
 * @return Heap text (caller frees), or NULL on allocation failure
 */
char *email_render_message(const email_message_t *msg);

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_RENDER_H */
