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
 * Reading a message (email_mime.h): the policy over a message's parts, and
 * the IMAP front end that gets those parts from raw bytes with GMime.
 */

#include "tools/email_mime.h"

#include <ctype.h>
#include <gmime/gmime.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "logging.h"
#include "tools/email_display.h"
#include "tools/html_parser.h"
#include "utils/string_utils.h"

/* An HTML part read only to turn it into text: how much of it is decoded.
 * Marketing mail can be a few hundred KB of HTML for a few hundred chars of text. */
#define HTML_FOR_TEXT_MAX (2 * 1024 * 1024)
/* Bytes asked of a decoding stream per read. */
#define DECODE_CHUNK (64 * 1024)
/* Between two pieces of body text. */
#define BODY_JOIN "\n\n"
/* Before a plain form read in place of an HTML one with no text */
#define PLAIN_FORM_NOTE                                                              \
   "[The text above is this email's plain-text version: its HTML has no text (only " \
   "images, or it couldn't be read), so the reader may not have seen it.]"

/* =============================================================================
 * Setup
 * ============================================================================= */

/* The charsets read as declared; any other declared charset is read as
 * windows-1252.  A finite list keeps the converters a hostile message can
 * reach to the ones mail actually uses. */
static const char *const k_charsets[] = { "iso-8859-1",
                                          "iso-8859-2",
                                          "iso-8859-3",
                                          "iso-8859-4",
                                          "iso-8859-5",
                                          "iso-8859-6",
                                          "iso-8859-7",
                                          "iso-8859-8",
                                          "iso-8859-9",
                                          "iso-8859-10",
                                          "iso-8859-13",
                                          "iso-8859-14",
                                          "iso-8859-15",
                                          "iso-8859-16",
                                          "windows-1250",
                                          "windows-1251",
                                          "windows-1252",
                                          "windows-1253",
                                          "windows-1254",
                                          "windows-1255",
                                          "windows-1256",
                                          "windows-1257",
                                          "windows-1258",
                                          "windows-874",
                                          "koi8-r",
                                          "koi8-u",
                                          "gb2312",
                                          "gbk",
                                          "gb18030",
                                          "big5",
                                          "big5-hkscs",
                                          "shift_jis",
                                          "euc-jp",
                                          "euc-kr",
                                          "iso-2022-jp",
                                          "iso-2022-kr",
                                          "tis-620",
                                          "utf-16",
                                          "utf-16le",
                                          "utf-16be",
                                          NULL };
#define CHARSET_COUNT (sizeof(k_charsets) / sizeof(k_charsets[0]) - 1)

static pthread_once_t s_init_once = PTHREAD_ONCE_INIT;
/* Parser options for every parse here: loose RFC 2047 (real mail is sloppy).
 * Built once and only read after, so threads share it; GMime's global
 * defaults are never changed. */
static GMimeParserOptions *s_options;
/* The allowlist in GMime's canonical spelling ("windows-1251" is
 * "windows-cp1251" there), and the canonical UTF-8, ASCII and fallback names:
 * a declared charset is canonicalized and compared to these. */
static const char *s_canon[CHARSET_COUNT];
static const char *s_canon_utf8;
static const char *s_canon_ascii;
static const char *s_canon_fallback;
/* The same in iconv's spelling: aliases canonicalize differently ("cp1251"
 * stays "cp1251", "windows-1251" becomes "windows-cp1251"), but both are
 * iconv's "CP1251". */
static const char *s_iconv[CHARSET_COUNT];
static const char *s_iconv_utf8;
static const char *s_iconv_ascii;

static void mime_init(void) {
   g_mime_init();
   s_options = g_mime_parser_options_new();
   g_mime_parser_options_set_rfc2047_compliance_mode(s_options, GMIME_RFC_COMPLIANCE_LOOSE);
   for (size_t i = 0; i < CHARSET_COUNT; i++) {
      s_canon[i] = g_mime_charset_canon_name(k_charsets[i]);
      s_iconv[i] = g_mime_charset_iconv_name(k_charsets[i]);
   }
   s_canon_utf8 = g_mime_charset_canon_name("utf-8");
   s_canon_ascii = g_mime_charset_canon_name("us-ascii");
   s_canon_fallback = g_mime_charset_canon_name("windows-1252");
   s_iconv_utf8 = g_mime_charset_iconv_name("utf-8");
   s_iconv_ascii = g_mime_charset_iconv_name("us-ascii");
}

/* =============================================================================
 * Charsets
 * ============================================================================= */

typedef enum {
   CS_UNDECLARED, /* none (or US-ASCII, often 8-bit in practice): UTF-8 if it is, else 1252 */
   CS_UTF8,       /* declared UTF-8: bad bytes become '?' */
   CS_CONVERT,    /* converted from *from */
} cs_kind_t;

/* Whether @p name looks like a charset name.  Every name GMime canonicalizes
 * stays in its tables for good, so a name that can't be one isn't handed to
 * it (a sender could otherwise grow them without bound). */
static bool charset_like(const char *name) {
   size_t n = 0;
   for (; name[n]; n++) {
      const unsigned char c = (unsigned char)name[n];
      if (n >= 40 || !(isalnum(c) || strchr("._:+-", c)))
         return false;
   }
   return n > 0;
}

static bool same(const char *a, const char *b) {
   return a && b && strcasecmp(a, b) == 0;
}

static cs_kind_t charset_for(const char *declared, const char **from) {
   static const char *const k_utf8_alias[] = { "utf8", "unicode-1-1-utf-8", NULL };
   static const char *const k_ascii_alias[] = { "ascii", "us", "ansi_x3.4-1968", "iso646-us",
                                                NULL };
   *from = NULL;
   if (!declared || !declared[0])
      return CS_UNDECLARED;
   if (!charset_like(declared)) {
      *from = s_canon_fallback;
      return CS_CONVERT;
   }
   for (int i = 0; k_utf8_alias[i]; i++) {
      if (strcasecmp(declared, k_utf8_alias[i]) == 0)
         return CS_UTF8;
   }
   for (int i = 0; k_ascii_alias[i]; i++) {
      if (strcasecmp(declared, k_ascii_alias[i]) == 0)
         return CS_UNDECLARED;
   }
   const char *canon = g_mime_charset_canon_name(declared);
   const char *iconv = g_mime_charset_iconv_name(declared);
   if (same(canon, s_canon_ascii) || same(iconv, s_iconv_ascii))
      return CS_UNDECLARED;
   if (same(canon, s_canon_utf8) || same(iconv, s_iconv_utf8))
      return CS_UTF8;
   for (size_t i = 0; i < CHARSET_COUNT; i++) {
      if (same(canon, s_canon[i]) || same(iconv, s_iconv[i])) {
         *from = s_canon[i];
         return CS_CONVERT;
      }
   }
   *from = s_canon_fallback;
   return CS_CONVERT;
}

/* =============================================================================
 * Decoding one part, bounded
 * ============================================================================= */

/* Whether @p s (@p len bytes) is well-formed UTF-8 with no NUL. */
static bool valid_utf8(const char *s, size_t len) {
   for (size_t i = 0; i < len;) {
      const unsigned char c = (unsigned char)s[i];
      if (c == 0)
         return false;
      if (c < 0x80) {
         i++;
         continue;
      }
      char tmp[5] = { 0 };
      memcpy(tmp, s + i, len - i < 4 ? len - i : 4);
      const size_t n = utf8_valid_seq_len(tmp);
      if (!n)
         return false;
      i += n;
   }
   return true;
}

