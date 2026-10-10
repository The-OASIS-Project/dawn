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
 * An email the user attached to a text turn (webui_email_ref.h).
 */

#include "webui/webui_email_ref.h"

#include <stdio.h>
#include <string.h>

#include "utils/string_utils.h"

#ifdef DAWN_ENABLE_EMAIL_TOOL
#include <sodium.h>

#include "tools/email_service.h"
#include "webui/webui_email_panel.h"
#include "webui/webui_email_panel_internal.h"
#endif

static __thread const webui_email_ref_t *t_turn_email;

void webui_turn_email_set(const webui_email_ref_t *ref) {
   t_turn_email = ref;
}

const webui_email_ref_t *webui_turn_email_get(void) {
   return t_turn_email;
}

void webui_row_add_email_ref(struct json_object *row, const char *role, const char *email_ref) {
   if (!row || !role || strcmp(role, "user") != 0 || !email_ref || !email_ref[0]) {
      return;
   }
   struct json_object *ref = json_tokener_parse(email_ref);
   if (ref && json_object_is_type(ref, json_type_object)) {
      json_object_object_add(row, "email_ref", ref);
   } else {
      json_object_put(ref);
   }
}

/* A display string from the client: at most WEBUI_EMAIL_REF_TEXT_MAX bytes cut
 * on a character, valid UTF-8, control characters as spaces.  Absent or not a
 * string: empty. */
static void display_text(struct json_object *obj, const char *key, char *out, size_t size) {
   struct json_object *v = NULL;
   out[0] = '\0';
   if (!json_object_object_get_ex(obj, key, &v) || !json_object_is_type(v, json_type_string)) {
      return;
   }
   snprintf(out, size, "%s", json_object_get_string(v));
   utf8_trim_incomplete(out); /* the cut never leaves half a character */
   sanitize_utf8_for_json(out);
   for (char *p = out; *p; p++) {
      if ((unsigned char)*p < 0x20 || *p == 0x7f) {
         *p = ' ';
      }
   }
}

#ifdef DAWN_ENABLE_EMAIL_TOOL
/* @p obj's account_id, when it is a positive integer; else 0. */
static int64_t account_id_of(struct json_object *obj) {
   struct json_object *v = NULL;
   if (!json_object_object_get_ex(obj, "account_id", &v) ||
       !json_object_is_type(v, json_type_int)) {
      return 0;
   }
   const int64_t id = json_object_get_int64(v);
   return id > 0 ? id : 0;
}

/* Whether @p id is a message id of @p acct's kind, exactly as the mail panel
 * gives them (its own check): a Gmail id, or an IMAP "folder:uid.uidvalidity". */
static bool message_id_ok(const email_account_t *acct, const char *id) {
   return session_attach_name_ok(id, SESSION_ATTACH_MESSAGE_ID_MAX) &&
          email_panel_msg_id_ok(id, email_service_account_uses_lease(acct));
}

/* A name for account @p acct the email tool resolves back to it (its `account`
 * argument matches the first enabled account by name or username): its
 * username, else its name, else none (two accounts share it).  Into @p out. */
static bool tool_name_for(int user_id, const email_account_t *acct, char *out, size_t size) {
   const char *const names[] = { acct->username, acct->name };
   for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
      if (!session_attach_name_ok(names[i], SESSION_ATTACH_ACCOUNT_MAX)) {
         continue;
      }
      email_account_t back;
      const bool same = email_service_find_account_by_name(user_id, names[i], &back) ==
                            EMAIL_RC_OK &&
                        back.id == acct->id;
      sodium_memzero(&back, sizeof(back));
      if (same) {
         snprintf(out, size, "%s", names[i]);
         return true;
      }
   }
   return false;
}

/* One ref, checked and filled in; false when it can't be used. */
static bool read_ref(int user_id, struct json_object *obj, webui_email_ref_t *out) {
   if (!json_object_is_type(obj, json_type_object) || !webui_email_client_enabled()) {
      return false;
   }
   const int64_t account_id = account_id_of(obj);
   struct json_object *mid = NULL;
   if (account_id <= 0 || !json_object_object_get_ex(obj, "message_id", &mid) ||
       !json_object_is_type(mid, json_type_string) ||
       (size_t)json_object_get_string_len(mid) != strlen(json_object_get_string(mid))) {
      return false;
   }
   /* The user's own enabled account, from the database: a foreign or missing
    * one is refused the same way, before anything is fetched. */
   email_account_t acct;
   const bool found = email_service_find_account_by_id(user_id, account_id, true, &acct) ==
                          EMAIL_RC_OK &&
                      message_id_ok(&acct, json_object_get_string(mid)) &&
                      tool_name_for(user_id, &acct, out->account, sizeof(out->account));
   sodium_memzero(&acct, sizeof(acct)); /* it holds the account's sealed password */
   if (!found) {
      return false;
   }
   snprintf(out->message_id, sizeof(out->message_id), "%s", json_object_get_string(mid));

   char from[WEBUI_EMAIL_REF_TEXT_MAX + 1];
   char subject[WEBUI_EMAIL_REF_TEXT_MAX + 1];
   display_text(obj, "from", from, sizeof(from));
   display_text(obj, "subject", subject, sizeof(subject));
   struct json_object *stored = json_object_new_object();
   if (!stored) {
      return false;
   }
   json_object_object_add(stored, "account_id", json_object_new_int64(account_id));
   json_object_object_add(stored, "message_id", json_object_new_string(out->message_id));
   json_object_object_add(stored, "from", json_object_new_string(from));
   json_object_object_add(stored, "subject", json_object_new_string(subject));
   const char *s = json_object_to_json_string_ext(stored, JSON_C_TO_STRING_PLAIN |
                                                              JSON_C_TO_STRING_NOSLASHESCAPE);
   const bool fits = s && strlen(s) < sizeof(out->stored);
   if (fits) {
      snprintf(out->stored, sizeof(out->stored), "%s", s);
   }
   json_object_put(stored);
   return fits;
}
#endif

bool webui_email_ref_from_payload(int user_id,
                                  struct json_object *payload,
                                  webui_email_ref_t *out,
                                  const char **code_out,
                                  const char **message_out) {
   memset(out, 0, sizeof(*out));
   struct json_object *refs = NULL;
   if (!payload || !json_object_object_get_ex(payload, "email_refs", &refs) ||
       json_object_is_type(refs, json_type_null)) {
      return true;
   }
   const bool is_list = json_object_is_type(refs, json_type_array);
   const size_t n = is_list ? json_object_array_length(refs) : 0;
   if (is_list && n == 0) {
      return true;
   }
   if (n > 1) {
      *code_out = WEBUI_ERR_EMAIL_REF_LIMIT;
      *message_out = "Attach one email per message.";
      return false;
   }
   bool ok = false;
#ifdef DAWN_ENABLE_EMAIL_TOOL
   ok = is_list && read_ref(user_id, json_object_array_get_idx(refs, 0), out);
#else
   (void)user_id;
#endif
   if (!ok) {
      memset(out, 0, sizeof(*out));
      *code_out = WEBUI_ERR_EMAIL_UNAVAILABLE;
      *message_out = "The attached email isn't available (its account is gone or off, or email "
                     "is off). Remove it and try again.";
      return false;
   }
   out->present = true;
   return true;
}
