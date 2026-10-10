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
 * Whether the mail panel's verbs answer (the email_client flag): its own
 * header, free of the WebUI's, for code that only needs to ask.
 */

#ifndef WEBUI_EMAIL_CLIENT_H
#define WEBUI_EMAIL_CLIENT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The panel's verbs answer (the email tool is on and its service up): the email_client flag.
 *  Defined in webui_email_panel.c; a build without the panel has none. */
bool webui_email_client_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_EMAIL_CLIENT_H */
