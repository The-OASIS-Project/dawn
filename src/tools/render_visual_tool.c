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
 * Visual Rendering Tool — inline SVG/HTML diagrams via LLM tool calling.
 *
 * Two tools registered:
 * - render_visual_load_guidelines: loads design guidelines from disk (two-step pattern)
 * - render_visual: wraps LLM-generated SVG/HTML in a <dawn-visual> tag for WebUI rendering
 *
 * The WebUI transcript renderer detects <dawn-visual> tags and renders
 * them as sandboxed iframes with theme CSS injection.
 */

#include "tools/render_visual_tool.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"
#include "tools/instruction_loader.h"
#include "tools/tool_registry.h"

#define TOOL_DIR "render_visual"

/* =============================================================================
 * Instruction Caching
 *
 * Track which modules have been loaded in the current conversation to avoid
 * returning 10KB+ of guidelines that the LLM already has in context.
 * Anchored to the session (via session_get_command_context) so it persists
 * across tool loop iterations and resets on conversation clear.
 *
 * Format: delimiter-bounded ",diagram,chart," to prevent substring collisions
 * (e.g., "art" matching inside "chart").
 * ============================================================================= */

#include "core/session_manager.h"

/**
 * Check if all requested modules were already loaded in this conversation.
 */
static bool modules_already_loaded(const char *loaded, const char *modules) {
   if (!loaded || loaded[0] == '\0') {
      return false;
   }

   char *copy = strdup(modules);
   if (!copy) {
      return false;
   }

   bool all_loaded = true;
   char *saveptr = NULL;
   char *mod = strtok_r(copy, ",", &saveptr);
   while (mod) {
      while (*mod == ' ')
         mod++;
      size_t len = strlen(mod);
      while (len > 0 && mod[len - 1] == ' ')
         len--;
      mod[len] = '\0';

      if (len > 0) {
         /* Delimiter-bounded search: look for ",module," in ",diagram,chart," */
         char needle[128];
         snprintf(needle, sizeof(needle), ",%s,", mod);
         if (!strstr(loaded, needle)) {
            all_loaded = false;
            break;
         }
      }
      mod = strtok_r(NULL, ",", &saveptr);
   }
   free(copy);
   return all_loaded;
}

/**
 * Record that these modules have been loaded. Appends each module
 * with delimiters: ",diagram,chart," format.
 */
static void record_loaded_modules(char *buf, size_t buf_size, const char *modules) {
   char *copy = strdup(modules);
   if (!copy) {
      return;
   }

   char *saveptr = NULL;
   char *mod = strtok_r(copy, ",", &saveptr);
   while (mod) {
      while (*mod == ' ')
         mod++;
      size_t len = strlen(mod);
      while (len > 0 && mod[len - 1] == ' ')
         len--;
      mod[len] = '\0';

      if (len > 0) {
         /* Check if already recorded (avoid duplicates) */
         char needle[128];
         snprintf(needle, sizeof(needle), ",%s,", mod);
         if (!strstr(buf, needle)) {
            size_t cur_len = strlen(buf);
            /* Append ",module" (leading comma provides delimiter boundary) */
            if (cur_len == 0) {
               /* First entry: start with "," for leading delimiter */
               snprintf(buf, buf_size, ",%s,", mod);
            } else if (cur_len + len + 2 < buf_size) {
               /* Overwrite trailing comma, append "module," */
               snprintf(buf + cur_len - 1, buf_size - cur_len + 1, ",%s,", mod);
            } else {
               OLOG_WARNING("render_visual: module cache full, skipping '%s'", mod);
            }
         }
      }
      mod = strtok_r(NULL, ",", &saveptr);
   }
   free(copy);
}

/* =============================================================================
 * Instruction Loader Callback
 * ============================================================================= */

