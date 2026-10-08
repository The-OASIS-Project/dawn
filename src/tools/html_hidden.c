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
 * Which text of an HTML email a reader sees (html_hidden.h).
 */

#include "tools/html_hidden.h"

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>

#include "tools/html_hidden_internal.h"

/* Open elements tracked; deeper ones are counted (VIS_DEPTH_MAX). */
#define VIS_DEPTH_MAX 256
/* Rules kept from a document's <style> blocks; a name's length; the classes
 * one selector may require. */
#define VIS_RULES_MAX 4096
#define VIS_CLASSES_MAX 4
#define VIS_BUCKETS 1024
/* Nested at-rules followed (@media inside @supports, ...). */
#define VIS_AT_DEPTH_MAX 4
/* A tag name as the stack keeps it (the parser cuts longer ones the same). */
#define VIS_TAG_MAX 64
/* Steps (frames scanned, rules compared) a document may cost; past it the
 * rest of the document counts as hidden.  Ordinary mail uses a small part. */
#define VIS_WORK_MAX 20000000UL

/* A window this wide or wider is a desktop's: a max-width below it is a
 * phone's layout. */
#define VIS_DESKTOP_PX 800.0

/* The elements the stack treats specially, as small codes. */
typedef enum {
   K_OTHER = 0,
   K_P,
   K_LI,
   K_DD,
   K_DT,
   K_TD,
   K_TH,
   K_TR,
   K_TBODY,
   K_THEAD,
   K_TFOOT,
   K_OPTION,
   K_HEAD,
   K_TABLE,
   K_UL,
   K_OL,
   K_DL,
   K_BUTTON,
   K_BODY,
   K_HTML,
   K_SELECT,
   K_DATALIST,
   K_CAPTION,
   K_TEMPLATE,
   K_OBJECT,
   K_MARQUEE,
   K_APPLET,
} vis_kind_t;

#define KB(k) (1u << (k))
/* Scope boundaries, as the HTML parser has them */
#define SCOPE_DEFAULT                                                                 \
   (KB(K_TD) | KB(K_TH) | KB(K_TABLE) | KB(K_HTML) | KB(K_CAPTION) | KB(K_TEMPLATE) | \
    KB(K_OBJECT) | KB(K_MARQUEE) | KB(K_APPLET))
#define SCOPE_BUTTON (SCOPE_DEFAULT | KB(K_BUTTON))
#define SCOPE_LIST (SCOPE_DEFAULT | KB(K_UL) | KB(K_OL))
#define SCOPE_TABLE (KB(K_TABLE) | KB(K_HTML) | KB(K_TEMPLATE))
#define SCOPE_ROW (SCOPE_TABLE | KB(K_TBODY) | KB(K_THEAD) | KB(K_TFOOT))
#define SCOPE_CELL (SCOPE_ROW | KB(K_TR))
#define SCOPE_SELECT (KB(K_SELECT) | KB(K_DATALIST) | KB(K_HTML))

typedef struct {
   char tag[VIS_TAG_MAX]; /* "" when any tag */
   char *name;            /* the first class, or the id; "" for a tag-only rule */
   char *extra;           /* further required classes, space-separated ("" none) */
   bool is_id;
   int specificity;
   html_vis_decl_t decl; /* its values, each with its place in the sheets */
   int next;             /* bucket chain, -1 ends */
} vis_rule_t;

typedef struct {
   char tag[VIS_TAG_MAX];
   unsigned char kind;
   bool special;        /* the HTML parser's "special" category */
   bool hard;           /* this element or one around it hides everything in it */
   bool vis;            /* effective visibility */
   bool tiny;           /* effective font */
   bool clear;          /* effective color has no alpha */
   bool fill;           /* effective text fill has no alpha */
   bool closed_details; /* a <details> not open: only its <summary> shows */
} vis_frame_t;

struct html_vis {
   vis_rule_t *rules;
   int rule_count;
   int next_order;
   html_vis_decl_t universal; /* hide-only rules for every element (`*`) */
   bool has_universal;
   int buckets[VIS_BUCKETS];
   uint32_t seed; /* per document: a sender can't aim names at one bucket */
   vis_frame_t stack[VIS_DEPTH_MAX];
   int depth;                  /* frames in use */
   int overflow;               /* opens past VIS_DEPTH_MAX, still to close */
   int hide_level;             /* overflow level an untracked element started hiding at, 0 none */
   char hide_tag[VIS_TAG_MAX]; /* that element's name: only its end tag ends the hiding */
   unsigned long work;
   bool exhausted;
   const char *html_tag, *html_tag_end, *body_tag, *body_tag_end;
};

static bool spend(html_vis_t *vis, unsigned long steps) {
   vis->work += steps;
   if (vis->work > VIS_WORK_MAX)
      vis->exhausted = true;
   return !vis->exhausted;
}

/* =============================================================================
 * Tags
 * ============================================================================= */

const char *html_tag_end(const char *start, const char *end) {
   for (const char *p = start; p < end; p++) {
      if (*p == '>')
         return p;
      if (*p != '=')
         continue;
      const char *q = p + 1;
      while (q < end && isspace((unsigned char)*q))
         q++;
      if (q < end && (*q == '"' || *q == '\'')) {
         const char *close = memchr(q + 1, *q, (size_t)(end - q - 1));
         if (!close)
            return NULL;
         p = close;
         continue;
      }
      /* Unquoted: the value runs to whitespace or '>'; a quote in it is a
       * character */
      while (q < end && !isspace((unsigned char)*q) && *q != '>')
         q++;
      if (q < end && *q == '>')
         return q;
      p = q;
   }
   return NULL;
}

