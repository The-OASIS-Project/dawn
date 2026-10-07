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
 * An IMAP account's capabilities and special folders (email_imap_roles.h).
 */

#define _GNU_SOURCE /* strcasestr */
#include "tools/email_imap_roles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Names a server that doesn't mark its folders uses, best first. */
static const char *const k_trash_names[] = { "Trash",   "Deleted Items", "Deleted Messages",
                                             "Deleted", "Bin",           NULL };
static const char *const k_archive_names[] = { "Archive", "Archives", "Archived", NULL };

/* The next line of *cursor, without its line break (length in *len); NULL at
 * the end. */
static const char *next_line(const char **cursor, size_t *len) {
   const char *line = *cursor;
   if (!line || !*line)
      return NULL;
   const char *end = strpbrk(line, "\r\n");
   *len = end ? (size_t)(end - line) : strlen(line);
   *cursor = end ? end + strspn(end, "\r\n") : line + *len;
   return line;
}

/* Whether @p line has the whitespace-separated word @p word (case-insensitive)
 * after its CAPABILITY keyword. */
static bool capability_has(const char *line, size_t len, const char *word) {
   const size_t wlen = strlen(word);
   size_t i = 0;
   while (i < len) {
      while (i < len && (line[i] == ' ' || line[i] == '[' || line[i] == ']'))
         i++;
      size_t start = i;
      while (i < len && line[i] != ' ' && line[i] != ']')
         i++;
      if (i - start == wlen && strncasecmp(line + start, word, wlen) == 0)
         return true;
   }
   return false;
}

void email_imap_roles_capability(const char *response, email_imap_roles_t *out) {
   if (!out)
      return;
   out->move = out->uidplus = out->special_use = out->namespace_ = out->gmail = false;
   const char *cursor = response;
   size_t len;
   for (const char *line; (line = next_line(&cursor, &len)) != NULL;) {
      /* "* CAPABILITY ..." or a response code "[CAPABILITY ...]" */
      const char *cap = NULL;
      for (size_t i = 0; i + 10 <= len; i++) {
         if (strncasecmp(line + i, "CAPABILITY", 10) == 0) {
            cap = line + i + 10;
            break;
         }
      }
      if (cap) {
         const size_t rest = len - (size_t)(cap - line);
         out->move = out->move || capability_has(cap, rest, "MOVE");
         out->uidplus = out->uidplus || capability_has(cap, rest, "UIDPLUS");
         out->special_use = out->special_use || capability_has(cap, rest, "SPECIAL-USE");
         out->namespace_ = out->namespace_ || capability_has(cap, rest, "NAMESPACE");
         out->gmail = out->gmail || capability_has(cap, rest, "X-GM-EXT-1");
      }
   }
}

/* Reads the quoted string at *p (before @p end) into @p out (NULL = skip it);
 * false when it isn't one, isn't closed, or doesn't fit. */
static bool read_quoted(const char **p, const char *end, char *out, size_t size) {
   const char *q = *p;
   if (q >= end || *q != '"')
      return false;
   size_t o = 0;
   for (q++; q < end && *q != '"'; q++) {
      if (*q == '\\' && q + 1 < end)
         q++;
      if (out) {
         if (o + 1 >= size)
            return false;
         out[o++] = *q;
      }
   }
   if (q >= end)
      return false;
   if (out)
      out[o] = '\0';
   *p = q + 1;
   return true;
}

static const char *skip_spaces(const char *p, const char *end) {
   while (p < end && *p == ' ')
      p++;
   return p;
}

/* One namespace group: NIL, or "((prefix delim [extensions]) ...)".  The first
 * namespace of group 0 is the personal one; every prefix of groups 1 and 2 is
 * another user's or shared.  False on a malformed group. */
