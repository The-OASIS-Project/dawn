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
 * result_read: more of a tool result the model was shown a view of, by its
 * handle.  Read-only; fails closed; readable only in the conversation that
 * stored it (core/tool_result_store.h).
 */

#include "tools/result_read_tool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/session_history.h"
#include "core/session_manager.h"
#include "core/tool_result_store.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "tools/result_read_ops.h"
#include "tools/tool_registry.h"
#include "utils/string_utils.h"

/* Every refusal is the same: it tells the model nothing about another
 * conversation's, user's or evicted result. */
#define RESULT_READ_REFUSAL \
   TOOL_RESULT_ERROR_MARK "That result isn't available; call the tool again."

static char *result_read_callback(const char *action, char *value, int *should_respond);

static const treg_param_t result_read_params[] = {
   {
       .name = "action",
       .description = "read: lines of the result; path: the value at a JSON path; grep: where "
                      "a text appears; count: how many items, keys or characters a path has; "
                      "distinct: a field's values across an array's items, with counts",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "read", "path", "grep", "count", "distinct" },
       .enum_count = 5,
   },
   {
       .name = "handle",
       .description = "The result's handle, trs_ and 12 letters or digits, as its view gave it",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = true,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
   {
       .name = "from",
       .description = "read: the first line (1-based; default 1)",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "from",
   },
   {
       .name = "to",
       .description = "read: the last line (default: the end)",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "to",
   },
   {
       .name = "path",
       .description = "path, count, distinct: a JSON path as views write them: $, .name, "
                      "[\"key\"], [i] (negative from the end), and last [i:j]",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "path",
   },
   {
       .name = "pattern",
       .description = "grep: the text to find (literal, not case-sensitive)",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "pattern",
   },
   {
       .name = "context",
       .description = "grep on text: lines to show around each match (0-3)",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "context",
   },
   {
       .name = "field",
       .description = "distinct: the key whose values to count in the array's items",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "field",
   },
};

static const tool_metadata_t result_read_metadata = {
   .name = "result_read",
   .device_string = "result_read",
   .topic = "dawn",
   .aliases = { NULL },
   .alias_count = 0,

   .description = "Read more of a tool result that was too large to show whole. Its view gives "
                  "a handle ([tool-result trs_...]) and marks what it left out with a path or "
                  "a line range. Ask for that part: path for JSON values, read for lines, grep "
                  "to find text, count and distinct to summarize an array. Answers are views "
                  "too, fitted to what fits.",
   .params = result_read_params,
   .param_count = TOOL_PARAM_COUNT(result_read_params),

   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_NONE,
   .skip_followup = false,
   .result_no_store = true, /* its answers are views of a stored result already */

   .is_available = NULL,
   .callback = result_read_callback,
};

int result_read_tool_register(void) {
   return tool_registry_register(&result_read_metadata);
}

/* The handle in @p arg ("trs_...", or the "[tool-result trs_...]" a view
 * shows) into @p id: whether there was one. */
static bool handle_of(const char *arg, char id[TOOL_RESULTS_ID_LEN]) {
   const char *p = arg ? strstr(arg, TOOL_RESULTS_ID_PREFIX) : NULL;
   if (!p || strlen(p) < TOOL_RESULTS_ID_LEN - 1) {
      return false;
   }
   memcpy(id, p, TOOL_RESULTS_ID_LEN - 1);
   id[TOOL_RESULTS_ID_LEN - 1] = '\0';
   return true;
}

static long custom_long(const char *value, const char *field) {
   char buf[32];
   return tool_param_extract_custom(value, field, buf, sizeof(buf)) ? strtol(buf, NULL, 10) : 0;
}

