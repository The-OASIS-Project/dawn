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
 * The mail panel's cursor and page merge (email_cursor.h).
 */

#include "webui/email_cursor.h"

#include <ctype.h>
#include <json-c/json.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CURSOR_VERSION 1

/* =============================================================================
 * Filter hash
 * ============================================================================= */

#define FNV_OFFSET 14695981039346656037ULL
#define FNV_PRIME 1099511628211ULL

static uint64_t fnv_bytes(uint64_t h, const void *data, size_t len) {
   const unsigned char *p = data;
   for (size_t i = 0; i < len; i++) {
      h ^= p[i];
      h *= FNV_PRIME;
   }
   return h;
}

/* A string and its terminator, so "ab"+"c" and "a"+"bc" differ; ASCII folded
 * when @p fold (folder names compare without case). */
static uint64_t fnv_str(uint64_t h, const char *s, bool fold) {
   for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
      const unsigned char c = fold ? (unsigned char)tolower(*p) : *p;
      h = fnv_bytes(h, &c, 1);
   }
   const unsigned char nul = 0;
   return fnv_bytes(h, &nul, 1);
}

uint64_t email_cursor_filter_hash(const char *verb,
                                  const int64_t *ids,
                                  int n_ids,
                                  bool unread_only,
                                  const char *folder,
                                  const char *query) {
   uint64_t h = FNV_OFFSET;
   h = fnv_str(h, verb, false);
   const unsigned char u = unread_only ? 1 : 0;
   h = fnv_bytes(h, &u, 1);
   h = fnv_str(h, folder, true);
   h = fnv_str(h, query, false);
   if (!ids) {
      return fnv_str(h, "*", false);
   }
   /* The set, not the order it was asked in. */
   int64_t sorted[EMAIL_CURSOR_ACCOUNTS] = { 0 };
   int n = n_ids < EMAIL_CURSOR_ACCOUNTS ? n_ids : EMAIL_CURSOR_ACCOUNTS;
   for (int i = 0; i < n; i++) {
      int64_t v = ids[i];
      int j = i;
      for (; j > 0 && sorted[j - 1] > v; j--)
         sorted[j] = sorted[j - 1];
      sorted[j] = v;
   }
   for (int i = 0; i < n; i++)
      h = fnv_bytes(h, &sorted[i], sizeof(sorted[i]));
   return h;
}

/* =============================================================================
 * Encode / decode
 * ============================================================================= */

static bool gmail_id_ok(const char *id) {
   size_t len = 0;
   for (; id[len]; len++) {
      if (!isxdigit((unsigned char)id[len]))
         return false;
   }
   return len <= EMAIL_CURSOR_ID_MAX;
}

static json_object *pos_json(const email_cursor_pos_t *p) {
   json_object *o = json_object_new_object();
   if (!o)
      return NULL;
   json_object_object_add(o, "id", json_object_new_int64(p->account_id));
   json_object_object_add(o, "k", json_object_new_string(p->is_imap ? "i" : "g"));
   if (p->is_imap) {
      if (p->before_uid)
         json_object_object_add(o, "u", json_object_new_int64(p->before_uid));
      if (p->uidvalidity)
         json_object_object_add(o, "v", json_object_new_int64(p->uidvalidity));
      if (p->folder_hash)
         json_object_object_add(o, "f", json_object_new_int64(p->folder_hash));
   } else {
      if (p->next_date > 0)
         json_object_object_add(o, "d", json_object_new_int64((int64_t)p->next_date));
      if (p->emitted > 0)
         json_object_object_add(o, "c", json_object_new_int(p->emitted));
      if (p->seen_count > 0) {
         json_object *seen = json_object_new_array();
         for (int i = 0; seen && i < p->seen_count; i++)
            json_object_array_add(seen, json_object_new_string(p->seen[i]));
         json_object_object_add(o, "s", seen);
      }
   }
   return o;
}

char *email_cursor_encode(const email_cursor_t *c) {
   if (!c || c->count < 0 || c->count > EMAIL_CURSOR_ACCOUNTS)
      return NULL;
   json_object *root = json_object_new_object();
   json_object *arr = json_object_new_array();
   if (!root || !arr) {
      json_object_put(root);
      json_object_put(arr);
      return NULL;
   }
   char f[17];
   snprintf(f, sizeof(f), "%016llx", (unsigned long long)c->filter);
   json_object_object_add(root, "v", json_object_new_int(CURSOR_VERSION));
   json_object_object_add(root, "f", json_object_new_string(f));
   for (int i = 0; i < c->count; i++)
      json_object_array_add(arr, pos_json(&c->pos[i]));
   json_object_object_add(root, "a", arr);

   size_t len = 0;
   const char *js = json_object_to_json_string_length(root, JSON_C_TO_STRING_PLAIN, &len);
   char *out = NULL;
   if (js && len <= EMAIL_CURSOR_JSON_MAX) {
      const size_t b64_len = sodium_base64_encoded_len(len,
                                                       sodium_base64_VARIANT_URLSAFE_NO_PADDING);
      out = malloc(b64_len);
      if (out)
         sodium_bin2base64(out, b64_len, (const unsigned char *)js, len,
                           sodium_base64_VARIANT_URLSAFE_NO_PADDING);
   }
   json_object_put(root);
   return out;
}

