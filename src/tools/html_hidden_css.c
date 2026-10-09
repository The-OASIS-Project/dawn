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
 * Reading CSS for html_hidden.h: numbers and lengths (locale-independent),
 * values as a browser tokenizes them, and what a set of declarations says
 * about whether its element's text can be seen.
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "tools/html_hidden.h"
#include "tools/html_hidden_internal.h"

/* Thresholds, in CSS px against a 16px font and a 600px screen: a font at
 * most this size can't be read; a box at most this size shows nothing; an
 * offset this far moves it off any screen. */
#define VIS_FONT_TINY_PX 4.0
#define VIS_BOX_ZERO_PX 1.0
#define VIS_OFFSCREEN_PX (-500.0)
#define VIS_OFFSCREEN_RIGHT_PX 3000.0
/* An alpha or opacity below this shows nothing readable. */
#define VIS_ALPHA_MIN 0.05

/* =============================================================================
 * Numbers and lengths (locale-independent)
 * ============================================================================= */

/* A CSS number at @p s; *end after it.  False when there is none. */
static bool css_number(const char *s, double *out, const char **end) {
   const char *p = s;
   double sign = 1.0;
   if (*p == '+' || *p == '-') {
      sign = *p == '-' ? -1.0 : 1.0;
      p++;
   }
   double v = 0.0;
   bool digits = false;
   while (isdigit((unsigned char)*p)) {
      if (v < 1e12)
         v = v * 10.0 + (*p - '0');
      p++;
      digits = true;
   }
   if (*p == '.' && isdigit((unsigned char)p[1])) {
      double scale = 0.1;
      p++;
      while (isdigit((unsigned char)*p)) {
         v += (*p - '0') * scale;
         scale /= 10.0;
         p++;
         digits = true;
      }
   }
   if (!digits)
      return false;
   if ((*p == 'e' || *p == 'E') &&
       (isdigit((unsigned char)p[1]) ||
        ((p[1] == '-' || p[1] == '+') && isdigit((unsigned char)p[2])))) {
      const char *q = p + 1;
      int esign = 1;
      if (*q == '-' || *q == '+') {
         esign = *q == '-' ? -1 : 1;
         q++;
      }
      int e = 0;
      while (isdigit((unsigned char)*q)) {
         if (e < 400)
            e = e * 10 + (*q - '0');
         q++;
      }
      /* Past 10^±330 a double is already inf or 0: scaling further is only
       * work a hostile style could repeat per number ("1e3999"). */
      if (e > 330)
         e = 330;
      while (e-- > 0)
         v = esign > 0 ? v * 10.0 : v / 10.0;
      p = q;
   }
   *out = sign * v;
   *end = p;
   return true;
}

/* A length at @p s in CSS px: absolute units as they are, em/ex/ch against a
 * 16px font (HV_LEN_RELATIVE: to the parent's), vw/vh against a 600px screen.
 * *unit says whether it had one ("0" and quirks-mode bare numbers have
 * none). */
hv_len_kind_t hv_css_length(const char *s, double *px, bool *unit) {
   double v;
   const char *u;
   if (!css_number(s, &v, &u))
      return HV_LEN_NONE;
   *unit = true;
   if (*u == '%') {
      *px = v * 16.0 / 100.0;
      return HV_LEN_PERCENT;
   }
   static const struct {
      const char *unit;
      double scale;
      bool relative;
   } k_units[] = { { "px", 1.0, false },   { "pt", 4.0 / 3.0, false }, { "pc", 16.0, false },
                   { "em", 16.0, true },   { "rem", 16.0, false },     { "ex", 8.0, true },
                   { "ch", 8.0, true },    { "vw", 6.0, false },       { "vh", 6.0, false },
                   { "vmin", 6.0, false }, { "vmax", 6.0, false },     { "cm", 37.8, false },
                   { "mm", 3.78, false },  { "in", 96.0, false },      { "q", 0.945, false } };
   for (size_t i = 0; i < sizeof(k_units) / sizeof(k_units[0]); i++) {
      const size_t n = strlen(k_units[i].unit);
      if (strncmp(u, k_units[i].unit, n) == 0 && !isalpha((unsigned char)u[n])) {
         *px = v * k_units[i].scale;
         return k_units[i].relative ? HV_LEN_RELATIVE : HV_LEN_ABSOLUTE;
      }
   }
   if (isalpha((unsigned char)*u))
      return HV_LEN_NONE;
   *unit = false;
   *px = v;
   return HV_LEN_ABSOLUTE;
}

