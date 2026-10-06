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
 * Email text made safe to show: a subject, a name, an address or a filename
 * from a message is the sender's text, shown to the model and in the browser.
 * Pure; no allocation.
 */

#ifndef EMAIL_DISPLAY_H
#define EMAIL_DISPLAY_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Also replace '/' and '\' (the value is a filename). */
#define EMAIL_DISPLAY_FILENAME 0x1u

/**
 * @brief Copy @p src (@p src_len bytes, may hold invalid UTF-8) as one display line
 *
 * Well-formed UTF-8 only (an ill-formed byte becomes '?'); line breaks and
 * tabs become a space; other C0 and C1 controls, DEL, bidi embeddings,
 * overrides, isolates and marks, and zero-width characters are removed
 * (they can make text read differently than it is, e.g. a reversed file
 * extension), and tag characters (invisible text a model reads).  Line and
 * paragraph separators become a space.  Leading and trailing spaces are trimmed.  The result is cut
 * on a character boundary to fit @p out_size.
 *
 * @return The length written (out is always NUL-terminated when out_size > 0)
 */
size_t email_display_sanitize(const char *src,
                              size_t src_len,
                              char *out,
                              size_t out_size,
                              unsigned flags);

/**
 * @brief Clean a message body in place for the model: tag characters
 *        (invisible text a model still reads) removed, line and paragraph
 *        separators turned into '\n'
 *
 * Expects valid UTF-8 (run after sanitize_utf8_for_json).  Other bidi and
 * zero-width characters stay: right-to-left text needs them.
 *
 * @return The new length
 */
size_t email_display_body_clean(char *s);

/**
 * @brief A Content-ID as the browser may use it: the value without its <>,
 *        letters, digits and "._@$+=-" only
 * @return false (and out "") when it holds anything else or doesn't fit
 */
bool email_display_content_id(const char *src, size_t src_len, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_DISPLAY_H */
