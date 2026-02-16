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
 * HTML Parser - Convert HTML to Markdown
 */

#include "tools/html_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "utils/string_utils.h"

// =============================================================================
// Constants
// =============================================================================

// Maximum nesting depth for lists
#define MAX_LIST_DEPTH 6

// Maximum tag content length for stack allocation (avoids per-tag malloc)
#define MAX_TAG_CONTENT_LEN 512

// Maximum output buffer size to prevent unbounded growth on malformed/malicious HTML.
// 2MB is sufficient for most documents including large papers/articles.
#define HTML_MAX_OUTPUT_SIZE (2 * 1024 * 1024)

// Initial output buffer size (1/3 of HTML, minimum 4KB)
#define HTML_INITIAL_BUFFER_MIN 4096

// Minimum content length to consider output valid
#define HTML_MIN_CONTENT_LEN 10

// Maximum consecutive newlines to preserve (prevents excessive whitespace)
#define MAX_CONSECUTIVE_NEWLINES 2

// =============================================================================
// Parser State
// =============================================================================

// Reduced buffer sizes for embedded systems (Jetson/RPi)
// Total struct size: ~3KB
typedef struct {
   char *output;         // Output buffer
   size_t out_pos;       // Current write position
   size_t out_capacity;  // Buffer capacity

   int in_pre;                           // Inside <pre> tag
   int in_code;                          // Inside <code> tag
   int in_blockquote;                    // Blockquote nesting level
   int in_list;                          // List nesting level
   int list_is_ordered[MAX_LIST_DEPTH];  // Track ordered vs unordered per level
   int list_item_num[MAX_LIST_DEPTH];    // Item number for ordered lists

   int last_was_newline;     // Last char written was newline
   int blank_line_pending;   // Need to write blank line before next content
   int suppress_whitespace;  // Don't emit leading whitespace

   // Current link being processed
   char link_href[512];
   char link_text[256];
   int in_link;
   size_t link_text_pos;

   // Base URL for resolving relative links
   char base_url[512];
   char base_scheme[16];
   char base_host[256];
   char base_path[512];
} html_parser_state_t;

// =============================================================================
// Helper Functions
// =============================================================================

static const char *find_closing_tag(const char *html, const char *tag_name) {
   char close_tag[72];  // tag_name(64) + "</>" + null = 68 bytes, round up
   snprintf(close_tag, sizeof(close_tag), "</%s>", tag_name);
   const char *found = strcasestr_portable(html, close_tag);
   if (found) {
      return found + strlen(close_tag) - 1;
   }
   return NULL;
}

/**
 * @brief Check if tag should be skipped (optimized with first-char switch)
 * Tags: script, style, nav, footer, header, aside, noscript, iframe, object,
 *       embed, svg, canvas, template, form, button, input, select, meta, link,
 *       figcaption, comments
 *
 * @param tag_name Tag name (already lowercased during extraction at line ~1000)
 * @param len Length of tag name (avoids repeated strlen calls)
 */
static int is_skip_tag(const char *tag_name, size_t len) {
   if (!tag_name || len == 0)
      return 0;

   // Quick rejection based on first character (already lowercase)
   char first = tag_name[0];

   // Use strcmp instead of strcasecmp since tag_name is already lowercased
   switch (first) {
      case 'a':
         return (len == 5 && strcmp(tag_name, "aside") == 0);
      case 'b':
         return (len == 6 && strcmp(tag_name, "button") == 0);
      case 'c':
         if (len == 6 && strcmp(tag_name, "canvas") == 0)
            return 1;
         if (len == 8 && strcmp(tag_name, "comments") == 0)
            return 1;
         return 0;
      case 'd':
         return (len == 6 && strcmp(tag_name, "dialog") == 0);
      case 'e':
         return (len == 5 && strcmp(tag_name, "embed") == 0);
      case 'f':
         if (len == 6 && strcmp(tag_name, "footer") == 0)
            return 1;
         if (len == 4 && strcmp(tag_name, "form") == 0)
            return 1;
         if (len == 10 && strcmp(tag_name, "figcaption") == 0)
            return 1;
         return 0;
      case 'h':
         return (len == 6 && strcmp(tag_name, "header") == 0);
      case 'i':
         if (len == 6 && strcmp(tag_name, "iframe") == 0)
            return 1;
         if (len == 5 && strcmp(tag_name, "input") == 0)
            return 1;
         return 0;
      case 'l':
         return (len == 4 && strcmp(tag_name, "link") == 0);
      case 'm':
         return (len == 4 && strcmp(tag_name, "meta") == 0);
      case 'n':
         if (len == 3 && strcmp(tag_name, "nav") == 0)
            return 1;
         if (len == 8 && strcmp(tag_name, "noscript") == 0)
            return 1;
         return 0;
      case 'o':
         return (len == 6 && strcmp(tag_name, "object") == 0);
      case 's':
         if (len == 6 && strcmp(tag_name, "script") == 0)
            return 1;
         if (len == 5 && strcmp(tag_name, "style") == 0)
            return 1;
         if (len == 3 && strcmp(tag_name, "svg") == 0)
            return 1;
         if (len == 6 && strcmp(tag_name, "select") == 0)
            return 1;
         return 0;
      case 't':
         return (len == 8 && strcmp(tag_name, "template") == 0);
      default:
         return 0;
   }
}

/* Forward declaration for extract_attr (defined later) */
static int extract_attr(const char *tag_content,
                        const char *attr_name,
                        char *value,
                        size_t value_size);

/**
 * @brief Noise patterns for class/id attributes
 * Elements with class or id containing these substrings are skipped
 */
