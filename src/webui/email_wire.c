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
 * The mail panel's wire shapes (email_wire.h).
 */

#include "webui/email_wire.h"

#include <stdlib.h>
#include <string.h>

#include "tools/email_parse.h"
#include "utils/string_utils.h"

#define JSON_FLAGS (JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE)
/* Room for the frame around the payload: {"type":"email_read_response","payload":…,
 * "req":…,"success":…}, the req escaping to at most twice its length. */
#define FRAME_OVERHEAD (128 + 2 * EMAIL_WIRE_REQ_MAX)

/* Mail text is whatever the sender sent: a bad byte would make the browser
 * close the socket on the whole frame, so every string goes out well-formed. */
static void add_str(json_object *o, const char *key, const char *val) {
   if (!val)
      val = "";
   char *fixed = utf8_repair_dup(val, strlen(val), NULL);
   json_object_object_add(o, key, json_object_new_string(fixed ? fixed : val));
   free(fixed);
}

json_object *email_wire_row(int64_t account_id, const email_summary_t *s, bool flags_known) {
   json_object *r = json_object_new_object();
   if (!r)
      return NULL;
   json_object_object_add(r, "account_id", json_object_new_int64(account_id));
   add_str(r, "message_id", s->message_id);
   if (s->thread_id[0])
      add_str(r, "thread_id", s->thread_id);
   add_str(r, "from_name", s->from_name);
   add_str(r, "from_addr", s->from_addr);
   add_str(r, "subject", s->subject);
   json_object_object_add(r, "date", json_object_new_int64((int64_t)s->date));
   add_str(r, "preview", s->preview);
   json_object_object_add(r, "unread", json_object_new_boolean(s->unread));
   if (flags_known) {
      json_object_object_add(r, "starred", json_object_new_boolean(s->starred));
      json_object_object_add(r, "important", json_object_new_boolean(s->important));
   }
   return r;
}

static json_object *addr_list(const email_addr_t *list, int count) {
   json_object *arr = json_object_new_array();
   for (int i = 0; arr && list && i < count; i++) {
      json_object *a = json_object_new_object();
      add_str(a, "name", list[i].name);
      add_str(a, "addr", list[i].addr);
      json_object_array_add(arr, a);
   }
   return arr;
}

static json_object *attachments_json(const email_message_t *m) {
   json_object *arr = json_object_new_array();
   for (int i = 0; arr && m->attachments && i < m->attachment_count; i++) {
      const email_attachment_t *at = &m->attachments[i];
      json_object *a = json_object_new_object();
      add_str(a, "part_id", at->part_id);
      add_str(a, "filename", at->filename);
      add_str(a, "mime", at->mime);
      json_object_object_add(a, "size", json_object_new_int64((int64_t)at->size));
      json_object_object_add(a, "inline", json_object_new_boolean(at->is_inline));
      if (at->content_id[0])
         add_str(a, "content_id", at->content_id);
      json_object_array_add(arr, a);
   }
   return arr;
}

static size_t json_len(json_object *o) {
   size_t len = 0;
   json_object_to_json_string_length(o, JSON_FLAGS, &len);
   return len;
}

/* What json-c adds to escape @p c in a string under JSON_FLAGS ('/' isn't
 * escaped with NOSLASHESCAPE). */
static size_t escape_extra(unsigned char c) {
   switch (c) {
      case '"':
      case '\\':
      case '\b':
      case '\f':
      case '\n':
      case '\r':
      case '\t':
         return 1; /* \x */
      default:
         return c < 0x20 ? 5 : 0; /* \u00XX */
   }
}

/* The escaped size of @p len bytes of @p s, without building it. */
static size_t escaped_len(const char *s, size_t len) {
   size_t n = len;
   for (size_t i = 0; i < len; i++)
      n += escape_extra((unsigned char)s[i]);
   return n;
}

