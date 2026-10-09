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
 *
 * This module extracts readable text content from HTML and converts it
 * to well-formatted Markdown. It handles:
 * - Headings (h1-h6)
 * - Bold/italic text
 * - Links (with relative URL resolution)
 * - Lists (ordered and unordered, nested)
 * - Code blocks and inline code
 * - Blockquotes
 * - Tables (simplified)
 * - HTML entity decoding
 *
 * Thread Safety: Functions in this module are thread-safe as they use only
 * stack-local state and caller-provided output buffers.
 *
 * Initialization: This module is stateless and requires no initialization
 * or cleanup. Each function call is independent and self-contained.
 */

#ifndef HTML_PARSER_H
#define HTML_PARSER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Error codes for HTML parser operations.
 * These are independent of url_fetcher error codes to maintain clean module separation.
 * Callers should check for HTML_PARSE_SUCCESS (0) to verify successful parsing.
 */
#define HTML_PARSE_SUCCESS 0             /* Parsing completed successfully */
#define HTML_PARSE_ERROR_INVALID_INPUT 1 /* NULL or invalid input parameters */
#define HTML_PARSE_ERROR_ALLOC 2         /* Memory allocation failed */
#define HTML_PARSE_ERROR_EMPTY 3         /* Output content too small/empty */

/**
 * @brief Extract readable Markdown from HTML content
 *
 * Strips scripts, styles, navigation, and converts HTML to Markdown.
 * Preserves document structure with headings, lists, links, and formatting.
 *
 * @param html Raw HTML content
 * @param html_len Length of HTML content
 * @param out_text Receives allocated Markdown text (caller frees)
 * @return HTML_PARSE_SUCCESS or error code
 */
int html_extract_text(const char *html, size_t html_len, char **out_text);

/**
 * @brief Extract readable Markdown from HTML content with base URL for links
 *
 * Same as html_extract_text but resolves relative URLs to absolute URLs.
 *
 * @param html Raw HTML content
 * @param html_len Length of HTML content
 * @param out_text Receives allocated Markdown text (caller frees)
 * @param base_url Base URL for resolving relative links (can be NULL)
 * @return HTML_PARSE_SUCCESS or error code
 */
int html_extract_text_with_base(const char *html,
                                size_t html_len,
                                char **out_text,
                                const char *base_url);

/**
 * @brief Extract plain text from HTML (no Markdown formatting)
 *
 * Like html_extract_text but treats tables as transparent containers and
 * emits link text without URLs. Ideal for HTML emails where tables are
 * used for layout and tracking URLs add noise.  Unlike the page extractors it
 * has no minimum length: a short (or empty) result is the message's text.
 *
 * @param html Raw HTML content
 * @param html_len Length of HTML content
 * @param out_text Receives allocated plain text (caller frees)
 * @return HTML_PARSE_SUCCESS or error code
 */
/* Every html_extract_* reads @p html_len bytes of @p html, which must also be
 * NUL-terminated at html_len (some searches run to the NUL). */
int html_extract_text_plain(const char *html, size_t html_len, char **out_text);

/**
 * @brief html_extract_text_plain, saying whether text a reader never sees
 *        (hidden by the email's styles) was left out.
 * @param hidden_dropped Set true when hidden text was left out (may be NULL)
 */
int html_extract_text_plain_ex(const char *html,
                               size_t html_len,
                               char **out_text,
                               bool *hidden_dropped);

#ifdef __cplusplus
}
#endif

#endif /* HTML_PARSER_H */
