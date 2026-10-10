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
 * When compaction summarizes messages, the history holds a CONVERSATION
 * SUMMARY block in their place. This tool retrieves the original messages
 * by querying the database, making compaction non-destructive from the model's
 * perspective.
 */

#include "tools/context_expand_tool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "core/session_manager.h"
#include "llm/llm_third_party.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "tools/tool_registry.h"
#ifdef ENABLE_WEBUI
#include "webui/webui_server.h"
#endif

#define EXPAND_TOKEN_BUDGET 4000
#define EXPAND_CHAR_BUDGET (EXPAND_TOKEN_BUDGET * 4)

/* Whether @p target is @p turn_conv or one it continues (following
 * continued_from up, as far as a continuation chain can go). */
static bool in_turn_lineage(int64_t target, int64_t turn_conv, int user_id) {
   int64_t c = turn_conv;
   for (int i = 0; c > 0 && i < CONV_CHAIN_MAX; i++) {
      if (c == target) {
         return true;
      }
      conversation_t conv = { 0 };
      if (conv_db_get(c, user_id, &conv) != AUTH_DB_SUCCESS) {
         conv_free(&conv);
         return false;
      }
      c = conv.continued_from;
      conv_free(&conv);
   }
   return false;
}

/* Whether conversation @p conv_id's private text may come back into this turn
 * (in conversation @p turn_conv): only into the conversation itself or its
 * continuations; named from anywhere else it would carry private text into a
 * conversation that is not private (and on into memory).  A continuation made
 * public on its own must not pull its private parent's text into itself. */
static bool private_readable(int64_t conv_id, int64_t turn_conv, int user_id) {
   if (!in_turn_lineage(conv_id, turn_conv, user_id)) {
      return false;
   }
   if (conv_id == turn_conv) {
      return true;
   }
   bool target_private = false;
   bool turn_private = false;
   return conv_db_is_private(conv_id, user_id, &target_private) == AUTH_DB_SUCCESS &&
          conv_db_is_private(turn_conv, user_id, &turn_private) == AUTH_DB_SUCCESS &&
          (!target_private || turn_private);
}

/* Whether a summary of conversation @p conv_id may be shown in this turn: the
 * user's, and not private text leaving its lineage. */
static bool summary_readable(int64_t conv_id, int64_t turn_conv, int user_id) {
   bool is_private = false;
   if (conv_db_is_private(conv_id, user_id, &is_private) != AUTH_DB_SUCCESS) {
      return false; /* not the user's */
   }
   return !is_private || private_readable(conv_id, turn_conv, user_id);
}

/* This turn's conversation (not the one being viewed), or 0. */
static int64_t turn_conversation(void) {
   session_t *session = session_get_command_context();
   return session ? session_turn_conversation(session) : 0;
}

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
       .description = "First message ID of a range to show (with end_id). "
                      "Omit every ID to show what this conversation's summary replaced.",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "start_id",
   },
   {
       .name = "end_id",
       .description = "Last message ID of a range to show (with start_id).",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "end_id",
   },
   {
       .name = "conversation_id",
       .description = "Conversation of the range. "
                      "Omit to use the current conversation or its parent.",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "conversation_id",
   },
   {
       .name = "node_id",
       .description = "A summary node's ID (from an earlier expand): returns it and the "
                      "summary before it, for drill-down.",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "node_id",
   },
};

static const tool_metadata_t context_expand_metadata = {
   .name = "context_expand",
   .default_kind = TOOL_KIND_READ,
   .device_string = "context_expand",
   .topic = "dawn",
   .aliases = { NULL },
   .alias_count = 0,

   .description = "Show the original messages a CONVERSATION SUMMARY block replaced. "
                  "Call it with no IDs for the latest summary's messages; use "
                  "start_id/end_id for a range of message IDs, or node_id to drill down "
                  "through earlier summaries.",
   .params = context_expand_params,
   .param_count = TOOL_PARAM_COUNT(context_expand_params),

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
   const char *frame; /* a frame one of the rows was in (an email's first) */
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
   /* A stored email or page: its frame lines are defused when this result is
    * neutralized, so the whole result goes in its frame instead. */
   const char *frame = llm_third_party_present(msg->content);
   if (frame && (!ec->frame || strcmp(frame, TOOL_FRAME_EMAIL) == 0)) {
      ec->frame = frame;
   }
   return 0;
}

/* The range of the latest summary of this turn's conversation (or of the one
 * it continues); false when there is none. */