/* Removes NUL bytes from @p buf (they'd end the string early); returns the new length. */
static size_t drop_nuls(char *buf, size_t len) {
   size_t o = 0;
   for (size_t i = 0; i < len; i++) {
      if (buf[i])
         buf[o++] = buf[i];
   }
   buf[o] = '\0';
   return o;
}

static bool ascii(const char *s, size_t len) {
   for (size_t i = 0; i < len; i++) {
      if ((unsigned char)s[i] >= 0x80)
         return false;
   }
   return true;
}

/* How many bytes at the end of @p s (@p len bytes) start a UTF-8 character
 * they don't finish (0-3). */
static size_t incomplete_tail(const char *s, size_t len) {
   for (size_t back = 1; back <= 3 && back <= len; back++) {
      const unsigned char c = (unsigned char)s[len - back];
      if ((c & 0xC0) == 0x80)
         continue; /* a continuation byte: look further back */
      if (c < 0x80)
         return 0;
      const size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
      return need > back ? back : 0;
   }
   return 0;
}

/* One read from @p stream: bytes read, or 0 at its end or on an error.  A
 * filter stream can produce nothing from a chunk of input (base64 whitespace,
 * a uuencode preamble) without being at its end, so that is read past. */
static ssize_t read_some(GMimeStream *stream, char *buf, size_t want) {
   for (;;) {
      const ssize_t got = g_mime_stream_read(stream, buf, want);
      if (got > 0)
         return got;
      if (got < 0 || g_mime_stream_eos(stream))
         return 0;
   }
}

/* Reads at most @p limit bytes from @p stream into a heap buffer; *more is set
 * when the stream had more. */
static char *read_bounded(GMimeStream *stream, size_t limit, size_t *len_out, bool *more) {
   *more = false;
   size_t cap = limit < DECODE_CHUNK ? limit + 1 : DECODE_CHUNK;
   char *buf = malloc(cap + 1);
   if (!buf)
      return NULL;
   size_t len = 0;
   for (;;) {
      if (len >= limit) {
         char probe;
         *more = read_some(stream, &probe, 1) > 0;
         break;
      }
      if (len == cap) {
         const size_t ncap = cap * 2 > limit + 1 ? limit + 1 : cap * 2;
         char *n = realloc(buf, ncap + 1);
         if (!n) {
            free(buf);
            return NULL;
         }
         buf = n;
         cap = ncap;
      }
      size_t want = cap - len;
      if (want > limit - len)
         want = limit - len;
      const ssize_t got = read_some(stream, buf + len, want);
      if (got <= 0)
         break;
      len += (size_t)got;
   }
   buf[len] = '\0';
   *len_out = len;
   return buf;
}

/* A part's content decoded to UTF-8, at most @p limit bytes of output (cut on a
 * character boundary).  *cut_out is set when the part had more, its content
 * was cut by the fetch, or the backend doesn't have it.  NULL when out of memory. */
static char *decode_part(const email_mime_part_t *part,
                         size_t limit,
                         size_t *len_out,
                         bool *cut_out) {
   *cut_out = false;
   *len_out = 0;
   if (!part->data || part->data_absent) {
      *cut_out = part->data_absent;
      return strdup("");
   }
   const char *from = NULL;
   const cs_kind_t cs = charset_for(part->charset, &from);
   /* GMime reads through a memory stream over the part's bytes, without a copy. */
   GByteArray *ba = g_byte_array_new_take((guint8 *)part->data, part->data_len);
   GMimeStream *src = g_mime_stream_mem_new_with_byte_array(ba);
   g_mime_stream_mem_set_owner(GMIME_STREAM_MEM(src), FALSE);
   GMimeStream *filtered = g_mime_stream_filter_new(src);
   GMimeContentEncoding enc = GMIME_CONTENT_ENCODING_DEFAULT;
   if (part->encoding == EMAIL_MIME_ENC_BASE64)
      enc = GMIME_CONTENT_ENCODING_BASE64;
   else if (part->encoding == EMAIL_MIME_ENC_QP)
      enc = GMIME_CONTENT_ENCODING_QUOTEDPRINTABLE;
   else if (part->encoding == EMAIL_MIME_ENC_UUENCODE)
      enc = GMIME_CONTENT_ENCODING_UUENCODE;
   if (enc != GMIME_CONTENT_ENCODING_DEFAULT) {
      GMimeFilter *f = g_mime_filter_basic_new(enc, FALSE);
      g_mime_stream_filter_add(GMIME_STREAM_FILTER(filtered), f);
      g_object_unref(f);
   }
   bool converted = false;
   if (cs == CS_CONVERT) {
      GMimeFilter *f = g_mime_filter_charset_new(from, "UTF-8");
      if (f) {
         g_mime_stream_filter_add(GMIME_STREAM_FILTER(filtered), f);
         g_object_unref(f);
         converted = true;
      }
   }
   /* Read a little past the cap so the cut lands on whole characters. */
   size_t len = 0;
   bool more = false;
   char *buf = read_bounded(filtered, limit + 4, &len, &more);
   g_object_unref(filtered);
   g_object_unref(src);
   g_byte_array_free(ba, FALSE); /* the bytes are the caller's */
   if (!buf)
      return NULL;
   len = drop_nuls(buf, len);
   /* A read that stopped early can end inside a character.  For text read as
    * UTF-8 that tail is dropped, or a whole valid body would look like
    * something else; text that turns out to be windows-1252 keeps every byte
    * (each is a character there). */
   const size_t tail = more || part->data_cut ? incomplete_tail(buf, len) : 0;
   const size_t whole = len - tail;
   const bool may_be_1252 = cs == CS_UNDECLARED || (cs == CS_CONVERT && !converted);
   /* Undeclared text cut after one high byte, with no non-ASCII before it, is
    * as likely windows-1252 as cut UTF-8; it is read as 1252 (the byte kept). */
   const bool utf8 = !may_be_1252 || (valid_utf8(buf, whole) && (tail == 0 || !ascii(buf, whole)));
   if (utf8) {
      buf[whole] = '\0';
      len = whole;
   } else {
      /* Undeclared 8-bit text that isn't UTF-8 (or a converter that wouldn't
       * open): read it as windows-1252. */
      gsize wlen = 0;
      gchar *w = g_convert_with_fallback(buf, (gssize)len, "UTF-8", "WINDOWS-1252", "?", NULL,
                                         &wlen, NULL);
      if (w) {
         char *n = malloc(wlen + 1);
         if (n) {
            memcpy(n, w, wlen);
            n[wlen] = '\0';
            free(buf);
            buf = n;
            len = drop_nuls(buf, wlen);
         }
         g_free(w);
      }
   }
   sanitize_utf8_for_json(buf);
   len = email_display_body_clean(buf);
   if (len > limit) {
      utf8_truncate(buf, limit);
      len = strlen(buf);
      more = true;
   }
   *len_out = len;
   *cut_out = more || part->data_cut;
   return buf;
}

/* =============================================================================
 * The message's structure
 * ============================================================================= */

static bool is_type(const email_mime_part_t *p, const char *type) {
   return strcasecmp(p->type, type) == 0;
}

static bool is_subtype(const email_mime_part_t *p, const char *sub) {
   return p->multipart && strncasecmp(p->type, "multipart/", 10) == 0 &&
          strcasecmp(p->type + 10, sub) == 0;
}

