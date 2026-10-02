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
 * The Gmail API's view of a message, without the network: its base64url, its
 * header arrays, and its MIME tree as the email_mime part list (so the same
 * reading policy serves Gmail and IMAP, and tests can drive it with JSON).
 */

#include <ctype.h>
#include <json-c/json.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "tools/email_mime.h"
#include "tools/gmail_client_internal.h"

/* Lookup table: 256 bytes of .rodata, replaces 5 branches per character */
static const int8_t BASE64URL_LUT[256] = {
   ['A'] = 0,  ['B'] = 1,  ['C'] = 2,  ['D'] = 3,  ['E'] = 4,  ['F'] = 5,  ['G'] = 6,  ['H'] = 7,
   ['I'] = 8,  ['J'] = 9,  ['K'] = 10, ['L'] = 11, ['M'] = 12, ['N'] = 13, ['O'] = 14, ['P'] = 15,
   ['Q'] = 16, ['R'] = 17, ['S'] = 18, ['T'] = 19, ['U'] = 20, ['V'] = 21, ['W'] = 22, ['X'] = 23,
   ['Y'] = 24, ['Z'] = 25, ['a'] = 26, ['b'] = 27, ['c'] = 28, ['d'] = 29, ['e'] = 30, ['f'] = 31,
   ['g'] = 32, ['h'] = 33, ['i'] = 34, ['j'] = 35, ['k'] = 36, ['l'] = 37, ['m'] = 38, ['n'] = 39,
   ['o'] = 40, ['p'] = 41, ['q'] = 42, ['r'] = 43, ['s'] = 44, ['t'] = 45, ['u'] = 46, ['v'] = 47,
   ['w'] = 48, ['x'] = 49, ['y'] = 50, ['z'] = 51, ['0'] = 52, ['1'] = 53, ['2'] = 54, ['3'] = 55,
   ['4'] = 56, ['5'] = 57, ['6'] = 58, ['7'] = 59, ['8'] = 60, ['9'] = 61, ['-'] = 62, ['_'] = 63,
};


bool gmail_b64url_char(unsigned char c) {
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
          c == '_';
}

unsigned char *gmail_base64url_decode(const char *input, size_t max_out, size_t *out_len) {
   *out_len = 0;
   if (!input || !input[0])
      return NULL;
   const size_t in_len = strlen(input);
   size_t limit = in_len / 4 * 3 + 3;
   if (max_out > 0 && limit > max_out)
      limit = max_out;
   unsigned char *out = malloc(limit + 1); /* + the NUL callers may rely on */
   if (!out)
      return NULL;
   size_t pos = 0;
   for (size_t i = 0; i < in_len && pos < limit;) {
      const unsigned char ca = (unsigned char)input[i++];
      const unsigned char cb = i < in_len ? (unsigned char)input[i++] : 0;
      const unsigned char cc = i < in_len ? (unsigned char)input[i++] : 0;
      const unsigned char cd = i < in_len ? (unsigned char)input[i++] : 0;
      if (!gmail_b64url_char(ca) || !gmail_b64url_char(cb))
         break;
      const bool have_c = gmail_b64url_char(cc);
      const bool have_d = have_c && gmail_b64url_char(cd);
      uint32_t triple = ((uint32_t)BASE64URL_LUT[ca] << 18) | ((uint32_t)BASE64URL_LUT[cb] << 12);
      if (have_c)
         triple |= (uint32_t)BASE64URL_LUT[cc] << 6;
      if (have_d)
         triple |= (uint32_t)BASE64URL_LUT[cd];
      out[pos++] = (unsigned char)(triple >> 16);
      if (have_c && pos < limit)
         out[pos++] = (unsigned char)(triple >> 8);
      if (have_d && pos < limit)
         out[pos++] = (unsigned char)triple;
      if (!have_d)
         break; /* padding or the end */
   }
   out[pos] = '\0';
   *out_len = pos;
   return out;
}

const char *gmail_find_header(struct json_object *headers_arr, const char *name) {
   if (!headers_arr || !json_object_is_type(headers_arr, json_type_array))
      return NULL;

   int len = json_object_array_length(headers_arr);
   for (int i = 0; i < len; i++) {
      struct json_object *header = json_object_array_get_idx(headers_arr, i);
      struct json_object *name_obj = NULL;
      if (json_object_object_get_ex(header, "name", &name_obj)) {
         if (strcasecmp(json_object_get_string(name_obj), name) == 0) {
            struct json_object *val_obj = NULL;
            if (json_object_object_get_ex(header, "value", &val_obj))
               return json_object_get_string(val_obj);
         }
      }
   }
   return NULL;
}

const char *gmail_json_str(struct json_object *obj, const char *key) {
   struct json_object *v = NULL;
   return json_object_object_get_ex(obj, key, &v) ? json_object_get_string(v) : NULL;
}

