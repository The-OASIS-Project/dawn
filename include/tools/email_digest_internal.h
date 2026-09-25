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
 * Email digest internals: the per-account paging decisions email_digest.c makes
 * after each page.  Header-only and I/O-free so the loop's termination can be
 * unit-tested without the network (tests/test_email_digest_step.c).  Not part of
 * the tool-facing API in email_digest.h.
 */

#ifndef EMAIL_DIGEST_INTERNAL_H
#define EMAIL_DIGEST_INTERNAL_H

#include <stdbool.h>
#include <time.h>

#include "tools/email_types.h"

/* Per-account paging state the digest feeds to email_digest_next_step() after
 * every page it fetches. */
typedef struct {
   int depth;           /* the account's digest depth (max messages to scan) */
   int fetched;         /* messages fetched so far (in or out of the window) */
   int pages;           /* pages fetched so far */
   int last_returned;   /* rows the last page returned */
   bool reached_cutoff; /* the last page's boundary row predates the window */
   bool has_token;      /* the last page returned a continuation token */
   bool token_repeated; /* ...and it equals the token that was sent (no progress) */
} email_digest_page_state_t;

typedef enum {
   EMAIL_DIGEST_MORE = 0,        /* fetch another page */
   EMAIL_DIGEST_STOP_WINDOW,     /* paged back past the window start: covered */
   EMAIL_DIGEST_STOP_EXHAUSTED,  /* the mailbox has no older messages */
   EMAIL_DIGEST_STOP_DEPTH,      /* the account's digest depth was reached */
   EMAIL_DIGEST_STOP_PAGE_LIMIT, /* page ceiling hit, or the cursor stopped advancing */
} email_digest_step_t;

/**
 * @brief Decide whether the digest should fetch another page for one account.
 *
 * Checked in order: window covered, mailbox exhausted (empty page or no token),
 * depth reached, then the two runaway guards — a cursor that didn't advance, and
 * the page ceiling EMAIL_DIGEST_MAX_PAGES (a backend may return short pages that
 * still carry a token; Gmail can even return an empty page with one).
 *
 * "No token" is always treated as exhausted: both backends issue a continuation
 * token whenever older matches remain, so a full page without one means the
 * mailbox genuinely ended at that boundary.
 */
static inline email_digest_step_t email_digest_next_step(const email_digest_page_state_t *st) {
   if (st->reached_cutoff)
      return EMAIL_DIGEST_STOP_WINDOW;
   if (st->last_returned == 0 || !st->has_token)
      return EMAIL_DIGEST_STOP_EXHAUSTED;
   if (st->fetched >= st->depth)
      return EMAIL_DIGEST_STOP_DEPTH;
   if (st->token_repeated || st->pages >= EMAIL_DIGEST_MAX_PAGES)
      return EMAIL_DIGEST_STOP_PAGE_LIMIT;
   return EMAIL_DIGEST_MORE;
}

/**
 * @brief Has this page paged back past the start of the digest window?
 *
 * Judged on the page's BOUNDARY row — the one the backend's own paging order
 * puts last — never on array position: rows arrive in whatever order the server
 * answered the batch fetch (IMAP servers typically answer UID FETCH ascending,
 * i.e. newest last), so "the last element" is not the oldest.
 *  - IMAP rows (uid > 0) page by UID, so the boundary is the lowest UID.  Using
 *    arrival order rather than date also means one old message moved back into
 *    the inbox (new high UID, original date) can't end paging early.
 *  - Gmail rows (uid == 0) page by date, so the boundary is the oldest dated row.
 * Rows with no parseable date (date == 0) never prove coverage.
 */
static inline bool email_digest_page_reached_cutoff(const email_summary_t *rows,
                                                    int n,
                                                    time_t cutoff) {
   const email_summary_t *boundary = NULL;
   for (int i = 0; i < n; i++) {
      const email_summary_t *r = &rows[i];
      if (r->uid > 0) {
         if (!boundary || boundary->uid == 0 || r->uid < boundary->uid)
            boundary = r;
      } else if (r->date > 0 && (!boundary || (boundary->uid == 0 && r->date < boundary->date))) {
         boundary = r;
      }
   }
   return boundary && boundary->date > 0 && boundary->date < cutoff;
}

#endif /* EMAIL_DIGEST_INTERNAL_H */