/* An integer member, or false if it's there but isn't one in [lo, hi]. */
static bool get_int(json_object *o, const char *key, int64_t lo, int64_t hi, int64_t *out) {
   json_object *v;
   *out = 0;
   if (!json_object_object_get_ex(o, key, &v))
      return true;
   if (!json_object_is_type(v, json_type_int))
      return false;
   const int64_t n = json_object_get_int64(v);
   if (n < lo || n > hi)
      return false;
   *out = n;
   return true;
}

/* A string member copied into @p out, or false if it's there but isn't one or doesn't fit. */
static bool get_str(json_object *o, const char *key, char *out, size_t size) {
   json_object *v;
   out[0] = '\0';
   if (!json_object_object_get_ex(o, key, &v))
      return true;
   if (!json_object_is_type(v, json_type_string))
      return false;
   const char *s = json_object_get_string(v);
   const size_t len = (size_t)json_object_get_string_len(v);
   if (len >= size || strlen(s) != len) /* too long, or an embedded NUL */
      return false;
   memcpy(out, s, len + 1);
   return true;
}

static bool pos_from_json(json_object *o, email_cursor_pos_t *p) {
   memset(p, 0, sizeof(*p));
   if (!json_object_is_type(o, json_type_object))
      return false;
   int64_t id;
   if (!get_int(o, "id", 1, INT64_MAX, &id) || id < 1)
      return false;
   p->account_id = id;
   char kind[4];
   if (!get_str(o, "k", kind, sizeof(kind)))
      return false;
   if (strcmp(kind, "i") == 0) {
      int64_t u, v, f;
      if (!get_int(o, "u", 0, UINT32_MAX, &u) || u == 1 || !get_int(o, "v", 0, UINT32_MAX, &v) ||
          !get_int(o, "f", 0, UINT32_MAX, &f))
         return false;
      p->is_imap = true;
      p->before_uid = (uint32_t)u;
      p->uidvalidity = (uint32_t)v;
      p->folder_hash = (uint32_t)f;
      return true;
   }
   if (strcmp(kind, "g") != 0)
      return false;
   int64_t d, emitted;
   /* Capped well below overflow: the fetch asks for rows before d + 1. */
   if (!get_int(o, "d", 0, EMAIL_CURSOR_DATE_MAX, &d) ||
       !get_int(o, "c", 0, EMAIL_CURSOR_EMITTED_MAX, &emitted) || (d == 0 && emitted != 0))
      return false;
   p->next_date = (time_t)d;
   p->emitted = (int)emitted;
   json_object *seen;
   if (!json_object_object_get_ex(o, "s", &seen))
      return p->emitted == 0;
   if (!json_object_is_type(seen, json_type_array) || d == 0)
      return false;
   const size_t n = json_object_array_length(seen);
   if (n == 0 || n > EMAIL_CURSOR_SEEN_MAX)
      return false;
   for (size_t i = 0; i < n; i++) {
      json_object *v = json_object_array_get_idx(seen, i);
      char *dst = p->seen[i];
      if (!json_object_is_type(v, json_type_string))
         return false;
      const size_t len = (size_t)json_object_get_string_len(v);
      const char *src = json_object_get_string(v);
      if (len == 0 || len > EMAIL_CURSOR_ID_MAX || strlen(src) != len)
         return false;
      memcpy(dst, src, len + 1);
      if (!gmail_id_ok(dst))
         return false;
   }
   p->seen_count = (int)n;
   return p->emitted >= p->seen_count; /* the ids are of rows it counted */
}

