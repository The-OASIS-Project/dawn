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
 * Actions waiting for the user's reply code (see core/tool_call_challenge.h).
 */

#include "core/tool_call_challenge.h"

#include <pthread.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/pending_slots.h" /* pending_slots_now: a clock that never steps back */

#define CHALLENGE_SLOTS 16
#define CHALLENGE_COUNTERS 128 /* channels whose limits are remembered (oldest give way) */
#define HOUR_SEC (60 * 60)
#define DAY_SEC (24 * HOUR_SEC)

typedef struct {
   bool active;
   bool sent;
   int64_t channel_id;
   int user_id;
   uint64_t turn_token;
   time_t expires_at;
   int wrong;
   char code[REPLY_CODE_LEN]; /* until it is texted; then only the digest */
   char digest[REPLY_CODE_DIGEST_HEX];
   char tool[TOOL_CALL_CHALLENGE_TOOL_MAX];
   char *args;
   char binding[REPLY_CODE_DIGEST_HEX];
   char description[TOOL_CALL_CHALLENGE_DESC_MAX];
} challenge_t;

/* When a channel was last sent codes: a ring of the last PER_DAY. */
typedef struct {
   int64_t channel_id;
   int count;
   int next;
   bool expired_unsent;        /* its last challenge ran out before its code was texted */
   tool_challenge_end_t ended; /* how its last texted code stopped working (once) */
   time_t ended_at;
   time_t sent_at[TOOL_CALL_CHALLENGE_PER_DAY];
} challenge_counter_t;

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static challenge_t s_challenges[CHALLENGE_SLOTS];
static challenge_counter_t s_counters[CHALLENGE_COUNTERS];

/* Caller holds s_mutex. */
static void clear_locked(challenge_t *c) {
   free(c->args);
   sodium_memzero(c, sizeof(*c));
}

static challenge_counter_t *counter_find_locked(int64_t channel_id);
static void end_locked(challenge_t *c, tool_challenge_end_t why, time_t now);

/* Caller holds s_mutex.  This channel's challenge; expired ones are cleared
 * (@p expired says whether this channel's was; one whose code was never texted
 * is remembered on its channel's counter, for take_unsent to report). */
static challenge_t *find_locked(int64_t channel_id, time_t now, bool *expired) {
   challenge_t *found = NULL;
   for (int i = 0; i < CHALLENGE_SLOTS; i++) {
      challenge_t *c = &s_challenges[i];
      if (!c->active) {
         continue;
      }
      if (now >= c->expires_at) {
         if (c->channel_id == channel_id && expired) {
            *expired = true;
         }
         if (!c->sent) {
            challenge_counter_t *k = counter_find_locked(c->channel_id);
            if (k) {
               k->expired_unsent = true;
               k->ended = TOOL_CHALLENGE_END_NONE; /* no code of it to answer */
            }
         }
         /* It ended when it ran out, not when this sweep found it. */
         end_locked(c, TOOL_CHALLENGE_END_EXPIRED, c->expires_at);
         continue;
      }
      if (c->channel_id == channel_id) {
         found = c;
      }
   }
   return found;
}

/* A counter's most recent code (0 when it has none): the latest time in its
 * ring, so a refunded entry (written a day back) never makes it look idle. */
static time_t newest_code(const challenge_counter_t *k) {
   time_t newest = 0;
   for (int i = 0; i < k->count; i++) {
      if (k->sent_at[i] > newest) {
         newest = k->sent_at[i];
      }
   }
   return newest;
}

/* Caller holds s_mutex.  The channel's own counter (it stays the channel's
 * with nothing counted: it also remembers how its last code ended), or NULL. */
static challenge_counter_t *counter_find_locked(int64_t channel_id) {
   for (int i = 0; channel_id > 0 && i < CHALLENGE_COUNTERS; i++) {
      if (s_counters[i].channel_id == channel_id) {
         return &s_counters[i];
      }
   }
   return NULL;
}

/* Caller holds s_mutex.  Clear @p c, remembering how a code the user was
 * texted stopped working, so a late reply with it gets DAWN's answer. */
static void end_locked(challenge_t *c, tool_challenge_end_t why, time_t now) {
   if (c->sent) {
      challenge_counter_t *k = counter_find_locked(c->channel_id);
      if (k) {
         k->ended = why;
         k->ended_at = now;
      }
   }
   clear_locked(c);
}