/* =============================================================================
 * Declarations
 * ============================================================================= */

/* The UTF-8 for code point @p cp at @p out (U+FFFD for none); its length. */
size_t hv_utf8_put(unsigned long cp, char *out) {
   if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
      cp = 0xFFFD;
   if (cp < 0x80) {
      out[0] = (char)cp;
      return 1;
   }
   if (cp < 0x800) {
      out[0] = (char)(0xC0 | (cp >> 6));
      out[1] = (char)(0x80 | (cp & 0x3F));
      return 2;
   }
   if (cp < 0x10000) {
      out[0] = (char)(0xE0 | (cp >> 12));
      out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[2] = (char)(0x80 | (cp & 0x3F));
      return 3;
   }
   out[0] = (char)(0xF0 | (cp >> 18));
   out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
   out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
   out[3] = (char)(0x80 | (cp & 0x3F));
   return 4;
}

/* @p css as a browser tokenizes it, simplified: comments removed, escapes
 * resolved (an escaped character is part of a word, so an escaped ';', '(',
 * quote or brace becomes a plain letter and splits nothing), whitespace runs
 * one space, ASCII lowercased.  Heap; NULL on allocation failure. */
char *hv_css_normalize(const char *css, size_t len) {
   char *out = malloc(len * 4 + 1);
   if (!out)
      return NULL;
   size_t n = 0;
   bool space = false;
   for (size_t i = 0; i < len; i++) {
      unsigned char c = (unsigned char)css[i];
      char enc[4];
      size_t enc_len = 0;
      if (c == '/' && i + 1 < len && css[i + 1] == '*') {
         size_t j = i + 2;
         while (j + 1 < len && !(css[j] == '*' && css[j + 1] == '/'))
            j++;
         i = j + 1 < len ? j + 1 : len;
         continue;
      }
      if (c == '\\' && i + 1 < len) {
         size_t j = i + 1;
         unsigned long cp = 0;
         int hex = 0;
         while (j < len && hex < 6 && isxdigit((unsigned char)css[j])) {
            const char h = (char)tolower((unsigned char)css[j]);
            cp = cp * 16 + (unsigned long)(h <= '9' ? h - '0' : h - 'a' + 10);
            j++;
            hex++;
         }
         if (hex > 0) {
            if (j < len && isspace((unsigned char)css[j]))
               j++; /* the one space ending an escape */
            i = j - 1;
         } else if (css[j] == '\n') {
            i = j; /* an escaped newline is nothing */
            continue;
         } else {
            cp = (unsigned char)css[j];
            i = j;
         }
         if (cp < 0x80 && strchr(";:(){}'\"!,", (int)cp) && cp != 0)
            cp = '_'; /* part of a word, never structure */
         enc_len = hv_utf8_put(cp < 0x80 ? (unsigned long)tolower((int)cp) : cp, enc);
      } else {
         if (c == '\0') {
            enc_len = hv_utf8_put(0xFFFD, enc); /* as a browser reads a NUL */
         } else if (isspace(c)) {
            space = true;
            continue;
         } else {
            enc[0] = (char)tolower(c);
            enc_len = 1;
         }
      }
      if (space && n > 0)
         out[n++] = ' ';
      space = false;
      memcpy(out + n, enc, enc_len);
      n += enc_len;
   }
   out[n] = '\0';
   return out;
}