/* What @p action asks of the open result @p doc. */
static char *answer(session_t *session,
                    const char *action,
                    const char *value,
                    tool_result_doc_t *doc,
                    size_t budget) {
   /* One byte past each limit, so an over-long value is seen, not cut. */
   char path[RESULT_READ_PATH_MAX + 2] = "$";
   char pattern[RESULT_READ_PATTERN_MAX + 2] = "";
   char field[RESULT_READ_PATH_MAX + 2] = "";
   (void)tool_param_extract_custom(value, "path", path, sizeof(path));
   (void)tool_param_extract_custom(value, "pattern", pattern, sizeof(pattern));
   (void)tool_param_extract_custom(value, "field", field, sizeof(field));
   if (strlen(field) > RESULT_READ_PATH_MAX) {
      return strdup(TOOL_RESULT_ERROR_MARK "The field is too long.");
   }

   if (strcmp(action, "read") == 0) {
      if (tool_result_store_load_body(doc) != TOOL_RESULT_OPEN_OK) {
         return strdup(RESULT_READ_REFUSAL);
      }
      return result_read_lines(doc->body, doc->len, custom_long(value, "from"),
                               custom_long(value, "to"), budget);
   }
   /* The tree answers without the body when it's cached. */
   tool_result_tree_t tree;
   const bool json = tool_result_store_tree_acquire(session, doc, &tree);
   char *out = NULL;
   if (strcmp(action, "grep") == 0 && !json) {
      out = tool_result_store_load_body(doc) == TOOL_RESULT_OPEN_OK
                ? result_read_grep_text(doc->body, doc->len, pattern,
                                        (int)custom_long(value, "context"), budget)
                : strdup(RESULT_READ_REFUSAL);
   } else if (strcmp(action, "grep") == 0) {
      out = result_read_grep_tree(tree.tree, pattern, budget);
   } else if (!json) {
      out = strdup(TOOL_RESULT_ERROR_MARK "This result is text, not JSON: use read or grep.");
   } else if (strcmp(action, "path") == 0) {
      out = result_read_path(tree.tree, path, budget);
   } else if (strcmp(action, "count") == 0) {
      out = result_read_count(tree.tree, path);
   } else if (strcmp(action, "distinct") == 0) {
      out = result_read_distinct(tree.tree, path, field, budget);
   } else {
      out = strdup(TOOL_RESULT_ERROR_MARK
                   "Unknown action: use read, path, grep, count or distinct.");
   }
   tool_result_store_tree_release(&tree);
   return out;
}

static char *result_read_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1;
   session_t *session = session_get_command_context();
   /* Fails closed: a session acting for a user, never the default voice user
    * a sessionless caller would get, and only for the model's own call (not
    * one arriving over MQTT naming a session). */
   const int user_id = session && llm_tools_executing() ? session_effective_user_id(session) : 0;

   char arg[128] = "";
   tool_param_extract_base(value, arg, sizeof(arg));
   char id[TOOL_RESULTS_ID_LEN];
   tool_result_doc_t doc;
   if (!action || !handle_of(arg, id)) {
      return strdup(RESULT_READ_REFUSAL);
   }
   const int rc = tool_result_store_open(session, user_id, id, &doc);
   if (rc == TOOL_RESULT_OPEN_FAILED) {
      return strdup(TOOL_RESULT_ERROR_MARK "Couldn't read that result just now; try again.");
   }
   if (rc != TOOL_RESULT_OPEN_OK) {
      OLOG_INFO("result_read: %s %s refused (session %u, user %d)", action, id,
                session ? session->session_id : 0U, user_id);
      return strdup(RESULT_READ_REFUSAL);
   }
   const size_t budget = tool_result_store_read_budget(session);
   char *out = answer(session, action, value, &doc, budget);
   if (out) {
      sanitize_utf8_for_json(out); /* a stored result's bytes are anyone's */
   }
   OLOG_INFO("result_read: %s %s (%s, %lld bytes) -> %zu bytes", action, id, doc.meta.tool_name,
             (long long)doc.meta.bytes, out ? strlen(out) : 0);
   tool_result_store_close(&doc);
   return out ? out : strdup(TOOL_RESULT_ERROR_MARK "Out of memory reading that result.");
}