static bool parse_ns_group(const char **pp, const char *end, int group, email_imap_ns_t *out) {
   const char *p = skip_spaces(*pp, end);
   if (end - p >= 3 && strncasecmp(p, "NIL", 3) == 0) {
      *pp = p + 3;
      return true;
   }
   if (p >= end || *p != '(')
      return false;
   p++;
   for (bool first = true;; first = false) {
      p = skip_spaces(p, end);
      if (p < end && *p == ')') {
         *pp = p + 1;
         return true;
      }
      if (p >= end || *p != '(')
         return false;
      p = skip_spaces(p + 1, end);
      char prefix[EMAIL_IMAP_NS_PREFIX_MAX];
      if (!read_quoted(&p, end, prefix, sizeof(prefix)))
         return false;
      p = skip_spaces(p, end);
      char delim[4] = "";
      if (end - p >= 3 && strncasecmp(p, "NIL", 3) == 0) {
         p += 3;
      } else if (!read_quoted(&p, end, delim, sizeof(delim)) || strlen(delim) > 1) {
         return false; /* "" is taken as NIL: some servers send it */
      }
      /* Extensions, to the namespace's closing paren. */
      for (int depth = 1; depth > 0;) {
         if (p >= end)
            return false;
         if (*p == '"') {
            if (!read_quoted(&p, end, NULL, 0))
               return false;
            continue;
         }
         depth += (*p == '(') - (*p == ')');
         p++;
      }
      if (group == 0 && first) {
         /* "INBOX" with delimiter "." means folders named "INBOX.x": keep the
          * prefix as the folders start, delimiter included. */
         const size_t plen = strlen(prefix);
         if (plen && delim[0] && prefix[plen - 1] != delim[0] && plen + 1 < sizeof(prefix)) {
            prefix[plen] = delim[0];
            prefix[plen + 1] = '\0';
         }
         snprintf(out->personal, sizeof(out->personal), "%s", prefix);
         out->delim = delim[0];
      } else if (group > 0 && prefix[0] && out->n_other < EMAIL_IMAP_NS_OTHER_MAX) {
         snprintf(out->other[out->n_other++], sizeof(out->other[0]), "%s", prefix);
      }
   }
}

void email_imap_namespace_parse(const char *response, email_imap_ns_t *out) {
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   const char *cursor = response;
   size_t len;
   for (const char *line; (line = next_line(&cursor, &len)) != NULL;) {
      if (len < 12 || strncasecmp(line, "* NAMESPACE ", 12) != 0)
         continue;
      const char *p = line + 12;
      const char *end = line + len;
      email_imap_ns_t ns;
      memset(&ns, 0, sizeof(ns));
      int g = 0;
      while (g < 3 && parse_ns_group(&p, end, g, &ns))
         g++;
      if (g == 3)
         *out = ns;
      return;
   }
}

/* One LIST line's attributes, delimiter and name; false when it isn't one
 * (or its name is a literal, which this reader doesn't follow, or its
 * attributes don't fit). */
static bool parse_list_line(const char *line,
                            size_t len,
                            char *attrs,
                            size_t attrs_size,
                            char *delim,
                            char *name,
                            size_t name_size) {
   if (len < 8 || strncasecmp(line, "* LIST ", 7) != 0)
      return false;
   const char *p = line + 7;
   const char *end = line + len;
   if (*p != '(')
      return false;
   const char *close = memchr(p, ')', (size_t)(end - p));
   if (!close)
      return false;
   const size_t alen = (size_t)(close - (p + 1));
   if (alen >= attrs_size)
      return false;
   memcpy(attrs, p + 1, alen);
   attrs[alen] = '\0';
   p = skip_spaces(close + 1, end);
   /* The hierarchy delimiter: one quoted character, or NIL. */
   char d[4] = "";
   if (end - p >= 3 && strncasecmp(p, "NIL", 3) == 0) {
      p += 3;
   } else if (!read_quoted(&p, end, d, sizeof(d)) || strlen(d) != 1) {
      return false;
   }
   *delim = d[0];
   p = skip_spaces(p, end);
   if (p >= end || *p == '{')
      return false;
   if (*p == '"')
      return read_quoted(&p, end, name, name_size) && name[0];
   const char *sp = memchr(p, ' ', (size_t)(end - p));
   const size_t nlen = sp ? (size_t)(sp - p) : (size_t)(end - p);
   if (nlen + 1 > name_size)
      return false;
   memcpy(name, p, nlen);
   name[nlen] = '\0';
   return nlen > 0;
}

/* Whether @p attrs (a LIST attribute list) holds @p flag ("\Trash"). */
static bool has_attr(const char *attrs, const char *flag) {
   const size_t flen = strlen(flag);
   for (const char *p = attrs; (p = strcasestr(p, flag)) != NULL; p += flen) {
      const char after = p[flen];
      if (after == '\0' || after == ' ')
         return true;
   }
   return false;
}

/* Whether @p name is under another user's or a shared namespace. */
static bool not_users(const char *name, const email_imap_ns_t *ns) {
   for (int i = 0; ns && i < ns->n_other; i++) {
      if (strncmp(name, ns->other[i], strlen(ns->other[i])) == 0)
         return true;
   }
   return false;
}

/* The table index @p name matches, or -1: the whole name, the name right under
 * the personal prefix, or right under INBOX. */
static int name_rank(const char *name,
                     char delim,
                     const email_imap_ns_t *ns,
                     const char *const *table) {
   const char *rest = NULL;
   const size_t plen = ns ? strlen(ns->personal) : 0;
   if (plen && strncmp(name, ns->personal, plen) == 0) {
      rest = name + plen;
   } else if (delim && strncasecmp(name, "INBOX", 5) == 0 && name[5] == delim) {
      rest = name + 6;
   }
   for (int k = 0; table[k]; k++) {
      if (strcasecmp(name, table[k]) == 0 || (rest && strcasecmp(rest, table[k]) == 0))
         return k;
   }
   return -1;
}

