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
 * Context Expand Tool — retrieve original messages from compacted summaries
 *
 * LCM Phase 3: when compaction summarizes messages, the summary includes a
 * [COMPACTED conv=N msgs=X-Y] tag. This tool retrieves the original messages
 * by querying the database, making compaction non-destructive from the model's
 * perspective.
 */

#include "tools/context_expand_tool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "core/session_manager.h"
#include "logging.h"
#include "tools/tool_registry.h"
#ifdef ENABLE_WEBUI
#include "webui/webui_server.h"
#endif

#define EXPAND_TOKEN_BUDGET 4000
#define EXPAND_CHAR_BUDGET (EXPAND_TOKEN_BUDGET * 4)

static char *context_expand_callback(const char *action, char *value, int *should_respond);

static const treg_param_t context_expand_params[] = {
   {
       .name = "action",
       .description = "Action to perform",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "expand" },
       .enum_count = 1,
   },
   {
       .name = "start_id",
       .description = "First message ID from the [COMPACTED] tag. "
                      "Required for raw message retrieval (with end_id). "
                      "Not needed when using node_id.",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "start_id",
   },
   {
       .name = "end_id",
       .description = "Last message ID from the [COMPACTED] tag. "
                      "Required for raw message retrieval (with start_id). "
                      "Not needed when using node_id.",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "end_id",
   },
   {
       .name = "conversation_id",
       .description = "Conversation ID from the [COMPACTED] tag. "
                      "Omit to use the current conversation or its parent. "
                      "Not needed when using node_id.",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "conversation_id",
   },
   {
       .name = "node_id",
       .description = "Summary node ID from the [COMPACTED] tag. "
                      "When provided alone, returns the summary hierarchy for "
                      "multi-resolution drill-down. No other parameters needed.",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "node_id",
   },
};

static const tool_metadata_t context_expand_metadata = {
   .name = "context_expand",
   .device_string = "context_expand",
   .topic = "dawn",
   .aliases = { NULL },
   .alias_count = 0,

   .description = "Retrieve original messages that were compacted into a summary. "
                  "Look for [COMPACTED conv=N msgs=X-Y node=Z depth=D] tags. "
                  "Use start_id/end_id for raw messages, or node_id for multi-resolution "
                  "drill-down (shows the prior summary level).",
   .params = context_expand_params,
   .param_count = 5,

   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_NONE,
   .skip_followup = false,

   .is_available = NULL,
   .callback = context_expand_callback,
};

int context_expand_tool_register(void) {
   return tool_registry_register(&context_expand_metadata);
}

typedef struct {
   char *buf;
   int offset;
   int capacity;
   int count;
} expand_ctx_t;

static int expand_message_cb(const conversation_message_t *msg, void *ctx) {
   expand_ctx_t *ec = (expand_ctx_t *)ctx;

   int remaining = ec->capacity - ec->offset - 1;
   if (remaining < 100)
      return 1;

   int written = snprintf(ec->buf + ec->offset, remaining, "[%s]: %s\n", msg->role,
                          msg->content ? msg->content : "");
   if (written >= remaining) {
      ec->buf[ec->offset] = '\0';
      return 1;
   }
   ec->offset += written;
   ec->count++;
   return 0;
}