/* A leaf that is the message's text rather than a file: text/plain or
 * text/html, not an attachment, no filename. */
static bool body_leaf(const email_mime_part_t *p) {
   return !p->multipart && !p->attachment && !p->filename[0] &&
          (is_type(p, "text/plain") || is_type(p, "text/html"));
}

/* Whether node @p id lies below node @p anc ("" is the top multipart). */
static bool descends(const char *id, const char *anc) {
   const size_t n = strlen(anc);
   if (n == 0)
      return true;
   return strncmp(id, anc, n) == 0 && id[n] == '.';
}

/* The index after node @p i's subtree. */
static int subtree_end(const email_mime_part_t *parts, int count, int i) {
   int j = i + 1;
   if (parts[i].multipart) {
      while (j < count && descends(parts[j].part_id, parts[i].part_id))
         j++;
   }
   return j;
}

/* Whether node @p i holds a body leaf of @p type. */
static bool holds(const email_mime_part_t *parts, int count, int i, const char *type) {
   const int end = subtree_end(parts, count, i);
   for (int j = i; j < end; j++) {
      if (body_leaf(&parts[j]) && is_type(&parts[j], type))
         return true;
   }
   return false;
}

/* The branch of alternative @p i to read: for the text, the first that holds
 * text/plain, else the last that holds HTML (the richest); for the HTML, the
 * last that holds HTML.  -1 when none does. */
static int choose_branch(const email_mime_part_t *parts, int count, int i, bool for_text) {
   const int end = subtree_end(parts, count, i);
   int html = -1;
   for (int j = i + 1; j < end; j = subtree_end(parts, count, j)) {
      if (for_text && holds(parts, count, j, "text/plain"))
         return j;
      if (holds(parts, count, j, "text/html"))
         html = j;
   }
   return html;
}

typedef struct {
   const email_mime_part_t *parts;
   int count;
   bool *used; /* read, or another form of the body (not an attachment) */
   email_mime_fetch_fn fetch;
   void *fetch_ctx;
   /* The body text so far */
   char *text;
   size_t len;
   size_t limit;
   bool as_shown; /* an alternative's HTML branch first (email_read_opts_t.text_as_shown) */
   bool hid;      /* an HTML piece had text its reader never sees, left out */
   bool cut;
   bool oom;
} walk_ctx_t;

/* Some senders put HTML in their text/plain part.  Only markup counts: the
 * text starts with a document or block tag, or closes one early on.  A '<'
 * alone doesn't: a plain reply quotes "Name <a@b.com> wrote:" and code says
 * "if (a<b)", and either through the HTML reader would lose text. */
static bool looks_like_html(const char *s, size_t len) {
   static const char *const k_opening[] = { "<!doctype", "<html",   "<head", "<body", "<div",
                                            "<p",        "<table",  "<br",   "<span", "<style",
                                            "<meta",     "<center", "<font", NULL };
   static const char *const k_closing[] = { "</div>",  "</p>",     "</a>", "</td>",
                                            "</span>", "</table>", NULL };
   size_t i = 0;
   while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
      i++;
   for (int k = 0; k_opening[k]; k++) {
      const size_t n = strlen(k_opening[k]);
      /* The tag name must end there: "<brian@example.com>" isn't "<br". */
      if (len - i > n && strncasecmp(s + i, k_opening[k], n) == 0 &&
          (s[i + n] == '>' || s[i + n] == '/' || s[i + n] == ' ' || s[i + n] == '\t' ||
           s[i + n] == '\r' || s[i + n] == '\n'))
         return true;
   }
   const size_t scan = len < 512 ? len : 512;
   for (size_t j = 0; j + 3 < scan; j++) {
      if (s[j] != '<' || s[j + 1] != '/')
         continue;
      for (int k = 0; k_closing[k]; k++) {
         const size_t n = strlen(k_closing[k]);
         if (scan - j >= n && strncasecmp(s + j, k_closing[k], n) == 0)
            return true;
      }
   }
   return false;
}

/* @p part's HTML as text, at most @p limit bytes.  Markup never reaches the
 * text: when the HTML can't be read, the piece is empty and *cut is set. */
static char *html_as_text(const email_mime_part_t *part, size_t limit, bool *cut, bool *hid) {
   size_t len = 0;
   char *html = decode_part(part, HTML_FOR_TEXT_MAX, &len, cut);
   if (!html)
      return NULL;
   char *text = NULL;
   bool dropped = false;
   const int rc = html_extract_text_plain_ex(html, len, &text, &dropped);
   *hid = *hid || dropped;
   free(html);
   if (rc != HTML_PARSE_SUCCESS || !text) {
      free(text);
      *cut = true;
      return strdup("");
   }
   if (strlen(text) > limit) {
      utf8_truncate(text, limit);
      *cut = true;
   }
   return text;
}

/* Part @p i with its content in hand: fetched now, through the backend, when
 * the backend holds it elsewhere.  @p local holds the copy that points at the
 * fetched bytes. */
static const email_mime_part_t *resolve(const email_mime_part_t *parts,
                                        int i,
                                        email_mime_fetch_fn fetch,
                                        void *fetch_ctx,
                                        email_mime_part_t *local) {
   const email_mime_part_t *p = &parts[i];
   if (!p->data_absent || !fetch)
      return p;
   const char *data = NULL;
   size_t len = 0;
   bool cut = false;
   if (!fetch(fetch_ctx, i, &data, &len, &cut) || !data)
      return p; /* reads as cut */
   *local = *p;
   local->data = data;
   local->data_len = len;
   local->data_absent = false;
   local->data_cut = cut;
   return local;
}

/* @p piece (taken) appended to the text, after BODY_JOIN; a blank piece adds
 * nothing (and leaves an alternative's other form to be read). */
static void append_piece(walk_ctx_t *c, char *piece) {
   const size_t sep = c->len ? strlen(BODY_JOIN) : 0;
   const size_t plen = strlen(piece);
   size_t k = 0;
   while (k < plen && isspace((unsigned char)piece[k]))
      k++;
   if (k == plen) {
      free(piece);
      return;
   }
   char *n = realloc(c->text, c->len + sep + plen + 1);
   if (!n) {
      free(piece);
      c->oom = true;
      return;
   }
   c->text = n;
   memcpy(c->text + c->len, BODY_JOIN, sep);
   memcpy(c->text + c->len + sep, piece, plen + 1);
   c->len += sep + plen;
   free(piece);
}

/* Reads body leaf @p i and appends it to the text. */
static void read_leaf(walk_ctx_t *c, int i) {
   if (c->len >= c->limit) {
      c->cut = true;
      return;
   }
   const size_t sep = c->len ? strlen(BODY_JOIN) : 0;
   if (c->len + sep >= c->limit) {
      c->cut = true;
      return;
   }
   const size_t room = c->limit - c->len - sep;
   /* Fetched only now, with room for it: nothing past the cap is fetched. */
   email_mime_part_t local;
   const email_mime_part_t *p = resolve(c->parts, i, c->fetch, c->fetch_ctx, &local);
   bool cut = false;
   size_t plen = 0;
   char *piece;
   if (is_type(p, "text/plain")) {
      piece = decode_part(p, room, &plen, &cut);
      if (piece && looks_like_html(piece, plen)) {
         free(piece);
         piece = html_as_text(p, room, &cut, &c->hid);
      }
   } else {
      piece = html_as_text(p, room, &cut, &c->hid);
   }
   if (!piece) {
      c->oom = true;
      return;
   }
   c->cut = c->cut || cut;
   append_piece(c, piece);
}