static const char *NOISE_CLASS_PATTERNS[] = {
   /* Modals and popups */
   "modal", "popup", "dialog", "overlay", "lightbox",
   /* Subscription and newsletter */
   "subscri", "newsletter", "signup", "sign-up", "optin", "opt-in",
   /* Cookie and consent */
   "cookie", "consent", "gdpr", "privacy-notice",
   /* Ads and promotions */
   "advert", "promo", "sponsor", "banner-ad", "ad-slot", "ad-container",
   /* Navigation and sidebars */
   "sidebar", "widget", "related-", "recommended", "tags",
   /* Social sharing */
   "share-", "social-", "follow-us",
   /* Comments sections */
   "comment", "disqus", "discuss",
   /* Author bios */
   "author-bio", "byline",
   /* Misc UI */
   "tooltip", "toast", "alert-", "notification", NULL /* Sentinel */
};

/**
 * @brief Check if class or id attribute contains noise patterns
 * @param tag_content Full tag content (e.g., "<div class=\"modal popup\">")
 * @return 1 if should skip, 0 otherwise
 */
static int has_noise_class_or_id(const char *tag_content) {
   if (!tag_content)
      return 0;

   /* Extract class attribute */
   char class_val[256] = { 0 };
   char id_val[128] = { 0 };

   extract_attr(tag_content, "class", class_val, sizeof(class_val));
   extract_attr(tag_content, "id", id_val, sizeof(id_val));

   /* If no class or id, not noise */
   if (class_val[0] == '\0' && id_val[0] == '\0')
      return 0;

   /* Lowercase for case-insensitive matching */
   for (char *p = class_val; *p; p++)
      *p = tolower((unsigned char)*p);
   for (char *p = id_val; *p; p++)
      *p = tolower((unsigned char)*p);

   /* Check each pattern */
   for (int i = 0; NOISE_CLASS_PATTERNS[i] != NULL; i++) {
      if (class_val[0] && strstr(class_val, NOISE_CLASS_PATTERNS[i]))
         return 1;
      if (id_val[0] && strstr(id_val, NOISE_CLASS_PATTERNS[i]))
         return 1;
   }

   return 0;
}

/**
 * @brief Parse URL into components (scheme, host, path)
 */
static void parse_url_components(const char *url,
                                 char *scheme,
                                 size_t scheme_size,
                                 char *host,
                                 size_t host_size,
                                 char *path,
                                 size_t path_size) {
   scheme[0] = '\0';
   host[0] = '\0';
   path[0] = '\0';

   if (!url)
      return;

   // Extract scheme
   const char *scheme_end = strstr(url, "://");
   if (scheme_end) {
      size_t slen = scheme_end - url;
      if (slen < scheme_size) {
         strncpy(scheme, url, slen);
         scheme[slen] = '\0';
      }
      url = scheme_end + 3;
   }

   // Extract host (up to / or end)
   const char *path_start = strchr(url, '/');
   size_t host_len = path_start ? (size_t)(path_start - url) : strlen(url);
   if (host_len < host_size) {
      strncpy(host, url, host_len);
      host[host_len] = '\0';
   }

   // Extract path
   if (path_start) {
      // Find last / for directory path
      const char *last_slash = strrchr(path_start, '/');
      if (last_slash && last_slash != path_start) {
         size_t plen = last_slash - path_start + 1;
         if (plen < path_size) {
            strncpy(path, path_start, plen);
            path[plen] = '\0';
         }
      } else {
         strncpy(path, "/", path_size - 1);
         path[path_size - 1] = '\0';
      }
   } else {
      strncpy(path, "/", path_size - 1);
      path[path_size - 1] = '\0';
   }
}

/**
 * @brief Resolve a potentially relative URL against a base URL
 */
static void resolve_url(const char *href,
                        const char *base_scheme,
                        const char *base_host,
                        const char *base_path,
                        char *resolved,
                        size_t resolved_size) {
   if (!href || !resolved) {
      if (resolved)
         resolved[0] = '\0';
      return;
   }

   // Already absolute URL
   if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) {
      strncpy(resolved, href, resolved_size - 1);
      resolved[resolved_size - 1] = '\0';
      return;
   }

   // Protocol-relative URL (//example.com/path)
   if (strncmp(href, "//", 2) == 0) {
      snprintf(resolved, resolved_size, "%s:%s", base_scheme, href);
      return;
   }

   // Absolute path (/path/to/resource)
   if (href[0] == '/') {
      snprintf(resolved, resolved_size, "%s://%s%s", base_scheme, base_host, href);
      return;
   }

   // Relative path
   snprintf(resolved, resolved_size, "%s://%s%s%s", base_scheme, base_host, base_path, href);
}

/**
 * @brief Write character to output buffer, growing if needed
 */
static void emit_char(html_parser_state_t *state, char c) {
   if (state->out_pos >= state->out_capacity - 1) {
      size_t new_cap = state->out_capacity * 2;
      // Prevent unbounded growth - cap at maximum size
      if (new_cap > HTML_MAX_OUTPUT_SIZE) {
         if (state->out_capacity >= HTML_MAX_OUTPUT_SIZE)
            return;  // Already at max, silently drop
         new_cap = HTML_MAX_OUTPUT_SIZE;
      }
      char *new_buf = realloc(state->output, new_cap);
      if (!new_buf)
         return;
      state->output = new_buf;
      state->out_capacity = new_cap;
   }
   state->output[state->out_pos++] = c;
   state->output[state->out_pos] = '\0';

   state->last_was_newline = (c == '\n');
   if (c != '\n' && c != ' ' && c != '\t') {
      state->blank_line_pending = 0;
   }
}

