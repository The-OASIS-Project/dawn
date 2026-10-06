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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Claude API format conversion utilities.
 * Converts OpenAI-format conversation history to Claude's native format.
 *
 * Extracted from llm_claude.c to reduce file size and improve maintainability.
 */

#include "llm/llm_claude_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "core/image_rehydrate.h"
#include "core/session_manager.h"
#include "llm/llm_cache_monitor.h"
#include "llm/llm_capabilities.h"
#include "llm/llm_claude_tools.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_interface.h"
#include "llm/llm_model_family.h"
#include "llm/llm_model_version.h"
#include "llm/llm_tool_images_render.h"
#include "llm/llm_tools.h"
#include "llm/llm_turn_blocks.h"
#include "logging.h"
#include "utils/string_utils.h"
#ifdef ENABLE_WEBUI
#include "webui/webui_server.h"
#endif

/**
 * @brief Check if current session is remote (WebSocket or DAP)
 */
static bool is_current_session_remote(void) {
   session_t *session = session_get_command_context();
   if (!session) {
      return false; /* No session context = local */
   }
   return (session->type != SESSION_TYPE_LOCAL);
}


/**
 * @brief Convert OpenAI image_url content block to Claude image format
 *
 * OpenAI format: {"type": "image_url", "image_url": {"url": "data:image/jpeg;base64,..."}}
 * Claude format: {"type": "image", "source": {"type": "base64", "media_type": "...", "data":
 * "..."}}
 *
 * @param block Content block to convert (or copy if not image_url)
 * @return New json_object in Claude format (caller must json_object_put), or NULL on error
 */
static json_object *convert_content_block_to_claude(json_object *block);

/* A tool result's @p parts converted into @p out.  An image that doesn't
 * convert (out of memory, not a data URI) is the fixed stand-in a reload
 * shows for a missing one, never dropped. */
static void convert_result_parts(json_object *parts, json_object *out) {
   const size_t n = json_object_array_length(parts);
   for (size_t i = 0; i < n; i++) {
      json_object *from = json_object_array_get_idx(parts, i);
      json_object *part = convert_content_block_to_claude(from);
      if (!part && llm_part_is_image(from)) {
         part = json_object_new_object();
         if (part) {
            json_object_object_add(part, "type", json_object_new_string("text"));
            json_object_object_add(part, "text",
                                   json_object_new_string(IMAGE_REHYDRATE_MISSING_TEXT));
         }
      }
      if (part) {
         json_object_array_add(out, part);
      }
   }
}

/* A tool_result @p block whose content is @p parts, its parts converted (an
 * image_url part an image block) and DAWN's own keys left out.  NULL on out
 * of memory. */
static json_object *convert_tool_result_to_claude(json_object *block, json_object *parts) {
   json_object *copy = json_object_new_object();
   json_object *converted = json_object_new_array();
   if (!copy || !converted) {
      json_object_put(copy);
      json_object_put(converted);
      return NULL;
   }
   convert_result_parts(parts, converted);
   json_object_object_foreach(block, key, val) {
      if (key[0] == '_') {
         continue;
      }
      json_object_object_add(copy, key,
                             strcmp(key, "content") == 0 ? json_object_get(converted)
                                                         : json_object_get(val));
   }
   json_object_put(converted);
   return copy;
}

static json_object *convert_content_block_to_claude(json_object *block) {
   if (!block) {
      return NULL;
   }

   json_object *type_obj;
   if (!json_object_object_get_ex(block, "type", &type_obj)) {
      // No type field - Claude requires type on all content blocks
      // Check if it has a "text" field (possible malformed text block)
      json_object *text_obj;
      if (json_object_object_get_ex(block, "text", &text_obj)) {
         // Convert to proper text block
         json_object *fixed_block = json_object_new_object();
         json_object_object_add(fixed_block, "type", json_object_new_string("text"));
         json_object_object_add(fixed_block, "text", json_object_get(text_obj));
         OLOG_WARNING("Claude: Fixed content block missing type (had text field)");
         return fixed_block;
      }
      // Unknown format - skip it to avoid Claude API errors
      OLOG_WARNING("Claude: Skipping content block with no type field");
      return NULL;
   }

   const char *block_type = json_object_get_string(type_obj);
   json_object *result_parts = NULL;
   if (block_type && strcmp(block_type, "tool_result") == 0 &&
       json_object_object_get_ex(block, "content", &result_parts) &&
       json_object_is_type(result_parts, json_type_array)) {
      /* A result that carried images: its parts converted too (an image is
       * an image_url part in every history, llm_tool_images_render.h). */
      return convert_tool_result_to_claude(block, result_parts);
   }
   if (!block_type || strcmp(block_type, "image_url") != 0) {
      /* Not image_url: the block as it is, less any key of DAWN's own (a
       * turn's context parts carry their kind, a result's images their
       * stored id). */
      return llm_history_wire_copy_object(block);
   }

   // This is an OpenAI image_url block - convert to Claude format
   json_object *image_url_obj;
   if (!json_object_object_get_ex(block, "image_url", &image_url_obj)) {
      OLOG_WARNING("Claude: image_url block missing image_url field");
      return NULL;
   }

   json_object *url_obj;
   if (!json_object_object_get_ex(image_url_obj, "url", &url_obj)) {
      OLOG_WARNING("Claude: image_url block missing url field");
      return NULL;
   }

   const char *url = json_object_get_string(url_obj);
   if (!url) {
      return NULL;
   }

   // Parse data URL: data:image/jpeg;base64,<data>
   const char *data_prefix = "data:";
   if (strncmp(url, data_prefix, strlen(data_prefix)) != 0) {
      OLOG_WARNING("Claude: image_url is not a data URL: %.50s...", url);
      return NULL;
   }

   // Find media type and base64 data
   const char *after_data = url + strlen(data_prefix);
   const char *semicolon = strchr(after_data, ';');
   const char *comma = strchr(after_data, ',');

   if (!semicolon || !comma || comma < semicolon) {
      OLOG_WARNING("Claude: Invalid data URL format");
      return NULL;
   }

   // Extract media type
   size_t media_type_len = semicolon - after_data;
   char media_type[64];
   if (media_type_len >= sizeof(media_type)) {
      media_type_len = sizeof(media_type) - 1;
   }
   strncpy(media_type, after_data, media_type_len); /* strncpy-ok: copies only the media-type
                                                       substring (data:<type>;), length clamped to
                                                       sizeof-1 above; explicit NUL follows */
   media_type[media_type_len] = '\0';

   // The base64 data is after the comma
   const char *base64_data = comma + 1;

   // Create Claude-format image block
   json_object *image_obj = json_object_new_object();
   json_object_object_add(image_obj, "type", json_object_new_string("image"));

   json_object *source_obj = json_object_new_object();
   json_object_object_add(source_obj, "type", json_object_new_string("base64"));
   json_object_object_add(source_obj, "media_type", json_object_new_string(media_type));
   json_object_object_add(source_obj, "data", json_object_new_string(base64_data));
   json_object_object_add(image_obj, "source", source_obj);

   OLOG_INFO("Claude: Converted OpenAI image_url to Claude image (media_type=%s)", media_type);
   return image_obj;
}