/* The properties read. */
enum {
   P_DISPLAY,
   P_VISIBILITY,
   P_OPACITY,
   P_FILTER,
   P_CONTENT_VISIBILITY,
   P_FONT_SIZE,
   P_FONT,
   P_COLOR,
   P_TEXT_FILL,
   P_BG_CLIP,
   P_BG_CLIP_WEBKIT,
   P_OVERFLOW,
   P_OVERFLOW_X,
   P_OVERFLOW_Y,
   P_WIDTH,
   P_MAX_WIDTH,
   P_HEIGHT,
   P_MAX_HEIGHT,
   P_LEFT,
   P_TOP,
   P_RIGHT,
   P_BOTTOM,
   P_TEXT_INDENT,
   P_MARGIN_LEFT,
   P_MARGIN_TOP,
   P_CLIP,
   P_CLIP_PATH,
   P_TRANSFORM,
   P_SCALE,
   P_ZOOM,
   P_COUNT
};

static const char *const k_props[P_COUNT] = { "display",
                                              "visibility",
                                              "opacity",
                                              "filter",
                                              "content-visibility",
                                              "font-size",
                                              "font",
                                              "color",
                                              "-webkit-text-fill-color",
                                              "background-clip",
                                              "-webkit-background-clip",
                                              "overflow",
                                              "overflow-x",
                                              "overflow-y",
                                              "width",
                                              "max-width",
                                              "height",
                                              "max-height",
                                              "left",
                                              "top",
                                              "right",
                                              "bottom",
                                              "text-indent",
                                              "margin-left",
                                              "margin-top",
                                              "clip",
                                              "clip-path",
                                              "transform",
                                              "scale",
                                              "zoom" };

/* The last value of each property (an !important one beats a later plain
 * one), pointing into the normalized text. */
typedef struct {
   const char *value[P_COUNT];
   size_t len[P_COUNT];
   bool important[P_COUNT];
} decl_values_t;

/* The end of the declaration starting at @p p: its ';' outside strings and
 * parentheses, or the end. */
static char *decl_end(char *p) {
   int paren = 0;
   char quote = 0;
   for (; *p; p++) {
      if (quote) {
         if (*p == quote)
            quote = 0;
      } else if (*p == '"' || *p == '\'') {
         quote = *p;
      } else if (*p == '(') {
         paren++;
      } else if (*p == ')' && paren > 0) {
         paren--;
      } else if (*p == ';' && paren == 0) {
         break;
      }
   }
   return p;
}

static void collect_values(char *css, decl_values_t *v) {
   memset(v, 0, sizeof(*v));
   char *p = css;
   while (*p) {
      char *start = p;
      char *stop = decl_end(p);
      p = *stop ? stop + 1 : stop;
      char *colon = memchr(start, ':', (size_t)(stop - start));
      if (!colon)
         continue;
      char *name = start;
      char *name_end = colon;
      while (name < name_end && *name == ' ')
         name++;
      while (name_end > name && name_end[-1] == ' ')
         name_end--;
      char *val = colon + 1;
      char *val_end = stop;
      while (val < val_end && *val == ' ')
         val++;
      while (val_end > val && val_end[-1] == ' ')
         val_end--;
      bool important = false;
      char *bang = NULL;
      for (char *b = val_end; b > val; b--) {
         if (b[-1] == '!') {
            bang = b - 1;
            break;
         }
      }
      if (bang) {
         char *w = bang + 1;
         while (w < val_end && *w == ' ')
            w++;
         if ((size_t)(val_end - w) == 9 && strncmp(w, "important", 9) == 0) {
            important = true;
            val_end = bang;
            while (val_end > val && val_end[-1] == ' ')
               val_end--;
         }
      }
      for (int k = 0; k < P_COUNT; k++) {
         const size_t n = strlen(k_props[k]);
         if ((size_t)(name_end - name) != n || strncmp(name, k_props[k], n) != 0)
            continue;
         if (v->important[k] && !important)
            break;
         v->value[k] = val;
         v->len[k] = (size_t)(val_end - val);
         v->important[k] = important;
         break;
      }
   }
}

