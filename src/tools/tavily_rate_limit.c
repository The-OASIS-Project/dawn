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
 * Tavily per-user rate limiter — fixed-window counters for minute + hour.
 */

#include "tools/tavily_rate_limit.h"

#include <pthread.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "logging.h"

/* Live cap getters — read g_config once into a local (TOCTOU-safe; WebUI
 * settings POST can mutate g_config mid-call). 0 in config means "use
 * compile-time default", matching the rest of the Tavily config surface. */
static int rl_per_minute(void) {
   int v = g_config.url_fetcher.tavily.rate_limit_per_minute;
   return v > 0 ? v : TAVILY_RL_PER_MINUTE_DEFAULT;
}
static int rl_per_hour(void) {
   int v = g_config.url_fetcher.tavily.rate_limit_per_hour;
   return v > 0 ? v : TAVILY_RL_PER_HOUR_DEFAULT;
}
static int rl_per_day(void) {
   int v = g_config.url_fetcher.tavily.rate_limit_per_day;
   return v > 0 ? v : TAVILY_RL_PER_DAY_DEFAULT;
}

/* TAVILY_RL_MAX_USERS is declared in the header so tests and operators can
 * inspect it. Definition lives there. */

typedef struct {
   int user_id; /* 0 = slot unused */
   time_t minute_window_start;
   int minute_count;
   time_t hour_window_start;
   int hour_count;
} bucket_t;

static bucket_t s_buckets[TAVILY_RL_MAX_USERS];
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Global daily counter — shared across all users. Bounds total monthly
 * quota burn. Rolling 24-hour window to match the rolling minute / hour
 * pattern of the per-user buckets. */
static time_t s_global_day_window_start = 0;
static int s_global_day_count = 0;

/* Sentinel id for user_id <= 0 — same bucket for all anonymous callers. */
#define ANON_USER_ID (-1)

static int normalize_user_id(int user_id) {
   return user_id > 0 ? user_id : ANON_USER_ID;
}

/* Caller holds s_mutex. Read-only: returns slot index for @p user_id, or
 * -1 if no slot exists yet. Never allocates — safe to call from diagnostic
 * paths without consuming a bucket entry. */
static int find_slot(int user_id) {
   for (int i = 0; i < TAVILY_RL_MAX_USERS; i++) {
      if (s_buckets[i].user_id == user_id) {
         return i;
      }
   }
   return -1;
}

/* Caller holds s_mutex. Returns slot index, or -1 if full. Allocates a new
 * slot for the user if none exists. Used by the charging path only. */
static int find_or_alloc_slot(int user_id) {
   int free_slot = -1;
   for (int i = 0; i < TAVILY_RL_MAX_USERS; i++) {
      if (s_buckets[i].user_id == user_id) {
         return i;
      }
      if (free_slot < 0 && s_buckets[i].user_id == 0) {
         free_slot = i;
      }
   }
   if (free_slot >= 0) {
      memset(&s_buckets[free_slot], 0, sizeof(bucket_t));
      s_buckets[free_slot].user_id = user_id;
      return free_slot;
   }
   return -1;
}

/* Caller holds s_mutex. Roll the per-window counters forward if expired.
 * NTP backward-jump handling: `now < window_start` means wall-clock slewed
 * backward; treat that as a fresh window so the limiter doesn't deadlock
 * with `now - window_start` staying negative forever. CLOCK_MONOTONIC
 * migration is the proper long-term fix, deferred to the post-ship TODO. */
static void roll_windows(bucket_t *b, time_t now) {
   if (now < b->minute_window_start || now - b->minute_window_start >= 60) {
      b->minute_window_start = now;
      b->minute_count = 0;
   }
   if (now < b->hour_window_start || now - b->hour_window_start >= 3600) {
      b->hour_window_start = now;
      b->hour_count = 0;
   }
}

/* Caller holds s_mutex. Roll the global 24-hour window forward if expired.
 * Same NTP backward-jump handling as roll_windows. */
static void roll_global_day(time_t now) {
   if (now < s_global_day_window_start || now - s_global_day_window_start >= 86400) {
      s_global_day_window_start = now;
      s_global_day_count = 0;
   }
}