static const char *msg_role(json_object *msg);
static const char *part_type(json_object *part);
static bool parts_have_type(json_object *parts, const char *type);
static json_object *text_block(const char *text);

/* Longest text kept from a tool result turned into a note. */
#define ORPHAN_RESULT_TEXT_MAX 1900

/* @p msg's role, when it's an object with one. */
static const char *msg_role(json_object *msg) {
   json_object *role = NULL;
   return (msg && json_object_object_get_ex(msg, "role", &role)) ? json_object_get_string(role)
                                                                 : NULL;
}

/* @p msg's content array, or NULL. */
static json_object *msg_parts(json_object *msg) {
   json_object *content = NULL;
   return (msg && json_object_object_get_ex(msg, "content", &content) &&
           json_object_is_type(content, json_type_array))
              ? content
              : NULL;
}

/* A part's type, and its call id: a tool_use's id or a tool_result's tool_use_id. */
static const char *part_type(json_object *part) {
   json_object *t = NULL;
   return json_object_object_get_ex(part, "type", &t) ? json_object_get_string(t) : NULL;
}

static const char *part_call_id(json_object *part) {
   const char *type = part_type(part);
   json_object *id = NULL;
   const char *key = (type && strcmp(type, "tool_use") == 0)      ? "id"
                     : (type && strcmp(type, "tool_result") == 0) ? "tool_use_id"
                                                                  : NULL;
   return (key && json_object_object_get_ex(part, key, &id)) ? json_object_get_string(id) : NULL;
}

/* Whether @p parts holds a part of type @p type for call @p id. */
static bool parts_have_call(json_object *parts, const char *type, const char *id) {
   const size_t n = parts ? json_object_array_length(parts) : 0;
   for (size_t i = 0; id && i < n; i++) {
      json_object *p = json_object_array_get_idx(parts, i);
      const char *t = part_type(p);
      const char *pid = part_call_id(p);
      if (t && pid && strcmp(t, type) == 0 && strcmp(pid, id) == 0) {
         return true;
      }
   }
   return false;
}

/* Whether @p parts holds a part of type @p type. */
static bool parts_have_type(json_object *parts, const char *type) {
   const size_t n = parts ? json_object_array_length(parts) : 0;
   for (size_t i = 0; i < n; i++) {
      const char *t = part_type(json_object_array_get_idx(parts, i));
      if (t && strcmp(t, type) == 0) {
         return true;
      }
   }
   return false;
}

/* A tool_result part's text: its content string, or its text parts joined.
 * Returns whether it was cut to fit @p out. */
static bool tool_result_text(json_object *block, char *out, size_t out_len) {
   out[0] = '\0';
   json_object *content = NULL;
   if (!json_object_object_get_ex(block, "content", &content) || !content) {
      return false;
   }
   if (!json_object_is_type(content, json_type_array)) {
      const int w = snprintf(out, out_len, "%s", json_object_get_string(content));
      return w >= 0 && (size_t)w >= out_len;
   }
   size_t off = 0;
   bool cut = false;
   const size_t n = json_object_array_length(content);
   for (size_t i = 0; i < n; i++) {
      json_object *p = json_object_array_get_idx(content, i), *text = NULL;
      if (!json_object_object_get_ex(p, "text", &text) || !json_object_get_string(text)) {
         continue;
      }
      if (off + 1 >= out_len) {
         cut = true;
         break;
      }
      const int w = snprintf(out + off, out_len - off, "%s%s", off ? "\n" : "",
                             json_object_get_string(text));
      if (w < 0) {
         continue;
      }
      if ((size_t)w >= out_len - off) {
         cut = true;
         off = out_len - 1;
      } else {
         off += (size_t)w;
      }
   }
   return cut;
}

/* A note standing in for a tool result that can't be sent as one. */
static json_object *result_note(json_object *block) {
   char text[ORPHAN_RESULT_TEXT_MAX + 1];
   const bool cut = tool_result_text(block, text, sizeof(text));
   char note[ORPHAN_RESULT_TEXT_MAX + 64];
   snprintf(note, sizeof(note), "[Earlier tool result: %s%s]", text, cut ? "..." : "");
   sanitize_utf8_for_json(note); /* a byte cut can split a character */
   json_object *out = json_object_new_object();
   if (out) {
      json_object_object_add(out, "type", json_object_new_string("text"));
      json_object_object_add(out, "text", json_object_new_string(note));
   }
   return out;
}