static bool latest_summary_range(int64_t *conv_out, int64_t *start_out, int64_t *end_out) {
   int64_t conv_id = turn_conversation();
   const int user_id = tool_get_current_user_id();
   if (conv_id <= 0 || user_id <= 0) {
      return false;
   }
   for (int hop = 0; hop < 2 && conv_id > 0; hop++) {
      conversation_t conv = { 0 };
      if (conv_db_get(conv_id, user_id, &conv) != AUTH_DB_SUCCESS) {
         conv_free(&conv);
         return false; /* not the user's */
      }
      const int64_t parent = conv.continued_from;
      conv_free(&conv);
      summary_node_t node = { 0 };
      if (summary_node_get_latest(conv_id, &node) == AUTH_DB_SUCCESS && node.msg_id_start > 0 &&
          node.msg_id_end >= node.msg_id_start) {
         *conv_out = conv_id;
         *start_out = node.msg_id_start;
         *end_out = node.msg_id_end;
         summary_node_free(&node);
         return true;
      }
      summary_node_free(&node);
      conv_id = parent;
   }
   return false;
}

static char *context_expand_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   *should_respond = 1;

   char tmp[32];
   int64_t start_id = 0, end_id = 0, conv_id = 0, node_id = 0;

   if (value && tool_param_extract_custom(value, "start_id", tmp, sizeof(tmp)))
      start_id = atoll(tmp);
   if (value && tool_param_extract_custom(value, "end_id", tmp, sizeof(tmp)))
      end_id = atoll(tmp);
   if (value && tool_param_extract_custom(value, "conversation_id", tmp, sizeof(tmp)))
      conv_id = atoll(tmp);
   if (value && tool_param_extract_custom(value, "node_id", tmp, sizeof(tmp)))
      node_id = atoll(tmp);

   /* Nothing named: the messages this conversation's summary replaced (the
    * CONVERSATION SUMMARY block carries no ids; its range is the latest
    * summary's).  The output budget bounds it, not the span of ids (global and
    * sparse). */
   bool latest_summary = false;
   if (node_id <= 0 && start_id <= 0 && end_id <= 0) {
      if (!latest_summary_range(&conv_id, &start_id, &end_id)) {
         return strdup("This conversation has no summary to expand.");
      }
      latest_summary = true;
   }

   /* Node-based expansion: return the node's prior summary for multi-resolution drill-down */
   if (node_id > 0) {
      int user_id = tool_get_current_user_id();
      if (user_id <= 0)
         return strdup(TOOL_RESULT_ERROR_MARK "Error: no authenticated user.");

      summary_node_t node = { 0 };
      /* One answer for a node that isn't there and one that isn't the user's:
       * node ids are sequential, and whose exist is not the model's to learn. */
      if (summary_node_get(node_id, &node) != AUTH_DB_SUCCESS) {
         summary_node_free(&node);
         return strdup(TOOL_RESULT_ERROR_MARK "Error: summary node not found.");
      }

      /* The user's own, and no private summary leaving its lineage (the prior
       * node may be another conversation's: the one this continues). */
      const int64_t turn_conv = turn_conversation();
      if (!summary_readable(node.conversation_id, turn_conv, user_id)) {
         summary_node_free(&node);
         return strdup(TOOL_RESULT_ERROR_MARK "Error: summary node not found.");
      }

      /* Pre-fetch prior node to right-size the buffer */
      summary_node_t prior = { 0 };
      bool has_prior = false;
      if (node.prior_node_id > 0) {
         has_prior = (summary_node_get(node.prior_node_id, &prior) == AUTH_DB_SUCCESS);
         if (has_prior && !summary_readable(prior.conversation_id, turn_conv, user_id)) {
            summary_node_free(&prior);
            has_prior = false;
         }
      }

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
   if (!latest_summary && end_id - start_id > 500)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: range too large (max 500 messages).");

   int user_id = tool_get_current_user_id();
   if (user_id <= 0)
      return strdup(TOOL_RESULT_ERROR_MARK "Error: no authenticated user.");

   /* The conversation this turn belongs to, and whether the model named one. */
   const int64_t turn_conv = turn_conversation();

   /* If conversation_id not provided, use current or its parent */
   if (conv_id <= 0) {
      session_t *session = session_get_command_context();
      if (session) {
         conv_id = session_turn_conversation(session); /* this turn's, not the view */
         if (conv_id > 0) {
            conversation_t conv = { 0 };
            if (conv_db_get(conv_id, user_id, &conv) == AUTH_DB_SUCCESS) {
               if (conv.continued_from > 0)
                  conv_id = conv.continued_from;
               conv_free(&conv);
            }
         }
      }
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

   /* A private conversation's messages come back only into its own lineage
    * (private_readable); ownership is checked either way. */
   const bool include_private = private_readable(conv_id, turn_conv, user_id);
   int rc = conv_db_get_messages_by_range(conv_id, user_id, start_id, end_id, /*max_rows=*/0,
                                          include_private, expand_message_cb, &ec);

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
   if (ec.frame) {
      llm_tools_result_third_party(ec.frame);
   }

   return ec.buf;
}