/* Whether value @p v starts with keyword @p kw as a word. */
static bool starts_word(const char *v, const char *kw) {
   const size_t k = strlen(kw);
   return strncmp(v, kw, k) == 0 && !(isalnum((unsigned char)v[k]) || v[k] == '-');
}

/* Whether @p v starts with a math function, and which. */
static const char *math_fn(const char *v) {
   static const char *const k_fns[] = { "calc(", "min(", "max(", "clamp(", "var(", NULL };
   for (int i = 0; k_fns[i]; i++) {
      if (strncmp(v, k_fns[i], strlen(k_fns[i])) == 0)
         return k_fns[i];
   }
   return NULL;
}

/* A font size: 1 too small to read, 0 readable, 2 relative to the parent's
 * (so as tiny as it is), -1 not a size. */
/* A math function's font size: its lengths, all absolute, decide it (min()
 * tiny when any is; the others when all are); anything else (var(), a
 * relative unit) depends on the parent. */
static int font_math_value(const char *val, const char *fn) {
   if (strcmp(fn, "var(") == 0)
      return 2;
   const char *close = strchr(val, ')');
   int lengths = 0;
   int tiny = 0;
   for (const char *p = val + strlen(fn); *p && p != close;) {
      double px;
      bool unit;
      const hv_len_kind_t k = isdigit((unsigned char)*p) || *p == '.' || *p == '-'
                                  ? hv_css_length(p, &px, &unit)
                                  : HV_LEN_NONE;
      if (k == HV_LEN_RELATIVE || k == HV_LEN_PERCENT)
         return 2;
      if (k == HV_LEN_ABSOLUTE) {
         lengths++;
         tiny += px <= VIS_FONT_TINY_PX;
         const char *e;
         double d;
         css_number(p, &d, &e);
         p = e;
         while (isalpha((unsigned char)*p))
            p++;
         continue;
      }
      p++;
   }
   if (lengths == 0)
      return 2;
   if (strcmp(fn, "min(") == 0)
      return tiny > 0 ? 1 : 0;
   return tiny == lengths ? 1 : 0;
}

/* A font size: 1 too small to read, 0 readable, 2 relative to the parent's
 * (so as tiny as it is), -1 not a size. */
static int font_size_value(const char *val) {
   if (starts_word(val, "larger") || starts_word(val, "smaller"))
      return 2;
   const char *fn = math_fn(val);
   if (fn)
      return font_math_value(val, fn);
   static const char *const k_keywords[] = { "xx-small", "x-small",  "small",     "medium", "large",
                                             "x-large",  "xx-large", "xxx-large", NULL };
   for (int i = 0; k_keywords[i]; i++) {
      if (starts_word(val, k_keywords[i]))
         return 0;
   }
   double px;
   bool unit;
   const hv_len_kind_t k = hv_css_length(val, &px, &unit);
   if (k == HV_LEN_NONE)
      return -1;
   if (px <= VIS_FONT_TINY_PX)
      return 1;
   return k == HV_LEN_RELATIVE || k == HV_LEN_PERCENT ? 2 : 0;
}

/* The `font` shorthand's size: the token before '/', or the first length
 * with a unit (a bare number there is a weight). */
static int font_shorthand_size(const char *val) {
   const char *slash = strchr(val, '/');
   if (slash) {
      const char *s = slash;
      while (s > val && s[-1] != ' ')
         s--;
      const int r = font_size_value(s);
      if (r >= 0)
         return r;
   }
   const char *p = val;
   while (*p) {
      while (*p == ' ')
         p++;
      double px;
      bool unit;
      if (math_fn(p) || starts_word(p, "larger") || starts_word(p, "smaller"))
         return font_size_value(p);
      if (hv_css_length(p, &px, &unit) != HV_LEN_NONE && (unit || px == 0.0))
         return font_size_value(p);
      while (*p && *p != ' ')
         p++;
   }
   return -1;
}