/* @p msg's content replaced by @p parts (taking them). */
static void set_parts(json_object *msg, json_object *parts) {
   json_object_object_add(msg, "content", parts);
}

/* ---- Request context (llm_history_kind.h) ---- */

static json_object *text_block(const char *text) {
   json_object *block = json_object_new_object();
   if (block) {
      json_object_object_add(block, "type", json_object_new_string("text"));
      json_object_object_add(block, "text", json_object_new_string(text ? text : ""));
   }
   return block;
}

static json_object *note_block(const char *label, const char *text) {
   const size_t len = strlen(label) + (text ? strlen(text) : 0) + 1;
   char *note = malloc(len);
   if (!note) {
      return NULL;
   }
   snprintf(note, len, "%s%s", label, text ? text : "");
   json_object *block = text_block(note);
   free(note);
   return block;
}

/* @p msg's content as an array of blocks (a string becomes one text block). */
static json_object *content_blocks(json_object *msg) {
   json_object *content = NULL;
   json_object_object_get_ex(msg, "content", &content);
   if (json_object_is_type(content, json_type_array)) {
      return content;
   }
   json_object *blocks = json_object_new_array();
   if (!blocks) {
      return NULL;
   }
   const char *text = json_object_get_string(content);
   if (text && *text) {
      json_object_array_add(blocks, text_block(text));
   }
   json_object_object_add(msg, "content", blocks);
   return blocks;
}

/**
 * A directive or an instruction change, where it sits: after the turn's user
 * message.  On a model that takes one it is a `role: "system"` message (a run
 * of them is one message: each must be followed by an assistant turn); on any
 * other it is a note at the end of that user message.  With no user message
 * before it (a question that was never saved) the note opens a user message
 * of its own, which the next user message joins.
 */
static void add_operator_message(json_object *messages,
                                 const char *label,
                                 const char *text,
                                 bool mid_system,
                                 json_object **last_message,
                                 const char **last_role) {
   if (!text || !*text) {
      return;
   }
   if (*last_role && strcmp(*last_role, "system") == 0 && *last_message) {
      json_object *content = NULL;
      json_object_object_get_ex(*last_message, "content", &content);
      if (json_object_is_type(content, json_type_array)) {
         json_object *block = text_block(text); /* beside a tool change's blocks */
         if (block) {
            json_object_array_add(content, block);
         }
         return;
      }
      const char *before = json_object_get_string(content);
      const size_t len = strlen(before ? before : "") + strlen(text) + 3;
      char *joined = malloc(len);
      if (joined) {
         snprintf(joined, len, "%s\n\n%s", before ? before : "", text);
         json_object_object_add(*last_message, "content", json_object_new_string(joined));
         free(joined);
      }
      return;
   }
   const bool after_user = *last_role && strcmp(*last_role, "user") == 0 && *last_message;
   if (after_user && mid_system) {
      json_object *sys = json_object_new_object();
      if (!sys) {
         return;
      }
      json_object_object_add(sys, "role", json_object_new_string("system"));
      json_object_object_add(sys, "content", json_object_new_string(text));
      json_object_array_add(messages, sys);
      *last_message = sys;
      *last_role = "system";
      return;
   }
   if (!after_user) {
      json_object *user = json_object_new_object();
      if (!user) {
         return;
      }
      json_object_object_add(user, "role", json_object_new_string("user"));
      json_object_object_add(user, "content", json_object_new_array());
      json_object_array_add(messages, user);
      *last_message = user;
      *last_role = "user";
   }
   json_object *blocks = content_blocks(*last_message);
   json_object *note = blocks ? note_block(label, text) : NULL;
   if (note) {
      json_object_array_add(blocks, note);
   }
}

/* Append @p from's blocks to @p to's (both user messages). */
static void join_user_messages(json_object *to, json_object *from) {
   json_object *into = content_blocks(to);
   json_object *add = content_blocks(from);
   const size_t n = add ? json_object_array_length(add) : 0;
   for (size_t i = 0; into && i < n; i++) {
      json_object_array_add(into, json_object_get(json_object_array_get_idx(add, i)));
   }
}

/**
 * A system message must follow a user message and be the last message or be
 * followed by an assistant turn.  One that isn't (a reply that was never
 * saved, so the next question follows it) becomes a note in the user message
 * before it instead, and that user message takes in the one after.
 */
static void place_system_messages(json_object *messages, const char *label) {
   for (size_t i = 0; i < json_object_array_length(messages); i++) {
      json_object *msg = json_object_array_get_idx(messages, i);
      if (!msg_role(msg) || strcmp(msg_role(msg), "system") != 0) {
         continue;
      }
      json_object *prev = i > 0 ? json_object_array_get_idx(messages, i - 1) : NULL;
      json_object *next = i + 1 < json_object_array_length(messages)
                              ? json_object_array_get_idx(messages, i + 1)
                              : NULL;
      const bool prev_user = prev && msg_role(prev) && strcmp(msg_role(prev), "user") == 0;
      const bool next_ok = !next || (msg_role(next) && strcmp(msg_role(next), "assistant") == 0);
      json_object *content = NULL;
      json_object_object_get_ex(msg, "content", &content);
      /* One carrying a tool change was placed where it may sit (a note
       * can't carry a definition). */
      if ((prev_user && next_ok) || json_object_is_type(content, json_type_array)) {
         continue;
      }
      json_object *note = note_block(label, json_object_get_string(content));
      if (prev_user) {
         json_object *blocks = content_blocks(prev);
         if (blocks && note) {
            json_object_array_add(blocks, note);
            note = NULL;
         }
         json_object_array_del_idx(messages, i, 1);
         if (next && msg_role(next) && strcmp(msg_role(next), "user") == 0) {
            join_user_messages(prev, next);
            json_object_array_del_idx(messages, i, 1);
         }
      } else {
         /* No user message before it: it becomes one. */
         json_object_object_add(msg, "role", json_object_new_string("user"));
         json_object *blocks = json_object_new_array();
         if (blocks && note) {
            json_object_array_add(blocks, note);
            note = NULL;
         }
         json_object_object_add(msg, "content", blocks);
         if (next && msg_role(next) && strcmp(msg_role(next), "user") == 0) {
            join_user_messages(msg, next);
            json_object_array_del_idx(messages, i + 1, 1);
         }
      }
      json_object_put(note);
      i = i > 0 ? i - 1 : 0; /* look at what's there now */
   }
}