bool email_cursor_decode(const char *b64, email_cursor_t *out) {
   if (!b64 || !out)
      return false;
   memset(out, 0, sizeof(*out));
   const size_t b64_len = strnlen(b64, EMAIL_CURSOR_B64_MAX + 1);
   if (b64_len == 0 || b64_len > EMAIL_CURSOR_B64_MAX)
      return false;

   unsigned char bin[EMAIL_CURSOR_JSON_MAX + 1];
   size_t bin_len = 0;
   if (sodium_base642bin(bin, EMAIL_CURSOR_JSON_MAX, b64, b64_len, NULL, &bin_len, NULL,
                         sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0 ||
       bin_len == 0)
      return false;
   bin[bin_len] = '\0';
   if (strlen((const char *)bin) != bin_len) /* an embedded NUL */
      return false;

   json_tokener *tok = json_tokener_new();
   if (!tok)
      return false;
   json_object *root = json_tokener_parse_ex(tok, (const char *)bin, (int)bin_len);
   const bool whole = json_tokener_get_error(tok) == json_tokener_success &&
                      json_tokener_get_parse_end(tok) == bin_len;
   json_tokener_free(tok);

   bool ok = false;
   json_object *v, *f, *a;
   if (root && whole && json_object_is_type(root, json_type_object) &&
       json_object_object_get_ex(root, "v", &v) && json_object_is_type(v, json_type_int) &&
       json_object_get_int64(v) == CURSOR_VERSION && json_object_object_get_ex(root, "f", &f) &&
       json_object_is_type(f, json_type_string) && json_object_object_get_ex(root, "a", &a) &&
       json_object_is_type(a, json_type_array)) {
      const char *fs = json_object_get_string(f);
      char *end = NULL;
      const size_t n = json_object_array_length(a);
      ok = json_object_get_string_len(f) == 16 && n <= EMAIL_CURSOR_ACCOUNTS;
      for (const char *p = fs; ok && *p; p++)
         ok = isxdigit((unsigned char)*p) && !isupper((unsigned char)*p);
      if (ok)
         out->filter = strtoull(fs, &end, 16);
      for (size_t i = 0; ok && i < n; i++) {
         ok = pos_from_json(json_object_array_get_idx(a, i), &out->pos[i]);
         for (size_t j = 0; ok && j < i; j++)
            ok = out->pos[j].account_id != out->pos[i].account_id;
      }
      if (ok)
         out->count = (int)n;
   }
   json_object_put(root);
   if (!ok)
      memset(out, 0, sizeof(*out));
   return ok;
}

/* =============================================================================
 * The merge
 * ============================================================================= */

static bool seen_has(const email_cursor_pos_t *p, const char *id) {
   for (int i = 0; i < p->seen_count; i++) {
      if (strcmp(p->seen[i], id) == 0)
         return true;
   }
   return false;
}

int email_cursor_gmail_skip(const email_cursor_pos_t *from) {
   const int n = from->emitted - from->seen_count;
   return n > 0 ? n : 0;
}

bool email_cursor_gmail_order(email_summary_t *rows, int count) {
   bool sorted = true;
   for (int i = 1; sorted && i < count; i++)
      sorted = rows[i].date <= rows[i - 1].date;
   if (sorted)
      return false;
   /* Insertion sort: stable, and a fetch is a page or two. */
   for (int i = 1; i < count; i++) {
      email_summary_t r = rows[i];
      int j = i;
      for (; j > 0 && rows[j - 1].date < r.date; j--)
         rows[j] = rows[j - 1];
      rows[j] = r;
   }
   return true;
}

uint32_t email_cursor_folder_hash(const char *folder) {
   /* FNV-1a, case folded (folder names compare without case). */
   uint32_t h = 2166136261u;
   for (const unsigned char *p = (const unsigned char *)(folder ? folder : ""); *p; p++) {
      h ^= (uint32_t)tolower(*p);
      h *= 16777619u;
   }
   return h ? h : 1;
}

bool email_cursor_imap_folder_ok(const email_cursor_pos_t *from, const char *folder) {
   if (!from->before_uid || !from->folder_hash)
      return true;
   return email_cursor_folder_hash(folder) == from->folder_hash;
}

int email_cursor_gmail_filter(email_summary_t *rows,
                              int count,
                              const email_cursor_pos_t *from,
                              int *skip_left) {
   if (from->next_date <= 0)
      return count; /* the first page: nothing emitted yet */
   int kept = 0;
   for (int i = 0; i < count; i++) {
      const email_summary_t *r = &rows[i];
      if (r->date > from->next_date)
         continue;
      if (r->date == from->next_date) {
         if (seen_has(from, r->message_id))
            continue;
         /* Emitted before, past what seen holds: the provider keeps a second's
          * order, so they're the first of the rest. */
         if (*skip_left > 0) {
            (*skip_left)--;
            continue;
         }
      }
      if (kept != i)
         rows[kept] = rows[i];
      kept++;
   }
   return kept;
}

/* Record @p id as emitted at @p pos's resume second, if there's room. */
static void seen_add(email_cursor_pos_t *pos, const char *id) {
   if (pos->seen_count >= EMAIL_CURSOR_SEEN_MAX || !gmail_id_ok(id) || !id[0] ||
       strlen(id) > EMAIL_CURSOR_ID_MAX || seen_has(pos, id))
      return;
   snprintf(pos->seen[pos->seen_count++], sizeof(pos->seen[0]), "%s", id);
}

/* A Gmail position at second @p d: what of it went out (carried from @p from
 * when it's the same second, then this page's). */
static void gmail_position(const email_merge_in_t *in,
                           int emitted,
                           time_t d,
                           email_cursor_pos_t *out) {
   const bool same = in->from.next_date == d;
   out->next_date = d;
   out->emitted = same ? in->from.emitted : 0;
   out->seen_count = 0;
   for (int i = 0; same && i < in->from.seen_count; i++)
      seen_add(out, in->from.seen[i]);
   for (int i = 0; i < emitted; i++) {
      if (in->rows[i].date == d) {
         if (out->emitted < EMAIL_CURSOR_EMITTED_MAX)
            out->emitted++;
         seen_add(out, in->rows[i].message_id);
      }
   }
}

static bool takes_part(const email_merge_in_t *in) {
   return in->err == EMAIL_ERR_NONE && in->row_count > 0;
}

/* Where @p in goes next; false when it's done. */
static bool next_position(const email_merge_in_t *in, int emitted, email_cursor_pos_t *out) {
   *out = in->from;
   out->account_id = in->account_id;
   out->is_imap = in->is_imap;
   if (in->err != EMAIL_ERR_NONE)
      return true; /* keeps its position for the next page */

   if (in->is_imap) {
      const uint32_t v = in->next_uidvalidity ? in->next_uidvalidity : in->from.uidvalidity;
      if (in->next_folder_hash)
         out->folder_hash = in->next_folder_hash;
      if (in->row_count == 0) {
         /* Nothing survived the fetch: the provider's own continuation. */
         if (!in->more || in->next_before_uid < 2)
            return false;
         out->before_uid = in->next_before_uid;
         out->uidvalidity = v;
         return true;
      }
      if (emitted == 0)
         return true; /* not reached this page */
      uint32_t lowest = UINT32_MAX;
      for (int i = 0; i < emitted; i++) {
         if (in->rows[i].uid < lowest)
            lowest = in->rows[i].uid;
      }
      if (emitted == in->row_count && !in->more)
         return false;
      if (lowest < 2)
         return false; /* nothing below it */
      out->before_uid = lowest;
      out->uidvalidity = v;
      return true;
   }

   /* Gmail */
   if (in->row_count == 0) {
      if (!in->more && !in->refused)
         return false;
      /* Pages came back with nothing to show but more past them: from the
       * newest (empty pages), keep the position for the next request; past a
       * resume second (every row was one already emitted, more of that second
       * than the fetch reaches), step past that second rather than fetch it
       * again. */
      if (in->from.next_date <= 1 || in->refused)
         return true; /* refused rows: ask again from here, don't step past them */
      gmail_position(in, 0, in->from.next_date - 1, out);
      return true;
   }
   if (emitted == 0)
      return true; /* not reached this page */
   if (emitted < in->row_count) {
      gmail_position(in, emitted, in->rows[emitted].date, out);
      return true;
   }
   if (!in->more)
      return false;
   /* All of the fetch went out: continue below the last row emitted. */
   gmail_position(in, emitted, in->rows[emitted - 1].date, out);
   return true;
}

bool email_merge_page(const email_merge_in_t *in,
                      int n,
                      int limit,
                      email_merge_pick_t *picks,
                      int *pick_count,
                      email_cursor_t *next) {
   *pick_count = 0;
   next->count = 0;
   if (n > EMAIL_CURSOR_ACCOUNTS)
      n = EMAIL_CURSOR_ACCOUNTS;
   int head[EMAIL_CURSOR_ACCOUNTS] = { 0 };

   while (*pick_count < limit) {
      /* An account still fetching whose fetched rows ran out: its next row
       * could be newer than anything left, so the page ends here. */
      bool short_one = false;
      for (int i = 0; i < n && !short_one; i++)
         short_one = takes_part(&in[i]) && head[i] >= in[i].row_count && in[i].more;
      if (short_one)
         break;

      int best = -1;
      for (int i = 0; i < n; i++) {
         if (!takes_part(&in[i]) || head[i] >= in[i].row_count)
            continue;
         if (best < 0 || in[i].rows[head[i]].date > in[best].rows[head[best]].date)
            best = i;
      }
      if (best < 0)
         break;
      picks[*pick_count].account = best;
      picks[*pick_count].row = head[best];
      (*pick_count)++;
      head[best]++;
   }

   for (int i = 0; i < n; i++) {
      email_cursor_pos_t pos;
      if (next_position(&in[i], head[i], &pos))
         next->pos[next->count++] = pos;
   }
   /* Failed accounts stay in: a next page retries them (paging is the client's). */
   return next->count > 0;
}