/* Whether a color has no visible alpha: transparent, rgba/hsla with alpha
 * near 0, a "/ alpha" form, or #rgba / #rrggbbaa with alpha near 0. */
static int color_clear(const char *val) {
   /* The parent's (currentcolor, as a color, is the inherited one) */
   static const char *const k_inherit[] = { "inherit",      "unset",        "revert",
                                            "revert-layer", "currentcolor", NULL };
   for (int i = 0; k_inherit[i]; i++) {
      if (starts_word(val, k_inherit[i]))
         return HV_PARENT;
   }
   if (starts_word(val, "initial"))
      return HV_SHOWN;
   if (starts_word(val, "transparent"))
      return 1;
   if (val[0] == '#') {
      size_t n = 1;
      while (isxdigit((unsigned char)val[n]))
         n++;
      n--;
      if (n == 4)
         return val[4] == '0' ? 1 : 0;
      if (n == 8)
         return val[7] == '0' && val[8] <= '3' && isdigit((unsigned char)val[8]) ? 1 : 0;
      return 0;
   }
   const char *open = strchr(val, '(');
   const char *close = open ? strchr(open, ')') : NULL;
   if (!open || !close)
      return 0;
   int numbers = 0;
   bool slash = false;
   double last = 1.0;
   bool last_pct = false;
   for (const char *p = open + 1; p < close;) {
      double d;
      const char *e;
      if (*p == '/')
         slash = true;
      if (css_number(p, &d, &e)) {
         numbers++;
         last = d;
         last_pct = *e == '%';
         p = e;
      } else {
         p++;
      }
   }
   if (numbers == 4 || slash) {
      const double alpha = last_pct ? last / 100.0 : last;
      return alpha < VIS_ALPHA_MIN ? 1 : 0;
   }
   return 0;
}

static int box_zero(const char *val) {
   double px;
   bool unit;
   const hv_len_kind_t k = hv_css_length(val, &px, &unit);
   if (k == HV_LEN_NONE)
      return -1;
   return px <= VIS_BOX_ZERO_PX && px >= 0.0 ? 1 : 0;
}

/* An offset that moves the element off the screen. */
static int offset_off(const char *val, bool rightward_too) {
   double px;
   bool unit;
   const hv_len_kind_t k = hv_css_length(val, &px, &unit);
   if (k == HV_LEN_NONE)
      return -1;
   if (k == HV_LEN_PERCENT)
      return px <= -16.0 ? 1 : 0; /* -100% */
   return px <= VIS_OFFSCREEN_PX || (rightward_too && px >= VIS_OFFSCREEN_RIGHT_PX) ? 1 : 0;
}

static int overflow_hides(const char *val) {
   return strstr(val, "hidden") || strstr(val, "clip") ? 1 : 0;
}

/* clip: rect() with no area; clip-path: an inset of half or more, or a
 * shape of zero size. */