/* How readily a counter gives way to another channel: a free one first, then
 * one with nothing counted, then the one whose latest code is oldest. */
static bool gives_way_before(const challenge_counter_t *a, const challenge_counter_t *b) {
   if ((a->channel_id == 0) != (b->channel_id == 0)) {
      return a->channel_id == 0;
   }
   if ((a->count == 0) != (b->count == 0)) {
      return a->count == 0;
   }
   return newest_code(a) < newest_code(b);
}

/* Caller holds s_mutex.  The channel's counter: its own, else the one that
 * gives way first (it makes room). */
static challenge_counter_t *counter_locked(int64_t channel_id) {
   challenge_counter_t *own = counter_find_locked(channel_id);
   if (own) {
      return own;
   }
   challenge_counter_t *spare = &s_counters[0];
   for (int i = 1; i < CHALLENGE_COUNTERS; i++) {
      if (gives_way_before(&s_counters[i], spare)) {
         spare = &s_counters[i];
      }
   }
   memset(spare, 0, sizeof(*spare));
   spare->channel_id = channel_id;
   return spare;
}

/* Caller holds s_mutex.  Count one more code for the channel; false when it
 * was sent too many in the last hour or day. */
static bool count_locked(int64_t channel_id, time_t now) {
   challenge_counter_t *k = counter_locked(channel_id);
   int hour = 0, day = 0;
   for (int i = 0; i < k->count; i++) {
      const time_t at = k->sent_at[i];
      if (now - at < DAY_SEC) {
         day++;
         if (now - at < HOUR_SEC) {
            hour++;
         }
      }
   }
   if (hour >= TOOL_CALL_CHALLENGE_PER_HOUR || day >= TOOL_CALL_CHALLENGE_PER_DAY) {
      return false;
   }
   k->expired_unsent = false;
   k->sent_at[k->next] = now;
   k->next = (k->next + 1) % TOOL_CALL_CHALLENGE_PER_DAY;
   if (k->count < TOOL_CALL_CHALLENGE_PER_DAY) {
      k->count++;
   }
   return true;
}

/* Caller holds s_mutex.  Take back the channel's newest count: the code of
 * its waiting challenge (each create that counts replaces the one before, so
 * the live challenge's code is always the newest). */
static void refund_newest_locked(int64_t channel_id, time_t now) {
   challenge_counter_t *k = counter_find_locked(channel_id);
   if (!k || k->count == 0) {
      return;
   }
   const int last = (k->next + TOOL_CALL_CHALLENGE_PER_DAY - 1) % TOOL_CALL_CHALLENGE_PER_DAY;
   if (k->count < TOOL_CALL_CHALLENGE_PER_DAY) {
      k->next = last; /* the ring isn't full: its newest entry goes */
      k->count--;
   }
   /* Full: a time a day old never counts (the clock is seconds since boot, so
    * 0 isn't old in the first day). */
   k->sent_at[last] = now - DAY_SEC;
}