static int cmp_str(const void *a, const void *b) {
   return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static bool in_sorted(const char *tag, const char *const *set, size_t n) {
   return bsearch(&tag, set, n, sizeof(set[0]), cmp_str) != NULL;
}

/* The HTML parser's special elements (sorted). */
static const char *const k_special[] = {
   "address",    "applet",  "area",   "article",   "aside",   "base",     "basefont", "bgsound",
   "blockquote", "body",    "br",     "button",    "caption", "center",   "col",      "colgroup",
   "dd",         "details", "dir",    "div",       "dl",      "dt",       "embed",    "fieldset",
   "figcaption", "figure",  "footer", "form",      "frame",   "frameset", "h1",       "h2",
   "h3",         "h4",      "h5",     "h6",        "head",    "header",   "hgroup",   "hr",
   "html",       "iframe",  "img",    "input",     "li",      "link",     "listing",  "main",
   "marquee",    "menu",    "meta",   "nav",       "noembed", "noframes", "noscript", "object",
   "ol",         "p",       "param",  "plaintext", "pre",     "script",   "search",   "section",
   "select",     "source",  "style",  "summary",   "table",   "tbody",    "td",       "template",
   "textarea",   "tfoot",   "th",     "thead",     "title",   "tr",       "track",    "ul",
   "wbr",        "xmp"
};

/* Opening these closes an open <p> (sorted). */
static const char *const k_closes_p[] = {
   "address", "article", "aside",   "blockquote", "center", "dd",        "details",
   "dialog",  "dir",     "div",     "dl",         "dt",     "fieldset",  "figcaption",
   "figure",  "footer",  "form",    "h1",         "h2",     "h3",        "h4",
   "h5",      "h6",      "header",  "hgroup",     "hr",     "li",        "listing",
   "main",    "menu",    "nav",     "ol",         "p",      "plaintext", "pre",
   "search",  "section", "summary", "table",      "ul",     "xmp"
};

/* What may stay inside an open <head> (sorted). */
static const char *const k_head_set[] = { "base",     "basefont", "bgsound", "link",     "meta",
                                          "noscript", "script",   "style",   "template", "title" };

static vis_kind_t kind_of(const char *tag) {
   static const struct {
      const char *name;
      vis_kind_t kind;
   } k_kinds[] = {
      { "p", K_P },
      { "li", K_LI },
      { "dd", K_DD },
      { "dt", K_DT },
      { "td", K_TD },
      { "th", K_TH },
      { "tr", K_TR },
      { "tbody", K_TBODY },
      { "thead", K_THEAD },
      { "tfoot", K_TFOOT },
      { "option", K_OPTION },
      { "head", K_HEAD },
      { "table", K_TABLE },
      { "ul", K_UL },
      { "ol", K_OL },
      { "dl", K_DL },
      { "button", K_BUTTON },
      { "body", K_BODY },
      { "html", K_HTML },
      { "select", K_SELECT },
      { "datalist", K_DATALIST },
      { "caption", K_CAPTION },
      { "template", K_TEMPLATE },
      { "object", K_OBJECT },
      { "marquee", K_MARQUEE },
      { "applet", K_APPLET },
   };
   for (size_t i = 0; i < sizeof(k_kinds) / sizeof(k_kinds[0]); i++) {
      if (strcmp(tag, k_kinds[i].name) == 0)
         return k_kinds[i].kind;
   }
   return K_OTHER;
}

/* =============================================================================
 * Attributes
 * ============================================================================= */

/* The next attribute of an open tag, from @p p (after the tag name) to
 * @p end: its name and raw value (NULL when it has none).  Returns where the
 * next one starts, or NULL when there are no more.  Read as html_tag_end
 * reads a tag. */
static const char *next_attr(const char *p,
                             const char *end,
                             const char **name,
                             size_t *name_len,
                             const char **value,
                             size_t *value_len) {
   while (p < end && (isspace((unsigned char)*p) || *p == '/'))
      p++;
   if (p >= end)
      return NULL;
   *name = p;
   p++; /* a name may start with '=' */
   while (p < end && !isspace((unsigned char)*p) && *p != '=' && *p != '/')
      p++;
   *name_len = (size_t)(p - *name);
   const char *q = p;
   while (q < end && isspace((unsigned char)*q))
      q++;
   *value = NULL;
   *value_len = 0;
   if (q >= end || *q != '=')
      return p;
   q++;
   while (q < end && isspace((unsigned char)*q))
      q++;
   if (q < end && (*q == '"' || *q == '\'')) {
      const char quote = *q++;
      const char *close = memchr(q, quote, (size_t)(end - q));
      *value = q;
      *value_len = (size_t)((close ? close : end) - q);
      return close ? close + 1 : end;
   }
   *value = q;
   while (q < end && !isspace((unsigned char)*q))
      q++;
   *value_len = (size_t)(q - *value);
   return q;
}

static bool name_is(const char *name, size_t len, const char *want) {
   return len == strlen(want) && strncasecmp(name, want, len) == 0;
}

/* Named character references a browser decodes in an attribute value (with
 * their ';'), as far as styles, classes and ids care. */
static const struct {
   const char *name;
   char c;
} k_named[] = { { "amp", '&' },    { "lt", '<' },     { "gt", '>' },       { "quot", '"' },
                { "apos", '\'' },  { "colon", ':' },  { "semi", ';' },     { "lpar", '(' },
                { "rpar", ')' },   { "sol", '/' },    { "bsol", '\\' },    { "num", '#' },
                { "excl", '!' },   { "period", '.' }, { "comma", ',' },    { "percnt", '%' },
                { "nbsp", ' ' },   { "Tab", '\t' },   { "NewLine", '\n' }, { "ast", '*' },
                { "hyphen", '-' }, { "dash", '-' },   { "lowbar", '_' },   { "plus", '+' },
                { "equals", '=' }, { "quest", '?' },  { "lbrace", '{' },   { "rbrace", '}' } };

/* @p v decoded (character references), heap, NUL-terminated. */
static char *attr_decode(const char *v, size_t len) {
   char *out = malloc(len * 4 + 1);
   if (!out)
      return NULL;
   size_t n = 0;
   for (size_t i = 0; i < len;) {
      if (v[i] != '&') {
         if (v[i] == '\0') {
            n += hv_utf8_put(0xFFFD, out + n);
            i++;
         } else {
            out[n++] = v[i++];
         }
         continue;
      }
      size_t j = i + 1;
      if (j < len && v[j] == '#') {
         j++;
         const bool hex = j < len && (v[j] == 'x' || v[j] == 'X');
         if (hex)
            j++;
         unsigned long cp = 0;
         const size_t digits_at = j;
         while (j < len && (hex ? isxdigit((unsigned char)v[j]) : isdigit((unsigned char)v[j]))) {
            if (cp < 0x110000) {
               const char d = (char)tolower((unsigned char)v[j]);
               cp = cp * (hex ? 16 : 10) + (unsigned long)(d <= '9' ? d - '0' : d - 'a' + 10);
            }
            j++;
         }
         if (j > digits_at) {
            if (j < len && v[j] == ';')
               j++;
            n += hv_utf8_put(cp, out + n);
            i = j;
            continue;
         }
      } else {
         size_t k = j;
         while (k < len && isalnum((unsigned char)v[k]))
            k++;
         if (k < len && v[k] == ';') {
            bool found = false;
            for (size_t e = 0; e < sizeof(k_named) / sizeof(k_named[0]); e++) {
               if (strlen(k_named[e].name) == k - j &&
                   strncmp(v + j, k_named[e].name, k - j) == 0) {
                  out[n++] = k_named[e].c;
                  found = true;
                  break;
               }
            }
            if (found) {
               i = k + 1;
               continue;
            }
         }
      }
      out[n++] = v[i++];
   }
   out[n] = '\0';
   return out;
}

/* =============================================================================
 * Rules from <style> blocks
 * ============================================================================= */

static uint32_t name_hash(uint32_t seed, const char *s, size_t len) {
   uint32_t h = 2166136261u ^ seed;
   for (size_t i = 0; i < len; i++) {
      h ^= (uint32_t)tolower((unsigned char)s[i]);
      h *= 16777619u;
   }
   h ^= h >> 15;
   h *= 0x2c1b3c6dU;
   h ^= h >> 12;
   return h;
}

static bool ident_char(unsigned char c) {
   return isalnum(c) || c == '-' || c == '_' || c >= 0x80;
}

/* The subject of one selector (its last compound): a tag, classes and an
 * id, pointing into the selector.  False when it can't be followed: a
 * pseudo-class or attribute part (a rule for :hover, or [hidden] in every CSS
 * reset, isn't hidden text).  Classes past VIS_CLASSES_MAX are left out (the
 * rule then asks for fewer, so hides more, never less).  *specificity counts
 * the whole selector; *qualified says it names more than its subject. */
typedef struct {
   const char *tag;
   size_t tag_len;
   const char *id;
   size_t id_len;
   const char *cls[VIS_CLASSES_MAX];
   size_t cls_len[VIS_CLASSES_MAX];
   int ncls;
   bool universal;
   bool qualified;
   int specificity;
} subject_t;

static bool parse_subject(const char *sel, size_t len, subject_t *s) {
   memset(s, 0, sizeof(*s));
   for (size_t i = 0; i < len; i++) {
      if (sel[i] == '#')
         s->specificity += 100;
      else if (sel[i] == '.' || sel[i] == '[' || sel[i] == ':')
         s->specificity += 10;
      else if (isalpha((unsigned char)sel[i]) && (i == 0 || strchr(" >+~(", sel[i - 1])))
         s->specificity += 1;
   }
   size_t start = len;
   while (start > 0 && !strchr(" >+~", sel[start - 1]))
      start--;
   s->qualified = start > 0;
   const char *p = sel + start;
   const char *end = sel + len;
   if (p >= end)
      return false;
   if (*p == '*') {
      p++;
   } else {
      size_t t = 0;
      while (p + t < end && ident_char((unsigned char)p[t]))
         t++;
      s->tag = p;
      s->tag_len = t;
      p += t;
   }
   while (p < end) {
      const char kind = *p++;
      if (kind != '.' && kind != '#')
         return false; /* [attr], :pseudo, or anything else */
      size_t n = 0;
      while (p + n < end && ident_char((unsigned char)p[n]))
         n++;
      if (n == 0)
         return false;
      if (kind == '#') {
         if (s->id)
            return false; /* two ids: matches nothing */
         s->id = p;
         s->id_len = n;
      } else if (s->ncls < VIS_CLASSES_MAX) {
         s->cls[s->ncls] = p;
         s->cls_len[s->ncls] = n;
         s->ncls++;
      }
      p += n;
   }
   s->universal = s->tag_len == 0 && !s->id && s->ncls == 0;
   return true;
}

/* A rule for one selector, merged into an existing one with the same subject
 * and specificity (so a sheet can't grow one name's chain); the walk is
 * charged to the document. */
static void add_rule(html_vis_t *vis,
                     const char *sel,
                     size_t len,
                     const html_vis_decl_t *in,
                     bool hide_only) {
   subject_t s;
   if (!parse_subject(sel, len, &s))
      return;
   html_vis_decl_t decl = *in;
   for (int i = 0; i < HV_FIELDS; i++)
      decl.order[i] = vis->next_order;
   vis->next_order++;
   if (hide_only || s.qualified || s.universal)
      hv_decl_hide_only(&decl);
   if (!hv_decl_says_anything(&decl))
      return;
   if (s.universal) {
      hv_decl_merge(&vis->universal, &decl);
      vis->has_universal = true;
      return;
   }
   const bool is_id = s.id != NULL;
   const char *key = is_id ? s.id : (s.ncls > 0 ? s.cls[0] : "");
   const size_t key_len = is_id ? s.id_len : (s.ncls > 0 ? s.cls_len[0] : 0);
   /* The other classes it needs, space-separated */
   size_t xl = 0;
   for (int i = is_id ? 0 : 1; i < s.ncls; i++)
      xl += s.cls_len[i] + 1;
   char *extra = malloc(xl + 1);
   if (!extra)
      return;
   xl = 0;
   for (int i = is_id ? 0 : 1; i < s.ncls; i++) {
      if (xl)
         extra[xl++] = ' ';
      memcpy(extra + xl, s.cls[i], s.cls_len[i]);
      xl += s.cls_len[i];
   }
   extra[xl] = '\0';
   char tag[VIS_TAG_MAX];
   snprintf(tag, sizeof(tag), "%.*s", (int)s.tag_len, s.tag ? s.tag : "");
   const char *hash_name = key_len ? key : tag;
   const size_t hash_len = key_len ? key_len : strlen(tag);
   const uint32_t b = name_hash(vis->seed, hash_name, hash_len) % VIS_BUCKETS;
   for (int i = vis->buckets[b]; i >= 0 && spend(vis, 1); i = vis->rules[i].next) {
      vis_rule_t *r = &vis->rules[i];
      if (r->is_id == is_id && r->specificity == s.specificity && strlen(r->name) == key_len &&
          strncasecmp(r->name, key, key_len) == 0 && strcmp(r->tag, tag) == 0 &&
          strcasecmp(r->extra, extra) == 0) {
         hv_decl_merge(&r->decl, &decl);
         free(extra);
         return;
      }
   }
   if (vis->exhausted || vis->rule_count >= VIS_RULES_MAX) {
      free(extra);
      return;
   }
   if (!vis->rules) {
      vis->rules = calloc(VIS_RULES_MAX, sizeof(vis_rule_t));
      if (!vis->rules) {
         free(extra);
         return;
      }
   }
   vis_rule_t *r = &vis->rules[vis->rule_count];
   r->name = strndup(key, key_len);
   if (!r->name) {
      free(extra);
      return;
   }
   r->extra = extra;
   snprintf(r->tag, sizeof(r->tag), "%s", tag);
   r->is_id = is_id;
   r->specificity = s.specificity;
   r->decl = decl;
   r->next = vis->buckets[b];
   vis->buckets[b] = vis->rule_count;
   vis->rule_count++;
}

/* The '}' closing the block whose '{' is at @p open, nested blocks counted,
 * strings and comments skipped; @p end when it never closes. */
static const char *block_end(const char *open, const char *end) {
   int depth = 0;
   char quote = 0;
   for (const char *p = open; p < end; p++) {
      if (quote) {
         if (*p == '\\' && p + 1 < end)
            p++;
         else if (*p == quote)
            quote = 0;
         continue;
      }
      if (*p == '"' || *p == '\'') {
         quote = *p;
      } else if (*p == '/' && p + 1 < end && p[1] == '*') {
         p += 2;
         while (p + 1 < end && !(p[0] == '*' && p[1] == '/'))
            p++;
         p++;
      } else if (*p == '\\' && p + 1 < end) {
         p++;
      } else if (*p == '{') {
         depth++;
      } else if (*p == '}' && --depth == 0) {
         return p;
      }
   }
   return end;
}

/* Whether an @media block applies on a desktop or webmail screen: anything
 * but print-only and a phone's (max-width / max-device-width) queries. */
/* Whether a media query is every screen: none, all, screen, only screen. */
static bool media_every_screen(const char *q, size_t len) {
   char buf[32];
   size_t n = 0;
   for (size_t i = 0; i < len; i++) {
      if (isspace((unsigned char)q[i]))
         continue;
      if (n >= sizeof(buf) - 1)
         return false;
      buf[n++] = (char)tolower((unsigned char)q[i]);
   }
   buf[n] = '\0';
   return n == 0 || strcmp(buf, "all") == 0 || strcmp(buf, "screen") == 0 ||
          strcmp(buf, "onlyscreen") == 0 || strcmp(buf, "onlyall") == 0;
}

static bool media_on_screen(const char *q, size_t len) {
   char *n = hv_css_normalize(q, len);
   if (!n)
      return false;
   bool on = true;
   /* A phone's layout: a max-width under a desktop window's */
   for (const char *m = strstr(n, "max-width"); m && on; m = strstr(m + 1, "max-width")) {
      const char *v = strchr(m, ':');
      double px;
      bool unit;
      if (v) {
         v++;
         while (*v == ' ')
            v++;
         if (hv_css_length(v, &px, &unit) != HV_LEN_NONE && px < VIS_DESKTOP_PX)
            on = false;
      }
   }
   if (strstr(n, "max-device-width"))
      on = false;
   if (strstr(n, "not screen") || strstr(n, "not all"))
      on = false;
   if (strstr(n, "print") && !strstr(n, "screen") && !strstr(n, "all") && !strstr(n, "not print"))
      on = false;
   free(n);
   return on;
}

/* The rules of a stylesheet.  @p hide_only: inside a block whose condition
 * can't be evaluated (@supports, @container, @layer, a media query other than
 * every screen), its rules may hide but never show. */
static void collect_sheet(html_vis_t *vis,
                          const char *p,
                          const char *end,
                          int depth,
                          bool hide_only) {
   while (p < end && spend(vis, 1)) {
      if (isspace((unsigned char)*p)) {
         p++;
         continue;
      }
      if (p + 1 < end && p[0] == '/' && p[1] == '*') {
         const char *c = p + 2;
         while (c + 1 < end && !(c[0] == '*' && c[1] == '/'))
            c++;
         p = c + 1 < end ? c + 2 : end;
         continue;
      }
      /* CSS ignores HTML comment markers at the top of a sheet */
      if ((size_t)(end - p) >= 4 && strncmp(p, "<!--", 4) == 0) {
         p += 4;
         continue;
      }
      if ((size_t)(end - p) >= 3 && strncmp(p, "-->", 3) == 0) {
         p += 3;
         continue;
      }
      /* The prelude: to '{', or ';' / '}' for a statement or stray close */
      const char *q = p;
      while (q < end && *q != '{' && *q != ';' && *q != '}')
         q++;
      if (q >= end)
         return;
      if (*q != '{') {
         p = q + 1;
         continue;
      }
      const char *close = block_end(q, end);
      spend(vis, (unsigned long)(close - q));
      if (*p == '@') {
         const char *kw = p + 1;
         const char *kw_end = kw;
         while (kw_end < q && (isalnum((unsigned char)*kw_end) || *kw_end == '-'))
            kw_end++;
         const size_t kw_len = (size_t)(kw_end - kw);
         const bool media = kw_len == 5 && strncasecmp(kw, "media", 5) == 0;
         const bool follow = depth < VIS_AT_DEPTH_MAX &&
                             ((kw_len == 8 && strncasecmp(kw, "supports", 8) == 0) ||
                              (kw_len == 5 && strncasecmp(kw, "layer", 5) == 0) ||
                              (kw_len == 9 && strncasecmp(kw, "container", 9) == 0) ||
                              (media && media_on_screen(kw_end, (size_t)(q - kw_end))));
         const bool every_screen = media && media_every_screen(kw_end, (size_t)(q - kw_end));
         if (follow)
            collect_sheet(vis, q + 1, close, depth + 1, hide_only || !every_screen);
      } else {
         html_vis_decl_t decl;
         html_vis_declarations(q + 1, (size_t)(close - q - 1), &decl);
         if (hv_decl_says_anything(&decl)) {
            char *sel = hv_css_normalize(p, (size_t)(q - p));
            if (sel) {
               const char *s = sel;
               for (const char *c = sel;; c++) {
                  if (*c == ',' || *c == '\0') {
                     size_t n = (size_t)(c - s);
                     while (n > 0 && *s == ' ') {
                        s++;
                        n--;
                     }
                     while (n > 0 && s[n - 1] == ' ')
                        n--;
                     if (n > 0)
                        add_rule(vis, s, n, &decl, hide_only);
                     if (*c == '\0')
                        break;
                     s = c + 1;
                  }
               }
               free(sel);
            }
         }
      }
      p = close < end ? close + 1 : end;
   }
}

/* Whether @p p starts "<name" or "</name" (per @p closing) as a whole tag
 * name. */
static bool tag_at(const char *p, const char *end, const char *name, bool closing) {
   const size_t n = strlen(name);
   const size_t skip = closing ? 2 : 1;
   if ((size_t)(end - p) < skip + n + 1 || p[0] != '<' || (closing && p[1] != '/'))
      return false;
   if (strncasecmp(p + skip, name, n) != 0)
      return false;
   const char c = p[skip + n];
   return c == '>' || c == '/' || isspace((unsigned char)c);
}

/* Where a comment starting at @p lt ("<!--") ends, as a browser ends it:
 * "<!-->" and "<!--->" at once, else at "-->" or "--!>". */
const char *html_comment_end(const char *lt, const char *end) {
   const char *c = lt + 4;
   if (c < end && *c == '>')
      return c + 1;
   if (c + 1 < end && c[0] == '-' && c[1] == '>')
      return c + 2;
   for (; c + 2 < end; c++) {
      if (c[0] == '-' && c[1] == '-' && c[2] == '>')
         return c + 3;
      if (c + 3 < end && c[0] == '-' && c[1] == '-' && c[2] == '!' && c[3] == '>')
         return c + 4;
   }
   return end;
}

html_vis_t *html_vis_new(const char *html, size_t len) {
   html_vis_t *vis = calloc(1, sizeof(*vis));
   if (!vis)
      return NULL;
   for (int i = 0; i < VIS_BUCKETS; i++)
      vis->buckets[i] = -1;
   if (getrandom(&vis->seed, sizeof(vis->seed), GRND_NONBLOCK) != (ssize_t)sizeof(vis->seed))
      vis->seed = (uint32_t)(uintptr_t)vis ^ 0x9e3779b9u;
   hv_decl_clear(&vis->universal);
   vis->stack[0].vis = true; /* the document: shown */
   vis->depth = 1;
   if (!html)
      return vis;
   /* The tags as a browser tokenizes them: comments skipped, attribute values
    * never mistaken for markup. */
   const char *end = html + len;
   const char *p = html;
   int svg = 0; /* inside <svg>/<math>, where <title> and the like aren't raw text */
   while (p < end) {
      const char *lt = memchr(p, '<', (size_t)(end - p));
      if (!lt)
         break;
      if ((size_t)(end - lt) >= 4 && strncmp(lt, "<!--", 4) == 0) {
         p = html_comment_end(lt, end);
         continue;
      }
      if (lt + 1 >= end || !(isalpha((unsigned char)lt[1]) || lt[1] == '/')) {
         p = lt + 1;
         continue;
      }
      const char *gt = html_tag_end(lt + 1, end);
      if (!gt)
         break;
      if (tag_at(lt, end, "svg", false) || tag_at(lt, end, "math", false))
         svg++;
      else if (svg > 0 && (tag_at(lt, end, "svg", true) || tag_at(lt, end, "math", true)))
         svg--;
      if (!vis->html_tag && tag_at(lt, end, "html", false)) {
         vis->html_tag = lt + 1;
         vis->html_tag_end = gt;
      } else if (!vis->body_tag && tag_at(lt, end, "body", false)) {
         vis->body_tag = lt + 1;
         vis->body_tag_end = gt;
      } else if (tag_at(lt, end, "style", false) ||
                 (svg == 0 &&
                  (tag_at(lt, end, "script", false) || tag_at(lt, end, "textarea", false) ||
                   tag_at(lt, end, "title", false) || tag_at(lt, end, "xmp", false)))) {
         /* Raw text: to its own end tag.  Only a style's text is a sheet. */
         const bool style = tag_at(lt, end, "style", false);
         const char *name = style ? "style" : NULL;
         char tagname[16];
         if (!name) {
            size_t n = 0;
            while (lt + 1 + n < gt && isalpha((unsigned char)lt[1 + n]) &&
                   n < sizeof(tagname) - 1) {
               tagname[n] = (char)tolower((unsigned char)lt[1 + n]);
               n++;
            }
            tagname[n] = '\0';
            name = tagname;
         }
         const char *close = gt + 1;
         while (close < end && !(*close == '<' && tag_at(close, end, name, true)))
            close++;
         if (style)
            collect_sheet(vis, gt + 1, close, 0, false);
         p = close < end ? close + 1 : end;
         continue;
      }
      p = gt + 1;
   }
   return vis;
}

void html_vis_free(html_vis_t *vis) {
   if (!vis)
      return;
   for (int i = 0; i < vis->rule_count; i++) {
      free(vis->rules[i].name);
      free(vis->rules[i].extra);
   }
   free(vis->rules);
   free(vis);
}

bool html_vis_root(const html_vis_t *vis, bool body, const char **tag, const char **tag_end) {
   if (!vis)
      return false;
   *tag = body ? vis->body_tag : vis->html_tag;
   *tag_end = body ? vis->body_tag_end : vis->html_tag_end;
   return *tag != NULL;
}

/* =============================================================================
 * Matching an element
 * ============================================================================= */

typedef struct {
   char **tok;
   size_t count;
   size_t cap;
} tokens_t;

static bool has_token(const tokens_t *t, const char *name, size_t len) {
   for (size_t i = 0; i < t->count; i++) {
      if (strlen(t->tok[i]) == len && strncasecmp(t->tok[i], name, len) == 0)
         return true;
   }
   return false;
}

/* Whether every class in @p extra (space-separated) is in @p t. */
static bool has_all(html_vis_t *vis, const tokens_t *t, const char *extra) {
   const char *p = extra;
   while (*p) {
      const char *e = strchr(p, ' ');
      const size_t n = e ? (size_t)(e - p) : strlen(p);
      if (!spend(vis, t->count) || !has_token(t, p, n))
         return false;
      p += n;
      if (*p == ' ')
         p++;
   }
   return true;
}

/* The cascade for one element, per property: an !important value beats a
 * plain one, then the higher specificity, then the later place in the
 * sheets. */
typedef struct {
   html_vis_decl_t d;
   int spec[HV_FIELDS];
} cascade_t;

static void cascade_init(cascade_t *c) {
   hv_decl_clear(&c->d);
   for (int i = 0; i < HV_FIELDS; i++)
      c->spec[i] = -1;
}

static void cascade_add(cascade_t *c, const html_vis_decl_t *d, int spec) {
   for (int i = 0; i < HV_FIELDS; i++) {
      if (d->f[i] < 0)
         continue;
      const unsigned bit = 1u << (unsigned)i;
      const bool imp = (d->important & bit) != 0;
      const bool cur_imp = (c->d.important & bit) != 0;
      const bool wins = c->d.f[i] < 0 || (imp && !cur_imp) ||
                        (imp == cur_imp && (spec > c->spec[i] ||
                                            (spec == c->spec[i] && d->order[i] >= c->d.order[i])));
      if (!wins)
         continue;
      c->d.f[i] = d->f[i];
      c->d.order[i] = d->order[i];
      c->spec[i] = spec;
      c->d.important = imp ? (c->d.important | bit) : (c->d.important & ~bit);
   }
}

static void find_rules(html_vis_t *vis,
                       const char *tag_name,
                       const char *key,
                       size_t len,
                       bool is_id,
                       const tokens_t *classes,
                       cascade_t *c) {
   if (vis->rule_count == 0)
      return;
   const uint32_t b = name_hash(vis->seed, key, len) % VIS_BUCKETS;
   for (int i = vis->buckets[b]; i >= 0 && spend(vis, 1); i = vis->rules[i].next) {
      const vis_rule_t *r = &vis->rules[i];
      if (r->is_id != is_id)
         continue;
      if (r->name[0]) {
         if (strlen(r->name) != len || strncasecmp(r->name, key, len) != 0)
            continue;
      } else if (strlen(r->tag) != len || strncmp(r->tag, key, len) != 0) {
         continue; /* a tag-only rule, keyed by its tag */
      }
      if (r->tag[0] && strcmp(r->tag, tag_name) != 0)
         continue;
      if (r->extra[0] && !has_all(vis, classes, r->extra))
         continue;
      cascade_add(c, &r->decl, r->specificity);
   }
}

/* Whether an open tag has attribute @p want. */
static bool has_attr(const char *tag, const char *tag_end, const char *want) {
   const char *p = tag;
   while (p < tag_end && !isspace((unsigned char)*p) && *p != '/')
      p++;
   const char *name;
   const char *value;
   size_t name_len;
   size_t value_len;
   while ((p = next_attr(p, tag_end, &name, &name_len, &value, &value_len)) != NULL) {
      if (name_is(name, name_len, want))
         return true;
   }
   return false;
}

/* What an element's attributes say: its matching rules by specificity and
 * order, then `hidden` (any display beats it), then its inline style (unless
 * a rule's is !important). */
static void element_decl(html_vis_t *vis,
                         const char *tag_name,
                         const char *tag,
                         const char *tag_end,
                         html_vis_decl_t *out) {
   cascade_t c;
   cascade_init(&c);
   html_vis_decl_t inline_decl;
   bool has_inline = false;
   bool hidden_attr = false;
   char *klass = NULL;
   char *id = NULL;
   const char *p = tag;
   while (p < tag_end && !isspace((unsigned char)*p) && *p != '/')
      p++;
   const char *name;
   const char *value;
   size_t name_len;
   size_t value_len;
   bool seen_style = false;
   while ((p = next_attr(p, tag_end, &name, &name_len, &value, &value_len)) != NULL &&
          spend(vis, 1)) {
      if (name_is(name, name_len, "hidden")) {
         hidden_attr = true;
         continue;
      }
      if (!value)
         continue;
      /* A repeated attribute: the browser keeps the first */
      if (name_is(name, name_len, "style") && !seen_style) {
         seen_style = true;
         char *v = attr_decode(value, value_len);
         if (v) {
            spend(vis, value_len);
            html_vis_declarations(v, strlen(v), &inline_decl);
            has_inline = true;
            free(v);
         }
      } else if (name_is(name, name_len, "class") && !klass) {
         klass = attr_decode(value, value_len);
      } else if (name_is(name, name_len, "id") && !id) {
         id = attr_decode(value, value_len);
      }
   }
   if (hidden_attr) {
      /* The lowest place in the cascade: any display beats it */
      html_vis_decl_t h;
      hv_decl_clear(&h);
      h.f[HV_DISPLAY] = 1;
      h.order[HV_DISPLAY] = -1;
      cascade_add(&c, &h, -1);
   }
   if (vis->has_universal)
      cascade_add(&c, &vis->universal, 0);
   tokens_t classes = { 0 };
   if (vis->rule_count > 0) {
      /* The element's classes, each once */
      if (klass) {
         for (char *c = klass; *c;) {
            while (*c && isspace((unsigned char)*c))
               *c++ = '\0';
            if (!*c)
               break;
            char *t = c;
            while (*c && !isspace((unsigned char)*c))
               c++;
            if (*c)
               *c++ = '\0';
            if (!spend(vis, classes.count + 1))
               break;
            if (has_token(&classes, t, strlen(t)))
               continue;
            if (classes.count == classes.cap) {
               const size_t cap = classes.cap ? classes.cap * 2 : 8;
               char **n = realloc(classes.tok, cap * sizeof(char *));
               if (!n)
                  break;
               classes.tok = n;
               classes.cap = cap;
            }
            classes.tok[classes.count++] = t;
         }
      }
      for (size_t i = 0; i < classes.count && !vis->exhausted; i++)
         find_rules(vis, tag_name, classes.tok[i], strlen(classes.tok[i]), false, &classes, &c);
      if (id && id[0])
         find_rules(vis, tag_name, id, strlen(id), true, &classes, &c);
      find_rules(vis, tag_name, tag_name, strlen(tag_name), false, &classes, &c);
   }
   if (has_inline) {
      /* Inline beats any rule's value, unless only the rule's is !important */
      for (int i = 0; i < HV_FIELDS; i++)
         inline_decl.order[i] = vis->next_order;
      cascade_add(&c, &inline_decl, 1 << 20);
   }
   *out = c.d;
   free(classes.tok);
   free(klass);
   free(id);
}

/* =============================================================================
 * The open elements
 * ============================================================================= */

/* Pops frames down to and including the innermost of kind @p kind, stopping
 * (without popping) at a frame whose kind is in @p bounds.  False when there
 * is none in scope. */
static bool pop_kind(html_vis_t *vis, vis_kind_t kind, unsigned bounds) {
   for (int i = vis->depth - 1; i >= 1 && spend(vis, 1); i--) {
      if (vis->stack[i].kind == kind) {
         vis->depth = i;
         return true;
      }
      if (bounds & KB(vis->stack[i].kind))
         return false;
   }
   return false;
}

/* The end tags HTML lets an author leave out: what opening @p tag closes. */
static void implied_closes(html_vis_t *vis, const char *tag, vis_kind_t kind) {
   const size_t n_head = sizeof(k_head_set) / sizeof(k_head_set[0]);
   if (vis->depth > 1 && vis->stack[vis->depth - 1].kind == K_HEAD &&
       !in_sorted(tag, k_head_set, n_head))
      vis->depth--;
   if (in_sorted(tag, k_closes_p, sizeof(k_closes_p) / sizeof(k_closes_p[0])))
      pop_kind(vis, K_P, SCOPE_BUTTON);
   switch (kind) {
      case K_LI:
         pop_kind(vis, K_LI, SCOPE_LIST);
         break;
      case K_DD:
      case K_DT:
         if (!pop_kind(vis, K_DD, SCOPE_DEFAULT | KB(K_DL)))
            pop_kind(vis, K_DT, SCOPE_DEFAULT | KB(K_DL));
         break;
      case K_TD:
      case K_TH:
         if (!pop_kind(vis, K_TD, SCOPE_CELL))
            pop_kind(vis, K_TH, SCOPE_CELL);
         break;
      case K_TR:
         if (!pop_kind(vis, K_TD, SCOPE_CELL))
            pop_kind(vis, K_TH, SCOPE_CELL);
         pop_kind(vis, K_TR, SCOPE_ROW);
         break;
      case K_TBODY:
      case K_THEAD:
      case K_TFOOT:
         if (!pop_kind(vis, K_TD, SCOPE_CELL))
            pop_kind(vis, K_TH, SCOPE_CELL);
         pop_kind(vis, K_TR, SCOPE_ROW);
         if (!pop_kind(vis, K_TBODY, SCOPE_TABLE) && !pop_kind(vis, K_THEAD, SCOPE_TABLE))
            pop_kind(vis, K_TFOOT, SCOPE_TABLE);
         break;
      case K_OPTION:
         pop_kind(vis, K_OPTION, SCOPE_SELECT);
         break;
      default:
         break;
   }
}

/* Elements a browser never shows the text of. */
static bool hidden_by_default(const char *tag) {
   static const char *const k_hidden[] = { "audio",    "datalist", "head", "noembed",
                                           "noframes", "rp",       "video" };
   return in_sorted(tag, k_hidden, sizeof(k_hidden) / sizeof(k_hidden[0]));
}

bool html_vis_open(html_vis_t *vis,
                   const char *tag_name,
                   const char *tag,
                   const char *tag_end,
                   bool is_void) {
   if (!vis || !tag_name || !isalpha((unsigned char)tag_name[0]))
      return vis ? html_vis_hidden(vis) : false;
   const vis_kind_t kind = kind_of(tag_name);
   if (vis->overflow == 0)
      implied_closes(vis, tag_name, kind);
   const vis_frame_t *parent = &vis->stack[vis->depth - 1];
   const bool parent_hidden = html_vis_hidden(vis);
   html_vis_decl_t d;
   element_decl(vis, tag_name, tag, tag_end, &d);
   if (kind == K_TD || kind == K_TH) {
      /* a table cell grows to fit what it holds */
      d.f[HV_WIDTH] = d.f[HV_MAX_WIDTH] = -1;
      d.f[HV_HEIGHT] = d.f[HV_MAX_HEIGHT] = -1;
   }
   vis_frame_t f;
   memset(&f, 0, sizeof(f));
   snprintf(f.tag, sizeof(f.tag), "%s", tag_name);
   f.kind = (unsigned char)kind;
   f.special = in_sorted(tag_name, k_special, sizeof(k_special) / sizeof(k_special[0]));
   f.hard = parent->hard || hv_decl_hard(&d) || hidden_by_default(tag_name);
   const int v = d.f[HV_VISIBILITY];
   f.vis = v == HV_SHOWN ? true : (v == HV_HIDDEN ? false : parent->vis);
   const int fs = d.f[HV_FONT_SIZE];
   f.tiny = fs == HV_HIDDEN ? true : (fs == HV_SHOWN ? false : parent->tiny);
   const int col = d.f[HV_COLOR];
   f.clear = col == HV_HIDDEN ? true : (col == HV_SHOWN ? false : parent->clear);
   const int fill = d.f[HV_TEXT_FILL];
   f.fill = fill == HV_OWN_COLOR
                ? f.clear
                : (fill == HV_HIDDEN ? true : (fill == HV_SHOWN ? false : parent->fill));
   /* A closed <details> shows only its <summary> */
   if (parent->closed_details && strcmp(tag_name, "summary") != 0)
      f.hard = true;
   f.closed_details = strcmp(tag_name, "details") == 0 && !has_attr(tag, tag_end, "open");
   const bool hidden = parent_hidden && vis->overflow > 0
                           ? true
                           : (f.hard || !f.vis || f.tiny || f.clear || f.fill || f.closed_details ||
                              vis->exhausted);
   if (is_void)
      return hidden;
   if (vis->overflow > 0 || vis->depth >= VIS_DEPTH_MAX) {
      /* Untracked: counted.  A hidden one hides until its own end tag, if
       * nothing opens inside it; once something does, a browser may ignore
       * that end tag (a <p> or a table cell open inside it), which can't be
       * told apart here, so the rest of the page is hidden. */
      vis->overflow++;
      if (vis->hide_level > 0) {
         vis->exhausted = true;
      } else if (hidden) {
         vis->hide_level = vis->overflow;
         snprintf(vis->hide_tag, sizeof(vis->hide_tag), "%s", tag_name);
      }
      return html_vis_hidden(vis);
   }
   vis->stack[vis->depth++] = f;
   return hidden;
}

/* Pops to the innermost frame named @p tag, as a browser applies an end tag:
 * a special element's end tag within default scope; any other's only when no
 * special element is open inside it. */
static void pop_named(html_vis_t *vis, const char *tag, bool special) {
   for (int i = vis->depth - 1; i >= 1 && spend(vis, 1); i--) {
      const vis_frame_t *f = &vis->stack[i];
      if (strcmp(f->tag, tag) == 0) {
         vis->depth = i;
         return;
      }
      if (special ? (SCOPE_DEFAULT & KB(f->kind)) != 0 : f->special)
         return;
   }
}

void html_vis_close(html_vis_t *vis, const char *tag_name) {
   if (!vis || !tag_name || !isalpha((unsigned char)tag_name[0]))
      return;
   if (vis->overflow > 0) {
      /* Only the hidden element's own end tag ends the hiding: a stray end
       * tag a browser ignores must not. */
      if (vis->hide_level > 0 && strcasecmp(tag_name, vis->hide_tag) == 0)
         vis->hide_level = 0;
      /* Leaving the untracked depth while still hiding keeps hiding the rest */
      if (vis->overflow > 1 || vis->hide_level == 0)
         vis->overflow--;
      return;
   }
   char tag[VIS_TAG_MAX];
   snprintf(tag, sizeof(tag), "%s", tag_name);
   switch (kind_of(tag)) {
      case K_P:
         pop_kind(vis, K_P, SCOPE_BUTTON);
         return;
      case K_LI:
         pop_kind(vis, K_LI, SCOPE_LIST);
         return;
      case K_DD:
         pop_kind(vis, K_DD, SCOPE_DEFAULT);
         return;
      case K_DT:
         pop_kind(vis, K_DT, SCOPE_DEFAULT);
         return;
      case K_TD:
         pop_kind(vis, K_TD, SCOPE_TABLE);
         return;
      case K_TH:
         pop_kind(vis, K_TH, SCOPE_TABLE);
         return;
      case K_TR:
         pop_kind(vis, K_TR, SCOPE_TABLE);
         return;
      case K_TBODY:
      case K_THEAD:
      case K_TFOOT:
      case K_CAPTION:
      case K_TABLE:
         /* These close the cells, rows and sections open inside them */
         pop_kind(vis, kind_of(tag), SCOPE_TABLE & ~KB(kind_of(tag)));
         return;
      case K_BODY:
      case K_HTML:
         return; /* the document stays open to its end */
      default:
         pop_named(vis, tag, in_sorted(tag, k_special, sizeof(k_special) / sizeof(k_special[0])));
         return;
   }
}

bool html_vis_hidden(const html_vis_t *vis) {
   if (!vis)
      return false;
   if (vis->exhausted || vis->hide_level > 0)
      return true;
   const vis_frame_t *f = &vis->stack[vis->depth - 1];
   return f->hard || !f->vis || f->tiny || f->clear || f->fill || f->closed_details;
}