bool tavily_rate_limit_check(int user_id) {
   int uid = normalize_user_id(user_id);
   time_t now = time(NULL);
   /* Snapshot live caps once per call — TOCTOU-safe under the mutex. */
   const int per_minute = rl_per_minute();
   const int per_hour = rl_per_hour();
   const int per_day = rl_per_day();

   pthread_mutex_lock(&s_mutex);

   /* Global daily ceiling — checked first so an exhausted day fails fast
    * without poking the per-user bucket table. Denied calls do NOT advance
    * the counter (consistent with per-user semantics). */
   roll_global_day(now);
   if (s_global_day_count >= per_day) {
      pthread_mutex_unlock(&s_mutex);
      OLOG_WARNING("tavily_rate_limit: GLOBAL daily ceiling hit (%d/%d) — monthly-quota backstop",
                   s_global_day_count, per_day);
      return false;
   }

   int idx = find_or_alloc_slot(uid);
   if (idx < 0) {
      /* All slots full — fail open rather than block legitimate work.
       * Personal deployments shouldn't hit this; logged once for diagnostics.
       * Charge the global counter so a bucket-table overflow can't bypass
       * the daily ceiling. */
      s_global_day_count++;
      pthread_mutex_unlock(&s_mutex);
      OLOG_WARNING("tavily_rate_limit: bucket table full (max=%d), allowing call",
                   TAVILY_RL_MAX_USERS);
      return true;
   }

   bucket_t *b = &s_buckets[idx];
   roll_windows(b, now);

   if (b->minute_count >= per_minute) {
      pthread_mutex_unlock(&s_mutex);
      OLOG_WARNING("tavily_rate_limit: user=%d hit minute cap (%d/%d)", user_id, b->minute_count,
                   per_minute);
      return false;
   }
   if (b->hour_count >= per_hour) {
      pthread_mutex_unlock(&s_mutex);
      OLOG_WARNING("tavily_rate_limit: user=%d hit hour cap (%d/%d)", user_id, b->hour_count,
                   per_hour);
      return false;
   }

   b->minute_count++;
   b->hour_count++;
   s_global_day_count++;
   pthread_mutex_unlock(&s_mutex);
   return true;
}

void tavily_rate_limit_reset(void) {
   pthread_mutex_lock(&s_mutex);
   memset(s_buckets, 0, sizeof(s_buckets));
   s_global_day_window_start = 0;
   s_global_day_count = 0;
   pthread_mutex_unlock(&s_mutex);
}

void tavily_rate_limit_remaining(int user_id, int *minute_remaining, int *hour_remaining) {
   int uid = normalize_user_id(user_id);
   time_t now = time(NULL);
   const int per_minute = rl_per_minute();
   const int per_hour = rl_per_hour();

   pthread_mutex_lock(&s_mutex);
   /* Read-only lookup — diagnostic queries must not consume a slot from the
    * fixed bucket table or an unauthenticated admin endpoint could exhaust
    * it. Users with no bucket yet report the full budget. */
   int idx = find_slot(uid);
   if (idx < 0) {
      pthread_mutex_unlock(&s_mutex);
      if (minute_remaining) {
         *minute_remaining = per_minute;
      }
      if (hour_remaining) {
         *hour_remaining = per_hour;
      }
      return;
   }

   bucket_t *b = &s_buckets[idx];
   roll_windows(b, now);
   int mrem = per_minute - b->minute_count;
   int hrem = per_hour - b->hour_count;
   pthread_mutex_unlock(&s_mutex);

   if (mrem < 0) {
      mrem = 0;
   }
   if (hrem < 0) {
      hrem = 0;
   }
   if (minute_remaining) {
      *minute_remaining = mrem;
   }
   if (hour_remaining) {
      *hour_remaining = hrem;
   }
}

int tavily_rate_limit_resolve_user_id(void) {
   session_t *sess = session_get_command_context();
   if (!sess) {
      sess = session_get_local();
   }
   return sess ? session_effective_user_id(sess) : 0;
}
