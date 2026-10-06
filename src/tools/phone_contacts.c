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

 * Phone contact resolver: see phone_contacts.h (the call and SMS paths' view
 * of contact_resolve.h).
 */

#include "tools/phone_contacts.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tools/contact_resolve.h"
#include "tools/tool_registry.h"

static void resolve(int user_id, const char *input, bool in_turn, phone_resolve_t *out) {
   if (!out) {
      return;
   }
   memset(out, 0, sizeof(*out));
   char *words = in_turn ? tool_user_words_dup() : NULL;
   const contact_resolve_opts_t opts = {
      .field = CONTACT_FIELD_PHONE,
      .spoken = in_turn && tool_turn_spoken(),
      .user_words = words,
   };
   contact_resolve_t r;
   contact_resolve(user_id, input, &opts, &r);
   free(words);

   /* A phone value is a normalized number, far shorter than the field. */
   const size_t vlen = strnlen(r.value, sizeof(r.value));
   if (vlen < sizeof(out->number)) {
      memcpy(out->number, r.value, vlen + 1);
   } else if (r.kind == CONTACT_RESOLVE_LITERAL || r.kind == CONTACT_RESOLVE_UNIQUE ||
              r.kind == CONTACT_RESOLVE_CONFIRM) {
      r.kind = CONTACT_RESOLVE_BAD_VALUE;
   }
   snprintf(out->name, sizeof(out->name), "%s", r.name);
   switch (r.kind) {
      case CONTACT_RESOLVE_LITERAL:
         out->kind = PHONE_RESOLVE_NUMBER;
         return;
      case CONTACT_RESOLVE_UNIQUE:
         out->kind = PHONE_RESOLVE_UNIQUE;
         return;
      case CONTACT_RESOLVE_CONFIRM:
         out->kind = PHONE_RESOLVE_CONFIRM;
         break;
      case CONTACT_RESOLVE_AMBIGUOUS:
         out->kind = PHONE_RESOLVE_AMBIGUOUS;
         break;
      case CONTACT_RESOLVE_SUGGEST:
         out->kind = PHONE_RESOLVE_SUGGEST;
         break;
      case CONTACT_RESOLVE_BAD_VALUE:
         out->kind = PHONE_RESOLVE_BAD_NUMBER;
         break;
      default:
         out->kind = PHONE_RESOLVE_NONE;
         break;
   }
   contact_resolve_question(input, CONTACT_FIELD_PHONE, &r, out->question, sizeof(out->question));
}

void phone_contacts_resolve(int user_id, const char *input, phone_resolve_t *out) {
   resolve(user_id, input, true, out);
}

void phone_contacts_resolve_as_given(int user_id, const char *input, phone_resolve_t *out) {
   resolve(user_id, input, false, out);
}

void phone_contacts_format_disambiguation(const char *input,
                                          const phone_resolve_t *r,
                                          char *buf,
                                          size_t buf_size) {
   if (!buf || buf_size == 0) {
      return;
   }
   if (r && r->question[0]) {
      snprintf(buf, buf_size, "%s", r->question);
   } else {
      snprintf(buf, buf_size, "Could not resolve '%s' to a phone number.",
               input && input[0] ? input : "that contact");
   }
}