static char *context_expand_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   *should_respond = 1;

   if (!value || value[0] == '\0')
      return strdup(TOOL_RESULT_ERROR_MARK "Error: no message range provided.");

   char tmp[32];
   int64_t start_id = 0, end_id = 0, conv_id = 0, node_id = 0;

   if (tool_param_extract_custom(value, "start_id", tmp, sizeof(tmp)))
      start_id = atoll(tmp);
   if (tool_param_extract_custom(value, "end_id", tmp, sizeof(tmp)))
      end_id = atoll(tmp);
   if (tool_param_extract_custom(value, "conversation_id", tmp, sizeof(tmp)))
      conv_id = atoll(tmp);
   if (tool_param_extract_custom(value, "node_id", tmp, sizeof(tmp)))
      node_id = atoll(tmp);

   /* Node-based expansion: return the node's prior summary for multi-resolution drill-down */
   if (node_id > 0) {
      int user_id = tool_get_current_user_id();
      if (user_id <= 0)
         return strdup(TOOL_RESULT_ERROR_MARK "Error: no authenticated user.");

      summary_node_t node = { 0 };
      if (summary_node_get(node_id, &node) != AUTH_DB_SUCCESS)
         return strdup(TOOL_RESULT_ERROR_MARK "Error: summary node not found.");

      /* Verify ownership via the node's conversation */
      conversation_t conv_check = { 0 };
      if (conv_db_get(node.conversation_id, user_id, &conv_check) != AUTH_DB_SUCCESS) {
         summary_node_free(&node);
         return strdup(TOOL_RESULT_ERROR_MARK "Error: access denied to that summary node.");
      }
      conv_free(&conv_check);

      /* Pre-fetch prior node to right-size the buffer */
      summary_node_t prior = { 0 };
      bool has_prior = false;
      if (node.prior_node_id > 0)
         has_prior = (summary_node_get(node.prior_node_id, &prior) == AUTH_DB_SUCCESS);

      size_t summary_len = node.summary_text ? strlen(node.summary_text) : 0;
      size_t prior_len = (has_prior && prior.summary_text) ? strlen(prior.summary_text) : 0;
      size_t buf_size = summary_len + prior_len + 512;
      char *buf = malloc(buf_size);
      if (!buf) {
         if (has_prior)
            summary_node_free(&prior);
         summary_node_free(&node);
         return strdup(TOOL_RESULT_ERROR_MARK "Error: memory allocation failed.");
      }

      size_t offset = 0;
      size_t cap = buf_size;
      int written;

      written = snprintf(buf, cap,
                         "Summary node %lld (depth %d, L%d, msgs %lld-%lld, conv %lld):\n\n",
                         (long long)node.id, node.depth, node.level + 1,
                         (long long)node.msg_id_start, (long long)node.msg_id_end,
                         (long long)node.conversation_id);
      if (written > 0 && (size_t)written < cap)
         offset = (size_t)written;

      if (has_prior && offset < cap - 1) {
         written = snprintf(buf + offset, cap - offset,
                            "Prior summary (node %lld, depth %d):\n%s\n\n", (long long)prior.id,
                            prior.depth, prior.summary_text ? prior.summary_text : "(empty)");
         if (written > 0 && offset + (size_t)written < cap)
            offset += (size_t)written;
         summary_node_free(&prior);
      } else if (has_prior) {
         summary_node_free(&prior);
      }

      if (offset < cap - 1) {
         written = snprintf(buf + offset, cap - offset, "This node's summary:\n%s\n",
                            node.summary_text ? node.summary_text : "(empty)");
         if (written > 0 && offset + (size_t)written < cap)
            offset += (size_t)written;
      }

      OLOG_INFO("context_expand: returned node %lld (depth %d, %zu bytes)", (long long)node_id,
                node.depth, offset);
      summary_node_free(&node);
      return buf;
   }

   if (start_id <= 0 || end_id <= 0)
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Error: start_id and end_id are required (positive integers).");
   if (end_id < start_id)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: end_id must be >= start_id.");
   if (end_id - start_id > 500)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: range too large (max 500 messages).");

   int user_id = tool_get_current_user_id();
   if (user_id <= 0)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: no authenticated user.");

   /* If conversation_id not provided, use current or its parent */
   if (conv_id <= 0) {
#ifdef ENABLE_WEBUI
      session_t *session = session_get_command_context();
      if (session) {
         conv_id = webui_get_active_conversation_id(session);
         if (conv_id > 0) {
            conversation_t conv = { 0 };
            if (conv_db_get(conv_id, user_id, &conv) == AUTH_DB_SUCCESS) {
               if (conv.continued_from > 0)
                  conv_id = conv.continued_from;
               conv_free(&conv);
            }
         }
      }
#endif
      if (conv_id <= 0)
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Error: conversation_id required (could not determine from context).");
   }

   OLOG_INFO("context_expand: expanding msgs %lld-%lld from conv %lld for user %d",
             (long long)start_id, (long long)end_id, (long long)conv_id, user_id);

   expand_ctx_t ec = { 0 };
   ec.capacity = EXPAND_CHAR_BUDGET + 256;
   ec.buf = malloc(ec.capacity);
   if (!ec.buf)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: memory allocation failed.");

   int header_len = snprintf(ec.buf, ec.capacity,
                             "Original messages %lld-%lld from conversation %lld:\n\n",
                             (long long)start_id, (long long)end_id, (long long)conv_id);
   if (header_len < 0)
      header_len = 0;
   if (header_len >= ec.capacity)
      header_len = ec.capacity - 1;
   ec.offset = header_len;

   /* include_private = true: user is expanding a [COMPACTED ...] block from
    * their own active session, which may itself be a private conversation.
    * Privacy is intra-conversation; ownership check still applies. */
   int rc = conv_db_get_messages_by_range(conv_id, user_id, start_id, end_id,
                                          /*include_private=*/true, expand_message_cb, &ec);

   if (rc == AUTH_DB_FORBIDDEN) {
      free(ec.buf);
      return strdup(TOOL_RESULT_ERROR_MARK "Error: access denied to that conversation.");
   }
   if (rc != AUTH_DB_SUCCESS) {
      free(ec.buf);
      return strdup(TOOL_RESULT_ERROR_MARK "Error: failed to retrieve messages.");
   }

   if (ec.count == 0) {
      free(ec.buf);
      return strdup("No messages found in the specified range.");
   }

   if (ec.offset >= EXPAND_CHAR_BUDGET) {
      snprintf(ec.buf + ec.offset, ec.capacity - ec.offset,
               "\n[... truncated at token budget, %d messages shown]", ec.count);
   }

   OLOG_INFO("context_expand: returned %d messages (%d bytes)", ec.count, ec.offset);

   return ec.buf;
}
