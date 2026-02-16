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
 * Search Tool - Web search via SearXNG
 *
 * Supports categories: web, news, science, it, social, dictionary, papers
 */

#include "tools/search_tool.h"

#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "logging.h"
#include "tools/search_summarizer.h"
#include "tools/tool_registry.h"
#include "tools/web_search.h"
#include "utils/string_utils.h"

/* ========== Constants ========== */

#define SEARCH_RESULT_BUFFER_SIZE 4096

/* ========== Forward Declarations ========== */

static char *search_tool_callback(const char *action, char *value, int *should_respond);
static bool search_tool_is_available(void);

/* ========== Tool Parameter Definition ========== */

static const treg_param_t search_params[] = {
   {
       .name = "category",
       .description = "Search category: 'web' (general), 'news' (current events), "
                      "'science' (scientific), 'social' (Reddit/Twitter), "
                      "'it' (tech/programming), 'dictionary' (definitions), "
                      "'papers' (academic)",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = false,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "web", "news", "science", "it", "social", "dictionary", "papers" },
       .enum_count = 7,
   },
   {
       .name = "query",
       .description = "The search query text",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = true,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

/* ========== Tool Metadata ========== */

static const tool_metadata_t search_metadata = {
   .name = "search",
   .device_string = "search",
   .topic = "dawn",
   .aliases = { NULL },
   .alias_count = 0,

   .description = "Search the web for information. Choose the most appropriate category: "
                  "'web' for general queries, 'news' for current events, "
                  "'science' for scientific topics, 'it' for tech/programming, "
                  "'social' for social media, 'dictionary' for definitions, "
                  "'papers' for academic research.",
   .params = search_params,
   .param_count = 2,

   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_NETWORK,
   .is_getter = true,
   .skip_followup = false,
   .default_remote = true,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .is_available = search_tool_is_available,

   .init = NULL,
   .cleanup = NULL,
   .callback = search_tool_callback,
};

/* ========== Availability Check ========== */

static bool search_tool_is_available(void) {
   return g_config.search.endpoint[0] != '\0';
}

/* ========== Helper Functions ========== */

static char *perform_search(const char *value, search_type_t type, const char *type_name) {
   LOG_INFO("search_tool: Performing %s search for '%s'", type_name, value);

   search_response_t *response = web_search_query_typed(value, SEARXNG_MAX_RESULTS, type);
   if (!response) {
      return strdup("Search request failed.");
   }

   if (response->error) {
      LOG_ERROR("search_tool: Search error: %s", response->error);
      char *result = malloc(256);
      if (result) {
         snprintf(result, 256, "Search failed: %s", response->error);
      }
      web_search_free_response(response);
      return result ? result : strdup("Search failed.");
   }

   if (response->count > 0) {
      char *result = malloc(SEARCH_RESULT_BUFFER_SIZE);
      if (result) {
         web_search_format_for_llm(response, result, SEARCH_RESULT_BUFFER_SIZE);
         LOG_INFO("search_tool: Returning %d %s results", response->count, type_name);

         /* Run through summarizer if enabled and over threshold */
         char *summarized = NULL;
         int sum_result = search_summarizer_process(result, value, &summarized);
         if (sum_result == SUMMARIZER_SUCCESS && summarized) {
            free(result);
            result = summarized;
         } else if (summarized) {
            /* Summarizer returned something even on error (passthrough policy) */
            free(result);
            result = summarized;
         }
         /* If summarizer failed with no output, keep original result */
      }

      /* Sanitize result to remove invalid UTF-8/control chars before sending to LLM */
      if (result) {
         sanitize_utf8_for_json(result);
      }

      web_search_free_response(response);
      return result ? result : strdup("Failed to format search results.");
   }

   web_search_free_response(response);
   char *msg = malloc(128);
   if (msg) {
      snprintf(msg, 128, "No %s results found for '%s'.", type_name, value);
   }
   return msg ? msg : strdup("No results found.");
}

/* ========== Callback Implementation ========== */

static char *search_tool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1; /* Always return results to LLM */

   /* Initialize web search module if needed */
   if (!web_search_is_initialized()) {
      const char *endpoint = g_config.search.endpoint[0] != '\0' ? g_config.search.endpoint : NULL;
      if (web_search_init(endpoint) != 0) {
         LOG_ERROR("search_tool: Failed to initialize web search module");
         return strdup("Web search service is not available.");
      }
   }

   /* Determine search type from action (category) */
   if (action == NULL || action[0] == '\0' || strcmp(action, "web") == 0) {
      return perform_search(value, SEARCH_TYPE_WEB, "web");
   } else if (strcmp(action, "news") == 0) {
      return perform_search(value, SEARCH_TYPE_NEWS, "news");
   } else if (strcmp(action, "science") == 0) {
      return perform_search(value, SEARCH_TYPE_SCIENCE, "science");
   } else if (strcmp(action, "it") == 0 || strcmp(action, "tech") == 0) {
      return perform_search(value, SEARCH_TYPE_IT, "tech");
   } else if (strcmp(action, "social") == 0) {
      return perform_search(value, SEARCH_TYPE_SOCIAL, "social");
   } else if (strcmp(action, "define") == 0 || strcmp(action, "dictionary") == 0) {
      return perform_search(value, SEARCH_TYPE_DICTIONARY, "dictionary");
   } else if (strcmp(action, "papers") == 0 || strcmp(action, "academic") == 0) {
      return perform_search(value, SEARCH_TYPE_PAPERS, "papers");
   }

   /* Fallback to web search for unknown categories */
   LOG_WARNING("search_tool: Unknown category '%s', defaulting to web search", action);
   return perform_search(value, SEARCH_TYPE_WEB, "web");
}

/* ========== Public API ========== */

int search_tool_register(void) {
   return tool_registry_register(&search_metadata);
}
