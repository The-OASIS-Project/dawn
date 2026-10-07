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
 * The mail panel's paging across accounts: one opaque cursor holding each
 * account's position, and the merge of a page's rows from every account.
 *
 * Each account's rows come in its own order (IMAP: UID descending; Gmail: the
 * provider's, newest first), and the merge only ever compares the accounts' next
 * rows, so what an account contributes is always a prefix of its order and its
 * position is exact.  An IMAP account resumes below the lowest UID it emitted.
 * A Gmail account resumes by date, not by page: the next fetch asks for rows
 * dated at or before the first row it didn't emit and skips the ones of that
 * same second it already emitted (by id, and past what the ids hold by count,
 * the provider's order within a second being stable), so mail arriving or
 * leaving between pages doesn't shift what comes next (Gmail's page tokens are
 * offsets).
 *
 * The cursor is the client's to hold and is untrusted: decoding checks its
 * shape and limits; the caller checks the filter hash (which is not keyed: it
 * only says which request the cursor belongs to) and every account id against
 * the user's accounts.  Pure: no I/O, no locks.
 */

#ifndef EMAIL_CURSOR_H
#define EMAIL_CURSOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "tools/email_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EMAIL_CURSOR_B64_MAX (16 * 1024)  /* the cursor as the client sends it */
#define EMAIL_CURSOR_JSON_MAX (12 * 1024) /* decoded */
#define EMAIL_CURSOR_ACCOUNTS 16
#define EMAIL_CURSOR_ID_MAX 64  /* a Gmail message id */
#define EMAIL_CURSOR_SEEN_MAX 8 /* emitted ids of the resume second a Gmail position keeps */
#define EMAIL_CURSOR_EMITTED_MAX 1000000
#define EMAIL_CURSOR_DATE_MAX 4102444800LL /* 2100-01-01: past any mail, far from overflow */

/* Where one account's next page starts. */
typedef struct {
   int64_t account_id;
   bool is_imap;
   /* IMAP: rows below this UID (0 = from the newest); the mailbox epoch it was read
    * under, and the folder (email_cursor_folder_hash, 0 = not recorded). */
   uint32_t before_uid;
   uint32_t uidvalidity;
   uint32_t folder_hash;
   /* Gmail: rows dated at or before @p next_date (0 = from the newest); of that
    * second, @p emitted rows already went out, the first of them in @p seen. */
   time_t next_date;
   int emitted;
   int seen_count;
   char seen[EMAIL_CURSOR_SEEN_MAX][EMAIL_CURSOR_ID_MAX + 1];
} email_cursor_pos_t;

typedef struct {
   uint64_t filter;
   int count;
   email_cursor_pos_t pos[EMAIL_CURSOR_ACCOUNTS];
} email_cursor_t;

/**
 * @brief Which request a cursor belongs to
 * @param verb   "email_list" or "email_search"
 * @param ids    The account ids asked for (any order), or NULL for all accounts
 * @param folder The folder asked for ("" when none)
 * @param query  The search text ("" for a list)
 */
uint64_t email_cursor_filter_hash(const char *verb,
                                  const int64_t *ids,
                                  int n_ids,
                                  bool unread_only,
                                  const char *folder,
                                  const char *query);

/**
 * @brief Drop from a Gmail fetch the rows a page before already emitted
 *
 * @p rows were fetched dated at or before @p from->next_date, newest first, in
 * chunks (a page, then top-ups); call this on each chunk in order.  Removes
 * rows dated after it, rows of that second listed in @p from->seen, and the
 * first @p *skip_left other rows of that second (emitted before, past what
 * @p seen holds).  Order is kept.
 *
 * @param skip_left Start it at email_cursor_gmail_skip(from) for a fetch
 * @return The rows left (compacted at the front of @p rows)
 */
int email_cursor_gmail_filter(email_summary_t *rows,
                              int count,
                              const email_cursor_pos_t *from,
                              int *skip_left);

/** How many emitted rows of @p from's second its ids don't name. */
int email_cursor_gmail_skip(const email_cursor_pos_t *from);

/**
 * @brief Put a Gmail fetch in date order, newest first
 *
 * Paging by date relies on the provider listing newest first.  Should a fetch
 * come back out of that order, it's sorted (stably, so a second keeps the
 * provider's order) so the position taken from it stays consistent.
 *
 * @return true when the rows weren't in order (and were sorted)
 */
bool email_cursor_gmail_order(email_summary_t *rows, int count);

/** The IMAP folder a position was read from, as a cursor records it (never 0). */
uint32_t email_cursor_folder_hash(const char *folder);

/**
 * @brief Whether a page read from @p folder continues @p from
 * @return false when @p from is past its first page, recorded a folder, and it
 *         isn't this one (the account's "all" folder changed: CURSOR_STALE)
 */
bool email_cursor_imap_folder_ok(const email_cursor_pos_t *from, const char *folder);

/** The cursor as the client gets it (heap, URL-safe base64), or NULL. */
char *email_cursor_encode(const email_cursor_t *c);

/**
 * @brief Read a client's cursor: its shape and limits only
 * @return false for anything malformed, too large, with more than
 *         EMAIL_CURSOR_ACCOUNTS entries or a repeated account (CURSOR_STALE)
 */
bool email_cursor_decode(const char *b64, email_cursor_t *out);

/* One account's part of a page, as fetched from its position. */
typedef struct {
   int64_t account_id;
   bool is_imap;
   email_err_t err;         /* not NONE: it failed; it keeps @p from and isn't merged */
   email_cursor_pos_t from; /* where this page started for it */
   const email_summary_t *rows;
   int row_count; /* IMAP: in UID-descending order; Gmail: from @p from's next row */
   bool more;     /* the provider has rows past the last one fetched */
   /* IMAP, used when no row survived the fetch: where the provider's next page starts. */
   uint32_t next_before_uid;
   uint32_t next_uidvalidity; /* the mailbox epoch this fetch saw (0 = not seen) */
   uint32_t next_folder_hash; /* IMAP: the folder this fetch read (0 = not known) */
} email_merge_in_t;

typedef struct {
   int account; /* index into the inputs */
   int row;
} email_merge_pick_t;

/**
 * @brief Merge one page across accounts
 *
 * Newest first by date, comparing only each account's next row (ties go to
 * the earlier account).  The page ends at @p limit rows, or as soon as an
 * account still fetching runs out of fetched rows, since its next row can't be
 * placed.  Failed accounts and accounts whose fetch had no rows don't take
 * part.
 *
 * @param picks    At least @p limit entries: the page's rows, in order
 * @param next     Every account's position after this page (done accounts
 *                 left out; failed ones keep theirs, so a next page retries
 *                 them); its filter is left as is
 * @return true when another page exists (any account not done, failed ones included)
 */
bool email_merge_page(const email_merge_in_t *in,
                      int n,
                      int limit,
                      email_merge_pick_t *picks,
                      int *pick_count,
                      email_cursor_t *next);

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_CURSOR_H */
