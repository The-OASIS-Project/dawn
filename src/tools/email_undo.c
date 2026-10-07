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
 * Undo tokens for trash and archive (email_undo.h): a small store under one
 * mutex, holding nothing across a call into anything else.  Every refusal
 * looks the same, so a token can't be probed for whose it is.
 */

#include "tools/email_undo.h"

#include <pthread.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"

typedef struct {
   char token[EMAIL_UNDO_TOKEN_LEN + 1];
   int user_id;
   time_t created;
   time_t claimed; /* when it went in flight */
   bool in_flight;
   email_undo_rec_t rec;
} undo_entry_t;

/* Entries are allocated as they're needed: a full store is ~600 KB. */
static undo_entry_t *s_entries[EMAIL_UNDO_GLOBAL];
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static time_t s_clock;

/* Seconds on the monotonic clock: a wall-clock step can't stretch or cut a
 * token's time. */
static time_t now_s(void) {
   if (s_clock)
      return s_clock;
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec;
}

/* An entry in flight is kept while its undo runs, but not forever: a caller
 * that never finishes or releases it loses it after twice the window. */
static bool expired(const undo_entry_t *e, time_t now) {
   if (e->in_flight)
      return now - e->claimed >= 2 * EMAIL_UNDO_TTL_SEC;
   return now - e->created >= EMAIL_UNDO_TTL_SEC;
}

static void drop(int i) {
   sodium_memzero(s_entries[i], sizeof(*s_entries[i]));
   free(s_entries[i]);
   s_entries[i] = NULL;
}

static bool token_shape_ok(const char *token) {
   if (!token || strnlen(token, EMAIL_UNDO_TOKEN_LEN + 1) != EMAIL_UNDO_TOKEN_LEN)
      return false;
   for (int i = 0; i < EMAIL_UNDO_TOKEN_LEN; i++) {
      const char c = token[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
         return false;
   }
   return true;
}

/* With s_mutex held: @p user_id's entry for @p token, or -1. */
static int find_locked(int user_id, const char *token) {
   for (int i = 0; i < EMAIL_UNDO_GLOBAL; i++) {
      undo_entry_t *e = s_entries[i];
      if (e && sodium_memcmp(e->token, token, EMAIL_UNDO_TOKEN_LEN) == 0)
         return e->user_id == user_id ? i : -1;
   }
   return -1;
}

bool email_undo_put(int user_id,
                    const email_undo_rec_t *rec,
                    char token[EMAIL_UNDO_TOKEN_LEN + 1]) {
   token[0] = '\0';
   if (!rec)
      return false;
   undo_entry_t *e = calloc(1, sizeof(*e));
   if (!e)
      return false;
   unsigned char raw[EMAIL_UNDO_TOKEN_LEN / 2];
   randombytes_buf(raw, sizeof(raw));
   sodium_bin2hex(e->token, sizeof(e->token), raw, sizeof(raw));
   sodium_memzero(raw, sizeof(raw));
   e->user_id = user_id;
   e->rec = *rec;

   pthread_mutex_lock(&s_mutex);
   const time_t now = now_s();
   e->created = now;
   int free_slot = -1, mine = 0, oldest = -1;
   for (int i = 0; i < EMAIL_UNDO_GLOBAL; i++) {
      if (s_entries[i] && expired(s_entries[i], now))
         drop(i);
      if (!s_entries[i]) {
         if (free_slot < 0)
            free_slot = i;
         continue;
      }
      if (s_entries[i]->user_id != user_id)
         continue;
      mine++;
      if (!s_entries[i]->in_flight &&
          (oldest < 0 || s_entries[i]->created < s_entries[oldest]->created))
         oldest = i;
   }
   if (mine >= EMAIL_UNDO_PER_USER) {
      /* Only this user's own oldest makes room; none free means no token. */
      if (oldest < 0) {
         free_slot = -1;
      } else {
         drop(oldest);
         free_slot = oldest;
      }
   }
   if (free_slot >= 0) {
      s_entries[free_slot] = e;
      memcpy(token, e->token, EMAIL_UNDO_TOKEN_LEN + 1);
   }
   pthread_mutex_unlock(&s_mutex);
   if (free_slot < 0) {
      OLOG_WARNING("email_undo: store full; this move can't be undone");
      sodium_memzero(e, sizeof(*e));
      free(e);
      return false;
   }
   return true;
}

bool email_undo_claim(int user_id, int64_t account_id, const char *token, email_undo_rec_t *out) {
   if (!token_shape_ok(token) || !out)
      return false;
   bool ok = false;
   pthread_mutex_lock(&s_mutex);
   const int i = find_locked(user_id, token);
   if (i >= 0) {
      undo_entry_t *e = s_entries[i];
      if (expired(e, now_s())) {
         drop(i);
      } else if (!e->in_flight && e->rec.account_id == account_id) {
         e->in_flight = true;
         e->claimed = now_s();
         *out = e->rec;
         ok = true;
      }
   }
   pthread_mutex_unlock(&s_mutex);
   return ok;
}

void email_undo_finish(int user_id, const char *token) {
   if (!token_shape_ok(token))
      return;
   pthread_mutex_lock(&s_mutex);
   const int i = find_locked(user_id, token);
   if (i >= 0)
      drop(i);
   pthread_mutex_unlock(&s_mutex);
}

void email_undo_release(int user_id, const char *token) {
   if (!token_shape_ok(token))
      return;
   pthread_mutex_lock(&s_mutex);
   const int i = find_locked(user_id, token);
   if (i >= 0)
      s_entries[i]->in_flight = false;
   pthread_mutex_unlock(&s_mutex);
}

void email_undo_set_clock(time_t now) {
   pthread_mutex_lock(&s_mutex);
   s_clock = now;
   pthread_mutex_unlock(&s_mutex);
}

void email_undo_reset(void) {
   pthread_mutex_lock(&s_mutex);
   for (int i = 0; i < EMAIL_UNDO_GLOBAL; i++) {
      if (s_entries[i])
         drop(i);
   }
   pthread_mutex_unlock(&s_mutex);
}
