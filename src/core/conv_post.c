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
 * Posts into a conversation from outside any turn, written through the
 * conversation lease so they never land between a turn's rows.  See
 * conv_post.h.
 */

#include "core/conv_post.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_messages.h"
#include "core/conv_turn_lease.h"
#include "dawn_error.h"
#include "logging.h"

/* Defined in webui_broadcasts.c (webui_absent.c without the WebUI): tells an open
 * chat that a conversation has new messages.  Declared here, not by including
 * webui_server.h, which is a layer above. */
void webui_broadcast_conversation_messages_appended(int user_id, int64_t conv_id);

typedef struct {
   conv_lease_item_t link; /* the lease's while queued */
   int64_t conv_id;
   int user_id;
   conv_post_written_fn on_written;
   char text[]; /* NUL-terminated */
} conv_post_item_t;

/* The lease's writer: one post's row, its tabs told, then the item freed. */
static bool write_post(conv_lease_item_t *link) {
   conv_post_item_t *item = (conv_post_item_t *)link;
   int64_t msg_id = 0;
   /* no-blocks: DAWN's own message, not a turn to replay; unprompted, so memory
    * extraction leaves it out. */
   const conv_message_row_t row = { .role = "assistant",
                                    .content = item->text,
                                    .unprompted = true };
   const bool ok = conv_db_add_row(item->conv_id, item->user_id, &row, &msg_id) ==
                       AUTH_DB_SUCCESS &&
                   msg_id > 0;
   if (ok) {
      webui_broadcast_conversation_messages_appended(item->user_id, item->conv_id);
      if (item->on_written) {
         item->on_written(item->conv_id, msg_id);
      }
   } else {
      OLOG_WARNING("conv_post: couldn't post to conversation %lld", (long long)item->conv_id);
   }
   free(item);
   return ok;
}

/* Only after the writer is gone (shutdown's last step): a post not written. */
static void drop_post(conv_lease_item_t *link) {
   conv_post_item_t *item = (conv_post_item_t *)link;
   OLOG_WARNING("conv_post: a post to conversation %lld was dropped", (long long)item->conv_id);
   free(item);
}

void conv_post_init(void) {
   conv_turn_lease_set_writer(write_post, drop_post);
}

int conv_post_unprompted_ex(int64_t conv_id,
                            int user_id,
                            const char *text,
                            conv_post_written_fn on_written) {
   if (conv_id <= 0 || user_id <= 0 || !text) {
      return FAILURE;
   }
   const size_t len = strlen(text);
   conv_post_item_t *item = malloc(sizeof(*item) + len + 1);
   if (!item) {
      OLOG_ERROR("conv_post: out of memory posting to conversation %lld", (long long)conv_id);
      return FAILURE;
   }
   memset(item, 0, sizeof(*item));
   item->conv_id = conv_id;
   item->user_id = user_id;
   item->on_written = on_written;
   memcpy(item->text, text, len + 1);

   const int rc = conv_turn_lease_post_or_defer(conv_id, &item->link);
   if (rc == CONV_TURN_LEASE_DEFERRED) {
      OLOG_INFO("conv_post: conversation %lld is in a turn; post queued for its end",
                (long long)conv_id);
      return SUCCESS;
   }
   if (rc == CONV_TURN_LEASE_WRITTEN) {
      return SUCCESS;
   }
   if (rc == CONV_TURN_LEASE_WRITE_FAILED) {
      return FAILURE; /* the item was consumed */
   }
   OLOG_ERROR("conv_post: no post writer registered; post to conversation %lld lost",
              (long long)conv_id);
   free(item);
   return FAILURE;
}

int conv_post_unprompted(int64_t conv_id, int user_id, const char *text) {
   return conv_post_unprompted_ex(conv_id, user_id, text, NULL);
}