/* The longest prefix of @p s whose escaped size fits @p room, at a character boundary. */
static size_t escaped_prefix(const char *s, size_t len, size_t room) {
   size_t used = 0, i = 0;
   for (; i < len; i++) {
      const size_t add = 1 + escape_extra((unsigned char)s[i]);
      if (used + add > room)
         break;
      used += add;
   }
   return i;
}

/* Back @p len up to the start of a UTF-8 character. */
static size_t utf8_floor(const char *s, size_t len) {
   while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80)
      len--;
   return len;
}

json_object *email_wire_read_payload(int64_t account_id,
                                     const email_message_t *m,
                                     bool unread,
                                     size_t frame_max) {
   json_object *msg = json_object_new_object();
   json_object *payload = json_object_new_object();
   if (!msg || !payload) {
      json_object_put(msg);
      json_object_put(payload);
      return NULL;
   }
   json_object_object_add(msg, "account_id", json_object_new_int64(account_id));
   add_str(msg, "message_id", m->message_id);
   if (m->thread_id[0])
      add_str(msg, "thread_id", m->thread_id);
   add_str(msg, "subject", m->subject);
   add_str(msg, "from_name", m->from_name);
   add_str(msg, "from_addr", m->from_addr);
   json_object_object_add(msg, "to", addr_list(m->to_list, m->to_count));
   json_object_object_add(msg, "cc", addr_list(m->cc_list, m->cc_count));
   if (m->to_total > m->to_count)
      json_object_object_add(msg, "to_total", json_object_new_int(m->to_total));
   if (m->cc_total > m->cc_count)
      json_object_object_add(msg, "cc_total", json_object_new_int(m->cc_total));
   if (m->reply_to.addr[0]) {
      json_object *rt = json_object_new_object();
      add_str(rt, "name", m->reply_to.name);
      add_str(rt, "addr", m->reply_to.addr);
      json_object_object_add(msg, "reply_to", rt);
   }
   const time_t date = m->internal_date ? m->internal_date : email_parse_rfc822_date(m->date_str);
   json_object_object_add(msg, "date", json_object_new_int64((int64_t)date));
   json_object_object_add(msg, "attachments", attachments_json(m));
   if (m->attachments_truncated)
      json_object_object_add(msg, "attachments_truncated", json_object_new_boolean(1));
   json_object_object_add(msg, "text_truncated", json_object_new_boolean(m->text_truncated));
   json_object_object_add(msg, "html_truncated", json_object_new_boolean(m->html_truncated));
   json_object_object_add(payload, "message", msg);
   json_object_object_add(payload, "unread", json_object_new_boolean(unread));

   /* Size the frame without serializing the bodies: the small fields once, the
    * bodies by counting their escapes.  The reply is then serialized once. */
   size_t text_len = m->body ? (size_t)m->body_len : 0;
   char *text_fixed = utf8_repair_dup(m->body ? m->body : "", text_len, &text_len);
   const char *text = text_fixed ? text_fixed : (m->body ? m->body : "");
   if (!text_fixed)
      text_len = m->body ? (size_t)m->body_len : 0;
   const size_t base = json_len(payload) + FRAME_OVERHEAD + sizeof(",\"body_text\":\"\"") +
                       escaped_len(text, text_len);
   json_object_object_add(msg, "body_text", json_object_new_string_len(text, (int)text_len));
   free(text_fixed);

   /* body_html last, cut to what the frame has room for. */
   if (m->body_html && m->body_html_len > 0) {
      size_t full = (size_t)m->body_html_len;
      char *html_fixed = utf8_repair_dup(m->body_html, full, &full);
      const char *html = html_fixed ? html_fixed : m->body_html;
      if (!html_fixed)
         full = (size_t)m->body_html_len;
      const size_t key = sizeof(",\"body_html\":\"\"");
      const size_t room = frame_max > base + key ? frame_max - base - key : 0;
      size_t len = escaped_prefix(html, full, room);
      if (len < full)
         len = utf8_floor(html, len);
      if (len < full)
         json_object_object_add(msg, "html_truncated", json_object_new_boolean(1));
      if (len > 0)
         json_object_object_add(msg, "body_html", json_object_new_string_len(html, (int)len));
      free(html_fixed);
   }
   return payload;
}