/**
 * @brief Write string with known length to output buffer (batch operation for efficiency)
 *
 * This function ensures capacity once and uses memcpy for better performance
 * compared to character-by-character emission.
 */
static void emit_str_n(html_parser_state_t *state, const char *s, size_t len) {
   if (len == 0 || !s)
      return;

   // Ensure capacity for entire string plus null terminator
   size_t needed = state->out_pos + len + 1;
   if (needed > state->out_capacity) {
      size_t new_cap = state->out_capacity;
      while (new_cap < needed)
         new_cap *= 2;
      // Prevent unbounded growth - cap at maximum size
      if (new_cap > HTML_MAX_OUTPUT_SIZE) {
         if (state->out_capacity >= HTML_MAX_OUTPUT_SIZE)
            return;  // Already at max, silently drop
         new_cap = HTML_MAX_OUTPUT_SIZE;
         // Truncate len if needed to fit within max
         if (needed > HTML_MAX_OUTPUT_SIZE)
            len = HTML_MAX_OUTPUT_SIZE - state->out_pos - 1;
         if (len == 0)
            return;
      }
      char *new_buf = realloc(state->output, new_cap);
      if (!new_buf)
         return;
      state->output = new_buf;
      state->out_capacity = new_cap;
   }

   // Batch copy
   memcpy(state->output + state->out_pos, s, len);
   state->out_pos += len;
   state->output[state->out_pos] = '\0';

   // Update state based on last character
   state->last_was_newline = (s[len - 1] == '\n');
   if (s[len - 1] != '\n' && s[len - 1] != ' ' && s[len - 1] != '\t') {
      state->blank_line_pending = 0;
   }
}

/**
 * @brief Write string to output buffer (convenience wrapper for null-terminated strings)
 */
static void emit_str(html_parser_state_t *state, const char *s) {
   if (s)
      emit_str_n(state, s, strlen(s));
}

/* Convenience macro for string literals - avoids strlen at runtime */
#define EMIT_LIT(state, lit) emit_str_n(state, lit, sizeof(lit) - 1)

/**
 * @brief Ensure we're at the start of a new line
 */
static void ensure_newline(html_parser_state_t *state) {
   if (!state->last_was_newline && state->out_pos > 0) {
      emit_char(state, '\n');
   }
}

/**
 * @brief Ensure blank line before next content (for paragraph separation)
 */
static void ensure_blank_line(html_parser_state_t *state) {
   ensure_newline(state);
   state->blank_line_pending = 1;
}

/**
 * @brief Write pending blank line if needed
 */
static void flush_blank_line(html_parser_state_t *state) {
   if (state->blank_line_pending && state->out_pos > 0) {
      emit_char(state, '\n');
      state->blank_line_pending = 0;
   }
}

/**
 * @brief Write list prefix based on current nesting
 */
static void emit_list_prefix(html_parser_state_t *state) {
   ensure_newline(state);

   // Indent based on nesting level
   for (int i = 0; i < state->in_list - 1; i++) {
      emit_str(state, "  ");
   }

   int level = state->in_list - 1;
   if (level >= 0 && level < MAX_LIST_DEPTH) {
      if (state->list_is_ordered[level]) {
         char num[16];
         snprintf(num, sizeof(num), "%d. ", state->list_item_num[level]++);
         emit_str(state, num);
      } else {
         emit_str(state, "- ");
      }
   }
}

/**
 * @brief Extract attribute value from tag
 */
static int extract_attr(const char *tag_content,
                        const char *attr_name,
                        char *value,
                        size_t value_size) {
   char search[64];
   snprintf(search, sizeof(search), "%s=", attr_name);

   const char *attr = strcasestr_portable(tag_content, search);
   if (!attr)
      return 0;

   attr += strlen(search);

   // Skip whitespace
   while (*attr && isspace(*attr))
      attr++;

   char quote = 0;
   if (*attr == '"' || *attr == '\'') {
      quote = *attr++;
   }

   size_t i = 0;
   while (*attr && i < value_size - 1) {
      if (quote && *attr == quote)
         break;
      if (!quote && (isspace(*attr) || *attr == '>'))
         break;
      value[i++] = *attr++;
   }
   value[i] = '\0';
   return i > 0;
}

/**
 * @brief Encode a Unicode code point as UTF-8
 */
static int encode_utf8(unsigned int codepoint, char *out) {
   if (codepoint < 0x80) {
      out[0] = (char)codepoint;
      out[1] = '\0';
      return 1;
   } else if (codepoint < 0x800) {
      out[0] = (char)(0xC0 | (codepoint >> 6));
      out[1] = (char)(0x80 | (codepoint & 0x3F));
      out[2] = '\0';
      return 2;
   } else if (codepoint < 0x10000) {
      out[0] = (char)(0xE0 | (codepoint >> 12));
      out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
      out[2] = (char)(0x80 | (codepoint & 0x3F));
      out[3] = '\0';
      return 3;
   } else if (codepoint < 0x110000) {
      out[0] = (char)(0xF0 | (codepoint >> 18));
      out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
      out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
      out[3] = (char)(0x80 | (codepoint & 0x3F));
      out[4] = '\0';
      return 4;
   }
   // Invalid codepoint
   out[0] = '?';
   out[1] = '\0';
   return 1;
}

/**
 * @brief Decode HTML entity at current position
 *
 * Optimized with:
 * 1. Fast-path for most common entities (&lt; &gt; &amp; &nbsp; &quot;)
 * 2. Second-char switch for remaining entities
 */
