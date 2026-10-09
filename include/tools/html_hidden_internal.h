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
 * Shared between html_hidden.c (the element stack, rules and matching) and
 * html_hidden_css.c (reading CSS values and declarations).  Not a public API.
 */

#ifndef HTML_HIDDEN_INTERNAL_H
#define HTML_HIDDEN_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "tools/html_hidden.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What a length is relative to (hv_css_length). */
typedef enum {
   HV_LEN_NONE,
   HV_LEN_ABSOLUTE,
   HV_LEN_RELATIVE,
   HV_LEN_PERCENT,
} hv_len_kind_t;

/**
 * @brief A length at @p s in CSS px: absolute units as they are, em/ex/ch
 *        against a 16px font (HV_LEN_RELATIVE: to the parent's), vw/vh
 *        against a 600px screen, a percentage as a share of 16px.
 * @param unit Set to whether it had a unit ("0" and bare numbers have none)
 */
hv_len_kind_t hv_css_length(const char *s, double *px, bool *unit);

/** @brief The UTF-8 for code point @p cp at @p out (U+FFFD for none); its length (1-4). */
size_t hv_utf8_put(unsigned long cp, char *out);

/**
 * @brief @p css as a browser tokenizes it, simplified: comments removed,
 *        escapes resolved, whitespace runs one space, ASCII lowercased.
 * @return Heap string, or NULL on allocation failure
 */
char *hv_css_normalize(const char *css, size_t len);

/** @brief A declaration set that says nothing. */
void hv_decl_clear(html_vis_decl_t *d);

/** @brief @p over cascaded onto @p base, field by field (!important kept). */
void hv_decl_merge(html_vis_decl_t *base, const html_vis_decl_t *over);

/** @brief Keeps only the values that hide. */
void hv_decl_hide_only(html_vis_decl_t *d);

/** @brief Whether a set hides its whole element (the non-inherited fields). */
bool hv_decl_hard(const html_vis_decl_t *d);

/** @brief Whether a set says anything at all. */
bool hv_decl_says_anything(const html_vis_decl_t *d);

#ifdef __cplusplus
}
#endif

#endif /* HTML_HIDDEN_INTERNAL_H */