static int clip_none(const char *val, bool path) {
   if (!path) {
      if (strncmp(val, "rect(", 5) != 0)
         return 0;
      double n[4] = { 1, 1, 1, 1 };
      int k = 0;
      for (const char *p = val + 5; *p && *p != ')' && k < 4;) {
         double d;
         const char *e;
         if (css_number(p, &d, &e)) {
            n[k++] = d;
            p = e;
         } else {
            p++;
         }
      }
      /* rect(top, right, bottom, left): empty when bottom <= top or right <= left */
      return k == 4 && (n[2] <= n[0] || n[1] <= n[3]) ? 1 : 0;
   }
   double d;
   const char *e;
   if (strncmp(val, "inset(", 6) == 0 && css_number(val + 6, &d, &e))
      return *e == '%' && d >= 50.0 ? 1 : 0;
   if (strncmp(val, "polygon(", 8) == 0) {
      /* Fewer than three points: no area */
      const char *close = strchr(val, ')');
      int commas = 0;
      for (const char *c = val + 8; *c && c != close; c++)
         commas += *c == ',';
      return commas < 2 ? 1 : 0;
   }
   if ((strncmp(val, "circle(", 7) == 0 && css_number(val + 7, &d, &e)) ||
       (strncmp(val, "ellipse(", 8) == 0 && css_number(val + 8, &d, &e)))
      return d <= 0.0 ? 1 : 0;
   return 0;
}

/* A transform that scales to (nearly) nothing or translates off screen. */
static int transform_out(const char *val) {
   static const char *const k_scale[] = { "scale(", "scalex(", "scaley(", "scale3d(", NULL };
   for (int i = 0; k_scale[i]; i++) {
      for (const char *p = strstr(val, k_scale[i]); p; p = strstr(p + 1, k_scale[i])) {
         double d;
         const char *e;
         if (css_number(p + strlen(k_scale[i]), &d, &e) && d < VIS_ALPHA_MIN && d > -VIS_ALPHA_MIN)
            return 1;
      }
   }
   /* A flat matrix: no area left */
   for (const char *p = strstr(val, "matrix("); p; p = strstr(p + 1, "matrix(")) {
      double m[4];
      int k = 0;
      const char *q = p + 7;
      while (k < 4 && *q && *q != ')') {
         double d;
         const char *e;
         if (css_number(q, &d, &e)) {
            m[k++] = d;
            q = e;
         } else {
            q++;
         }
      }
      if (k == 4 && m[0] * m[3] - m[1] * m[2] < VIS_ALPHA_MIN * VIS_ALPHA_MIN &&
          m[0] * m[3] - m[1] * m[2] > -VIS_ALPHA_MIN * VIS_ALPHA_MIN)
         return 1;
   }
   static const char *const k_translate[] = { "translate(", "translatex(", "translatey(",
                                              "translate3d(", NULL };
   for (int i = 0; k_translate[i]; i++) {
      for (const char *p = strstr(val, k_translate[i]); p; p = strstr(p + 1, k_translate[i])) {
         const char *q = p + strlen(k_translate[i]);
         for (int arg = 0; arg < 2 && *q && *q != ')'; arg++) {
            while (*q == ' ' || *q == ',')
               q++;
            if (offset_off(q, true) == 1)
               return 1;
            while (*q && *q != ',' && *q != ')' && *q != ' ')
               q++;
         }
      }
   }
   return 0;
}

static void set_field(html_vis_decl_t *d, int field, int value, bool important) {
   if (value < 0)
      return;
   d->f[field] = (signed char)value;
   const unsigned bit = 1u << (unsigned)field;
   d->important = important ? (d->important | bit) : (d->important & ~bit);
}


/* A declaration set that says nothing. */
void hv_decl_clear(html_vis_decl_t *d) {
   memset(d->f, -1, sizeof(d->f));
   memset(d->order, 0, sizeof(d->order));
   d->important = 0;
}