static void gmail_leaf(gmail_parts_t *w,
                       struct json_object *node,
                       const char *id,
                       bool related,
                       bool alt) {
   if (w->count >= EMAIL_MIME_MAX_PARTS) {
      w->cut = true;
      return;
   }
   const int k = w->count++;
   email_mime_part_t *p = &w->parts[k];
   memset(p, 0, sizeof(*p));
   snprintf(p->part_id, sizeof(p->part_id), "%s", id[0] ? id : "1");
   const char *mime = gmail_json_str(node, "mimeType");
   snprintf(p->type, sizeof(p->type), "%s", mime && mime[0] ? mime : "application/octet-stream");
   for (char *c = p->type; *c; c++)
      *c = (char)tolower((unsigned char)*c);
   p->in_related = related;
   p->in_alternative = alt;

   struct json_object *headers = NULL;
   json_object_object_get_ex(node, "headers", &headers);
   const char *fn = gmail_json_str(node, "filename");
   if (fn)
      snprintf(p->filename, sizeof(p->filename), "%s", fn);
   email_mime_part_headers(gmail_find_header(headers, "Content-Type"),
                           gmail_find_header(headers, "Content-Disposition"), p);
   const char *cid = gmail_find_header(headers, "Content-ID");
   if (cid)
      snprintf(p->content_id, sizeof(p->content_id), "%s", cid);

   struct json_object *body = NULL;
   if (!json_object_object_get_ex(node, "body", &body))
      return;
   struct json_object *size = NULL;
   if (json_object_object_get_ex(body, "size", &size)) {
      const int64_t v = json_object_get_int64(size);
      p->size = v > 0 ? (size_t)v : 0;
   }
   const char *data = gmail_json_str(body, "data");
   const char *aid = gmail_json_str(body, "attachmentId");
   if (strncmp(p->type, "text/", 5) == 0 && data && data[0]) {
      size_t len = 0;
      unsigned char *bytes = gmail_base64url_decode(data, GMAIL_TEXT_PART_MAX, &len);
      if (bytes) {
         w->owned[k] = bytes;
         w->owned_len[k] = len;
         p->data = (const char *)bytes;
         p->data_len = len;
         p->data_cut = len >= GMAIL_TEXT_PART_MAX;
      }
   } else if (aid && aid[0]) {
      w->attachment_ids[k] = aid;
      p->data_absent = true;
   }
}

static void gmail_walk(gmail_parts_t *w,
                       struct json_object *node,
                       const char *id,
                       int depth,
                       bool related,
                       bool alt) {
   const char *mime = gmail_json_str(node, "mimeType");
   struct json_object *kids = NULL;
   if (!mime || strncasecmp(mime, "multipart/", 10) != 0) {
      gmail_leaf(w, node, id, related, alt);
      return;
   }
   /* A multipart with no parts array is an empty container (as on IMAP), not a leaf. */
   if (!json_object_object_get_ex(node, "parts", &kids) ||
       !json_object_is_type(kids, json_type_array))
      kids = NULL;
   if (depth >= EMAIL_MIME_MAX_DEPTH || w->count >= EMAIL_MIME_MAX_PARTS) {
      w->cut = true;
      return;
   }
   email_mime_part_t *c = &w->parts[w->count++];
   memset(c, 0, sizeof(*c));
   c->multipart = true;
   snprintf(c->part_id, sizeof(c->part_id), "%s", id);
   snprintf(c->type, sizeof(c->type), "%s", mime);
   for (char *ch = c->type; *ch; ch++)
      *ch = (char)tolower((unsigned char)*ch);
   related = related || strcasecmp(mime + 10, "related") == 0;
   alt = alt || strcasecmp(mime + 10, "alternative") == 0;
   const int n = kids ? (int)json_object_array_length(kids) : 0;
   for (int i = 0; i < n && !w->cut; i++) {
      char child[32];
      if (!email_mime_child_id(id, i + 1, child, sizeof(child))) {
         w->cut = true; /* an id that doesn't fit would name another part */
         return;
      }
      gmail_walk(w, json_object_array_get_idx(kids, i), child, depth + 1, related, alt);
   }
}

int gmail_parts_from_payload(struct json_object *payload, gmail_parts_t *out) {
   memset(out, 0, sizeof(*out));
   out->parts = calloc(EMAIL_MIME_MAX_PARTS, sizeof(*out->parts));
   out->attachment_ids = calloc(EMAIL_MIME_MAX_PARTS, sizeof(*out->attachment_ids));
   out->owned = calloc(EMAIL_MIME_MAX_PARTS, sizeof(*out->owned));
   out->owned_len = calloc(EMAIL_MIME_MAX_PARTS, sizeof(*out->owned_len));
   if (!out->parts || !out->attachment_ids || !out->owned || !out->owned_len) {
      gmail_parts_free(out);
      return 1;
   }
   if (payload)
      gmail_walk(out, payload, "", 0, false, false);
   return 0;
}

void gmail_parts_free(gmail_parts_t *p) {
   for (int i = 0; p->owned && i < p->count; i++)
      free(p->owned[i]);
   for (int i = 0; p->ids_owned && p->attachment_ids && i < p->count; i++)
      free((char *)p->attachment_ids[i]);
   free(p->owned);
   free(p->owned_len);
   free(p->attachment_ids);
   free(p->parts);
   memset(p, 0, sizeof(*p));
}
