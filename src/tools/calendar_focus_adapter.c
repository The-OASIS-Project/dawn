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
 * Calendar event focus adapter — Phase 1d of Dynamic Context Injection.
 *
 * source_id          = "calendar_event"
 * source_type        = FOCUS_SOURCE_EXTERNAL
 * requires_embedding = false   /  Adapter consulted on every compose;
 *                                 keyword + recency + structured time
 *                                 signal — no embedding required for v1.
 *
 * DESIGN: this adapter goes DIRECT to calendar_db_*, NOT through
 * calendar_service_*.  Rationale:
 *   1. calendar_service_* may trigger background sync on stale
 *      accounts → covert network call, forbidden by the
 *      no-live-network invariant in the focus-injection hot path.
 *   2. calendar_service_* helpers return count-via-int, conflating
 *      "no results" with "error" — incompatible with focus-source
 *      framework's strict SUCCESS/FAILURE contract.
 *   3. calendar_db_* signatures already match the framework's
 *      SUCCESS/FAILURE convention (verified at calendar_db.c:649,
 *      725) and are pure SQLite under the auth_db mutex.
 *
 * Pipeline (DB-direct chain, all calls pure cache):
 *   1. calendar_db_account_list(user_id, ...) — user-scoped at SQL.
 *   2. Cap to EXTERNAL_MAX_ACCOUNTS_PER_COMPOSE; OLOG_WARNING if more
 *      accounts exist.
 *   3. For each account: calendar_db_calendar_list(account_id, ...)
 *      → aggregate active (is_active==true) calendar_ids into a flat
 *      int64_t array.  account_id chain is user-scoped transitively.
 *   4. If the message asks about a time or the schedule
 *      (calendar_query_window: "today", "next week", "on friday", "my
 *      calendar", "last week", ...): calendar_db_occurrences_in_range over
 *      that window, plus calendar_db_allday_occurrences_in_range over its
 *      dates (calendar_window_dates).  A message about something else gets
 *      no window: no ambient list of the coming week on unrelated turns.
 *   5. Events the message names: calendar_db_events_nearest gives each
 *      event's occurrence nearest to now (timed or all-day), from a month
 *      back to three months ahead, and one is kept per distinct title.  An event is named
 *      when the message contains its title's distinctive words (titles are
 *      labels the user refers to by a key word; generic words like
 *      "meeting" or "call" don't count).  Named events get semantic_score
 *      0.5-0.7 by the share of the title matched; window-only occurrences
 *      get FOCUS_SCORE_NA.  Merged unique by occurrence.id.
 *   6. Render "[YYYY-MM-DD HH:MM] <summary>" ("[YYYY-MM-DD all day]
 *      <summary>" for an all-day event)
 *      through focus_candidate_init (FOCUS_TEXT_MAX_BYTES truncation).
 *   7. Compute asymmetric recency: past events 7-day half-life;
 *      future events 14-day half-life.  Importance = base + (within
 *      24h && future ? imminent boost : 0) + (today ? today boost
 *      : 0); clamped to 1.0.
 *
 * Provenance: {0,0,0} sentinel — calendar occurrences have no
 * conv-based provenance.
 *
 * item_id format: "calendar_occ:<occurrence.id>" — `occurrence.id` is
 * the local DB row id (server-generated, opaque).  NEVER constructed
 * from `event_uid` (iCal UID, server-controlled by the CalDAV
 * provider but ultimately user-influenceable through event creation
 * on the upstream calendar).
 *
 * Network-call audit (verified cache-only on 2026-05-08 by reading
 * src/tools/calendar_db.c — pure SQLite via AUTH_DB_LOCK_OR_FAIL → s_db
 * prepared statements; no curl_*, socket(), connect(), lws_*, SSL_*
 * calls in any function or its helpers):
 *   - calendar_db_account_list             (calendar_db.c:221)
 *   - calendar_db_calendar_list            (calendar_db.c:415)
 *   - calendar_db_occurrences_in_range
 *   - calendar_db_allday_occurrences_in_range
 *   - calendar_db_events_nearest
 *
 * Filter-on-retrieval is FRAMEWORK-OWNED + trust-tier-gated.  This
 * adapter does NOT call `memory_filter_check()` — `focus_compose()`
 * decides based on `source_type`.  Calendar events are
 * FOCUS_SOURCE_EXTERNAL (the user owns their CalDAV account) and pass
 * through without filtering.  If a multi-user calendar-share threat
 * model ever applies, reclassify as FOCUS_SOURCE_USER_CONTENT.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/focus/focus_candidate_helpers.h"
#include "core/focus/focus_source.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_terms.h"
#include "tools/calendar_db.h"
#include "tools/calendar_query_window.h"
#include "tools/external_focus_adapters_internal.h"

/* Constants — file-static, all TODO(1j) for bench-driven tuning. */

/* Asymmetric recency half-lives.  Past events fade faster than future
 * events because the user usually wants to know about what's coming
 * up; a yesterday meeting is less relevant than a meeting in 3 days
 * about the same topic.  TODO(1j). */
#define CALENDAR_PAST_HALF_LIFE_SECONDS (7 * 86400)
#define CALENDAR_FUTURE_HALF_LIFE_SECONDS (14 * 86400)

/* Importance boosts on top of the base.  Imminent (next 24h, future)
 * is the strongest signal; same-day adds a smaller boost.  Both stack
 * for events in the next 24h that are also today.  Clamped to 1.0. */
#define CALENDAR_IMMINENT_BOOST 0.3f
#define CALENDAR_TODAY_BOOST 0.2f
#define CALENDAR_DEFAULT_IMPORTANCE 0.5f

/* Heuristic semantic score for an event the message names (vs. one that is
 * only in the window asked about): base + span * the share of the title's
 * distinctive words matched, 0.5 to 0.7.  Adapter doesn't embed; this is the
 * "named" presence signal handed to the framework ranker. */
#define CALENDAR_NAMED_BASE_SEMANTIC 0.5f
#define CALENDAR_NAMED_SPAN_SEMANTIC 0.2f

/* Where named events are looked for: a month back, three months ahead.
 * Wider than any one question's window so an event named without a date
 * ("my piano lesson") is found; past the pull cap, the events farthest
 * from now are left out. */
#define CALENDAR_NAMED_PAST_SECONDS (30 * 86400)
#define CALENDAR_NAMED_FUTURE_SECONDS (90 * 86400)
#define CALENDAR_NAMED_PULL_MAX 128

/* Per-account upper bound on calendars + per-pull upper bound on
 * occurrences.  Keep them low — the focus pool is itself capped at
 * top_k (≤64) so over-fetching just to throw away is wasted IO.
 * EXTERNAL_MAX_ACCOUNTS_PER_COMPOSE × MAX_CALENDARS_PER_ACCOUNT
 * = 3 × 16 = 48 calendar IDs max, well within sqlite IN-clause
 * limits. */
#define MAX_CALENDARS_PER_ACCOUNT 16
#define MAX_OCCURRENCES_PER_PULL 64

/* Compile-time-folded calendar_ids[] cap.  Macro form (rather than
 * `const int`) so the array decays to a fixed-size array under C11
 * §6.7.6.2 — `const int` is not an integer constant expression in C
 * and would emit a VLA, which the project standard discourages
 * ("prefer static allocation").  Stack footprint = 48 × 8 B = 384 B. */
#define MAX_TOTAL_CALENDARS (EXTERNAL_MAX_ACCOUNTS_PER_COMPOSE * MAX_CALENDARS_PER_ACCOUNT)

/* "calendar_occ:9223372036854775807\0" → 33 chars; fits buf. */

/* Aggregate work buffers (stack) sized to the static caps above so
 * we never heap-allocate during the DB walk; only the final
 * `focus_candidate_t` array is heap-allocated. */

/* When an occurrence starts, locally.  An all-day one is its date's local
 * midnight (its stored dtstart is the date's UTC midnight). */
static time_t occ_local_start(const calendar_occurrence_t *occ) {
   int y = 0;
   int m = 0;
   int d = 0;
   if (occ->all_day && sscanf(occ->dtstart_date, "%4d-%2d-%2d", &y, &m, &d) == 3) {
      struct tm tm = { .tm_year = y - 1900, .tm_mon = m - 1, .tm_mday = d, .tm_isdst = -1 };
      const time_t t = mktime(&tm);
      if (t != (time_t)-1)
         return t;
   }
   return occ->dtstart;
}

static bool same_local_day(time_t a, time_t b) {
   struct tm ta, tb;
   if (localtime_r(&a, &ta) == NULL || localtime_r(&b, &tb) == NULL)
      return false;
   return ta.tm_year == tb.tm_year && ta.tm_yday == tb.tm_yday;
}

/* 0.5 ** (age / half) with source-specific half-life — same exponential
 * shape as focus_recency_decay_uniform but asymmetric: past events use
 * the shorter half-life, future events the longer.  Returns 0.0 for
 * unset event_ts so the ranker doesn't credit unknowns. */
static double calendar_recency_decay(time_t event_ts, time_t now) {
   if (event_ts <= 0)
      return 0.0;
   const double half = (event_ts < now) ? (double)CALENDAR_PAST_HALF_LIFE_SECONDS
                                        : (double)CALENDAR_FUTURE_HALF_LIFE_SECONDS;
   const double age = (event_ts < now) ? (double)(now - event_ts) : (double)(event_ts - now);
   const double decay = pow(0.5, age / half);
   if (decay <= 0.0)
      return 0.0;
   if (decay >= 1.0)
      return 1.0;
   return decay;
}

static int format_event_text(const calendar_occurrence_t *occ, char *out, size_t outlen) {
   /* Render localtime (matches DAWN's user-facing convention; CalDAV
    * dtstart is stored as epoch seconds in the DB, timezone-agnostic). */
   const char *summary = (occ->summary[0] != '\0') ? occ->summary : "(untitled event)";
   int n;
   if (occ->all_day && occ->dtstart_date[0] != '\0') {
      /* All-day events are dates, not times: dtstart is the date's UTC
       * midnight, which localtime would show as the evening before. */
      n = snprintf(out, outlen, "[%s all day] %s", occ->dtstart_date, summary);
   } else {
      struct tm tm;
      if (localtime_r(&occ->dtstart, &tm) == NULL)
         return FAILURE;
      n = snprintf(out, outlen, "[%04d-%02d-%02d %02d:%02d] %s", tm.tm_year + 1900, tm.tm_mon + 1,
                   tm.tm_mday, tm.tm_hour, tm.tm_min, summary);
   }
   if (n < 0 || (size_t)n >= outlen)
      return FAILURE;
   return SUCCESS;
}

/* Words in event titles that say what kind of entry it is, not which one
 * ("Call with the bank"): stems, never enough to name an event. */
static const char *const GENERIC_TITLE_STEMS[] = { "meet",    "call", "appoint", "event",
                                                   "session", "sync", "remind",  "task" };

/* Longest title stem line kept. */
#define TITLE_STEMS_MAX 256

static bool is_generic_stem(const char *w, size_t len) {
   for (size_t g = 0; g < sizeof(GENERIC_TITLE_STEMS) / sizeof(GENERIC_TITLE_STEMS[0]); g++) {
      if (strlen(GENERIC_TITLE_STEMS[g]) == len && strncmp(GENERIC_TITLE_STEMS[g], w, len) == 0) {
         return true;
      }
   }
   return false;
}

/* How many of @p lines contain word @p w (@p len bytes). */
static int titles_with(char (*lines)[TITLE_STEMS_MAX], int n, const char *w, size_t len) {
   int count = 0;
   for (int i = 0; i < n; i++) {
      for (const char *p = lines[i]; *p;) {
         const char *end = strchr(p, ' ');
         const size_t wl = end ? (size_t)(end - p) : strlen(p);
         if (wl == len && strncmp(p, w, len) == 0) {
            count++;
            break;
         }
         if (!end) {
            break;
         }
         p = end + 1;
      }
   }
   return count;
}

/* Whether the message names title @p i and how much of it matched (0..1).
 * Titles are short labels the user refers to by a key word ("the dentist"),
 * so a title is named when two of its distinctive words match, or half of
 * them including one no other title has.  Generic words ("meeting") never
 * count, and a word many titles share (the user's own name, a franchise)
 * doesn't name one of them on its own.  @p lines holds distinct titles. */
static bool title_named(const memory_terms_t *terms,
                        char (*lines)[TITLE_STEMS_MAX],
                        int n,
                        int i,
                        float *share) {
   int distinct = 0;
   int matched = 0;
   bool unique_match = false;
   char word[MEMORY_TERM_LEN];
   for (const char *p = lines[i]; *p;) {
      const char *end = strchr(p, ' ');
      const size_t len = end ? (size_t)(end - p) : strlen(p);
      if (len > 0 && len < sizeof(word) && !is_generic_stem(p, len)) {
         distinct++;
         memcpy(word, p, len);
         word[len] = '\0';
         if (memory_terms_has(terms, word)) {
            matched++;
            unique_match = unique_match || titles_with(lines, n, p, len) == 1;
         }
      }
      if (!end) {
         break;
      }
      p = end + 1;
   }
   *share = distinct > 0 ? (float)matched / (float)distinct : 0.0f;
   return matched >= 2 || (matched > 0 && 2 * matched >= distinct && unique_match);
}

/* Events the message names: each event's occurrence nearest to now within
 * the named-event window, kept when the message names its title
 * (title_named).  Events sharing a title (one created per week) count as one
 * title, and the nearest stands for them.
 * *@p out (heap, caller frees) holds them, and @p *scores (heap) how much of
 * each title matched, 0..1. */
static int find_named_events(const int64_t *calendar_ids,
                             int total_calendars,
                             const char *query,
                             time_t now,
                             calendar_occurrence_t **out,
                             float **scores,
                             int *count) {
   *out = NULL;
   *scores = NULL;
   *count = 0;
   calendar_occurrence_t *events = malloc(CALENDAR_NAMED_PULL_MAX * sizeof(*events));
   memory_terms_t *terms = malloc(sizeof(*terms));
   char(*lines)[TITLE_STEMS_MAX] = malloc(CALENDAR_NAMED_PULL_MAX * sizeof(*lines));
   float *share = malloc(CALENDAR_NAMED_PULL_MAX * sizeof(*share));
   bool *is_named = malloc(CALENDAR_NAMED_PULL_MAX * sizeof(*is_named));
   int n = 0;
   int rc = FAILURE;
   if (!events || !terms || !lines || !share || !is_named ||
       calendar_db_events_nearest(calendar_ids, total_calendars, now - CALENDAR_NAMED_PAST_SECONDS,
                                  now + CALENDAR_NAMED_FUTURE_SECONDS, now, events,
                                  CALENDAR_NAMED_PULL_MAX, &n) != SUCCESS) {
      goto done;
   }
   memory_terms_from_text(query, true, terms);
   rc = SUCCESS;
   if (terms->count == 0 || n == 0) {
      goto done;
   }
   /* One entry per distinct title, nearest first (the order they came in). */
   int kept = 0;
   for (int i = 0; i < n; i++) {
      memory_terms_stem_line(events[i].summary, true, lines[kept], sizeof(lines[kept]));
      bool seen = false;
      for (int j = 0; j < kept && !seen; j++) {
         seen = strcmp(lines[j], lines[kept]) == 0;
      }
      if (!seen) {
         events[kept++] = events[i];
      }
   }
   /* Decide every title against the full set first (a word's rarity is
    * measured across all titles), then compact the named ones. */
   int named = 0;
   for (int i = 0; i < kept; i++) {
      is_named[i] = title_named(terms, lines, kept, i, &share[i]);
   }
   for (int i = 0; i < kept; i++) {
      if (is_named[i]) {
         share[named] = share[i];
         events[named++] = events[i];
      }
   }
   if (named > 0) {
      *out = events;
      *scores = share;
      *count = named;
      events = NULL;
      share = NULL;
   }
done:
   free(events);
   free(terms);
   free(lines);
   free(share);
   free(is_named);
   return rc;
}

static int calendar_adapter_query(int user_id,
                                  bool include_private,
                                  const char *query_text,
                                  const float *query_embedding,
                                  size_t embed_dim,
                                  time_t now,
                                  int max_candidates,
                                  focus_candidate_t **out_candidates,
                                  int *out_count) {
   (void)include_private; /* 1f: calendar private flag not yet
                             modeled; per-account read_only is
                             separate (write protection only). */
   (void)query_embedding; /* requires_embedding=false; embedding
                             consumed only by document/memory adapters. */
   (void)embed_dim;
   *out_candidates = NULL;
   *out_count = 0;
   if (max_candidates <= 0)
      return SUCCESS;

   /* Occurrence buffers (~1.4 KB each) are on the heap.  Heap
    * allocations + cleanup are funneled through a single `cleanup:`
    * epilogue at the end of the function so adding a future workspace
    * buffer means one calloc + one free, not chasing every error
    * branch. */

   calendar_occurrence_t *merged = NULL;
   calendar_occurrence_t *range_buf = NULL;
   calendar_occurrence_t *named_buf = NULL;
   float *named_match = NULL; /* per named event: share of its title matched */
   int range_count = 0;
   int named_count = 0;
   float *named_score = NULL; /* per merged entry; < 0 when window-only */
   focus_candidate_t *out = NULL;
   int rc = SUCCESS;
   int produced = 0;

   /* Step 1: enumerate accounts (user-scoped at SQL). */
   calendar_account_t accounts[CALENDAR_MAX_ACCOUNTS];
   int account_count = 0;
   if (calendar_db_account_list(user_id, accounts, CALENDAR_MAX_ACCOUNTS, &account_count) !=
       SUCCESS) {
      OLOG_ERROR("calendar_adapter: calendar_db_account_list failed (user_id=%d)", user_id);
      rc = FAILURE;
      goto cleanup;
   }
   if (account_count <= 0)
      goto cleanup; /* SUCCESS, zero candidates */

   const int effective_account_count = (account_count > EXTERNAL_MAX_ACCOUNTS_PER_COMPOSE)
                                           ? EXTERNAL_MAX_ACCOUNTS_PER_COMPOSE
                                           : account_count;
   if (account_count > EXTERNAL_MAX_ACCOUNTS_PER_COMPOSE) {
      OLOG_WARNING("calendar_adapter: user_id=%d has %d accounts; capping at %d for focus compose "
                   "(remaining accounts dropped in db_account_list order)",
                   user_id, account_count, EXTERNAL_MAX_ACCOUNTS_PER_COMPOSE);
   }

   /* Step 2: aggregate active calendar IDs across the considered
    * accounts.  account_id is user-scoped transitively (we got it
    * from a user-scoped account_list above).  Active filter applied
    * client-side because calendar_db_calendar_list returns all
    * calendars on the account.  Stack array sized via MAX_TOTAL_CALENDARS
    * macro (NOT a `const int` that would emit a VLA). */
   int64_t calendar_ids[MAX_TOTAL_CALENDARS];
   int total_calendars = 0;
   for (int a = 0; a < effective_account_count; a++) {
      calendar_calendar_t cals[MAX_CALENDARS_PER_ACCOUNT];
      int cal_count = 0;
      if (calendar_db_calendar_list(accounts[a].id, cals, MAX_CALENDARS_PER_ACCOUNT, &cal_count) !=
          SUCCESS) {
         OLOG_WARNING("calendar_adapter: calendar_db_calendar_list failed (account_id=%lld) — "
                      "continuing with remaining accounts",
                      (long long)accounts[a].id);
         continue;
      }
      for (int c = 0; c < cal_count && total_calendars < MAX_TOTAL_CALENDARS; c++) {
         if (!cals[c].is_active)
            continue;
         calendar_ids[total_calendars++] = cals[c].id;
      }
   }
   if (total_calendars <= 0)
      goto cleanup; /* SUCCESS, zero candidates */

   /* Step 3: the window the message asks about, if any (a time or the
    * schedule).  An unrelated message gets no ambient list of the coming
    * week: reminders are the scheduler's job. */
   calendar_window_t win;
   calendar_query_window(query_text, g_config.general.ai_name, now, &win);
   if (win.asks) {
      /* Timed events, then all-day ones (a separate, date-keyed query), each
       * up to MAX_OCCURRENCES_PER_PULL. */
      range_buf = calloc(2 * MAX_OCCURRENCES_PER_PULL, sizeof(*range_buf));
      if (!range_buf ||
          calendar_db_occurrences_in_range(calendar_ids, total_calendars, win.start, win.end,
                                           range_buf, MAX_OCCURRENCES_PER_PULL,
                                           &range_count) != SUCCESS) {
         OLOG_ERROR("calendar_adapter: calendar_db_occurrences_in_range failed");
         rc = FAILURE;
         goto cleanup;
      }
      char start_date[CALENDAR_DATE_LEN];
      char end_date[CALENDAR_DATE_LEN];
      calendar_window_dates(win.start, win.end, start_date, end_date);
      int allday_count = 0;
      if (calendar_db_allday_occurrences_in_range(calendar_ids, total_calendars, start_date,
                                                  end_date, range_buf + range_count,
                                                  MAX_OCCURRENCES_PER_PULL,
                                                  &allday_count) == SUCCESS) {
         range_count += allday_count;
      } else {
         OLOG_WARNING("calendar_adapter: all-day occurrences lookup failed — timed only");
      }
   }

   /* Step 4: events the message names.  query_text is user-controlled; do NOT
    * log it. */
   if (query_text != NULL && query_text[0] != '\0' &&
       find_named_events(calendar_ids, total_calendars, query_text, now, &named_buf, &named_match,
                         &named_count) != SUCCESS) {
      OLOG_WARNING("calendar_adapter: named-event lookup failed (query_len=%zu)",
                   strlen(query_text));
      named_count = 0;
   }

   /* Merge/dedupe: named events first (so they keep their semantic score),
    * then the window's, not already seen. */
   const int max_merged = range_count + named_count;
   if (max_merged <= 0)
      goto cleanup; /* SUCCESS, zero candidates */
   merged = calloc((size_t)max_merged, sizeof(*merged));
   named_score = calloc((size_t)max_merged, sizeof(*named_score));
   if (merged == NULL || named_score == NULL) {
      OLOG_ERROR("calendar_adapter: OOM allocating merge workspace (max=%d)", max_merged);
      rc = FAILURE;
      goto cleanup;
   }
   int merged_n = 0;
   for (int i = 0; i < named_count && merged_n < max_merged; i++) {
      merged[merged_n] = named_buf[i];
      named_score[merged_n] = named_match[i];
      merged_n++;
   }
   for (int i = 0; i < range_count && merged_n < max_merged; i++) {
      bool dup = false;
      for (int j = 0; j < merged_n; j++) {
         if (merged[j].id == range_buf[i].id) {
            dup = true;
            break;
         }
      }
      if (dup)
         continue;
      merged[merged_n] = range_buf[i];
      named_score[merged_n] = -1.0f;
      merged_n++;
   }
   if (merged_n <= 0)
      goto cleanup; /* SUCCESS, zero candidates */

   /* Trim to max_candidates.  Order chosen here is "named first then
    * window" rather than score-sorted because the framework's ranker
    * re-sorts everything anyway; what matters is that we don't drop named
    * events when the window is large. */
   const int kept = (merged_n > max_candidates) ? max_candidates : merged_n;

   out = calloc((size_t)kept, sizeof(*out));
   if (out == NULL) {
      OLOG_ERROR("calendar_adapter: OOM allocating candidate array (n=%d)", kept);
      rc = FAILURE;
      goto cleanup;
   }

   bool truncated_warned = false;
   for (int i = 0; i < kept; i++) {
      const calendar_occurrence_t *occ = &merged[i];

      char rendered[FOCUS_TEXT_MAX_BYTES + 64];
      if (format_event_text(occ, rendered, sizeof(rendered)) != SUCCESS) {
         /* Render failure is non-fatal: skip this occurrence and let
          * the rest of `kept` proceed.  `produced` will lag `kept` but
          * the SUCCESS path returns `produced` as the count, so the
          * tail of `out[]` past `produced` is unused — caller sees
          * only the populated slots.  Asymmetric with item_id /
          * focus_candidate_init failures, which are fatal because
          * they indicate a programming error not a data quality issue. */
         OLOG_WARNING("calendar_adapter: format_event_text failed (occ_id=%lld) — skipping",
                      (long long)occ->id);
         continue;
      }

      char item_id[FOCUS_ITEM_ID_BUFLEN];
      if (focus_candidate_format_item_id(item_id, sizeof(item_id), "calendar_occ", occ->id) !=
          SUCCESS) {
         OLOG_ERROR("calendar_adapter: item_id formatting failed (occ_id=%lld)",
                    (long long)occ->id);
         focus_adapter_failure_cleanup(out, produced, out_candidates, out_count);
         out = NULL; /* ownership transferred to failure-cleanup */
         rc = FAILURE;
         goto cleanup;
      }

      /* A named event's score grows with how much of its title matched, so a
       * one-word overlap with a long title ranks below a full match. */
      const float semantic = named_score[i] >= 0.0f
                                 ? CALENDAR_NAMED_BASE_SEMANTIC +
                                       CALENDAR_NAMED_SPAN_SEMANTIC * named_score[i]
                                 : FOCUS_SCORE_NA;
      const time_t starts = occ_local_start(occ);
      const float recency = (float)calendar_recency_decay(starts, now);

      float importance = CALENDAR_DEFAULT_IMPORTANCE;
      const time_t delta = starts - now;
      if (delta > 0 && delta < 86400)
         importance += CALENDAR_IMMINENT_BOOST;
      if (same_local_day(starts, now))
         importance += CALENDAR_TODAY_BOOST;
      if (importance > 1.0f)
         importance = 1.0f;

      if (focus_candidate_init(&out[produced], "calendar_event", FOCUS_SOURCE_EXTERNAL, rendered,
                               item_id, starts, semantic, recency, importance,
                               &truncated_warned) != SUCCESS) {
         OLOG_ERROR("calendar_adapter: focus_candidate_init failed (occ_id=%lld)",
                    (long long)occ->id);
         focus_adapter_failure_cleanup(out, produced, out_candidates, out_count);
         out = NULL; /* ownership transferred to failure-cleanup */
         rc = FAILURE;
         goto cleanup;
      }
      /* Provenance intentionally zeroed — calendar occurrences have
       * no conv-based provenance. */
      produced++;
   }

cleanup:
   free(merged);
   free(range_buf);
   free(named_buf);
   free(named_match);
   free(named_score);
   if (rc == SUCCESS && out != NULL) {
      *out_candidates = out;
      *out_count = produced;
   }
   /* On FAILURE, focus_adapter_failure_cleanup already zeroed the
    * out-params and freed `out`; on SUCCESS-with-no-candidates
    * (early-exit at any "zero candidates" gate), out_candidates /
    * out_count remain NULL/0 from the function's top-of-body
    * initialization. */
   return rc;
}

static const focus_source_adapter_t k_calendar_focus_adapter = {
   .source_id = "calendar_event",
   .source_type = FOCUS_SOURCE_EXTERNAL,
   .requires_embedding = false,
   .query = calendar_adapter_query,
};

int calendar_focus_adapter_register(void) {
   return focus_register_source(&k_calendar_focus_adapter);
}
