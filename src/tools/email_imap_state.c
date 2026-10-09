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
 * A message's read state on IMAP: parsing FETCH (FLAGS) and STATUS (UNSEEN)
 * replies.  Pure, so it can be tested against server transcripts.
 */

#include "tools/email_imap_state.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>


/* The next line of @p *cursor (without its CR/LF), or NULL at the end. */
static const char *next_line(const char **cursor, size_t *len) {
   const char *p = *cursor;
   if (!p || !*p)
      return NULL;
   const char *nl = strchr(p, '\n');
   const char *end = nl ? nl : p + strlen(p);
   *cursor = nl ? nl + 1 : end;
   size_t n = (size_t)(end - p);
   if (n > 0 && p[n - 1] == '\r')
      n--;
   *len = n;
   return p;
}

/* Whether @p p (inside [line, end)) starts the word @p word followed by a
 * space, after a space or '('. */
static bool word_at(const char *line, const char *p, const char *end, const char *word) {
   const size_t w = strlen(word);
   return p > line && (p[-1] == ' ' || p[-1] == '(') && (size_t)(end - p) > w &&
          strncasecmp(p, word, w) == 0 && p[w] == ' ';
}

/* Whether the FLAGS group [open, close] holds @p flag as a whole word, read
 * in place (a group with many keywords isn't cut short). */
static bool group_has_flag(const char *open, const char *close, const char *flag) {
   const size_t flen = strlen(flag);
   for (const char *p = open + 1; p + flen <= close; p++) {
      const bool left = p[-1] == '(' || p[-1] == ' ' || p[-1] == '\t';
      const bool right = p + flen == close || p[flen] == ' ' || p[flen] == '\t';
      if (left && right && strncasecmp(p, flag, flen) == 0)
         return true;
   }
   return false;
}

int email_imap_fetch_seen(const char *response, uint32_t uid) {
   if (!response || uid == 0)
      return EMAIL_IMAP_UID_ABSENT;
   const char *cursor = response;
   size_t len;
   for (const char *line; (line = next_line(&cursor, &len)) != NULL;) {
      if (len < 2 || line[0] != '*' || line[1] != ' ')
         continue;
      const char *end = line + len;
      bool uid_match = false;
      const char *flags = NULL;
      for (const char *p = line + 2; p < end; p++) {
         if (word_at(line, p, end, "UID") && p + 4 < end && isdigit((unsigned char)p[4])) {
            char *e = NULL;
            const unsigned long v = strtoul(p + 4, &e, 10);
            if (e != p + 4 && v == uid)
               uid_match = true;
         } else if (!flags && word_at(line, p, end, "FLAGS") && p + 6 < end && p[6] == '(') {
            flags = p + 6;
         }
      }
      if (!uid_match || !flags)
         continue;
      const char *close = memchr(flags, ')', (size_t)(end - flags));
      if (!close)
         continue;
      return group_has_flag(flags, close, "\\Seen") ? EMAIL_IMAP_UID_SEEN : EMAIL_IMAP_UID_UNSEEN;
   }
   return EMAIL_IMAP_UID_ABSENT;
}

bool email_imap_status_unseen(const char *response, int *unseen) {
   if (!response || !unseen)
      return false;
   const char *cursor = response;
   size_t len;
   for (const char *line; (line = next_line(&cursor, &len)) != NULL;) {
      if (len < 9 || strncasecmp(line, "* STATUS ", 9) != 0)
         continue;
      const char *end = line + len;
      const char *p = line + 9;
      /* The mailbox (quoted or an atom) must be INBOX: a server may also send
       * STATUS for other mailboxes unasked. */
      const char *name = p;
      size_t name_len;
      if (p < end && *p == '"') {
         name = ++p;
         for (; p < end && *p != '"'; p++) {
            if (*p == '\\' && p + 1 < end)
               p++;
         }
         if (p >= end)
            continue; /* the name never closes: not a STATUS line to trust */
         name_len = (size_t)(p - name);
         p++;
      } else {
         while (p < end && *p != ' ')
            p++;
         name_len = (size_t)(p - name);
      }
      if (name_len != 5 || strncasecmp(name, "INBOX", 5) != 0)
         continue;
      while (p < end && *p == ' ')
         p++;
      if (p >= end || *p != '(')
         continue;
      for (; p < end; p++) {
         if (word_at(line, p, end, "UNSEEN") && p + 7 < end && isdigit((unsigned char)p[7])) {
            char *e = NULL;
            const long v = strtol(p + 7, &e, 10);
            if (e != p + 7 && v >= 0 && v <= 0x7fffffff) {
               *unseen = (int)v;
               return true;
            }
         }
      }
   }
   return false;
}

size_t email_imap_uid_set(const uint32_t *uids, int n, char *out, size_t out_size) {
   if (!uids || n <= 0 || !out || out_size == 0)
      return 0;
   size_t pos = 0;
   for (int i = 0; i < n; i++) {
      if (uids[i] == 0)
         return 0;
      const int w = snprintf(out + pos, out_size - pos, "%s%u", i > 0 ? "," : "", uids[i]);
      if (w < 0 || (size_t)w >= out_size - pos) {
         out[0] = '\0';
         return 0;
      }
      pos += (size_t)w;
   }
   return pos;
}

bool email_imap_select_failed(bool login_denied, int logins_before, int logins_after) {
   /* libcurl reports a refused SELECT with the login code; with no LOGIN sent
    * during that perform, the session was already in, so it was the folder. */
   return login_denied && logins_after == logins_before;
}