json_object *email_wire_changed(int64_t account_id,
                                bool gmail_api,
                                email_move_kind_t kind,
                                bool undo,
                                const email_summary_t *created,
                                int nc,
                                const char *const *destroyed,
                                int nd,
                                bool refresh) {
   json_object *payload = json_object_new_object();
   json_object *frame = json_object_new_object();
   json_object *rows = json_object_new_array();
   json_object *gone = json_object_new_array();
   if (!payload || !frame || !rows || !gone) {
      json_object_put(payload);
      json_object_put(frame);
      json_object_put(rows);
      json_object_put(gone);
      return NULL;
   }
   json_object_object_add(payload, "account_id", json_object_new_int64(account_id));
   json_object_object_add(payload, "state", NULL);
   json_object_object_add(payload, "kind",
                          json_object_new_string(kind == EMAIL_MOVE_ARCHIVE ? "archive" : "trash"));
   json_object_object_add(payload, "undo", json_object_new_boolean(undo));
   for (int i = 0; created && i < nc; i++) /* only Gmail knows starred/important */
      json_object_array_add(rows, email_wire_row(account_id, &created[i], gmail_api));
   json_object_object_add(payload, "created", rows);
   json_object_object_add(payload, "updated", json_object_new_array());
   for (int i = 0; destroyed && i < nd; i++) {
      if (!destroyed[i])
         continue;
      char *fixed = utf8_repair_dup(destroyed[i], strlen(destroyed[i]), NULL);
      json_object_array_add(gone, json_object_new_string(fixed ? fixed : destroyed[i]));
      free(fixed);
   }
   json_object_object_add(payload, "destroyed", gone);
   if (refresh)
      json_object_object_add(payload, "refresh", json_object_new_boolean(1));
   json_object_object_add(frame, "type", json_object_new_string("email_changed"));
   json_object_object_add(frame, "payload", payload);
   return frame;
}

bool email_wire_undo_token_ok(const char *s) {
   if (!s)
      return false;
   for (int i = 0; i < EMAIL_UNDO_TOKEN_LEN; i++) {
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return false;
   }
   return s[EMAIL_UNDO_TOKEN_LEN] == '\0';
}

/* A failed entry: { <key>: id, error_code, error }. */
static void add_failed(json_object *arr, const char *key, const char *id, email_err_t e) {
   if (e == EMAIL_ERR_NONE)
      e = EMAIL_ERR_FAILED;
   json_object *f = json_object_new_object();
   if (!f)
      return;
   add_str(f, key, id);
   json_object_object_add(f, "error_code", json_object_new_string(email_error_name(e)));
   json_object_object_add(f, "error", json_object_new_string(email_wire_error_text(e)));
   json_object_array_add(arr, f);
}

/* {<a>: [], <b>: []} into @p p, or NULL with nothing leaked. */
static json_object *two_lists(const char *a, json_object **la, const char *b, json_object **lb) {
   json_object *p = json_object_new_object();
   *la = json_object_new_array();
   *lb = json_object_new_array();
   if (!p || !*la || !*lb) {
      json_object_put(p);
      json_object_put(*la);
      json_object_put(*lb);
      return NULL;
   }
   json_object_object_add(p, a, *la);
   json_object_object_add(p, b, *lb);
   return p;
}

json_object *email_wire_move_payload(const char *const *ids,
                                     const email_move_result_t *results,
                                     int n) {
   json_object *done, *failed;
   json_object *p = two_lists("done", &done, "failed", &failed);
   for (int i = 0; p && i < n; i++) {
      const email_move_result_t *r = &results[i];
      if (r->outcome == EMAIL_MOVE_FAILED) {
         add_failed(failed, "message_id", ids[i], r->err);
         continue;
      }
      json_object *d = json_object_new_object();
      if (!d)
         continue;
      add_str(d, "message_id", r->message_id[0] ? r->message_id : ids[i]);
      json_object_object_add(d, "undo", r->undo[0] ? json_object_new_string(r->undo) : NULL);
      if (r->outcome == EMAIL_MOVE_LEFT_FLAGGED)
         json_object_object_add(d, "left_flagged", json_object_new_boolean(1));
      json_object_array_add(done, d);
   }
   return p;
}