static char *load_guidelines_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   *should_respond = 1;

   if (!value || value[0] == '\0') {
      return strdup("Error: provide module names (e.g., 'diagram', 'chart', 'interactive').");
   }

   /* Check session cache — return short message if modules already in context */
   session_t *session = session_get_command_context();
   if (session && modules_already_loaded(session->visual_modules_loaded, value)) {
      OLOG_INFO("render_visual: guidelines already loaded for '%s', returning cache hint", value);
      return strdup("Guidelines already loaded in this conversation. "
                    "Refer to the instructions provided above.");
   }

   char *content = NULL;
   int rc = instruction_loader_load(TOOL_DIR, value, &content);
   if (rc != 0 || !content) {
      return strdup("Error: failed to load visual guidelines. Check that the "
                    "tool_instructions/render_visual/ directory exists.");
   }

   /* Record loaded modules on the session for future cache hits */
   if (session) {
      record_loaded_modules(session->visual_modules_loaded, sizeof(session->visual_modules_loaded),
                            value);
   }
   return content;
}

/* =============================================================================
 * Render Visual Callback
 *
 * Wraps the LLM's generated code in a <dawn-visual> tag. The WebUI
 * transcript renderer detects this tag and renders an inline iframe.
 * The tag format preserves the title and type for history replay.
 * ============================================================================= */

static char *render_visual_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   *should_respond = 1;

   if (!value || value[0] == '\0') {
      return strdup("Error: provide visual parameters as JSON.");
   }

   /* Parse the JSON value to extract title, type, and code.
    * The LLM sends: {"title": "...", "type": "svg|html", "code": "..."} as the value
    * but the registry maps all custom fields into a single value string.
    * We need to parse it as JSON. */
   struct json_object *json = json_tokener_parse(value);
   if (!json) {
      return strdup("Error: invalid JSON in render_visual parameters.");
   }

   struct json_object *title_obj = NULL;
   struct json_object *type_obj = NULL;
   struct json_object *code_obj = NULL;

   json_object_object_get_ex(json, "title", &title_obj);
   json_object_object_get_ex(json, "type", &type_obj);
   json_object_object_get_ex(json, "code", &code_obj);

   const char *title = title_obj ? json_object_get_string(title_obj) : "visual";
   const char *type = type_obj ? json_object_get_string(type_obj) : "svg";
   const char *code = code_obj ? json_object_get_string(code_obj) : NULL;

   if (!code || code[0] == '\0') {
      json_object_put(json);
      return strdup("Error: 'code' parameter is required.");
   }

   /* Validate type */
   if (strcmp(type, "svg") != 0 && strcmp(type, "html") != 0) {
      json_object_put(json);
      return strdup("Error: 'type' must be 'svg' or 'html'.");
   }

   /* Sanitize title — strip characters that would break the tag attribute,
    * the JS regex that parses it, or filesystem paths for downloads */
   char safe_title[256];
   size_t si = 0;
   for (size_t i = 0; title[i] != '\0' && si < sizeof(safe_title) - 1; i++) {
      char c = title[i];
      if (c != '"' && c != '<' && c != '>' && c != '&' && c != '/' && c != '\\') {
         safe_title[si++] = c;
      }
   }
   safe_title[si] = '\0';

   /* Escape any </dawn-visual in the code to prevent tag breakout.
    * A closing tag in the code would prematurely end the wrapper,
    * allowing unsanitized content to appear in the transcript. */
   const char *poison = "</dawn-visual";
   size_t code_len = strlen(code);
   char *safe_code = NULL;
   if (strstr(code, poison) != NULL) {
      /* Replace </dawn-visual with <\/dawn-visual (HTML-safe escape) */
      size_t poison_len = strlen(poison);
      /* Count occurrences to size the buffer */
      size_t count = 0;
      const char *p = code;
      while ((p = strstr(p, poison)) != NULL) {
         count++;
         p += poison_len;
      }
      /* Each replacement adds 1 byte (the backslash) */
      safe_code = (char *)malloc(code_len + count + 1);
      if (safe_code) {
         size_t out = 0;
         p = code;
         const char *prev = code;
         while ((p = strstr(prev, poison)) != NULL) {
            size_t chunk = (size_t)(p - prev);
            memcpy(safe_code + out, prev, chunk);
            out += chunk;
            safe_code[out++] = '<';
            safe_code[out++] = '\\';
            /* Copy rest of poison tag (skip the '<') */
            memcpy(safe_code + out, poison + 1, poison_len - 1);
            out += poison_len - 1;
            prev = p + poison_len;
         }
         /* Copy remainder */
         size_t tail = code_len - (size_t)(prev - code);
         memcpy(safe_code + out, prev, tail);
         out += tail;
         safe_code[out] = '\0';
         code = safe_code;
         code_len = out;
      }
   }

   /* Build the <dawn-visual> tag for WebUI rendering. */
   size_t type_len = strlen(type);
   size_t buf_size = code_len + si + type_len + 128;

   char *result = (char *)malloc(buf_size);
   if (!result) {
      free(safe_code);
      json_object_put(json);
      return strdup("Error: memory allocation failed.");
   }

   snprintf(result, buf_size, "<dawn-visual title=\"%s\" type=\"%s\">\n%s\n</dawn-visual>",
            safe_title, type, code);
   free(safe_code);

   json_object_put(json);

   OLOG_INFO("render_visual: generated %s visual '%s' (%zu bytes code)", type, title, code_len);
   return result;
}

