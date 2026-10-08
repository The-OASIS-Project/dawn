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
 * Which text of an HTML email a reader sees.  The HTML reader (html_parser.c,
 * plain mode) tells this module every tag it opens and closes; it keeps the
 * open elements on a stack with what each one's styles do to its text, and
 * says whether text at this point is shown, so a sender's hidden text is left
 * out of what the model reads.
 *
 * A filter, not a browser.  It follows inline styles (the last declaration,
 * !important, comments, escapes, character references), the `hidden`
 * attribute, rules in <style> blocks keyed by their subject's tag, classes or
 * id (with specificity; @media for every screen size but phones and print),
 * inherited properties a child can reset, and the end tags HTML lets an
 * author leave out or ignores.  It does not follow (a CSS engine would be
 * needed): which ancestors a selector names, pseudo-classes, attribute
 * selectors, formatting elements a browser reopens, text colored like its
 * background, external stylesheets.  Past a work limit the rest of a document
 * counts as hidden: a hostile document gets less read, never more.  Untrusted
 * mail is framed as data for the model whatever this leaves in.
 */

#ifndef HTML_HIDDEN_H
#define HTML_HIDDEN_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a set of declarations says, one field per CSS property (f[]): -1 not
 * said, else a value.  Each property cascades on its own, as in a browser;
 * whether an element is hidden is decided from the winners.  Hide flags are
 * 1 hidden / 0 not; the inherited ones (visibility, color, text fill, font
 * size) also take 2: the parent's (inherit, unset, revert, a relative size);
 * text fill takes 3: the element's own color. */
enum {
   HV_DISPLAY,
   HV_VISIBILITY,
   HV_OPACITY,
   HV_FILTER, /* filter: opacity() */
   HV_CONTENT_VISIBILITY,
   HV_FONT_SIZE, /* font-size, or the font shorthand's */
   HV_COLOR,
   HV_TEXT_FILL, /* -webkit-text-fill-color (not over background-clipped text) */
   HV_OVERFLOW_X,
   HV_OVERFLOW_Y,
   HV_WIDTH, /* at most 1px */
   HV_MAX_WIDTH,
   HV_HEIGHT,
   HV_MAX_HEIGHT,
   HV_LEFT, /* moved a page or more off the screen */
   HV_TOP,
   HV_RIGHT,
   HV_BOTTOM,
   HV_TEXT_INDENT,
   HV_MARGIN_LEFT,
   HV_MARGIN_TOP,
   HV_CLIP, /* clipped to nothing */
   HV_CLIP_PATH,
   HV_TRANSFORM, /* scaled to nothing, or translated off the screen */
   HV_SCALE,
   HV_ZOOM,
   HV_FIELDS
};

/* Values of the inherited fields */
#define HV_SHOWN 0
#define HV_HIDDEN 1
#define HV_PARENT 2
#define HV_OWN_COLOR 3

typedef struct {
   signed char f[HV_FIELDS];
   int order[HV_FIELDS];   /* where each value came from in the sheets (cascade order) */
   unsigned int important; /* bit per field: set by an !important declaration */
} html_vis_decl_t;

typedef struct html_vis html_vis_t;

/**
 * @brief The '>' ending the tag whose name starts at @p start (after '<'), as
 *        a browser reads it: a quote opens a string only as an attribute's
 *        value.  NULL when the tag never ends.
 */
const char *html_tag_end(const char *start, const char *end);

/**
 * @brief Where the comment starting at @p lt ("<!--") ends, as a browser ends
 *        it: "<!-->" and "<!--->" at once, else after "-->" or "--!>"; @p end
 *        when it never does.
 */
const char *html_comment_end(const char *lt, const char *end);

/**
 * @brief A visibility tracker for one document, with the rules of its <style>
 *        blocks, and where its <html> and <body> tags are.
 * @return NULL on allocation failure
 */
html_vis_t *html_vis_new(const char *html, size_t len);

void html_vis_free(html_vis_t *vis);

/**
 * @brief The document's first <html> or <body> open tag outside comments:
 *        @p tag after its '<', @p tag_end at its '>'.  False when it has none.
 */
bool html_vis_root(const html_vis_t *vis, bool body, const char **tag, const char **tag_end);

/**
 * @brief An element opens: its open tag spans @p tag (after '<') to
 *        @p tag_end (its '>').  A void element isn't kept open.
 * @param tag_name Lowercased
 * @return Whether the element's own text (an image's alt, say) is hidden
 */
bool html_vis_open(html_vis_t *vis,
                   const char *tag_name,
                   const char *tag,
                   const char *tag_end,
                   bool is_void);

/** @brief A closing tag, applied where a browser applies it. */
void html_vis_close(html_vis_t *vis, const char *tag_name);

/** @brief Whether text at this point is hidden. */
bool html_vis_hidden(const html_vis_t *vis);

/**
 * @brief What CSS declarations (an inline style, or a rule's block) say,
 *        read as a browser reads them; character references already decoded.
 */
void html_vis_declarations(const char *css, size_t len, html_vis_decl_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HTML_HIDDEN_H */