void html_vis_declarations(const char *css, size_t len, html_vis_decl_t *out) {
   hv_decl_clear(out);
   char *norm = css ? hv_css_normalize(css, len) : NULL;
   if (!norm)
      return;
   decl_values_t v;
   collect_values(norm, &v);
   bool text_clip = false;
   for (int k = P_BG_CLIP; k <= P_BG_CLIP_WEBKIT; k++) {
      if (v.value[k] && v.len[k] >= 4 && strncmp(v.value[k], "text", 4) == 0)
         text_clip = true;
   }
   for (int k = 0; k < P_COUNT; k++) {
      if (!v.value[k])
         continue;
      char *buf = malloc(v.len[k] + 1);
      if (!buf)
         break;
      memcpy(buf, v.value[k], v.len[k]);
      buf[v.len[k]] = '\0';
      const bool imp = v.important[k];
      double d;
      const char *e;
      switch (k) {
         case P_DISPLAY:
            set_field(out, HV_DISPLAY, starts_word(buf, "none"), imp);
            break;
         case P_VISIBILITY:
            if (starts_word(buf, "hidden") || starts_word(buf, "collapse"))
               set_field(out, HV_VISIBILITY, HV_HIDDEN, imp);
            else if (starts_word(buf, "visible") || starts_word(buf, "initial"))
               set_field(out, HV_VISIBILITY, HV_SHOWN, imp);
            else if (starts_word(buf, "inherit") || starts_word(buf, "unset") ||
                     starts_word(buf, "revert") || starts_word(buf, "revert-layer"))
               set_field(out, HV_VISIBILITY, HV_PARENT, imp);
            break;
         case P_OPACITY:
            if (css_number(buf, &d, &e))
               set_field(out, HV_OPACITY, (*e == '%' ? d / 100.0 : d) < VIS_ALPHA_MIN, imp);
            break;
         case P_FILTER: {
            const char *f = strstr(buf, "opacity(");
            if (f && css_number(f + 8, &d, &e))
               set_field(out, HV_FILTER, (*e == '%' ? d / 100.0 : d) < VIS_ALPHA_MIN, imp);
            else
               set_field(out, HV_FILTER, 0, imp);
            break;
         }
         case P_CONTENT_VISIBILITY:
            set_field(out, HV_CONTENT_VISIBILITY, starts_word(buf, "hidden"), imp);
            break;
         case P_FONT_SIZE:
            set_field(out, HV_FONT_SIZE, font_size_value(buf), imp);
            break;
         case P_FONT:
            set_field(out, HV_FONT_SIZE, font_shorthand_size(buf), imp);
            break;
         case P_COLOR:
            set_field(out, HV_COLOR, color_clear(buf), imp);
            break;
         case P_TEXT_FILL:
            /* Gradient text fills transparent and paints the background
             * through it: shown.  currentcolor fills with the element's own
             * color. */
            if (text_clip)
               set_field(out, HV_TEXT_FILL, HV_SHOWN, imp);
            else if (starts_word(buf, "currentcolor"))
               set_field(out, HV_TEXT_FILL, HV_OWN_COLOR, imp);
            else
               set_field(out, HV_TEXT_FILL, color_clear(buf), imp);
            break;
         case P_OVERFLOW:
            set_field(out, HV_OVERFLOW_X, overflow_hides(buf), imp);
            set_field(out, HV_OVERFLOW_Y, overflow_hides(buf), imp);
            break;
         case P_OVERFLOW_X:
            set_field(out, HV_OVERFLOW_X, overflow_hides(buf), imp);
            break;
         case P_OVERFLOW_Y:
            set_field(out, HV_OVERFLOW_Y, overflow_hides(buf), imp);
            break;
         case P_WIDTH:
            set_field(out, HV_WIDTH, box_zero(buf), imp);
            break;
         case P_MAX_WIDTH:
            set_field(out, HV_MAX_WIDTH, box_zero(buf), imp);
            break;
         case P_HEIGHT:
            set_field(out, HV_HEIGHT, box_zero(buf), imp);
            break;
         case P_MAX_HEIGHT:
            set_field(out, HV_MAX_HEIGHT, box_zero(buf), imp);
            break;
         case P_LEFT:
            set_field(out, HV_LEFT, offset_off(buf, true), imp);
            break;
         case P_TEXT_INDENT:
            set_field(out, HV_TEXT_INDENT, offset_off(buf, true), imp);
            break;
         case P_TOP:
            set_field(out, HV_TOP, offset_off(buf, false), imp);
            break;
         case P_RIGHT:
            set_field(out, HV_RIGHT, offset_off(buf, false), imp);
            break;
         case P_BOTTOM:
            set_field(out, HV_BOTTOM, offset_off(buf, false), imp);
            break;
         case P_MARGIN_LEFT:
            set_field(out, HV_MARGIN_LEFT, offset_off(buf, false), imp);
            break;
         case P_MARGIN_TOP:
            set_field(out, HV_MARGIN_TOP, offset_off(buf, false), imp);
            break;
         case P_CLIP:
            set_field(out, HV_CLIP, clip_none(buf, false), imp);
            break;
         case P_CLIP_PATH:
            set_field(out, HV_CLIP_PATH, clip_none(buf, true), imp);
            break;
         case P_TRANSFORM:
            set_field(out, HV_TRANSFORM, transform_out(buf), imp);
            break;
         case P_SCALE:
            if (css_number(buf, &d, &e)) {
               const double sc = *e == '%' ? d / 100.0 : d;
               set_field(out, HV_SCALE, sc < VIS_ALPHA_MIN && sc > -VIS_ALPHA_MIN, imp);
            }
            break;
         case P_ZOOM:
            /* zoom: 0 means 1; a positive zoom near 0 shrinks it away */
            if (css_number(buf, &d, &e)) {
               const double z = *e == '%' ? d / 100.0 : d;
               set_field(out, HV_ZOOM, z > 0.0 && z < VIS_ALPHA_MIN, imp);
            }
            break;
         default:
            break;
      }
      free(buf);
   }
   free(norm);
}