void email_imap_roles_from_list(const char *response,
                                const email_imap_ns_t *ns,
                                email_imap_roles_t *out) {
   if (!out)
      return;
   out->trash[0] = '\0';
   out->archive[0] = '\0';
   out->all[0] = '\0';
   bool trash_marked = false;
   bool archive_marked = false;
   int trash_rank = -1;
   int archive_rank = -1;
   char trash_by_name[EMAIL_IMAP_ROLE_FOLDER_MAX] = "";
   char archive_by_name[EMAIL_IMAP_ROLE_FOLDER_MAX] = "";

   const char *cursor = response;
   size_t len;
   for (const char *line; (line = next_line(&cursor, &len)) != NULL;) {
      char attrs[1024];
      char delim;
      char name[EMAIL_IMAP_ROLE_FOLDER_MAX];
      if (!parse_list_line(line, len, attrs, sizeof(attrs), &delim, name, sizeof(name)) ||
          has_attr(attrs, "\\Noselect") || has_attr(attrs, "\\NonExistent") || not_users(name, ns))
         continue;
      if (!trash_marked && has_attr(attrs, "\\Trash")) {
         snprintf(out->trash, sizeof(out->trash), "%s", name);
         trash_marked = true;
      }
      if (!archive_marked && has_attr(attrs, "\\Archive")) {
         snprintf(out->archive, sizeof(out->archive), "%s", name);
         archive_marked = true;
      }
      if (!out->all[0] && has_attr(attrs, "\\All")) {
         snprintf(out->all, sizeof(out->all), "%s", name);
      }
      const int tr = name_rank(name, delim, ns, k_trash_names);
      if (tr >= 0 && (trash_rank < 0 || tr < trash_rank)) {
         trash_rank = tr;
         snprintf(trash_by_name, sizeof(trash_by_name), "%s", name);
      }
      const int ar = name_rank(name, delim, ns, k_archive_names);
      if (ar >= 0 && (archive_rank < 0 || ar < archive_rank)) {
         archive_rank = ar;
         snprintf(archive_by_name, sizeof(archive_by_name), "%s", name);
      }
   }
   /* A marked folder wins; then Gmail's All Mail for archive; then a name. */
   if (!trash_marked) {
      snprintf(out->trash, sizeof(out->trash), "%s", trash_by_name);
   }
   if (!archive_marked) {
      /* Archive into \All only on Gmail, where it means "out of the inbox";
       * elsewhere a copy there would be a second copy. */
      snprintf(out->archive, sizeof(out->archive), "%s",
               out->gmail && out->all[0] ? out->all : archive_by_name);
   }
}

char email_imap_inbox_child_delim(const char *response) {
   const char *cursor = response;
   size_t len;
   for (const char *line; (line = next_line(&cursor, &len)) != NULL;) {
      char attrs[1024];
      char delim;
      char name[EMAIL_IMAP_ROLE_FOLDER_MAX];
      if (parse_list_line(line, len, attrs, sizeof(attrs), &delim, name, sizeof(name)) &&
          strcasecmp(name, "INBOX") == 0)
         return has_attr(attrs, "\\HasNoChildren") ? '\0' : delim;
   }
   return '\0';
}

bool email_imap_fetch_has_uid(const char *response, uint32_t uid) {
   const char *cursor = response;
   size_t len;
   for (const char *line; (line = next_line(&cursor, &len)) != NULL;) {
      if (len < 2 || line[0] != '*' || line[1] != ' ')
         continue;
      /* "UID <n>" as a word, outside quoted strings (a Gmail label can hold it) */
      const char *end = line + len;
      for (const char *p = line + 2; p < end;) {
         if (*p == '"') {
            if (!read_quoted(&p, end, NULL, 0))
               break;
            continue;
         }
         if ((p[-1] == ' ' || p[-1] == '(') && end - p > 4 && strncasecmp(p, "UID ", 4) == 0) {
            char *e = NULL;
            const unsigned long v = strtoul(p + 4, &e, 10);
            if (e != p + 4 && v == uid)
               return true;
         }
         p++;
      }
   }
   return false;
}

bool email_imap_quote_folder(const char *folder, char *out, size_t out_size) {
   if (!folder || !folder[0] || !out || out_size < 3)
      return false;
   size_t o = 0;
   out[o++] = '"';
   for (const char *p = folder; *p; p++) {
      if (*p == '\r' || *p == '\n')
         return false;
      const bool esc = (*p == '"' || *p == '\\');
      if (o + (esc ? 2 : 1) + 2 > out_size)
         return false;
      if (esc)
         out[o++] = '\\';
      out[o++] = *p;
   }
   out[o++] = '"';
   out[o] = '\0';
   return true;
}