static int decode_entity(const char *src, char *out) {
   if (src[0] != '&') {
      out[0] = '&';
      out[1] = '\0';
      return 1;
   }

   // Fast path for most common entities (90%+ of real-world usage)
   // Check these before the switch to minimize comparisons
   if (src[1] == 'l' && src[2] == 't' && src[3] == ';') {
      out[0] = '<';
      out[1] = '\0';
      return 4;
   }
   if (src[1] == 'g' && src[2] == 't' && src[3] == ';') {
      out[0] = '>';
      out[1] = '\0';
      return 4;
   }
   if (src[1] == 'a' && src[2] == 'm' && src[3] == 'p' && src[4] == ';') {
      out[0] = '&';
      out[1] = '\0';
      return 5;
   }
   if (src[1] == 'n' && src[2] == 'b' && src[3] == 's' && src[4] == 'p' && src[5] == ';') {
      out[0] = ' ';
      out[1] = '\0';
      return 6;
   }
   if (src[1] == 'q' && src[2] == 'u' && src[3] == 'o' && src[4] == 't' && src[5] == ';') {
      out[0] = '"';
      out[1] = '\0';
      return 6;
   }

   // Numeric entities: &#NNN; or &#xHHH;
   if (src[1] == '#') {
      unsigned int value = 0;
      const char *p = src + 2;
      int base = 10;

      if (*p == 'x' || *p == 'X') {
         base = 16;
         p++;
      }

      while (*p && *p != ';') {
         if (base == 16 && isxdigit(*p)) {
            value = value * 16 + (isdigit(*p) ? *p - '0' : tolower(*p) - 'a' + 10);
         } else if (base == 10 && isdigit(*p)) {
            value = value * 10 + (*p - '0');
         } else {
            break;
         }
         p++;
      }

      if (*p == ';' && value > 0) {
         encode_utf8(value, out);
         return (int)(p - src + 1);
      }
   }

   // Named entities - switch on second character for quick rejection
   // Note: Common entities (&lt;, &gt;, &amp;, &nbsp;, &quot;) are handled by fast-path above
   char second = tolower(src[1]);
   switch (second) {
      case 'a':
         // &amp; handled by fast-path
         if (strncasecmp(src, "&apos;", 6) == 0) {
            out[0] = '\'';
            out[1] = '\0';
            return 6;
         }
         break;
      case 'b':
         if (strncasecmp(src, "&bull;", 6) == 0) {
            out[0] = '\xE2';
            out[1] = '\x80';
            out[2] = '\xA2';
            out[3] = '\0';
            return 6;
         }
         break;
      case 'c':
         if (strncasecmp(src, "&copy;", 6) == 0) {
            out[0] = '\xC2';
            out[1] = '\xA9';
            out[2] = '\0';
            return 6;
         }
         if (strncasecmp(src, "&cent;", 6) == 0) {
            out[0] = '\xC2';
            out[1] = '\xA2';
            out[2] = '\0';
            return 6;
         }
         break;
      case 'd':
         if (strncasecmp(src, "&deg;", 5) == 0) {
            out[0] = '\xC2';
            out[1] = '\xB0';
            out[2] = '\0';
            return 5;
         }
         if (strncasecmp(src, "&divide;", 8) == 0) {
            out[0] = '\xC3';
            out[1] = '\xB7';
            out[2] = '\0';
            return 8;
         }
         if (strncasecmp(src, "&dagger;", 8) == 0) {
            out[0] = '\xE2';
            out[1] = '\x80';
            out[2] = '\xA0';
            out[3] = '\0';
            return 8;
         }
         if (strncasecmp(src, "&Dagger;", 8) == 0) {
            out[0] = '\xE2';
            out[1] = '\x80';
            out[2] = '\xA1';
            out[3] = '\0';
            return 8;
         }
         break;
      case 'e':
         if (strncasecmp(src, "&euro;", 6) == 0) {
            out[0] = '\xE2';
            out[1] = '\x82';
            out[2] = '\xAC';
            out[3] = '\0';
            return 6;
         }
         break;
      case 'f':
         if (strncasecmp(src, "&frac12;", 8) == 0) {
            out[0] = '\xC2';
            out[1] = '\xBD';
            out[2] = '\0';
            return 8;
         }
         if (strncasecmp(src, "&frac14;", 8) == 0) {
            out[0] = '\xC2';
            out[1] = '\xBC';
            out[2] = '\0';
            return 8;
         }
         if (strncasecmp(src, "&frac34;", 8) == 0) {
            out[0] = '\xC2';
            out[1] = '\xBE';
            out[2] = '\0';
            return 8;
         }
         break;
      // case 'g': &gt; handled by fast-path
      case 'h':
         if (strncasecmp(src, "&hellip;", 8) == 0) {
            out[0] = '\xE2';
            out[1] = '\x80';
            out[2] = '\xA6';
            out[3] = '\0';
            return 8;
         }
         break;
      case 'l':
         // &lt; handled by fast-path
         if (strncasecmp(src, "&ldquo;", 7) == 0) {
            out[0] = '\xE2';
            out[1] = '\x80';
            out[2] = '\x9C';
            out[3] = '\0';
            return 7;
         }
         if (strncasecmp(src, "&lsquo;", 7) == 0) {
            out[0] = '\xE2';
            out[1] = '\x80';
            out[2] = '\x98';
            out[3] = '\0';
            return 7;
         }
         break;
      case 'm':
         if (strncasecmp(src, "&mdash;", 7) == 0) {
            out[0] = '\xE2';
            out[1] = '\x80';
            out[2] = '\x94';
            out[3] = '\0';
            return 7;
         }
         break;
      case 'n':
         // &nbsp; handled by fast-path
         if (strncasecmp(src, "&ndash;", 7) == 0) {
            out[0] = '\xE2';
            out[1] = '\x80';
            out[2] = '\x93';
            out[3] = '\0';
            return 7;
         }
         break;
      case 'p':
         if (strncasecmp(src, "&plusmn;", 8) == 0) {
            out[0] = '\xC2';
            out[1] = '\xB1';
            out[2] = '\0';
            return 8;
         }
         if (strncasecmp(src, "&pound;", 7) == 0) {
            out[0] = '\xC2';
            out[1] = '\xA3';
            out[2] = '\0';
            return 7;
         }
         if (strncasecmp(src, "&para;", 6) == 0) {
            out[0] = '\xC2';
            out[1] = '\xB6';
            out[2] = '\0';
            return 6;
         }
         break;
      // case 'q': &quot; handled by fast-path
      case 'r':
         if (strncasecmp(src, "&reg;", 5) == 0) {
            out[0] = '\xC2';
            out[1] = '\xAE';
            out[2] = '\0';
            return 5;
         }
         if (strncasecmp(src, "&rdquo;", 7) == 0) {
            out[0] = '\xE2';
            out[1] = '\x80';
            out[2] = '\x9D';
            out[3] = '\0';
            return 7;
         }
         if (strncasecmp(src, "&rsquo;", 7) == 0) {
            out[0] = '\xE2';
            out[1] = '\x80';
            out[2] = '\x99';
            out[3] = '\0';
            return 7;
         }
         break;
      case 's':
         if (strncasecmp(src, "&sect;", 6) == 0) {
            out[0] = '\xC2';
            out[1] = '\xA7';
            out[2] = '\0';
            return 6;
         }
         break;
      case 't':
         if (strncasecmp(src, "&trade;", 7) == 0) {
            out[0] = '\xE2';
            out[1] = '\x84';
            out[2] = '\xA2';
            out[3] = '\0';
            return 7;
         }
         if (strncasecmp(src, "&times;", 7) == 0) {
            out[0] = '\xC3';
            out[1] = '\x97';
            out[2] = '\0';
            return 7;
         }
         break;
      case 'y':
         if (strncasecmp(src, "&yen;", 5) == 0) {
            out[0] = '\xC2';
            out[1] = '\xA5';
            out[2] = '\0';
            return 5;
         }
         break;
   }

   // Unknown entity - just return the ampersand
   out[0] = '&';
   out[1] = '\0';
   return 1;
}