/* @p over cascaded onto @p base: a field @p over says replaces @p base's,
 * unless only @p base's is !important. */
void hv_decl_merge(html_vis_decl_t *base, const html_vis_decl_t *over) {
   for (int i = 0; i < HV_FIELDS; i++) {
      if (over->f[i] < 0)
         continue;
      const unsigned bit = 1u << (unsigned)i;
      if ((base->important & bit) && !(over->important & bit))
         continue;
      base->f[i] = over->f[i];
      base->order[i] = over->order[i];
      base->important = (base->important & ~bit) | (over->important & bit);
   }
}

/* Only what hides: a rule that names ancestors (or every element) may hide
 * an element we can't tell it applies to, but never shows one. */
void hv_decl_hide_only(html_vis_decl_t *d) {
   for (int i = 0; i < HV_FIELDS; i++) {
      if (d->f[i] != 1)
         d->f[i] = -1;
   }
}

/* Whether a merged declaration set hides the whole element. */
bool hv_decl_hard(const html_vis_decl_t *d) {
   const signed char *f = d->f;
   return f[HV_DISPLAY] == 1 || f[HV_OPACITY] == 1 || f[HV_FILTER] == 1 ||
          f[HV_CONTENT_VISIBILITY] == 1 ||
          (f[HV_OVERFLOW_X] == 1 && (f[HV_WIDTH] == 1 || f[HV_MAX_WIDTH] == 1)) ||
          (f[HV_OVERFLOW_Y] == 1 && (f[HV_HEIGHT] == 1 || f[HV_MAX_HEIGHT] == 1)) ||
          f[HV_LEFT] == 1 || f[HV_TOP] == 1 || f[HV_RIGHT] == 1 || f[HV_BOTTOM] == 1 ||
          f[HV_TEXT_INDENT] == 1 || f[HV_MARGIN_LEFT] == 1 || f[HV_MARGIN_TOP] == 1 ||
          f[HV_CLIP] == 1 || f[HV_CLIP_PATH] == 1 || f[HV_TRANSFORM] == 1 || f[HV_SCALE] == 1 ||
          f[HV_ZOOM] == 1;
}

bool hv_decl_says_anything(const html_vis_decl_t *d) {
   for (int i = 0; i < HV_FIELDS; i++) {
      if (d->f[i] >= 0)
         return true;
   }
   return false;
}
