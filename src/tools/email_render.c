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
 * An email as the model reads it (email_render.h).
 */

#include "tools/email_render.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "core/strbuf.h"
#include "tools/email_parse.h"

#define KIB ((size_t)1024)
#define MIB (KIB * KIB)

/* What the headers and attachment lines take beyond the body, to start with
 * and at most: headers, 32+32 addresses and 16 attachment lines fit well
 * within the most. */
#define RENDER_HEADROOM_START (2 * KIB)
#define RENDER_HEADROOM_MAX (64 * KIB)

/* @p bytes as "512 B", "12 KB", "3.4 MB". */
static void format_size(size_t bytes, char *out, size_t size) {
   if (bytes < KIB) {
      snprintf(out, size, "%zu B", bytes);
   } else if (bytes < MIB) {
      snprintf(out, size, "%zu KB", (bytes + KIB / 2) / KIB);
   } else {
      snprintf(out, size, "%.1f MB", (double)bytes / (double)MIB);
   }
}

static void append_addrs(strbuf_t *sb,
                         const char *label,
                         const email_addr_t *list,
                         int count,
                         int total) {
   if (count <= 0) {
      return;
   }
   strbuf_appendf(sb, "%s: ", label);
   for (int i = 0; i < count; i++) {
      /* A name that is just the address again isn't shown twice. */
      if (list[i].name[0] && strcasecmp(list[i].name, list[i].addr) != 0) {
         strbuf_appendf(sb, "%s%s <%s>", i ? ", " : "", list[i].name, list[i].addr);
      } else {
         strbuf_appendf(sb, "%s%s", i ? ", " : "", list[i].addr);
      }
   }
   if (total > count) {
      strbuf_appendf(sb, " (and %d more)", total - count);
   }
   strbuf_append(sb, "\n");
}

char *email_render_message(const email_message_t *msg) {
   if (!msg) {
      return NULL;
   }
   strbuf_t sb;
   const size_t body = msg->body_len > 0 ? (size_t)msg->body_len : 0;
   strbuf_init_with_max(&sb, body + RENDER_HEADROOM_START, body + RENDER_HEADROOM_MAX);
   char from[2 * sizeof(msg->from_name) + sizeof(msg->from_addr) + 8];
   email_display_mailbox(msg->from_name, msg->from_addr, from, sizeof(from));
   strbuf_appendf(&sb, "From: %s\n", from);
   append_addrs(&sb, "To", msg->to_list, msg->to_count, msg->to_total);
   append_addrs(&sb, "Cc", msg->cc_list, msg->cc_count, msg->cc_total);
   if (msg->reply_to.addr[0] && strcasecmp(msg->reply_to.addr, msg->from_addr) != 0) {
      strbuf_appendf(&sb, "Reply-To: %s\n", msg->reply_to.addr);
   }
   strbuf_appendf(&sb, "Subject: %s\nDate: %s\n", msg->subject, msg->date_str);
   if (msg->attachment_count > 0) {
      strbuf_append(&sb, "Attachments:\n");
      for (int i = 0; i < msg->attachment_count; i++) {
         const email_attachment_t *a = &msg->attachments[i];
         char size[32];
         format_size(a->size, size, sizeof(size));
         strbuf_appendf(&sb, "  %d. %s (%s, %s%s)\n", i + 1,
                        a->filename[0] ? a->filename : "(unnamed)", a->mime, size,
                        a->is_inline ? ", inline" : "");
      }
      if (msg->attachments_truncated) {
         strbuf_append(&sb, "  (more attachments not listed)\n");
      }
   }
   strbuf_appendf(&sb, "\n%s", msg->body && msg->body[0] ? msg->body : "(No body)");
   if (msg->text_truncated) {
      strbuf_append(&sb, "\n[Message truncated]");
   }
   if (strbuf_oom(&sb)) {
      strbuf_free(&sb);
      return NULL;
   }
   return strbuf_steal(&sb);
}