/* Marks the body leaves of every branch of alternative @p i but @p chosen:
 * other forms of the same body, not attachments. */
static void mark_other_forms(walk_ctx_t *c, int i, int chosen) {
   const int end = subtree_end(c->parts, c->count, i);
   for (int j = i + 1; j < end; j = subtree_end(c->parts, c->count, j)) {
      if (j == chosen)
         continue;
      const int bend = subtree_end(c->parts, c->count, j);
      for (int k = j; k < bend; k++) {
         if (body_leaf(&c->parts[k]))
            c->used[k] = true;
      }
   }
}

/* The body text below node @p i, in document order. */
static void text_walk(walk_ctx_t *c, int i) {
   const email_mime_part_t *p = &c->parts[i];
   if (!p->multipart) {
      if (!body_leaf(p))
         return;
      c->used[i] = true;
      read_leaf(c, i);
      return;
   }
   if (is_subtype(p, "alternative")) {
      /* As shown: the HTML a reader saw, not a plain form a sender can make
       * say something else. */
      int chosen = c->as_shown ? choose_branch(c->parts, c->count, i, false) : -1;
      if (chosen < 0)
         chosen = choose_branch(c->parts, c->count, i, true);
      mark_other_forms(c, i, chosen);
      if (chosen < 0)
         return;
      const size_t before = c->len;
      /* This branch's own hidden text, apart from earlier parts' */
      const bool hid_before = c->hid;
      c->hid = false;
      text_walk(c, chosen);
      const bool branch_hid = c->hid;
      c->hid = hid_before || branch_hid;
      if (c->as_shown && c->len == before && !c->oom) {
         /* The HTML gave no text.  When its text was hidden, there is none
          * to read: the plain form isn't what the reader saw.  When it had
          * none (only images) or couldn't be read, the plain form is read,
          * saying so. */
         const int plain = choose_branch(c->parts, c->count, i, true);
         if (!branch_hid && plain >= 0 && plain != chosen) {
            text_walk(c, plain);
            if (c->len > before && !c->oom && c->len + sizeof(PLAIN_FORM_NOTE) < c->limit) {
               char *note = strdup(PLAIN_FORM_NOTE);
               if (!note) {
                  c->oom = true;
                  return;
               }
               append_piece(c, note);
            }
         }
         return;
      }
      /* A plain branch with no text in it (blank, or only markup): the HTML
       * branch is the message. */
      const int html = choose_branch(c->parts, c->count, i, false);
      if (c->len == before && html >= 0 && html != chosen)
         text_walk(c, html);
      return;
   }
   const int end = subtree_end(c->parts, c->count, i);
   for (int j = i + 1; j < end && !c->oom; j = subtree_end(c->parts, c->count, j))
      text_walk(c, j);
}

/* The HTML part below node @p i: of an alternative, its HTML branch; of
 * anything else, the first HTML found.  -1 when none. */
static int find_html(const email_mime_part_t *parts, int count, int i) {
   const email_mime_part_t *p = &parts[i];
   if (!p->multipart)
      return body_leaf(p) && is_type(p, "text/html") ? i : -1;
   if (is_subtype(p, "alternative")) {
      const int branch = choose_branch(parts, count, i, false);
      return branch >= 0 ? find_html(parts, count, branch) : -1;
   }
   const int end = subtree_end(parts, count, i);
   for (int j = i + 1; j < end; j = subtree_end(parts, count, j)) {
      const int h = find_html(parts, count, j);
      if (h >= 0)
         return h;
   }
   return -1;
}

static void walk_top(walk_ctx_t *c) {
   for (int i = 0; i < c->count && !c->oom; i = subtree_end(c->parts, c->count, i))
      text_walk(c, i);
}

static int html_top(const email_mime_part_t *parts, int count) {
   for (int i = 0; i < count; i = subtree_end(parts, count, i)) {
      const int h = find_html(parts, count, i);
      if (h >= 0)
         return h;
   }
   return -1;
}

/* =============================================================================
 * The policy
 * ============================================================================= */

static void free_body_fields(email_message_t *out) {
   free(out->body);
   out->body = NULL;
   out->body_len = 0;
   free(out->body_html);
   out->body_html = NULL;
   out->body_html_len = 0;
   free(out->attachments);
   out->attachments = NULL;
   out->attachment_count = 0;
}

int email_mime_apply(const email_mime_part_t *parts,
                     int count,
                     bool parts_cut,
                     const email_read_opts_t *opts,
                     email_mime_fetch_fn fetch,
                     void *fetch_ctx,
                     email_message_t *out) {
   pthread_once(&s_init_once, mime_init);
   bool *used = calloc(count > 0 ? (size_t)count : 1, sizeof(bool));
   if (!used)
      return 1;

   walk_ctx_t c = { .parts = parts,
                    .count = count,
                    .used = used,
                    .fetch = fetch,
                    .fetch_ctx = fetch_ctx,
                    .as_shown = opts->text_as_shown,
                    .limit = opts->max_text_chars > 0 ? (size_t)opts->max_text_chars
                                                      : EMAIL_MAX_READ_BODY_LEN };
   walk_top(&c);
   if (c.oom) {
      free(c.text);
      goto oom;
   }
   if (c.len == 0 && !c.cut) {
      /* No body part by the rules: a message whose only text is a named part
       * that isn't an attachment (a single "name=" text part) reads that. */
      for (int i = 0; i < count; i++) {
         const email_mime_part_t *p = &parts[i];
         if (!used[i] && !p->multipart && !p->attachment &&
             (is_type(p, "text/plain") || is_type(p, "text/html"))) {
            used[i] = true;
            read_leaf(&c, i);
            if (c.len > 0 || c.oom)
               break;
         }
      }
      if (c.oom) {
         free(c.text);
         goto oom;
      }
   }
   out->body = c.text ? c.text : strdup("");
   if (!out->body)
      goto oom;
   out->body_len = (int)strlen(out->body);
   /* A message cut before any of its text was reached has lost it. */
   out->text_truncated = c.cut || (parts_cut && out->body_len == 0);
   out->hidden_text = c.hid;

   const int html = html_top(parts, count);
   if (html >= 0)
      used[html] = true; /* the body's HTML form, not an attachment */
   if (opts->want_html && html >= 0 && opts->max_html_bytes > 0) {
      size_t len = 0;
      bool hcut = false;
      email_mime_part_t local;
      const email_mime_part_t *hp = resolve(parts, html, fetch, fetch_ctx, &local);
      out->body_html = decode_part(hp, opts->max_html_bytes, &len, &hcut);
      if (!out->body_html)
         goto oom;
      out->body_html_len = len;
      out->html_truncated = hcut;
   }

   int listed = 0;
   for (int i = 0; i < count; i++) {
      if (!used[i] && !parts[i].multipart)
         listed++;
   }
   const int keep = listed < EMAIL_MAX_ATTACHMENTS ? listed : EMAIL_MAX_ATTACHMENTS;
   if (keep > 0) {
      out->attachments = calloc((size_t)keep, sizeof(email_attachment_t));
      if (!out->attachments)
         goto oom;
   }
   for (int i = 0; i < count && out->attachment_count < keep; i++) {
      if (used[i] || parts[i].multipart)
         continue;
      email_attachment_t *a = &out->attachments[out->attachment_count++];
      safe_strncpy(a->part_id, parts[i].part_id, sizeof(a->part_id));
      email_display_sanitize(parts[i].filename, strlen(parts[i].filename), a->filename,
                             sizeof(a->filename), EMAIL_DISPLAY_FILENAME);
      email_display_sanitize(parts[i].type, strlen(parts[i].type), a->mime, sizeof(a->mime), 0);
      for (char *ch = a->mime; *ch; ch++)
         *ch = (char)tolower((unsigned char)*ch);
      email_display_content_id(parts[i].content_id, strlen(parts[i].content_id), a->content_id,
                               sizeof(a->content_id));
      a->size = parts[i].size;
      a->is_inline = !parts[i].attachment &&
                     (parts[i].inline_disp || (parts[i].in_related && a->content_id[0]));
   }
   out->attachments_truncated = listed > keep || parts_cut;
   free(used);
   return 0;

oom:
   free(used);
   free_body_fields(out);
   return 1;
}