tool_challenge_rc_t tool_call_challenge_create(const tool_challenge_t *in) {
   if (!in || in->channel_id <= 0) {
      return TOOL_CHALLENGE_FULL; /* no channel to text a code to */
   }
   const time_t now = pending_slots_now();
   pthread_mutex_lock(&s_mutex);
   challenge_t *c = find_locked(in->channel_id, now, NULL);
   /* The same call asked again keeps its code, unless an earlier turn's code
    * is about to run out (then a fresh one, counted).  It now belongs to this
    * turn too, so the turn can't prepare something else after it. */
   if (c && in->binding && in->binding[0] && strcmp(c->binding, in->binding) == 0 &&
       (c->turn_token == in->turn_token || c->expires_at - now >= TOOL_CALL_CHALLENGE_MIN_SEC)) {
      c->turn_token = in->turn_token;
      pthread_mutex_unlock(&s_mutex);
      return TOOL_CHALLENGE_SAME;
   }
   if (c && c->turn_token == in->turn_token) {
      pthread_mutex_unlock(&s_mutex);
      return TOOL_CHALLENGE_BUSY;
   }
   for (int i = 0; i < CHALLENGE_SLOTS && !c; i++) {
      if (!s_challenges[i].active) {
         c = &s_challenges[i];
      }
   }
   if (!c) {
      pthread_mutex_unlock(&s_mutex);
      return TOOL_CHALLENGE_FULL;
   }
   char *copy = strdup(in->args ? in->args : "");
   if (!copy) {
      pthread_mutex_unlock(&s_mutex);
      return TOOL_CHALLENGE_FULL;
   }
   if (!count_locked(in->channel_id, now)) {
      pthread_mutex_unlock(&s_mutex);
      free(copy);
      return TOOL_CHALLENGE_LIMIT;
   }
   /* A newer request replaces the channel's older one: its code stops working. */
   if (c->active) {
      end_locked(c, TOOL_CHALLENGE_END_REPLACED, now);
   }
   int valid = in->valid_for_sec;
   if (valid <= 0 || valid > TOOL_CALL_CHALLENGE_TTL_SEC) {
      valid = TOOL_CALL_CHALLENGE_TTL_SEC;
   }
   c->active = true;
   c->channel_id = in->channel_id;
   c->user_id = in->user_id;
   c->turn_token = in->turn_token;
   c->expires_at = now + valid;
   reply_code_new(c->code);
   reply_code_digest(c->code, c->digest);
   snprintf(c->tool, sizeof(c->tool), "%s", in->tool ? in->tool : "");
   c->args = copy;
   snprintf(c->binding, sizeof(c->binding), "%s", in->binding ? in->binding : "");
   snprintf(c->description, sizeof(c->description), "%s", in->description ? in->description : "");
   pthread_mutex_unlock(&s_mutex);
   return TOOL_CHALLENGE_OK;
}

bool tool_call_challenge_live(int64_t channel_id) {
   pthread_mutex_lock(&s_mutex);
   const bool live = find_locked(channel_id, pending_slots_now(), NULL) != NULL;
   pthread_mutex_unlock(&s_mutex);
   return live;
}

tool_take_rc_t tool_call_challenge_take_unsent(int64_t channel_id,
                                               char code[REPLY_CODE_LEN],
                                               char *description,
                                               size_t description_len,
                                               int *seconds) {
   const time_t now = pending_slots_now();
   pthread_mutex_lock(&s_mutex);
   challenge_t *c = find_locked(channel_id, now, NULL);
   if (!c || c->sent) {
      challenge_counter_t *k = c ? NULL : counter_find_locked(channel_id);
      const bool expired = k && k->expired_unsent;
      if (k) {
         k->expired_unsent = false;
      }
      pthread_mutex_unlock(&s_mutex);
      return expired ? TOOL_TAKE_EXPIRED : TOOL_TAKE_NONE;
   }
   snprintf(code, REPLY_CODE_LEN, "%s", c->code);
   if (description && description_len) {
      snprintf(description, description_len, "%s", c->description);
   }
   if (seconds) {
      *seconds = (int)(c->expires_at - now);
   }
   sodium_memzero(c->code, sizeof(c->code));
   c->sent = true;
   pthread_mutex_unlock(&s_mutex);
   return TOOL_TAKE_OK;
}

tool_redeem_rc_t tool_call_challenge_redeem(int64_t channel_id,
                                            int user_id,
                                            const char *code,
                                            tool_redeemed_t *out,
                                            int *tries) {
   memset(out, 0, sizeof(*out));
   char got[REPLY_CODE_DIGEST_HEX];
   reply_code_digest(code ? code : "", got);
   pthread_mutex_lock(&s_mutex);
   bool expired = false;
   challenge_t *c = find_locked(channel_id, pending_slots_now(), &expired);
   if (!c || c->user_id != user_id) {
      pthread_mutex_unlock(&s_mutex);
      return expired ? TOOL_REDEEM_EXPIRED : TOOL_REDEEM_NONE;
   }
   if (!c->sent || !reply_code_digest_equal(c->digest, got)) {
      const int wrong = ++c->wrong;
      if (tries) {
         *tries = wrong;
      }
      if (wrong >= TOOL_CALL_CHALLENGE_TRIES) {
         end_locked(c, TOOL_CHALLENGE_END_VOIDED, pending_slots_now());
         pthread_mutex_unlock(&s_mutex);
         return TOOL_REDEEM_VOIDED;
      }
      pthread_mutex_unlock(&s_mutex);
      return TOOL_REDEEM_WRONG;
   }
   snprintf(out->tool, sizeof(out->tool), "%s", c->tool);
   snprintf(out->binding, sizeof(out->binding), "%s", c->binding);
   snprintf(out->description, sizeof(out->description), "%s", c->description);
   out->args = c->args;
   c->args = NULL;
   /* The user had the code: a used code doesn't count against the channel. */
   refund_newest_locked(channel_id, pending_slots_now());
   end_locked(c, TOOL_CHALLENGE_END_USED, pending_slots_now());
   pthread_mutex_unlock(&s_mutex);
   return TOOL_REDEEM_OK;
}

