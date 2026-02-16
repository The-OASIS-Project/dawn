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
 * URL Tool - Fetch and extract content from web pages
 *
 * Fetches URL content, converts to Markdown, and optionally summarizes.
 */

#include "tools/url_tool.h"

#include <stdlib.h>
#include <string.h>

#include "logging.h"
#include "tools/search_summarizer.h"
#include "tools/tool_registry.h"
#include "tools/url_fetcher.h"
#include "utils/string_utils.h"

/* ========== Constants ========== */

/* Hard limit on content size to prevent API errors (e.g., HTTP 400 from too-large requests)
 * Most LLM APIs have context limits; 8000 chars is a safe limit for tool results
 * This limit applies after summarization as a fallback safety measure */
#define URL_CONTENT_MAX_CHARS 8000

/* ========== Forward Declarations ========== */

static char *url_tool_callback(const char *action, char *value, int *should_respond);

/* ========== Tool Parameter Definition ========== */

static const treg_param_t url_params[] = {
   {
       .name = "url",
       .description = "The URL to fetch (must be http:// or https://)",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = true,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

/* ========== Tool Metadata ========== */

static const tool_metadata_t url_metadata = {
   .name = "url_fetch",
   .device_string = "url_fetch",
   .topic = "dawn",
   .aliases = { "fetch", "url" },
   .alias_count = 2,

   .description = "Fetch and extract readable content from a web page URL. "
                  "Returns the page content as structured Markdown text. "
                  "Large pages are automatically summarized. "
                  "Use this to get full details from search result URLs when "
                  "snippets don't provide enough information to answer the question.",
   .params = url_params,
   .param_count = 1,

   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_NETWORK,
   .is_getter = true,
   .skip_followup = false,
   .default_remote = true,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = NULL,
   .callback = url_tool_callback,
};

/* ========== Callback Implementation ========== */

static char *url_tool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1; /* Always return results to LLM */

   if (value == NULL || strlen(value) == 0) {
      LOG_WARNING("url_tool: No URL provided");
      return strdup("Please provide a URL to fetch.");
   }

   /* Support both "get" action and NULL/empty action (for direct calls) */
   if (action != NULL && action[0] != '\0' && strcmp(action, "get") != 0) {
      LOG_WARNING("url_tool: Unknown action '%s'", action);
      return strdup("Unknown URL action. Use: get");
   }

   LOG_INFO("url_tool: Fetching URL '%s'", value);

   /* Validate URL */
   if (!url_is_valid(value)) {
      LOG_WARNING("url_tool: Invalid URL '%s'", value);
      return strdup("Invalid URL. Must start with http:// or https://");
   }

   /* Fetch and extract content */
   char *content = NULL;
   size_t content_size = 0;
   int result = url_fetch_content(value, &content, &content_size);

   if (result != URL_FETCH_SUCCESS) {
      const char *err = url_fetch_error_string(result);
      LOG_WARNING("url_tool: Fetch failed: %s", err);
      char *msg = malloc(256);
      if (msg) {
         snprintf(msg, 256, "Failed to fetch URL: %s", err);
         return msg;
      }
      return strdup("Failed to fetch URL.");
   }

   LOG_INFO("url_tool: Extracted %zu bytes of content", content_size);

   /* Run through summarizer if enabled and over threshold */
   char *summarized = NULL;
   int sum_result = search_summarizer_process(content, value, &summarized);
   if (sum_result == SUMMARIZER_SUCCESS && summarized) {
      free(content);
      content = summarized;
   } else if (summarized) {
      /* Summarizer returned something even on error (passthrough policy) */
      free(content);
      content = summarized;
   }
   /* If summarizer failed with no output, keep original content */

   /* Hard limit on content size */
   if (content && strlen(content) > URL_CONTENT_MAX_CHARS) {
      LOG_WARNING("url_tool: Content too large (%zu bytes), truncating to %d", strlen(content),
                  URL_CONTENT_MAX_CHARS);
      /* Allocate space for truncated content + truncation notice */
      char *truncated = malloc(URL_CONTENT_MAX_CHARS + 100);
      if (truncated) {
         strncpy(truncated, content, URL_CONTENT_MAX_CHARS - 50);
         truncated[URL_CONTENT_MAX_CHARS - 50] = '\0';
         strcat(truncated, "\n\n[Content truncated - original was too large]");
         free(content);
         content = truncated;
      } else {
         /* If malloc fails, just truncate in place */
         content[URL_CONTENT_MAX_CHARS] = '\0';
      }
   }

   /* Sanitize content to remove invalid UTF-8/control chars before sending to LLM */
   if (content) {
      sanitize_utf8_for_json(content);
   }

   return content;
}

/* ========== Public API ========== */

int url_tool_register(void) {
   return tool_registry_register(&url_metadata);
}