// =============================================================================
// Tag Handlers
// =============================================================================

static void handle_tag_open(html_parser_state_t *state,
                            const char *tag_name,
                            const char *tag_content) {
   // Headings
   if (tag_name[0] == 'h' && tag_name[1] >= '1' && tag_name[1] <= '6' && tag_name[2] == '\0') {
      ensure_blank_line(state);
      flush_blank_line(state);
      int level = tag_name[1] - '0';
      for (int i = 0; i < level; i++)
         emit_char(state, '#');
      emit_char(state, ' ');
      state->suppress_whitespace = 1;
      return;
   }

   // Paragraphs and divs
   if (strcasecmp(tag_name, "p") == 0 || strcasecmp(tag_name, "div") == 0) {
      ensure_blank_line(state);
      return;
   }

   // Figure elements (images with captions) - ensure separation
   if (strcasecmp(tag_name, "figure") == 0) {
      ensure_blank_line(state);
      return;
   }

   // Line breaks
   if (strcasecmp(tag_name, "br") == 0) {
      emit_char(state, '\n');
      return;
   }

   // Horizontal rule
   if (strcasecmp(tag_name, "hr") == 0) {
      ensure_blank_line(state);
      flush_blank_line(state);
      emit_str(state, "---");
      ensure_blank_line(state);
      return;
   }

   // Bold
   if (strcasecmp(tag_name, "b") == 0 || strcasecmp(tag_name, "strong") == 0) {
      emit_str(state, "**");
      return;
   }

   // Italic
   if (strcasecmp(tag_name, "i") == 0 || strcasecmp(tag_name, "em") == 0) {
      emit_char(state, '*');
      return;
   }

   // Code (inline)
   if (strcasecmp(tag_name, "code") == 0 && !state->in_pre) {
      emit_char(state, '`');
      state->in_code = 1;
      return;
   }

   // Preformatted / code blocks
   if (strcasecmp(tag_name, "pre") == 0) {
      ensure_blank_line(state);
      flush_blank_line(state);
      emit_str(state, "```\n");
      state->in_pre = 1;
      return;
   }

   // Blockquote
   if (strcasecmp(tag_name, "blockquote") == 0) {
      ensure_blank_line(state);
      state->in_blockquote++;
      return;
   }

   // Unordered list
   if (strcasecmp(tag_name, "ul") == 0) {
      ensure_newline(state);
      if (state->in_list < MAX_LIST_DEPTH) {
         state->list_is_ordered[state->in_list] = 0;
         state->in_list++;
      }
      return;
   }

   // Ordered list
   if (strcasecmp(tag_name, "ol") == 0) {
      ensure_newline(state);
      if (state->in_list < MAX_LIST_DEPTH) {
         state->list_is_ordered[state->in_list] = 1;
         state->list_item_num[state->in_list] = 1;
         state->in_list++;
      }
      return;
   }

   // List item
   if (strcasecmp(tag_name, "li") == 0) {
      emit_list_prefix(state);
      state->suppress_whitespace = 1;
      return;
   }

   // Links
   if (strcasecmp(tag_name, "a") == 0) {
      state->in_link = 1;
      state->link_text_pos = 0;
      state->link_text[0] = '\0';
      state->link_href[0] = '\0';
      extract_attr(tag_content, "href", state->link_href, sizeof(state->link_href));
      return;
   }

   // Table elements - simplified
   if (strcasecmp(tag_name, "tr") == 0) {
      ensure_newline(state);
      return;
   }
   if (strcasecmp(tag_name, "td") == 0 || strcasecmp(tag_name, "th") == 0) {
      emit_str(state, "| ");
      return;
   }
}

