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
 * Phone contact resolver: the phone tool's view of contact_resolve.h.  Turns
 * a call/SMS target (a number, or a spoken/typed contact name) into a number
 * to dial, or into the question to ask when it isn't certain: never a silent
 * guess.  Shared by the call and SMS paths.
 */

#ifndef PHONE_CONTACTS_H
#define PHONE_CONTACTS_H

#include <stddef.h>

typedef enum {
   PHONE_RESOLVE_NUMBER = 0, /* input was a dialable phone number the user gave */
   PHONE_RESOLVE_UNIQUE,     /* exactly one contact, certain */
   PHONE_RESOLVE_CONFIRM,    /* one contact (or number) to confirm with the user first:
                                only part of the name, sounds like another contact,
                                or not something the user said */
   PHONE_RESOLVE_AMBIGUOUS,  /* multiple contacts matched the name */
   PHONE_RESOLVE_SUGGEST,    /* no direct match; fuzzy near-misses found */
   PHONE_RESOLVE_BAD_NUMBER, /* input looked numeric but is not a valid number,
                                or the matched contact has no valid number on file */
   PHONE_RESOLVE_NONE        /* nothing matched */
} phone_resolve_kind_t;

typedef struct {
   phone_resolve_kind_t kind;
   char number[24];     /* set for NUMBER, UNIQUE and CONFIRM */
   char name[64];       /* set for UNIQUE and CONFIRM (empty for a number) */
   char question[1024]; /* for everything but NUMBER and UNIQUE: what to ask */
} phone_resolve_t;

/**
 * @brief Resolve a phone "target" (number or contact name) for the calling
 *        turn (contact_resolve.h): its spoken flag and the user's own words
 *        come from the command context
 *
 * Only NUMBER and UNIQUE may be dialed or texted without asking; CONFIRM
 * names one contact or number to put to the user first (a preview); the rest
 * carry the question to ask instead.
 *
 * @param user_id User whose contacts are searched.
 * @param input   Target string (number or name); NULL/empty -> PHONE_RESOLVE_NONE.
 * @param out     Result (always fully initialized on return; must be non-NULL).
 */
void phone_contacts_resolve(int user_id, const char *input, phone_resolve_t *out);

/**
 * @brief Resolve a target with no turn checks (not spoken, no user words):
 *        for the service layer, which dials or texts what the tool layer
 *        already decided on (a confirmed preview's number)
 */
void phone_contacts_resolve_as_given(int user_id, const char *input, phone_resolve_t *out);

/**
 * @brief The question for a result the caller can't act on (r->question; a
 *        generic line for NUMBER/UNIQUE, which callers shouldn't pass)
 */
void phone_contacts_format_disambiguation(const char *input,
                                          const phone_resolve_t *r,
                                          char *buf,
                                          size_t buf_size);

#endif /* PHONE_CONTACTS_H */