/* =============================================================================
 * Headers
 * ============================================================================= */

/* One mailbox into @p out; false for a group or an empty address. */
static bool mailbox_to(InternetAddress *ia, email_addr_t *out) {
   if (!INTERNET_ADDRESS_IS_MAILBOX(ia))
      return false;
   const char *addr = internet_address_mailbox_get_addr(INTERNET_ADDRESS_MAILBOX(ia));
   const char *name = internet_address_get_name(ia);
   email_display_sanitize(addr, addr ? strlen(addr) : 0, out->addr, sizeof(out->addr), 0);
   email_display_sanitize(name, name ? strlen(name) : 0, out->name, sizeof(out->name), 0);
   return out->addr[0] != '\0';
}

/* Flattens @p list (groups' members included, one level) into @p arr. */
static void collect(InternetAddressList *list,
                    email_addr_t *arr,
                    int cap,
                    int *count,
                    int *total,
                    int depth) {
   const int n = list ? internet_address_list_length(list) : 0;
   for (int i = 0; i < n; i++) {
      InternetAddress *ia = internet_address_list_get_address(list, i);
      if (INTERNET_ADDRESS_IS_GROUP(ia)) {
         if (depth == 0)
            collect(internet_address_group_get_members(INTERNET_ADDRESS_GROUP(ia)), arr, cap, count,
                    total, 1);
         continue;
      }
      email_addr_t a;
      if (!mailbox_to(ia, &a))
         continue;
      (*total)++;
      if (arr && *count < cap)
         arr[(*count)++] = a;
   }
}

static int list_from(InternetAddressList *list,
                     int cap,
                     email_addr_t **arr_out,
                     int *count_out,
                     int *total_out) {
   *arr_out = NULL;
   *count_out = 0;
   *total_out = 0;
   int total = 0;
   int count = 0;
   collect(list, NULL, 0, &count, &total, 0);
   if (total == 0)
      return 0;
   const int keep = total < cap ? total : cap;
   email_addr_t *arr = calloc((size_t)keep, sizeof(email_addr_t));
   if (!arr)
      return 1;
   total = 0;
   collect(list, arr, keep, &count, &total, 0);
   *arr_out = arr;
   *count_out = count;
   *total_out = total;
   return 0;
}

/* Address entries GMime parses with no '@', and group colons: its loose parse
 * of each reads on through the entries after it until an '@', '<', ';', ':'
 * or the end, so their cost is quadratic in their number.  Counted as GMime splits a list:
 * a ',' or ';' outside quotes and comments ends an entry; comments (nested,
 * with escapes) are skipped, as GMime skips them, so an '@' or '"' inside one
 * counts for nothing.  The state carries over a folded line. */
typedef struct {
   int bare;    /* entries ended so far with no '@' */
   int comment; /* comment depth */
   bool quote;
   bool at;   /* the entry being read has an '@' */
   bool text; /* ... and anything at all */
} bare_scan_t;

/* Scans @p text into @p b; returns the offset of the separator that took the
 * count past @p limit, or @p len. */
static size_t bare_scan(bare_scan_t *b, const char *text, size_t len, int limit) {
   for (size_t i = 0; i < len; i++) {
      const char c = text[i];
      if (b->comment > 0) {
         if (c == '\\' && i + 1 < len)
            i++;
         else if (c == '(')
            b->comment++;
         else if (c == ')')
            b->comment--;
         continue;
      }
      if (b->quote) {
         if (c == '\\' && i + 1 < len)
            i++;
         else if (c == '"')
            b->quote = false;
         continue;
      }
      if (c == '(') {
         b->comment = 1;
      } else if (c == '"') {
         b->quote = true;
         b->text = true;
      } else if (c == ',' || c == ';') {
         if (b->text && !b->at && ++b->bare > limit)
            return i;
         b->at = false;
         b->text = false;
      } else if (c == ':') {
         /* A group's colon: GMime's parse pays for each one too */
         if (++b->bare > limit)
            return i;
         b->text = true;
      } else if (c == '@') {
         b->at = true;
      } else if (c != ' ' && c != '\t') {
         b->text = true;
      }
   }
   return len;
}

/* A header ended: its last entry, quote or comment doesn't carry into the
 * next. */
static void bare_scan_end_header(bare_scan_t *b) {
   b->comment = 0;
   b->quote = false;
   b->at = false;
   b->text = false;
}

int email_mime_addr_list(const char *value,
                         int cap,
                         email_addr_t **list_out,
                         int *count_out,
                         int *total_out) {
   pthread_once(&s_init_once, mime_init);
   *list_out = NULL;
   *count_out = 0;
   *total_out = 0;
   if (!value || !value[0])
      return 0;
   /* A huge header is cut at its last comma before the limit: a list costs
    * per address to build, and only `cap` are kept.  The addresses cut off are
    * counted by their '@' (a close estimate) so the total stays honest. */
   char *cut = NULL;
   int beyond = 0;
   const size_t len = strlen(value);
   /* Past the bare-entry budget, cut at the entry that went over (a value
    * from Gmail's API never passed the prescan). */
   bare_scan_t bs = { 0 };
   const size_t bare_at = bare_scan(&bs, value, len, EMAIL_MIME_PRESCAN_BARE_ADDRESSES);
   if (len > EMAIL_MIME_ADDR_VALUE_MAX || bare_at < len) {
      size_t at = bare_at < EMAIL_MIME_ADDR_VALUE_MAX ? bare_at : EMAIL_MIME_ADDR_VALUE_MAX;
      while (at > 0 && value[at] != ',' && value[at] != ';')
         at--;
      cut = malloc(at + 1);
      if (!cut)
         return 1;
      memcpy(cut, value, at);
      cut[at] = '\0';
      for (size_t i = at; i < len; i++)
         beyond += value[i] == '@';
      value = cut;
   }
   InternetAddressList *list = internet_address_list_parse(s_options, value);
   int rc = 0;
   if (list) {
      rc = list_from(list, cap, list_out, count_out, total_out);
      g_object_unref(list);
   }
   free(cut);
   *total_out += beyond;
   return rc;
}

bool email_mime_addr_first(const char *value, email_addr_t *out) {
   memset(out, 0, sizeof(*out));
   email_addr_t *arr = NULL;
   int count = 0;
   int total = 0;
   if (email_mime_addr_list(value, 1, &arr, &count, &total) != 0 || count == 0) {
      free(arr);
      return false;
   }
   *out = arr[0];
   free(arr);
   return true;
}