static void handle_tag_close(html_parser_state_t *state, const char *tag_name) {
   // Headings
   if (tag_name[0] == 'h' && tag_name[1] >= '1' && tag_name[1] <= '6' && tag_name[2] == '\0') {
      ensure_blank_line(state);
      return;
   }

   // Paragraphs and divs
   if (strcasecmp(tag_name, "p") == 0 || strcasecmp(tag_name, "div") == 0) {
      ensure_blank_line(state);
      return;
   }

   // Bold
   if (strcasecmp(tag_name, "b") == 0 || strcasecmp(tag_name, "strong") == 0) {
      emit_str(state, "**");
      return;
   }

   // Italic
   if (strcasecmp(tag_name, "i") == 0 || strcasecmp(tag_name, "em") == 0) {
      emit_char(state, '*');
      return;
   }

   // Code (inline)
   if (strcasecmp(tag_name, "code") == 0 && !state->in_pre) {
      emit_char(state, '`');
      state->in_code = 0;
      return;
   }

   // Preformatted
   if (strcasecmp(tag_name, "pre") == 0) {
      ensure_newline(state);
      emit_str(state, "```");
      ensure_blank_line(state);
      state->in_pre = 0;
      return;
   }

   // Blockquote
   if (strcasecmp(tag_name, "blockquote") == 0) {
      if (state->in_blockquote > 0)
         state->in_blockquote--;
      ensure_blank_line(state);
      return;
   }

   // Lists
   if (strcasecmp(tag_name, "ul") == 0 || strcasecmp(tag_name, "ol") == 0) {
      if (state->in_list > 0)
         state->in_list--;
      if (state->in_list == 0)
         ensure_blank_line(state);
      return;
   }

   // List item
   if (strcasecmp(tag_name, "li") == 0) {
      ensure_newline(state);
      return;
   }

   // Links
   if (strcasecmp(tag_name, "a") == 0) {
      if (state->in_link && state->link_text[0]) {
         // Only emit link if we have both text and href
         if (state->link_href[0] && strncmp(state->link_href, "javascript:", 11) != 0) {
            // Resolve relative URLs if we have a base URL
            char resolved_href[2048];
            if (state->base_host[0]) {
               resolve_url(state->link_href, state->base_scheme, state->base_host, state->base_path,
                           resolved_href, sizeof(resolved_href));
            } else {
               strncpy(resolved_href, state->link_href, sizeof(resolved_href) - 1);
               resolved_href[sizeof(resolved_href) - 1] = '\0';
            }

            emit_char(state, '[');
            emit_str(state, state->link_text);
            emit_str(state, "](");
            emit_str(state, resolved_href);
            emit_char(state, ')');
         } else {
            // No href or javascript link - just emit text
            emit_str(state, state->link_text);
         }
      }
      state->in_link = 0;
      return;
   }

   // Table row end
   if (strcasecmp(tag_name, "tr") == 0) {
      emit_str(state, " |");
      ensure_newline(state);
      return;
   }
}

// =============================================================================
// CSS Stripping (Post-Processing)
// =============================================================================

/**
 * @brief Check if a line looks like a CSS rule
 *
 * Matches patterns like:
 * - .class { property: value }
 * - #id { property: value }
 * - element { property: value }
 * - [attr] { property: value }
 *
 * Also matches lines that are pure CSS property-value pairs:
 * - property: value;
 */
static bool is_css_line(const char *line, size_t len) {
   if (len == 0)
      return false;

   /* Skip leading whitespace */
   size_t i = 0;
   while (i < len && isspace(line[i]))
      i++;

   if (i >= len)
      return false;

   /* Check for @ rules that weren't in block form */
   if (line[i] == '@')
      return true;

   /* Check for selectors starting with . # [ or alphanumeric */
   bool has_selector_start = (line[i] == '.' || line[i] == '#' || line[i] == '[' ||
                              line[i] == '*' || isalpha(line[i]));

   if (!has_selector_start)
      return false;

   /* Look for { and } on the same line with : inside - strong CSS indicator */
   const char *open_brace = memchr(line + i, '{', len - i);
   const char *close_brace = memchr(line + i, '}', len - i);
   const char *colon = memchr(line + i, ':', len - i);

   if (open_brace && close_brace && colon && open_brace < colon && colon < close_brace) {
      return true;
   }

   /* Check for standalone property: value; pattern */
   if (colon && !open_brace) {
      /* Look for ; after colon */
      const char *semicolon = memchr(colon, ';', len - (colon - line));
      if (semicolon) {
         /* Check if before colon looks like a CSS property (alphanumeric and -) */
         bool looks_like_property = true;
         for (const char *p = line + i; p < colon && looks_like_property; p++) {
            if (!isalnum(*p) && *p != '-' && !isspace(*p)) {
               looks_like_property = false;
            }
         }
         if (looks_like_property)
            return true;
      }
   }

   return false;
}