/**
 * Cache breakpoint on the conversation: the last block of the last user
 * message, so the next request (the next tool round, or the next turn) reads
 * everything up to here from the cache.  With the tools' and the system
 * prompt's, three of the four breakpoints a request may carry.
 */
static void mark_conversation_breakpoint(json_object *messages) {
   for (size_t i = json_object_array_length(messages); i-- > 0;) {
      json_object *msg = json_object_array_get_idx(messages, i);
      if (!msg_role(msg) || strcmp(msg_role(msg), "user") != 0) {
         continue;
      }
      json_object *blocks = content_blocks(msg);
      size_t n = blocks ? json_object_array_length(blocks) : 0;
      /* The last block that can take one: an empty text block can't. */
      json_object *text = NULL;
      while (n > 0 &&
             json_object_object_get_ex(json_object_array_get_idx(blocks, n - 1), "text", &text) &&
             !json_object_get_string_len(text)) {
         n--;
      }
      if (n == 0) {
         return;
      }
      json_object *last = json_object_array_get_idx(blocks, n - 1);
      json_object *copy = json_object_new_object();
      json_object *cc = json_object_new_object();
      if (!copy || !cc) {
         json_object_put(copy);
         json_object_put(cc);
         return;
      }
      /* A copy: the block may be shared with the history. */
      json_object_object_foreach(last, key, val) {
         json_object_object_add(copy, key, json_object_get(val));
      }
      json_object_object_add(cc, "type", json_object_new_string("ephemeral"));
      json_object_object_add(copy, "cache_control", cc);
      json_object_array_put_idx(blocks, n - 1, copy);
      return;
   }
}

/**
 * @brief Make every tool call and result a pair Claude accepts
 *
 * Claude requires each tool_use to be answered by a tool_result in the very
 * next message, each tool_result to answer a tool_use in the message just
 * before it, and a message's tool_results to come before its other content.
 * A history can still break that: a compaction point recorded inside a tool
 * exchange by an older build, a conversion from another provider, a call
 * whose result was lost.  Any break fails the request, and every later one.
 * Per adjacent assistant/user pair: a call without its result there is
 * dropped (an assistant left empty says so), and a result without its call
 * there becomes a note with its text, so nothing the model saw is lost.
 *
 * @return How many calls and results were changed
 */
static int repair_tool_pairs(json_object *messages) {
   int changed = 0;
   const size_t n = json_object_array_length(messages);
   for (size_t i = 0; i < n; i++) {
      json_object *msg = json_object_array_get_idx(messages, i);
      const char *role = msg_role(msg);
      json_object *parts = msg_parts(msg);
      if (!role || !parts) {
         continue;
      }
      if (strcmp(role, "assistant") == 0) {
         json_object *next = (i + 1 < n) ? json_object_array_get_idx(messages, i + 1) : NULL;
         const char *next_role = msg_role(next);
         json_object *answers = (next_role && strcmp(next_role, "user") == 0) ? msg_parts(next)
                                                                              : NULL;
         json_object *kept = json_object_new_array();
         const size_t k = json_object_array_length(parts);
         for (size_t j = 0; kept && j < k; j++) {
            json_object *p = json_object_array_get_idx(parts, j);
            const char *t = part_type(p);
            if (t && strcmp(t, "tool_use") == 0 &&
                !parts_have_call(answers, "tool_result", part_call_id(p))) {
               changed++;
               continue;
            }
            json_object_array_add(kept, json_object_get(p));
         }
         if (!kept || json_object_array_length(kept) == k) {
            json_object_put(kept);
            continue;
         }
         /* Nothing said and nothing called: a turn of reasoning alone isn't one
          * Claude takes.  Its thinking goes, a note says what happened, and any
          * other part (a server tool's call and result) stays.  (Editing an
          * earlier turn never fails the request: DAWN asks for such thinking
          * to be dropped, not refused.) */
         if (!parts_have_type(kept, "text") && !parts_have_type(kept, "tool_use")) {
            json_object *rest = json_object_new_array();
            json_object *note = rest ? json_object_new_object() : NULL;
            if (!note) {
               json_object_put(rest);
               json_object_put(kept);
               continue;
            }
            const size_t r = json_object_array_length(kept);
            for (size_t j = 0; j < r; j++) {
               json_object *p = json_object_array_get_idx(kept, j);
               const char *t = part_type(p);
               if (t && (strcmp(t, "thinking") == 0 || strcmp(t, "redacted_thinking") == 0)) {
                  continue;
               }
               json_object_array_add(rest, json_object_get(p));
            }
            json_object_object_add(note, "type", json_object_new_string("text"));
            json_object_object_add(note, "text",
                                   json_object_new_string("[Tool call not completed]"));
            json_object_array_add(rest, note);
            json_object_put(kept);
            kept = rest;
         }
         set_parts(msg, kept);
      } else if (strcmp(role, "user") == 0) {
         json_object *prev = (i > 0) ? json_object_array_get_idx(messages, i - 1) : NULL;
         const char *prev_role = msg_role(prev);
         json_object *calls = (prev_role && strcmp(prev_role, "assistant") == 0) ? msg_parts(prev)
                                                                                 : NULL;
         /* Results first, then everything else, in order. */
         json_object *results = json_object_new_array();
         json_object *rest = json_object_new_array();
         bool reordered = false;
         bool seen_other = false;
         const size_t k = json_object_array_length(parts);
         for (size_t j = 0; results && rest && j < k; j++) {
            json_object *p = json_object_array_get_idx(parts, j);
            const char *t = part_type(p);
            if (!t || strcmp(t, "tool_result") != 0) {
               json_object_array_add(rest, json_object_get(p));
               seen_other = true;
               continue;
            }
            if (!parts_have_call(calls, "tool_use", part_call_id(p))) {
               json_object *note = result_note(p);
               if (note) {
                  json_object_array_add(rest, note);
               }
               changed++;
               reordered = true;
               continue;
            }
            reordered = reordered || seen_other;
            json_object_array_add(results, json_object_get(p));
         }
         if (!results || !rest || !reordered) {
            json_object_put(results);
            json_object_put(rest);
            continue;
         }
         const size_t r = json_object_array_length(rest);
         for (size_t j = 0; j < r; j++) {
            json_object_array_add(results, json_object_get(json_object_array_get_idx(rest, j)));
         }
         json_object_put(rest);
         set_parts(msg, results);
      }
   }
   if (changed > 0) {
      OLOG_WARNING("Claude: %d tool call(s)/result(s) without their pair repaired", changed);
   }
   return changed;
}