void email_mime_header_text(const char *value, char *out, size_t out_size) {
   pthread_once(&s_init_once, mime_init);
   if (!value) {
      if (out_size)
         out[0] = '\0';
      return;
   }
   char *decoded = g_mime_utils_header_decode_text(s_options, value);
   const char *src = decoded ? decoded : value;
   email_display_sanitize(src, strlen(src), out, out_size, 0);
   g_free(decoded);
}

void email_mime_part_headers(const char *content_type,
                             const char *disposition,
                             email_mime_part_t *p) {
   pthread_once(&s_init_once, mime_init);
   if (content_type && content_type[0]) {
      GMimeContentType *ct = g_mime_content_type_parse(s_options, content_type);
      if (ct) {
         const char *cs = g_mime_content_type_get_parameter(ct, "charset");
         safe_strncpy(p->charset, cs ? cs : "", sizeof(p->charset));
         const char *name = g_mime_content_type_get_parameter(ct, "name");
         if (!p->filename[0] && name)
            safe_strncpy(p->filename, name, sizeof(p->filename));
         g_object_unref(ct);
      }
   }
   if (disposition && disposition[0]) {
      GMimeContentDisposition *cd = g_mime_content_disposition_parse(s_options, disposition);
      if (cd) {
         const char *d = g_mime_content_disposition_get_disposition(cd);
         p->attachment = d && strcasecmp(d, GMIME_DISPOSITION_ATTACHMENT) == 0;
         p->inline_disp = d && strcasecmp(d, GMIME_DISPOSITION_INLINE) == 0;
         const char *fn = g_mime_content_disposition_get_parameter(cd, "filename");
         if (fn)
            safe_strncpy(p->filename, fn, sizeof(p->filename)); /* the better name */
         g_object_unref(cd);
      }
   }
}

bool email_mime_child_id(const char *parent, int index, char *out, size_t size) {
   const int n = parent[0] ? snprintf(out, size, "%s.%d", parent, index)
                           : snprintf(out, size, "%d", index);
   return n > 0 && (size_t)n < size;
}

/* =============================================================================
 * IMAP: raw bytes through GMime
 * ============================================================================= */

/* A header line's name as GMime reads it: up to the colon, trailing spaces
 * and tabs trimmed ("To :" is "to"), lowercased.  "" when the line has no
 * colon or the name doesn't fit. */
static void header_name(const char *line, size_t len, char *out, size_t size) {
   out[0] = '\0';
   const char *colon = memchr(line, ':', len);
   if (!colon)
      return;
   size_t n = (size_t)(colon - line);
   while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t'))
      n--;
   if (n == 0 || n >= size)
      return;
   for (size_t i = 0; i < n; i++)
      out[i] = (char)tolower((unsigned char)line[i]);
   out[n] = '\0';
}

/* Whether header @p name holds addresses GMime parses into a list. */
static bool address_header(const char *name) {
   static const char *const k_names[] = {
      "from",        "to",        "cc",        "bcc",        "reply-to",      "sender",
      "resent-from", "resent-to", "resent-cc", "resent-bcc", "resent-sender", NULL
   };
   for (int i = 0; k_names[i]; i++) {
      if (strcmp(name, k_names[i]) == 0)
         return true;
   }
   return false;
}

/* Which of the address headers GMime parses into its message's lists (it
 * leaves the Resent-* ones as text): 0-5, or -1. */
static int parsed_address_type(const char *name) {
   static const char *const k_names[] = { "sender", "from", "reply-to", "to", "cc", "bcc" };
   for (int i = 0; i < 6; i++) {
      if (strcmp(name, k_names[i]) == 0)
         return i;
   }
   return -1;
}

/* The boundaries a message declares.  GMime ends a part only at a line that
 * starts with "--" and one of them, so only those lines open a part's
 * headers; a body's "-->", "--- a/file" or "-- " signature doesn't.  When the
 * scan can't be sure what was declared (too many, or a Content-Type it
 * couldn't read) every "--" line counts: more counting, never less. */
#define PRESCAN_BOUNDARY_NAMES 64
#define PRESCAN_BOUNDARY_MAX 72 /* RFC 2046 boundaries are at most 70 characters */
#define PRESCAN_CONTENT_TYPE_MAX 2048
typedef struct {
   char name[PRESCAN_BOUNDARY_NAMES][PRESCAN_BOUNDARY_MAX];
   size_t len[PRESCAN_BOUNDARY_NAMES];
   int count;
   bool any_dashes; /* every "--" line counts */
} boundary_set_t;

static void add_boundary(boundary_set_t *b, const char *v, size_t n) {
   if (n == 0)
      return;
   if (b->count >= PRESCAN_BOUNDARY_NAMES) {
      b->any_dashes = true;
      return;
   }
   /* A longer one is kept as its prefix: matching a prefix counts more lines. */
   if (n > PRESCAN_BOUNDARY_MAX)
      n = PRESCAN_BOUNDARY_MAX;
   memcpy(b->name[b->count], v, n);
   b->len[b->count++] = n;
}

/* Whether @p line ("--" already checked) starts with a declared boundary. */
static bool is_boundary(const boundary_set_t *b, const char *line, size_t text) {
   if (b->any_dashes)
      return true;
   for (int i = 0; i < b->count; i++) {
      if (text >= 2 + b->len[i] && memcmp(line + 2, b->name[i], b->len[i]) == 0)
         return true;
   }
   return false;
}

/* The scan's state: where GMime parses headers and what they declare. */
typedef struct {
   bool in_header;
   bool message_next;    /* this block's part is a message: headers follow it */
   bool block_has_type;  /* this block declared a Content-Type */
   bool block_is_part;   /* this block opened after a boundary (a part's headers) */
   bool digest;          /* a multipart/digest was declared */
   bool address;         /* the header being read holds addresses */
   size_t header_bytes;  /* the header being read, folded lines included */
   size_t address_bytes; /* every address header so far */
   bare_scan_t bare;     /* address entries without an '@', all headers together */
   int addr_count[6];    /* this header block's headers of each type GMime parses */
   /* The Content-Type being read, unfolded, until it ends */
   char ct[PRESCAN_CONTENT_TYPE_MAX];
   size_t ct_len;
   bool in_ct;
   bool ct_too_long;
   boundary_set_t declared;
} prescan_t;

/* Unfolds as GMime does: the line break goes, everything else stays (a folded
 * line's leading whitespace included), so a value folded inside its quotes
 * reads as GMime reads it.  Room is kept for the NUL end_header adds. */
static void ct_append(prescan_t *st, const char *text, size_t len) {
   if (st->ct_len + len + 1 > sizeof(st->ct)) {
      st->ct_too_long = true;
      return;
   }
   memcpy(st->ct + st->ct_len, text, len);
   st->ct_len += len;
}

/* The header being read has ended: read what a Content-Type declared.  GMime
 * parses the value (its own reading of the type, comments, spacing and RFC
 * 2231 included), so the scan's verdict is the one GMime will act on.  A
 * value it can't parse, or too long to have read whole, counts everything. */