/* Maximum characters to scan when looking for CSS block end.
 * Prevents hanging on malformed input (e.g., unclosed braces).
 * 64KB should handle any legitimate CSS block while bounding worst-case. */
#define CSS_BLOCK_MAX_SCAN 65536

/* CSS at-rule prefixes to strip (with braced blocks) */
static const char *CSS_BLOCK_KEYWORDS[] = { "@layer",    "@media",   "@keyframes", "@font-face",
                                            "@supports", "@charset", "@import",    "@namespace" };
#define CSS_BLOCK_KEYWORD_COUNT (sizeof(CSS_BLOCK_KEYWORDS) / sizeof(CSS_BLOCK_KEYWORDS[0]))

/**
 * @brief Find the end of a CSS block starting with @keyword
 *
 * Handles nested braces properly. Returns NULL if block exceeds
 * CSS_BLOCK_MAX_SCAN characters to prevent hanging on malformed input.
 */
static const char *find_css_block_end(const char *start, const char *end) {
   const char *p = start;
   const char *max_scan = start + CSS_BLOCK_MAX_SCAN;
   if (max_scan > end)
      max_scan = end;

   /* Find opening brace */
   while (p < max_scan && *p != '{')
      p++;

   if (p >= max_scan)
      return NULL;

   p++; /* Skip { */
   int depth = 1;

   while (p < max_scan && depth > 0) {
      if (*p == '{')
         depth++;
      else if (*p == '}')
         depth--;
      p++;
   }

   return (depth == 0) ? p : NULL;
}

/**
 * @brief Strip CSS artifacts from markdown output (single-pass)
 *
 * Removes:
 * - @layer { ... } blocks
 * - @media { ... } blocks
 * - @keyframes { ... } blocks
 * - @font-face { ... } blocks
 * - @supports { ... } blocks
 * - Lines that look like CSS rules
 * - Consecutive blank lines (max 2 newlines in a row)
 *
 * @param text Markdown text (modified in place)
 * @return New length of text
 */
static size_t strip_css_artifacts(char *text) {
   if (!text)
      return 0;

   size_t len = strlen(text);
   char *read = text;
   char *write = text;
   const char *end = text + len;
   int consecutive_newlines = 0;
   bool at_line_start = true;

   while (read < end) {
      /* Check for CSS @ blocks */
      if (*read == '@') {
         bool is_css_block = false;
         for (size_t k = 0; k < CSS_BLOCK_KEYWORD_COUNT; k++) {
            size_t kw_len = strlen(CSS_BLOCK_KEYWORDS[k]);
            if (strncmp(read, CSS_BLOCK_KEYWORDS[k], kw_len) == 0) {
               is_css_block = true;
               break;
            }
         }

         if (is_css_block) {
            const char *block_end = find_css_block_end(read, end);
            if (block_end) {
               /* Skip entire block */
               read = (char *)block_end;
               /* Skip trailing whitespace/newlines */
               while (read < end && (*read == ' ' || *read == '\t' || *read == '\n'))
                  read++;
               at_line_start = true;
               continue;
            }
         }
      }

      /* Check for CSS lines at start of line */
      if (at_line_start) {
         const char *line_end = memchr(read, '\n', end - read);
         size_t line_len = line_end ? (size_t)(line_end - read) : (size_t)(end - read);

         if (is_css_line(read, line_len)) {
            /* Skip this line */
            read = line_end ? (char *)line_end + 1 : (char *)end;
            /* Don't increment consecutive_newlines - we're skipping the line entirely */
            continue;
         }
      }

      /* Handle newlines with consecutive limit */
      if (*read == '\n') {
         consecutive_newlines++;
         if (consecutive_newlines <= MAX_CONSECUTIVE_NEWLINES) {
            *write++ = *read;
         }
         read++;
         at_line_start = true;
         continue;
      }

      /* Non-newline character */
      consecutive_newlines = 0;
      at_line_start = false;
      *write++ = *read++;
   }

   *write = '\0';
   return (size_t)(write - text);
}

// =============================================================================
// Public API
// =============================================================================