json_object *convert_to_claude_format(struct json_object *openai_conversation,
                                      const char *input_text,
                                      const char *model,
                                      const char *carrier,
                                      int iteration,
                                      bool inline_tools) {
   /* A model that takes no images reads a fixed text for each a tool returned. */
   json_object *shown_history = NULL;
   if (!is_vision_enabled_for_current_llm()) {
      shown_history = llm_tool_images_without(openai_conversation);
      if (!shown_history) {
         return NULL;
      }
      openai_conversation = shown_history;
   }
   json_object *claude_request = json_object_new_object();

   // Model: use passed model, or fall back to config default
   const char *model_name = model;
   if (!model_name || model_name[0] == '\0') {
      model_name = llm_get_default_claude_model();
   }
   json_object_object_add(claude_request, "model", json_object_new_string(model_name));
   /* models.toml knows the model by Anthropic's id, not an OpenRouter slug. */
   char model_id[LLM_MODEL_NAME_MAX];
   const cloud_provider_t model_provider = llm_model_anthropic_id(model_name, model_id,
                                                                  sizeof(model_id));

   /* Reasoning: the session's mode and effort, resolved against what this
    * model accepts (models.toml [thinking.anthropic]), sent explicitly.  Never
    * omitted: on current Claude models an omitted `thinking` runs adaptive at
    * the model's default effort, whatever the user picked. */
   llm_thinking_resolved_t thinking;
   llm_thinking_resolve_current(LLM_CLOUD, model_provider, model_name, &thinking);
   const int thinking_budget = (thinking.controllable && thinking.budget)
                                   ? llm_budget_tokens_for_effort(thinking.effort)
                                   : 0;

   /* max_tokens must exceed a thinking budget: leave room for the answer. */
   int max_tokens = g_config.llm.max_tokens;
   if (thinking_budget > 0 && max_tokens <= thinking_budget) {
      max_tokens = thinking_budget + 4096;
      OLOG_INFO("Claude: Adjusted max_tokens to %d (budget %d + 4096 response buffer)", max_tokens,
                thinking_budget);
   }
   json_object_object_add(claude_request, "max_tokens", json_object_new_int(max_tokens));

   if (thinking.controllable) {
      json_object *thinking_obj = json_object_new_object();
      json_object_object_add(thinking_obj, "type",
                             json_object_new_string(llm_think_mode_name(thinking.mode)));
      if (thinking.mode == LLM_THINK_ADAPTIVE) {
         /* The default display is "omitted" (empty thinking text); ask for a
          * summary so DAWN's thinking UI shows the reasoning. */
         json_object_object_add(thinking_obj, "display", json_object_new_string("summarized"));
         json_object *output_config = json_object_new_object();
         json_object_object_add(output_config, "effort", json_object_new_string(thinking.effort));
         json_object_object_add(claude_request, "output_config", output_config);
      } else if (thinking.mode == LLM_THINK_ENABLED) {
         json_object_object_add(thinking_obj, "budget_tokens",
                                json_object_new_int(thinking_budget));
      }
      json_object_object_add(claude_request, "thinking", thinking_obj);
   }
   OLOG_INFO("Claude: thinking=%s%s%s model=%s%s", llm_think_mode_name(thinking.mode),
             thinking.effort[0] ? "/" : "", thinking.effort, model_name,
             thinking.clamped ? " (setting resolved to what the model accepts)" : "");

   /* The request's tools; a conversation's tool changes go in place only
    * beside its own set (not on a no-tools turn, not on a research run's). */
   const bool tools_in_place = llm_tools_enabled(NULL) &&
                               llm_claude_tools_add(claude_request, openai_conversation,
                                                    inline_tools, is_current_session_remote(),
                                                    iteration) &&
                               inline_tools;

   // Extract system message and user/assistant messages
   json_object *system_array = json_object_new_array();
   json_object *messages_array = json_object_new_array();

   int conv_len = json_object_array_length(openai_conversation);
   const char *last_role = NULL;
   json_object *last_message = NULL;
   char note_label[LLM_CONTEXT_TAG_MAX + 24];
   llm_operator_note_label(llm_history_tag(openai_conversation), note_label, sizeof(note_label));
   const bool mid_system = llm_model_mid_system(model_id);

   /* Tool calls and results are paired once, after every message is built
    * (repair_tool_pairs), not filtered here message by message. */

   for (int i = 0; i < conv_len; i++) {
      json_object *msg = json_object_array_get_idx(openai_conversation, i);
      json_object *role_obj, *content_obj;

      if (!json_object_object_get_ex(msg, "role", &role_obj) ||
          !json_object_object_get_ex(msg, "content", &content_obj)) {
         continue;
      }

      const char *role = json_object_get_string(role_obj);

      /* An assistant turn with provider-neutral blocks is rendered from them:
       * its text, tool calls and Anthropic reasoning, exactly as produced. */
      json_object *rendered_blocks = NULL;
      json_object *blocks_obj = NULL;
      if (strcmp(role, "assistant") == 0 &&
          json_object_object_get_ex(msg, LLM_TURN_BLOCKS_KEY, &blocks_obj)) {
         rendered_blocks = llm_turn_blocks_render_claude(blocks_obj, carrier);
         if (rendered_blocks) {
            content_obj = rendered_blocks;
         }
      }

      // Claude-format assistant messages (a content array)
      if (strcmp(role, "assistant") == 0 && json_object_is_type(content_obj, json_type_array)) {
         int content_len = json_object_array_length(content_obj);
         json_object *filtered_content = json_object_new_array();

         for (int j = 0; j < content_len; j++) {
            json_object *block = json_object_array_get_idx(content_obj, j);
            json_object *type_obj;

            if (!json_object_object_get_ex(block, "type", &type_obj)) {
               // Skip malformed blocks without type - Claude requires type field
               OLOG_WARNING("Claude: Skipping content block without type field");
               continue;
            }

            /* The block as produced, less any key of DAWN's own. */
            json_object *plain = convert_content_block_to_claude(block);
            if (plain) {
               json_object_array_add(filtered_content, plain);
            }
         }

         // Only add if we have content
         if (json_object_array_length(filtered_content) > 0) {
            last_message = json_object_new_object();
            json_object_object_add(last_message, "role", json_object_new_string("assistant"));
            json_object_object_add(last_message, "content", filtered_content);
            json_object_array_add(messages_array, last_message);
            last_role = "assistant";
         } else {
            json_object_put(filtered_content);
         }
         json_object_put(rendered_blocks);
         continue;
      }

      // Convert assistant messages with tool_calls (OpenAI format) to Claude tool_use format
      json_object *tool_calls_obj;
      if (strcmp(role, "assistant") == 0 &&
          json_object_object_get_ex(msg, "tool_calls", &tool_calls_obj)) {
         // Build Claude content array with tool_use blocks
         json_object *content_array = json_object_new_array();

         // Include any text content first
         const char *text_content = json_object_get_string(content_obj);
         if (text_content && strlen(text_content) > 0) {
            json_object *text_block = json_object_new_object();
            json_object_object_add(text_block, "type", json_object_new_string("text"));
            json_object_object_add(text_block, "text", json_object_new_string(text_content));
            json_object_array_add(content_array, text_block);
         }

         // Convert each tool call to Claude tool_use format
         int num_calls = json_object_array_length(tool_calls_obj);
         int added_tool_uses = 0;
         for (int j = 0; j < num_calls; j++) {
            json_object *call = json_object_array_get_idx(tool_calls_obj, j);
            json_object *func_obj, *id_obj, *name_obj, *args_obj;

            const char *call_id = "";
            const char *name = "";
            const char *args_str = "{}";

            if (json_object_object_get_ex(call, "id", &id_obj)) {
               call_id = json_object_get_string(id_obj);
            }

            if (json_object_object_get_ex(call, "function", &func_obj)) {
               if (json_object_object_get_ex(func_obj, "name", &name_obj)) {
                  name = json_object_get_string(name_obj);
               }
               if (json_object_object_get_ex(func_obj, "arguments", &args_obj)) {
                  args_str = json_object_get_string(args_obj);
               }
            }

            // Create tool_use block
            json_object *tool_use = json_object_new_object();
            json_object_object_add(tool_use, "type", json_object_new_string("tool_use"));
            json_object_object_add(tool_use, "id", json_object_new_string(call_id));
            json_object_object_add(tool_use, "name", json_object_new_string(name));

            // Parse args string to JSON object
            json_object *input = json_tokener_parse(args_str);
            if (input) {
               json_object_object_add(tool_use, "input", input);
            } else {
               json_object_object_add(tool_use, "input", json_object_new_object());
            }

            json_object_array_add(content_array, tool_use);
            added_tool_uses++;
         }

         // Silence unused variable warning
         (void)added_tool_uses;

         // Only add assistant message if we have content (text or tool_use blocks)
         if (json_object_array_length(content_array) > 0) {
            last_message = json_object_new_object();
            json_object_object_add(last_message, "role", json_object_new_string("assistant"));
            json_object_object_add(last_message, "content", content_array);
            json_object_array_add(messages_array, last_message);
            last_role = "assistant";
         } else {
            json_object_put(content_array);  // Free unused array
         }
         continue;
      }

      // Convert tool role messages (OpenAI format) to Claude tool_result format
      if (strcmp(role, "tool") == 0) {
         json_object *tool_call_id_obj;
         const char *tool_call_id = NULL;
         const char *result_content = json_object_get_string(content_obj);

         if (json_object_object_get_ex(msg, "tool_call_id", &tool_call_id_obj)) {
            tool_call_id = json_object_get_string(tool_call_id_obj);
         }

         // Handle orphaned tool messages (restored from DB without tool_call_id)
         if (!tool_call_id || strlen(tool_call_id) == 0) {
            OLOG_WARNING(
                "Claude: Orphaned tool message without tool_call_id, converting to summary");
            // Convert to assistant summary to preserve context
            if (result_content && strlen(result_content) > 0) {
               char summary[2048];
               snprintf(summary, sizeof(summary), "[Previous tool result: %.1900s%s]",
                        result_content, strlen(result_content) > 1900 ? "..." : "");
               /* `%.1900s` is a BYTE precision, so it splits multi-byte characters —
                * and an invalid sequence here fails the ENTIRE provider request, not
                * just this message.  This is the more frequent of the two cuts in this
                * branch: it runs for every orphaned tool result, while the 4096-byte
                * combine below only runs when appending to an existing assistant turn.
                * Both are sanitized; neither is safe alone, since the combine can land
                * mid-codepoint on the concatenation even when its inputs are clean. */
               sanitize_utf8_for_json(summary);

               json_object *summary_msg = json_object_new_object();
               json_object_object_add(summary_msg, "role", json_object_new_string("assistant"));
               json_object_object_add(summary_msg, "content", json_object_new_string(summary));

               // Handle role alternation
               if (last_role != NULL && strcmp(last_role, "assistant") == 0 &&
                   last_message != NULL) {
                  // Append to existing assistant content
                  json_object *last_content;
                  if (json_object_object_get_ex(last_message, "content", &last_content)) {
                     if (json_object_is_type(last_content, json_type_array)) {
                        // Content is an array - append a text block
                        json_object *text_block = json_object_new_object();
                        json_object_object_add(text_block, "type", json_object_new_string("text"));
                        json_object_object_add(text_block, "text", json_object_new_string(summary));
                        json_object_array_add(last_content, text_block);
                     } else {
                        // Content is a string - combine strings
                        const char *existing = json_object_get_string(last_content);
                        size_t existing_len = existing ? strlen(existing) : 0;
                        size_t summary_len = strlen(summary);
                        size_t needed = existing_len + summary_len + 2; /* +2 for \n and \0 */

                        char combined[4096];
                        if (needed > sizeof(combined)) {
                           OLOG_WARNING("Claude: Combined content truncated from %zu to %zu bytes",
                                        needed, sizeof(combined));
                        }
                        snprintf(combined, sizeof(combined), "%s\n%s", existing ? existing : "",
                                 summary);
                        /* snprintf cuts on a BYTE boundary, so a multi-byte character
                         * straddling the limit leaves a partial sequence — and the whole
                         * request then dies at the provider, not just this field:
                         * "The request body is not valid JSON: str is not valid UTF-8:
                         * surrogates not allowed" (HTTP 400).  Live-verified on a resumed
                         * research job (conv 1041), which is the likely victim: resume
                         * hydrates history whose tool rows arrive orphaned, this branch
                         * combines their summaries, and search results are full of
                         * non-ASCII, so the cut lands mid-codepoint often. */
                        sanitize_utf8_for_json(combined);
                        json_object_object_add(last_message, "content",
                                               json_object_new_string(combined));
                     }
                  }
                  json_object_put(summary_msg);
               } else {
                  json_object_array_add(messages_array, summary_msg);
                  last_message = summary_msg;
                  last_role = "assistant";
               }
            }
            continue;
         }

         // Create tool_result block in a user message (Claude requirement)
         json_object *result_array = json_object_new_array();
         json_object *result_block = json_object_new_object();
         json_object_object_add(result_block, "type", json_object_new_string("tool_result"));
         json_object_object_add(result_block, "tool_use_id", json_object_new_string(tool_call_id));
         if (json_object_is_type(content_obj, json_type_array)) {
            /* A result that carried images: its text and image parts, in the
             * result, as a live Claude history holds them. */
            json_object *parts = json_object_new_array();
            if (parts) {
               convert_result_parts(content_obj, parts);
            }
            json_object_object_add(result_block, "content", parts);
         } else {
            json_object_object_add(result_block, "content",
                                   json_object_new_string(result_content ? result_content : ""));
         }
         json_object_array_add(result_array, result_block);

         // Tool results must be in user messages for Claude
         if (last_role != NULL && strcmp(last_role, "user") == 0 && last_message != NULL) {
            // Append to existing user message content array
            json_object *last_content = NULL;
            if (json_object_object_get_ex(last_message, "content", &last_content) &&
                json_object_is_type(last_content, json_type_array)) {
               /* result_block is already owned by result_array (added above); take a second
                * reference for last_content before freeing the wrapper, otherwise the put()
                * below frees result_block out from under last_content — a use-after-free /
                * double-free that crashes on consecutive tool results (e.g. parallel tool
                * calls producing several tool_result messages in a row). */
               json_object_array_add(last_content, json_object_get(result_block));
               json_object_put(result_array);  // Don't need the wrapper array
            } else {
               /* A plain-text user message: keep its text beside the result
                * (results lead; repair_tool_pairs keeps that order). */
               const char *text = last_content ? json_object_get_string(last_content) : NULL;
               if (text && *text) {
                  json_object *text_block = json_object_new_object();
                  if (text_block) {
                     json_object_object_add(text_block, "type", json_object_new_string("text"));
                     json_object_object_add(text_block, "text", json_object_new_string(text));
                     json_object_array_add(result_array, text_block);
                  }
               }
               json_object_object_add(last_message, "content", result_array);
            }
         } else {
            last_message = json_object_new_object();
            json_object_object_add(last_message, "role", json_object_new_string("user"));
            json_object_object_add(last_message, "content", result_array);
            json_object_array_add(messages_array, last_message);
            last_role = "user";
         }
         continue;
      }

      const message_kind_t kind = llm_history_kind_of(msg);
      if (strcmp(role, "system") == 0 && kind == MESSAGE_KIND_TOOL_CHANGE) {
         /* In place when it goes there; else it is in `tools` already. */
         llm_claude_tool_change_add(messages_array, openai_conversation, (size_t)i, tools_in_place,
                                    &last_message, &last_role);
         continue;
      }
      if (strcmp(role, "system") == 0 &&
          (kind == MESSAGE_KIND_DIRECTIVE || kind == MESSAGE_KIND_INSTRUCTION)) {
         /* In place, never in the top-level system prompt: that stays as the
          * conversation first sent it. */
         add_operator_message(messages_array, note_label, json_object_get_string(content_obj),
                              mid_system, &last_message, &last_role);
         continue;
      }

      if (strcmp(role, "system") == 0) {
         // System message goes in separate "system" array with cache control
         json_object *system_block = json_object_new_object();
         json_object_object_add(system_block, "type", json_object_new_string("text"));
         json_object_object_add(system_block, "text", json_object_get(content_obj));

         // Cache control on first system block only (main prompt).
         // Claude allows max 4 cache_control blocks per request — injected
         // context messages (phone events, etc.) don't need caching.
         if (json_object_array_length(system_array) == 0) {
            json_object *cache_control = json_object_new_object();
            json_object_object_add(cache_control, "type", json_object_new_string("ephemeral"));
            json_object_object_add(system_block, "cache_control", cache_control);
         }

         json_object_array_add(system_array, system_block);
      } else {
         // Claude requires strict role alternation - consolidate consecutive same-role messages
         if (last_role != NULL && strcmp(last_role, role) == 0 && last_message != NULL) {
            // Same role as previous - need to consolidate
            json_object *last_content_obj;
            if (json_object_object_get_ex(last_message, "content", &last_content_obj)) {
               bool last_is_array = json_object_is_type(last_content_obj, json_type_array);
               bool curr_is_array = json_object_is_type(content_obj, json_type_array);

               if (last_is_array) {
                  // Last is array - append current content to it
                  if (curr_is_array) {
                     // Both arrays - copy all blocks from current to last
                     // Convert any OpenAI image_url blocks to Claude image format
                     int curr_len = json_object_array_length(content_obj);
                     for (int j = 0; j < curr_len; j++) {
                        json_object *block = json_object_array_get_idx(content_obj, j);
                        json_object *converted = convert_content_block_to_claude(block);
                        if (converted) {
                           json_object_array_add(last_content_obj, converted);
                        }
                     }
                  } else {
                     // Current is string - add as text block
                     const char *current_str = json_object_get_string(content_obj);
                     if (current_str && strlen(current_str) > 0) {
                        json_object *text_block = json_object_new_object();
                        json_object_object_add(text_block, "type", json_object_new_string("text"));
                        json_object_object_add(text_block, "text",
                                               json_object_new_string(current_str));
                        json_object_array_add(last_content_obj, text_block);
                     }
                  }
               } else if (curr_is_array) {
                  // Last is string, current is array - convert last to array and merge
                  const char *last_str = json_object_get_string(last_content_obj);
                  json_object *new_array = json_object_new_array();

                  // Add last string as text block first
                  if (last_str && strlen(last_str) > 0) {
                     json_object *text_block = json_object_new_object();
                     json_object_object_add(text_block, "type", json_object_new_string("text"));
                     json_object_object_add(text_block, "text", json_object_new_string(last_str));
                     json_object_array_add(new_array, text_block);
                  }

                  // Add all blocks from current array
                  // Convert any OpenAI image_url blocks to Claude image format
                  int curr_len = json_object_array_length(content_obj);
                  for (int j = 0; j < curr_len; j++) {
                     json_object *block = json_object_array_get_idx(content_obj, j);
                     json_object *converted = convert_content_block_to_claude(block);
                     if (converted) {
                        json_object_array_add(new_array, converted);
                     }
                  }

                  json_object_object_add(last_message, "content", new_array);
               } else {
                  // Both are strings - simple concatenation
                  const char *last_str = json_object_get_string(last_content_obj);
                  const char *curr_str = json_object_get_string(content_obj);
                  if (!last_str)
                     last_str = "";
                  if (!curr_str)
                     curr_str = "";

                  size_t new_len = strlen(last_str) + strlen(curr_str) + 4;
                  char *combined = malloc(new_len);
                  if (combined) {
                     snprintf(combined, new_len, "%s\n\n%s", last_str, curr_str);
                     json_object_object_add(last_message, "content",
                                            json_object_new_string(combined));
                     free(combined);
                  }
               }
            }
         } else {
            // Different role or first message - add new message
            last_message = json_object_new_object();
            json_object_object_add(last_message, "role", json_object_new_string(role));

            // If content is an array, convert any image_url blocks to Claude format
            if (json_object_is_type(content_obj, json_type_array)) {
               json_object *converted_array = json_object_new_array();
               int content_len = json_object_array_length(content_obj);
               for (int j = 0; j < content_len; j++) {
                  json_object *block = json_object_array_get_idx(content_obj, j);
                  json_object *converted = convert_content_block_to_claude(block);
                  if (converted) {
                     json_object_array_add(converted_array, converted);
                  }
               }
               json_object_object_add(last_message, "content", converted_array);
            } else if (strcmp(role, "user") == 0 && json_object_get_string_len(content_obj) > 0) {
               /* A user turn is always blocks: the conversation breakpoint goes on
                * its last one, and a turn must read the same with it or without
                * it (the next request moves it on). */
               json_object *blocks = json_object_new_array();
               json_object_array_add(blocks, text_block(json_object_get_string(content_obj)));
               json_object_object_add(last_message, "content", blocks);
            } else {
               json_object_object_add(last_message, "content", json_object_get(content_obj));
            }

            json_object_array_add(messages_array, last_message);
            last_role = role;
         }
      }
   }

   // Add system array if not empty
   if (json_object_array_length(system_array) > 0) {
      json_object_object_add(claude_request, "system", system_array);
   } else {
      json_object_put(system_array);
   }

   place_system_messages(messages_array, note_label);
   (void)repair_tool_pairs(messages_array);
   /* A one-off request (a compaction's summary) is never read back: its cache
    * write would only cost. */
   if (!llm_cache_monitor_one_off_call()) {
      mark_conversation_breakpoint(messages_array);
   }
   json_object_object_add(claude_request, "messages", messages_array);
   json_object_put(shown_history);

   return claude_request;
}