static void end_header(prescan_t *st) {
   if (st->in_ct) {
      GMimeContentType *ct = NULL;
      if (!st->ct_too_long) {
         st->ct[st->ct_len] = '\0';
         ct = g_mime_content_type_parse(s_options, st->ct);
      }
      const char *type = ct ? g_mime_content_type_get_media_type(ct) : NULL;
      const char *sub = ct ? g_mime_content_type_get_media_subtype(ct) : NULL;
      if (!type || !sub) {
         st->declared.any_dashes = true;
         st->message_next = true;
      } else if (strcasecmp(type, "message") == 0) {
         st->message_next = true;
      } else if (strcasecmp(type, "multipart") == 0) {
         st->digest = st->digest || strcasecmp(sub, "digest") == 0;
         const char *b = g_mime_content_type_get_parameter(ct, "boundary");
         if (b && b[0] && !strpbrk(b, " \t\r\n"))
            add_boundary(&st->declared, b, strlen(b));
         else
            /* No boundary (parts GMime finds some other way), or one with
             * whitespace, which this scan might match differently: count
             * every "--" line. */
            st->declared.any_dashes = true;
      }
      if (ct)
         g_object_unref(ct);
   }
   st->in_ct = false;
   st->ct_len = 0;
   st->ct_too_long = false;
   st->address = false;
   bare_scan_end_header(&st->bare);
   st->header_bytes = 0;
}

size_t email_mime_prescan(const char *raw, size_t raw_len, bool *cut_out) {
   pthread_once(&s_init_once, mime_init);
   *cut_out = false;
   int boundaries = 0;
   int headers = 0;
   /* Where GMime parses headers: the message's own, each part's after a
    * boundary line, and a forwarded message's at the start of its content (a
    * part declared as a message type, or a digest's part that declares
    * nothing: RFC 2046 makes that message/rfc822).  Every line of those blocks
    * counts, whatever it looks like; a body's lines don't (they'd cut a long
    * log for nothing).  Address headers also count together: GMime builds
    * every address, at a cost that grows faster than their number. */
   prescan_t *st = calloc(1, sizeof(*st));
   if (!st) {
      *cut_out = true;
      return 0;
   }
   st->in_header = true;
   size_t at = raw_len;
   for (size_t i = 0; i < raw_len;) {
      const char *nl = memchr(raw + i, '\n', raw_len - i);
      const size_t end = nl ? (size_t)(nl - raw) + 1 : raw_len;
      const size_t len = end - i;
      const char *line = raw + i;
      size_t text = len;
      while (text > 0 && (line[text - 1] == '\n' || line[text - 1] == '\r'))
         text--;
      if (len > EMAIL_MIME_PRESCAN_LINE)
         goto cut;
      if (text >= 2 && line[0] == '-' && line[1] == '-' && is_boundary(&st->declared, line, text)) {
         if (++boundaries > EMAIL_MIME_PRESCAN_BOUNDARIES)
            goto cut;
         const bool keep = st->in_header && st->message_next;
         end_header(st);
         memset(st->addr_count, 0, sizeof(st->addr_count));
         /* Inside the headers of a part that opens a message, a boundary
          * line doesn't end what GMime will read as that message's headers. */
         if (!keep) {
            st->in_header = true;
            st->message_next = false;
            st->block_has_type = false;
            st->block_is_part = true;
         }
      } else if (st->in_header) {
         if (text == 0) {
            end_header(st);
            memset(st->addr_count, 0, sizeof(st->addr_count));
            st->in_header = st->message_next ||
                            (st->digest && st->block_is_part && !st->block_has_type);
            st->message_next = false;
            st->block_has_type = false;
            st->block_is_part = false;
         } else if (line[0] == ' ' || line[0] == '\t') {
            st->header_bytes += len;
            if (st->header_bytes > EMAIL_MIME_PRESCAN_HEADER_BYTES)
               goto cut;
            if (st->address && (st->address_bytes += len) > EMAIL_MIME_PRESCAN_ADDRESS_BYTES)
               goto cut;
            if (st->address &&
                bare_scan(&st->bare, line, text, EMAIL_MIME_PRESCAN_BARE_ADDRESSES) < text)
               goto cut;
            if (st->in_ct)
               ct_append(st, line, text);
         } else {
            end_header(st);
            if (++headers > EMAIL_MIME_PRESCAN_HEADERS)
               goto cut;
            st->header_bytes = len;
            if (st->header_bytes > EMAIL_MIME_PRESCAN_HEADER_BYTES)
               goto cut;
            char name[32];
            header_name(line, text, name, sizeof(name));
            st->address = address_header(name);
            if (st->address && (st->address_bytes += len) > EMAIL_MIME_PRESCAN_ADDRESS_BYTES)
               goto cut;
            if (st->address) {
               const char *colon = memchr(line, ':', text);
               const size_t vlen = colon ? text - (size_t)(colon + 1 - line) : 0;
               if (colon &&
                   bare_scan(&st->bare, colon + 1, vlen, EMAIL_MIME_PRESCAN_BARE_ADDRESSES) < vlen)
                  goto cut;
            }
            /* GMime parses every header of a type again each time another
             * arrives: their cost is quadratic in how many there are. */
            const int type = parsed_address_type(name);
            if (type >= 0 && ++st->addr_count[type] > EMAIL_MIME_PRESCAN_ADDRESS_REPEATS)
               goto cut;
            if (strcmp(name, "content-type") == 0) {
               st->in_ct = true;
               st->block_has_type = true;
               const char *colon = memchr(line, ':', text);
               ct_append(st, colon + 1, text - (size_t)(colon + 1 - line));
            }
         }
      }
      i = end;
      continue;
cut:
      *cut_out = true;
      at = i;
      break;
   }
   free(st);
   return at;
}

typedef struct {
   const char *raw;
   size_t raw_len;
   bool raw_cut;
   email_mime_part_t *parts;
   int count;
   bool cut; /* a limit stopped the walk */
} imap_walk_t;

static email_mime_part_t *new_node(imap_walk_t *w, GMimeObject *obj, const char *id) {
   if (w->count >= EMAIL_MIME_MAX_PARTS) {
      w->cut = true;
      return NULL;
   }
   email_mime_part_t *p = &w->parts[w->count++];
   memset(p, 0, sizeof(*p));
   safe_strncpy(p->part_id, id, sizeof(p->part_id));
   GMimeContentType *ct = g_mime_object_get_content_type(obj);
   const char *type = ct ? g_mime_content_type_get_media_type(ct) : NULL;
   const char *sub = ct ? g_mime_content_type_get_media_subtype(ct) : NULL;
   snprintf(p->type, sizeof(p->type), "%s/%s", type ? type : "application",
            sub ? sub : "octet-stream");
   for (char *c = p->type; *c; c++)
      *c = (char)tolower((unsigned char)*c);
   return p;
}