int html_extract_text_with_base(const char *html,
                                size_t html_len,
                                char **out_text,
                                const char *base_url) {
   if (!html || !out_text) {
      return HTML_PARSE_ERROR_INVALID_INPUT;
   }

   // Initialize parser state
   html_parser_state_t state = { 0 };

   // Heuristic: Markdown output is typically much smaller than HTML
   // (we strip scripts, styles, navigation, comments, tags themselves, etc.)
   // Start with ~1/3 of HTML size, minimum buffer for small pages
   // The buffer will grow via realloc if needed (doubling strategy)
   size_t initial_size = html_len / 3;
   if (initial_size < HTML_INITIAL_BUFFER_MIN)
      initial_size = HTML_INITIAL_BUFFER_MIN;

   state.out_capacity = initial_size;
   state.output = malloc(state.out_capacity);
   if (!state.output) {
      return HTML_PARSE_ERROR_ALLOC;
   }
   state.output[0] = '\0';
   state.last_was_newline = 1;
   state.suppress_whitespace = 1;

   // Parse base URL for link resolution
   if (base_url) {
      strncpy(state.base_url, base_url, sizeof(state.base_url) - 1);
      state.base_url[sizeof(state.base_url) - 1] = '\0';  // Ensure null termination
      parse_url_components(base_url, state.base_scheme, sizeof(state.base_scheme), state.base_host,
                           sizeof(state.base_host), state.base_path, sizeof(state.base_path));
   }

   const char *p = html;
   const char *end = html + html_len;

   // Try to find <body> to skip header cruft
   const char *body = strcasestr_portable(html, "<body");
   if (body) {
      // Skip to after the opening body tag
      const char *body_end = strchr(body, '>');
      if (body_end)
         p = body_end + 1;
   }

   while (p < end) {
      // HTML comment
      if (strncmp(p, "<!--", 4) == 0) {
         const char *comment_end = strstr(p + 4, "-->");
         if (comment_end) {
            p = comment_end + 3;
            continue;
         }
      }

      // Start of tag
      if (*p == '<') {
         const char *tag_start = p + 1;

         // Find end of tag
         const char *tag_end = strchr(tag_start, '>');
         if (!tag_end) {
            p++;
            continue;
         }

         // Extract tag name
         char tag_name[64] = { 0 };
         int is_closing = 0;
         const char *name_start = tag_start;

         if (*name_start == '/') {
            is_closing = 1;
            name_start++;
         }

         size_t name_len = 0;
         while (name_start + name_len < tag_end && !isspace(name_start[name_len]) &&
                name_start[name_len] != '/' && name_start[name_len] != '>' &&
                name_len < sizeof(tag_name) - 1) {
            tag_name[name_len] = tolower(name_start[name_len]);
            name_len++;
         }
         tag_name[name_len] = '\0';

         // Check for skip tags
         if (!is_closing && is_skip_tag(tag_name, name_len)) {
            const char *close = find_closing_tag(p, tag_name);
            if (close) {
               p = close + 1;
               continue;
            }
         }

         // Stop at </body>
         if (is_closing && strcasecmp(tag_name, "body") == 0) {
            break;
         }

         // Extract tag content for attribute parsing (stack-allocated, avoid malloc)
         size_t tag_content_len = tag_end - tag_start;
         char tag_content[MAX_TAG_CONTENT_LEN];
         size_t copy_len = (tag_content_len < MAX_TAG_CONTENT_LEN - 1) ? tag_content_len
                                                                       : MAX_TAG_CONTENT_LEN - 1;
         memcpy(tag_content, tag_start, copy_len);
         tag_content[copy_len] = '\0';

         // Check for noise class/id patterns (modals, popups, subscriptions, etc.)
         if (!is_closing && has_noise_class_or_id(tag_content)) {
            const char *close = find_closing_tag(p, tag_name);
            if (close) {
               p = close + 1;
               continue;
            }
         }

         if (is_closing) {
            handle_tag_close(&state, tag_name);
         } else {
            handle_tag_open(&state, tag_name, tag_content);
         }

         p = tag_end + 1;
         continue;
      }

      // Entity
      if (*p == '&') {
         char decoded[16];
         int consumed = decode_entity(p, decoded);

         if (state.in_link) {
            // Accumulate link text
            size_t dlen = strlen(decoded);
            if (state.link_text_pos + dlen < sizeof(state.link_text) - 1) {
               strcpy(state.link_text + state.link_text_pos, decoded);
               state.link_text_pos += dlen;
            }
         } else {
            if (state.in_blockquote && state.last_was_newline) {
               for (int i = 0; i < state.in_blockquote; i++) {
                  emit_str(&state, "> ");
               }
            }
            flush_blank_line(&state);
            emit_str(&state, decoded);
         }

         p += consumed;
         state.suppress_whitespace = 0;
         continue;
      }

      // Whitespace
      if (isspace(*p)) {
         if (state.in_pre) {
            emit_char(&state, *p);
         } else if (state.in_link) {
            // Link text: always convert whitespace to space (don't depend on last_was_newline)
            if (state.link_text_pos < sizeof(state.link_text) - 1 && state.link_text_pos > 0 &&
                state.link_text[state.link_text_pos - 1] != ' ') {
               state.link_text[state.link_text_pos++] = ' ';
               state.link_text[state.link_text_pos] = '\0';
            }
         } else if (!state.suppress_whitespace && !state.last_was_newline) {
            emit_char(&state, ' ');
         }
         p++;
         continue;
      }

      // Regular text
      if (state.in_link) {
         if (state.link_text_pos < sizeof(state.link_text) - 1) {
            state.link_text[state.link_text_pos++] = *p;
            state.link_text[state.link_text_pos] = '\0';
         }
      } else {
         if (state.in_blockquote && state.last_was_newline) {
            for (int i = 0; i < state.in_blockquote; i++) {
               emit_str(&state, "> ");
            }
         }
         flush_blank_line(&state);
         emit_char(&state, *p);
      }

      state.suppress_whitespace = 0;
      p++;
   }

   // Trim trailing whitespace
   while (state.out_pos > 0 && isspace(state.output[state.out_pos - 1])) {
      state.output[--state.out_pos] = '\0';
   }

   // Strip CSS artifacts that leaked through (CSS-in-JS, critical CSS, etc.)
   state.out_pos = strip_css_artifacts(state.output);

   // Trim again after CSS stripping
   while (state.out_pos > 0 && isspace(state.output[state.out_pos - 1])) {
      state.output[--state.out_pos] = '\0';
   }

   // Check minimum content
   if (state.out_pos < HTML_MIN_CONTENT_LEN) {
      free(state.output);
      return HTML_PARSE_ERROR_EMPTY;
   }

   *out_text = state.output;
   return HTML_PARSE_SUCCESS;
}

int html_extract_text(const char *html, size_t html_len, char **out_text) {
   return html_extract_text_with_base(html, html_len, out_text, NULL);
}

#undef EMIT_LIT