/* =============================================================================
 * Tool Metadata — Guidelines Loader
 * ============================================================================= */

static const treg_param_t load_guidelines_params[] = {
   {
       .name = "modules",
       .description = "Comma-separated list of guideline modules to load. "
                      "Available: 'diagram' (flowcharts, architecture, structural), "
                      "'chart' (Chart.js data visualization), "
                      "'interactive' (widgets with controls/animation), "
                      "'mockup' (UI mockups), 'art' (illustrations)",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = true,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

static const tool_metadata_t load_guidelines_metadata = {
   .name = "render_visual_load_guidelines",
   .device_string = "visual guidelines",
   .topic = "dawn",

   .description =
       "Load design guidelines before creating a visual. Call this BEFORE render_visual. "
       "Returns detailed rules for creating high-quality SVG/HTML visuals.",
   .params = load_guidelines_params,
   .param_count = 1,

   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_FILESYSTEM,
   .default_local = true,
   .default_remote = true,

   .callback = load_guidelines_callback,
};

/* =============================================================================
 * Tool Metadata — Render Visual
 * ============================================================================= */

static const treg_param_t render_visual_params[] = {
   {
       .name = "action",
       .description = "Always 'render'",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "render" },
       .enum_count = 1,
   },
   {
       .name = "details",
       .description = "JSON-encoded STRING (NOT a nested JSON object) with three required fields:\n"
                      "  title: short snake_case identifier, ≤32 chars (e.g. 'tcp_handshake')\n"
                      "  type:  'svg' for static diagrams, 'html' for interactive widgets\n"
                      "  code:  raw SVG or HTML markup. SVG starts with <svg>; HTML should NOT "
                      "include DOCTYPE/html/head/body tags (the renderer wraps it in a sandboxed "
                      "iframe).\n"
                      "Example: '{\"title\":\"tcp_handshake\",\"type\":\"svg\",\"code\":\"<svg "
                      "...>...</svg>\"}' — note all internal quotes are escaped because the entire "
                      "JSON object is itself a string.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = true,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

static const tool_metadata_t render_visual_metadata = {
   .name = "render_visual",
   .device_string = "visual renderer",
   .topic = "dawn",

   .description = "Render an inline SVG or HTML visual in the conversation. "
                  "IMPORTANT: Always call render_visual_load_guidelines first to load "
                  "the design guidelines for the type of visual you want to create.",
   .params = render_visual_params,
   .param_count = 2,

   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_NONE,
   .default_local = true,
   .default_remote = true,

   .callback = render_visual_callback,
};

/* =============================================================================
 * Registration
 * ============================================================================= */

int render_visual_tool_register(void) {
   int rc = tool_registry_register(&load_guidelines_metadata);
   if (rc != 0) {
      OLOG_ERROR("Failed to register render_visual_load_guidelines tool");
      return rc;
   }

   rc = tool_registry_register(&render_visual_metadata);
   if (rc != 0) {
      OLOG_ERROR("Failed to register render_visual tool");
      return rc;
   }

   OLOG_INFO("Visual rendering tool registered (two-step pattern)");
   return 0;
}