static void leaf(imap_walk_t *w, GMimeObject *obj, const char *id, bool in_related, bool in_alt) {
   email_mime_part_t *p = new_node(w, obj, id);
   if (!p)
      return;
   p->in_related = in_related;
   p->in_alternative = in_alt;
   const char *cs = g_mime_object_get_content_type_parameter(obj, "charset");
   safe_strncpy(p->charset, cs ? cs : "", sizeof(p->charset));
   const char *cid = g_mime_object_get_content_id(obj);
   safe_strncpy(p->content_id, cid ? cid : "", sizeof(p->content_id));
   GMimeContentDisposition *cd = g_mime_object_get_content_disposition(obj);
   if (cd) {
      const char *d = g_mime_content_disposition_get_disposition(cd);
      p->attachment = d && strcasecmp(d, GMIME_DISPOSITION_ATTACHMENT) == 0;
      p->inline_disp = d && strcasecmp(d, GMIME_DISPOSITION_INLINE) == 0;
   }
   if (GMIME_IS_MESSAGE_PART(obj)) {
      /* A forwarded message is one attachment; its own parts aren't walked. */
      snprintf(p->type, sizeof(p->type), "message/rfc822");
      p->data_absent = true;
      return;
   }
   if (!GMIME_IS_PART(obj)) {
      p->data_absent = true;
      return;
   }
   const char *fn = g_mime_part_get_filename(GMIME_PART(obj));
   safe_strncpy(p->filename, fn ? fn : "", sizeof(p->filename));
   GMimeDataWrapper *dw = g_mime_part_get_content(GMIME_PART(obj));
   GMimeStream *s = dw ? g_mime_data_wrapper_get_stream(dw) : NULL;
   /* With persist_stream on, a part's content is a window on our own buffer:
    * point into it rather than copy it. */
   if (!s || !GMIME_IS_STREAM_MEM(s) || !GMIME_STREAM_MEM(s)->buffer ||
       GMIME_STREAM_MEM(s)->buffer->data != (const guint8 *)w->raw) {
      p->data_absent = true;
      return;
   }
   g_mime_stream_reset(s);
   const gint64 start = g_mime_stream_tell(s);
   const gint64 len = g_mime_stream_length(s);
   if (start < 0 || len < 0 || (size_t)start > w->raw_len || (size_t)len > w->raw_len - start) {
      p->data_absent = true;
      return;
   }
   p->data = w->raw + start;
   p->data_len = (size_t)len;
   p->data_cut = w->raw_cut && (size_t)(start + len) >= w->raw_len;
   switch (g_mime_data_wrapper_get_encoding(dw)) {
      case GMIME_CONTENT_ENCODING_BASE64:
         p->encoding = EMAIL_MIME_ENC_BASE64;
         p->size = p->data_len / 4 * 3; /* line breaks make this a slight overestimate */
         break;
      case GMIME_CONTENT_ENCODING_QUOTEDPRINTABLE:
         p->encoding = EMAIL_MIME_ENC_QP;
         p->size = p->data_len;
         break;
      case GMIME_CONTENT_ENCODING_UUENCODE:
         p->encoding = EMAIL_MIME_ENC_UUENCODE;
         p->size = p->data_len / 4 * 3;
         break;
      default:
         p->size = p->data_len;
         break;
   }
}

static void imap_walk(imap_walk_t *w,
                      GMimeObject *obj,
                      const char *id,
                      int depth,
                      bool in_related,
                      bool in_alt) {
   if (!obj)
      return;
   if (!GMIME_IS_MULTIPART(obj)) {
      leaf(w, obj, id[0] ? id : "1", in_related, in_alt);
      return;
   }
   if (depth >= EMAIL_MIME_MAX_DEPTH) {
      w->cut = true;
      return;
   }
   email_mime_part_t *node = new_node(w, obj, id);
   if (!node)
      return;
   node->multipart = true;
   const char *sub = node->type + 10; /* "multipart/" */
   const bool related = in_related || strcasecmp(sub, "related") == 0;
   const bool alt = in_alt || strcasecmp(sub, "alternative") == 0;
   const int n = g_mime_multipart_get_count(GMIME_MULTIPART(obj));
   for (int i = 0; i < n && !w->cut; i++) {
      char child[32];
      if (!email_mime_child_id(id, i + 1, child, sizeof(child))) {
         w->cut = true; /* an id that doesn't fit would name another part */
         return;
      }
      imap_walk(w, g_mime_multipart_get_part(GMIME_MULTIPART(obj), i), child, depth + 1, related,
                alt);
   }
}

static void headers_from(GMimeMessage *msg, email_message_t *out) {
   InternetAddressList *from = g_mime_message_get_from(msg);
   if (from && internet_address_list_length(from) > 0) {
      email_addr_t a = { 0 };
      if (mailbox_to(internet_address_list_get_address(from, 0), &a)) {
         safe_strncpy(out->from_name, a.name, sizeof(out->from_name));
         safe_strncpy(out->from_addr, a.addr, sizeof(out->from_addr));
      }
   }
   const char *subject = g_mime_message_get_subject(msg);
   email_display_sanitize(subject, subject ? strlen(subject) : 0, out->subject,
                          sizeof(out->subject), 0);
   const char *date = g_mime_object_get_header(GMIME_OBJECT(msg), "Date");
   email_display_sanitize(date, date ? strlen(date) : 0, out->date_str, sizeof(out->date_str), 0);
   InternetAddressList *rt = g_mime_message_get_addresses(msg, GMIME_ADDRESS_TYPE_REPLY_TO);
   if (rt && internet_address_list_length(rt) > 0)
      mailbox_to(internet_address_list_get_address(rt, 0), &out->reply_to);
}

int email_mime_parse_raw(char *raw,
                         size_t raw_len,
                         bool raw_cut,
                         const email_read_opts_t *opts,
                         email_message_t *out) {
   pthread_once(&s_init_once, mime_init);
   bool scan_cut = false;
   const size_t len = email_mime_prescan(raw, raw_len, &scan_cut);
   if (scan_cut)
      OLOG_WARNING("email_mime: message cut at %zu of %zu bytes (part or header limit)", len,
                   raw_len);

   int rc = 1;
   email_mime_part_t *parts = NULL;
   GByteArray *ba = g_byte_array_new_take((guint8 *)raw, len);
   GMimeStream *stream = g_mime_stream_mem_new_with_byte_array(ba);
   g_mime_stream_mem_set_owner(GMIME_STREAM_MEM(stream), FALSE);
   GMimeParser *parser = g_mime_parser_new_with_stream(stream);
   g_mime_parser_set_persist_stream(parser, TRUE);
   GMimeMessage *msg = g_mime_parser_construct_message(parser, s_options);
   if (!msg)
      goto done;

   headers_from(msg, out);
   if (list_from(g_mime_message_get_addresses(msg, GMIME_ADDRESS_TYPE_TO), EMAIL_MAX_ADDRS,
                 &out->to_list, &out->to_count, &out->to_total) != 0 ||
       list_from(g_mime_message_get_addresses(msg, GMIME_ADDRESS_TYPE_CC), EMAIL_MAX_ADDRS,
                 &out->cc_list, &out->cc_count, &out->cc_total) != 0)
      goto done;
   if (opts->headers_only) {
      rc = 0;
      goto done;
   }

   parts = calloc(EMAIL_MIME_MAX_PARTS, sizeof(*parts));
   if (!parts)
      goto done;
   imap_walk_t w = { .raw = raw, .raw_len = len, .raw_cut = raw_cut || scan_cut, .parts = parts };
   imap_walk(&w, g_mime_message_get_mime_part(msg), "", 0, false, false);
   rc = email_mime_apply(parts, w.count, w.cut || w.raw_cut, opts, NULL, NULL, out);

done:
   free(parts);
   if (msg)
      g_object_unref(msg);
   g_object_unref(parser);
   g_object_unref(stream);
   g_byte_array_free(ba, FALSE); /* the bytes are the caller's */
   if (rc != 0) {
      free(out->to_list);
      out->to_list = NULL;
      out->to_count = out->to_total = 0;
      free(out->cc_list);
      out->cc_list = NULL;
      out->cc_count = out->cc_total = 0;
   }
   return rc;
}

/* =============================================================================
 * The message
 * ============================================================================= */

void email_message_free(email_message_t *msg) {
   if (!msg)
      return;
   free_body_fields(msg);
   free(msg->to_list);
   msg->to_list = NULL;
   msg->to_count = msg->to_total = 0;
   free(msg->cc_list);
   msg->cc_list = NULL;
   msg->cc_count = msg->cc_total = 0;
}