bool tool_call_challenge_live_in_turn(int64_t channel_id, uint64_t turn_token) {
   pthread_mutex_lock(&s_mutex);
   const challenge_t *c = find_locked(channel_id, pending_slots_now(), NULL);
   const bool in_turn = c && turn_token != 0 && c->turn_token == turn_token;
   pthread_mutex_unlock(&s_mutex);
   return in_turn;
}

bool tool_call_challenge_cancel_earlier(int64_t channel_id, uint64_t turn_token) {
   pthread_mutex_lock(&s_mutex);
   challenge_t *c = find_locked(channel_id, pending_slots_now(), NULL);
   const bool earlier = c && c->turn_token != turn_token;
   if (earlier) {
      end_locked(c, TOOL_CHALLENGE_END_REPLACED, pending_slots_now());
   }
   pthread_mutex_unlock(&s_mutex);
   return earlier;
}

void tool_call_challenge_cancel_unsent(int64_t channel_id) {
   const time_t now = pending_slots_now();
   pthread_mutex_lock(&s_mutex);
   challenge_t *c = find_locked(channel_id, now, NULL);
   if (c) {
      /* It never reached the user (as far as the send could tell: a text the
       * carrier took after a timeout is refunded too, and its code is dead):
       * it doesn't count against the channel. */
      refund_newest_locked(channel_id, now);
      challenge_counter_t *k = counter_find_locked(channel_id);
      if (k) {
         k->ended = TOOL_CHALLENGE_END_NONE; /* no code of it to answer */
      }
      clear_locked(c);
   }
   pthread_mutex_unlock(&s_mutex);
}

/* Caller holds s_mutex.  How the channel's last texted code ended, if within
 * the window; NONE otherwise. */
static tool_challenge_end_t ended_locked(int64_t channel_id, time_t now) {
   const challenge_counter_t *k = counter_find_locked(channel_id);
   if (!k || k->ended == TOOL_CHALLENGE_END_NONE ||
       now - k->ended_at > TOOL_CALL_CHALLENGE_ENDED_SEC) {
      return TOOL_CHALLENGE_END_NONE;
   }
   return k->ended;
}

bool tool_call_challenge_ended_recently(int64_t channel_id) {
   const time_t now = pending_slots_now();
   pthread_mutex_lock(&s_mutex);
   const bool recent = find_locked(channel_id, now, NULL) == NULL &&
                       ended_locked(channel_id, now) != TOOL_CHALLENGE_END_NONE;
   pthread_mutex_unlock(&s_mutex);
   return recent;
}

tool_challenge_end_t tool_call_challenge_take_ended(int64_t channel_id) {
   const time_t now = pending_slots_now();
   pthread_mutex_lock(&s_mutex);
   const tool_challenge_end_t why = ended_locked(channel_id, now);
   challenge_counter_t *k = counter_find_locked(channel_id);
   if (k) {
      k->ended = TOOL_CHALLENGE_END_NONE; /* said once */
   }
   pthread_mutex_unlock(&s_mutex);
   return why;
}

bool tool_call_challenge_cancel(int64_t channel_id) {
   const time_t now = pending_slots_now();
   pthread_mutex_lock(&s_mutex);
   challenge_t *c = find_locked(channel_id, now, NULL);
   if (c) {
      end_locked(c, TOOL_CHALLENGE_END_CANCELLED, now);
   }
   pthread_mutex_unlock(&s_mutex);
   return c != NULL;
}

void tool_call_challenge_clear_all(void) {
   pthread_mutex_lock(&s_mutex);
   for (int i = 0; i < CHALLENGE_SLOTS; i++) {
      if (s_challenges[i].active) {
         clear_locked(&s_challenges[i]);
      }
   }
   memset(s_counters, 0, sizeof(s_counters));
   pthread_mutex_unlock(&s_mutex);
}