json_object *email_wire_undo_payload(int64_t account_id,
                                     const char *const *tokens,
                                     const email_undo_result_t *const *results,
                                     int n,
                                     bool flags_known) {
   json_object *restored, *failed;
   json_object *p = two_lists("restored", &restored, "failed", &failed);
   for (int i = 0; p && i < n; i++) {
      const email_undo_result_t *r = results[i];
      if (!r || r->err != EMAIL_ERR_NONE) {
         add_failed(failed, "undo", tokens[i], r ? r->err : EMAIL_ERR_UNDO_EXPIRED);
         continue;
      }
      json_object *o = json_object_new_object();
      if (!o)
         continue;
      json_object_object_add(o, "undo", json_object_new_string(tokens[i]));
      json_object_object_add(o, "row",
                             r->row_ok ? email_wire_row(account_id, &r->row, flags_known) : NULL);
      json_object_array_add(restored, o);
   }
   return p;
}

const char *email_wire_error_text(email_err_t err) {
   switch (err) {
      case EMAIL_ERR_NONE:
         return "";
      case EMAIL_ERR_AUTH_FAILED:
         return "The server refused the login";
      case EMAIL_ERR_AUTH_REVOKED:
         return "Access was revoked; reconnect the account";
      case EMAIL_ERR_UNREACHABLE:
         return "The mail server couldn't be reached";
      case EMAIL_ERR_TIMEOUT:
         return "The mail server didn't answer in time";
      case EMAIL_ERR_RATE_LIMITED:
         return "The provider asked to slow down; try again shortly";
      case EMAIL_ERR_NOT_FOUND:
         return "No such message";
      case EMAIL_ERR_READ_ONLY:
         return "The account is read-only";
      case EMAIL_ERR_FOLDER_MISSING:
         return "The folder doesn't exist";
      case EMAIL_ERR_NO_TRASH:
         return "The account has no Trash folder";
      case EMAIL_ERR_NO_ACCOUNT:
         return "No email account is set up";
      case EMAIL_ERR_ACCOUNT_NOT_FOUND:
         return "No such account";
      case EMAIL_ERR_CANCELLED:
         return "Stopped";
      case EMAIL_ERR_CURSOR_STALE:
         return "The list changed; start again from the top";
      case EMAIL_ERR_SUPERSEDED:
         return "Replaced by a newer request";
      case EMAIL_ERR_UNSUPPORTED_QUERY:
         return "This account can't search for that text";
      case EMAIL_ERR_BUSY:
         return "Busy; try again in a moment";
      case EMAIL_ERR_SHUTTING_DOWN:
         return "Shutting down";
      case EMAIL_ERR_CANNOT_CALCULATE:
         return "The changes can't be worked out; reload";
      case EMAIL_ERR_INVALID_REQUEST:
         return "The request isn't valid";
      case EMAIL_ERR_UNAVAILABLE:
         return "Email is turned off";
      case EMAIL_ERR_NOT_REMOVED:
         return "Copied, but the original couldn't be removed; don't retry";
      case EMAIL_ERR_UNDO_EXPIRED:
         return "This can no longer be undone";
      case EMAIL_ERR_IN_TRASH:
         return "It's in Trash or Spam; restore it first";
      case EMAIL_ERR_OUTCOME_UNKNOWN:
         return "The server didn't confirm; check the folder";
      case EMAIL_ERR_FAILED:
         break;
   }
   return "Something went wrong";
}

const char *email_wire_account_status(email_err_t err) {
   switch (err) {
      case EMAIL_ERR_NONE:
         return "ok";
      case EMAIL_ERR_AUTH_REVOKED:
         return "auth_revoked";
      case EMAIL_ERR_AUTH_FAILED:
         return "auth_failed";
      default:
         return "unreachable";
   }
}
