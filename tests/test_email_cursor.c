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
 * The mail panel's cursor and page merge: encoding limits, and paging across
 * accounts that never repeats or skips a row.
 */

#include <json-c/json.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "webui/email_cursor.h"

void setUp(void) {
}
void tearDown(void) {
}

/* A JSON text as the client would send it. */
static char *b64_of(const char *json) {
   const size_t len = strlen(json);
   const size_t out_len = sodium_base64_encoded_len(len, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
   char *out = malloc(out_len);
   sodium_bin2base64(out, out_len, (const unsigned char *)json, len,
                     sodium_base64_VARIANT_URLSAFE_NO_PADDING);
   return out;
}

static bool decodes(const char *json) {
   char *b = b64_of(json);
   email_cursor_t c;
   const bool ok = email_cursor_decode(b, &c);
   free(b);
   return ok;
}

/* =============================================================================
 * Encode / decode
 * ============================================================================= */

static void test_round_trip(void) {
   email_cursor_t c;
   memset(&c, 0, sizeof(c));
   c.filter = 0x0123456789abcdefULL;
   c.count = 3;
   c.pos[0] = (email_cursor_pos_t){ .account_id = 4,
                                    .is_imap = true,
                                    .before_uid = 900,
                                    .uidvalidity = 77 };
   c.pos[1] = (email_cursor_pos_t){ .account_id = 9, .is_imap = false, .next_date = 1700000000 };
   c.pos[1].emitted = 3;
   c.pos[1].seen_count = 2;
   snprintf(c.pos[1].seen[0], sizeof(c.pos[1].seen[0]), "18c2f0a1b0");
   snprintf(c.pos[1].seen[1], sizeof(c.pos[1].seen[1]), "18c2f0a1b1");
   c.pos[2] = (email_cursor_pos_t){ .account_id = 12, .is_imap = true }; /* from the top */

   char *b = email_cursor_encode(&c);
   TEST_ASSERT_NOT_NULL(b);
   email_cursor_t d;
   TEST_ASSERT_TRUE(email_cursor_decode(b, &d));
   free(b);
   TEST_ASSERT_EQUAL_UINT64(c.filter, d.filter);
   TEST_ASSERT_EQUAL_INT(3, d.count);
   TEST_ASSERT_TRUE(d.pos[0].is_imap);
   TEST_ASSERT_EQUAL_UINT32(900, d.pos[0].before_uid);
   TEST_ASSERT_EQUAL_UINT32(77, d.pos[0].uidvalidity);
   TEST_ASSERT_FALSE(d.pos[1].is_imap);
   TEST_ASSERT_EQUAL_INT(3, d.pos[1].emitted);
   TEST_ASSERT_EQUAL_INT(2, d.pos[1].seen_count);
   TEST_ASSERT_EQUAL_STRING("18c2f0a1b1", d.pos[1].seen[1]);
   TEST_ASSERT_EQUAL_INT64(1700000000, (int64_t)d.pos[1].next_date);
   TEST_ASSERT_EQUAL_UINT32(0, d.pos[2].before_uid);
}

static void test_garbage_is_refused(void) {
   email_cursor_t c;
   TEST_ASSERT_FALSE(email_cursor_decode("", &c));
   TEST_ASSERT_FALSE(email_cursor_decode("!!!!", &c));
   TEST_ASSERT_FALSE(email_cursor_decode(NULL, &c));
   /* Too long before it's even decoded. */
   char *big = malloc(EMAIL_CURSOR_B64_MAX + 2);
   memset(big, 'A', EMAIL_CURSOR_B64_MAX + 1);
   big[EMAIL_CURSOR_B64_MAX + 1] = '\0';
   TEST_ASSERT_FALSE(email_cursor_decode(big, &c));
   free(big);

   TEST_ASSERT_TRUE(decodes("{\"v\":1,\"f\":\"0123456789abcdef\",\"a\":[{\"id\":1,\"k\":\"i\"}]}"));
   TEST_ASSERT_FALSE(decodes("{\"v\":2,\"f\":\"0123456789abcdef\",\"a\":[]}"));
   TEST_ASSERT_FALSE(decodes("{\"v\":1,\"f\":\"0123456789ABCDEF\",\"a\":[]}"));
   TEST_ASSERT_FALSE(decodes("{\"v\":1,\"f\":\"0123\",\"a\":[]}"));
   TEST_ASSERT_FALSE(decodes("{\"v\":1,\"f\":\"0123456789abcdef\",\"a\":{}}"));
   /* Trailing text after the object. */
   TEST_ASSERT_FALSE(decodes("{\"v\":1,\"f\":\"0123456789abcdef\",\"a\":[]} x"));
   /* An account twice, a bad id, an unknown kind. */
   TEST_ASSERT_FALSE(decodes("{\"v\":1,\"f\":\"0123456789abcdef\",\"a\":[{\"id\":1,\"k\":\"i\"},"
                             "{\"id\":1,\"k\":\"g\"}]}"));
   TEST_ASSERT_FALSE(
       decodes("{\"v\":1,\"f\":\"0123456789abcdef\",\"a\":[{\"id\":0,\"k\":\"i\"}]}"));
   TEST_ASSERT_FALSE(
       decodes("{\"v\":1,\"f\":\"0123456789abcdef\",\"a\":[{\"id\":1,\"k\":\"x\"}]}"));
   /* IMAP: a UID of 1 has nothing below it; values past 32 bits. */
   TEST_ASSERT_FALSE(
       decodes("{\"v\":1,\"f\":\"0123456789abcdef\",\"a\":[{\"id\":1,\"k\":\"i\",\"u\":1}]}"));
   TEST_ASSERT_FALSE(decodes(
       "{\"v\":1,\"f\":\"0123456789abcdef\",\"a\":[{\"id\":1,\"k\":\"i\",\"u\":4294967296}]}"));
   TEST_ASSERT_FALSE(
       decodes("{\"v\":1,\"f\":\"0123456789abcdef\",\"a\":[{\"id\":1,\"k\":\"i\",\"v\":\"7\"}]}"));
   /* Gmail: ids that aren't hex (next or seen), seen without a date, too many seen. */
   TEST_ASSERT_TRUE(decodes("{\"v\":1,\"f\":\"0123456789abcdef\",\"a\":[{\"id\":1,\"k\":\"g\","
                            "\"d\":5,\"c\":2,\"s\":[\"cd\"]}]}"));
   /* More ids than rows it counted; a count with no date; a negative count. */
   TEST_ASSERT_FALSE(decodes("{\"v\":1,\"f\":\"0123456789abcdef\",\"a\":[{\"id\":1,\"k\":\"g\","
                             "\"d\":5,\"c\":1,\"s\":[\"cd\",\"ef\"]}]}"));
   TEST_ASSERT_FALSE(
       decodes("{\"v\":1,\"f\":\"0123456789abcdef\",\"a\":[{\"id\":1,\"k\":\"g\",\"c\":3}]}"));
   TEST_ASSERT_FALSE(decodes(
       "{\"v\":1,\"f\":\"0123456789abcdef\",\"a\":[{\"id\":1,\"k\":\"g\",\"d\":5,\"c\":-1}]}"));
   TEST_ASSERT_FALSE(decodes("{\"v\":1,\"f\":\"0123456789abcdef\",\"a\":[{\"id\":1,\"k\":\"g\","
                             "\"d\":5,\"c\":1,\"s\":[\"a/b\"]}]}"));
   TEST_ASSERT_FALSE(decodes("{\"v\":1,\"f\":\"0123456789abcdef\",\"a\":[{\"id\":1,\"k\":\"g\","
                             "\"d\":5,\"c\":1,\"s\":[\"a\\u0000b\"]}]}"));
   TEST_ASSERT_FALSE(decodes("{\"v\":1,\"f\":\"0123456789abcdef\",\"a\":[{\"id\":1,\"k\":\"g\","
                             "\"s\":[\"ab\"]}]}"));
   TEST_ASSERT_FALSE(
       decodes("{\"v\":1,\"f\":\"0123456789abcdef\",\"a\":[{\"id\":1,\"k\":\"g\","
               "\"d\":5,\"c\":9,\"s\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\"]}]}"));
   TEST_ASSERT_FALSE(decodes("{\"v\":1,\"f\":\"0123456789abcdef\",\"a\":[{\"id\":1,\"k\":\"g\","
                             "\"d\":5,\"s\":\"ab\"}]}"));
}

static void test_a_full_cursor_fits_its_limits(void) {
   email_cursor_t c;
   memset(&c, 0, sizeof(c));
   c.count = EMAIL_CURSOR_ACCOUNTS;
   for (int i = 0; i < c.count; i++) {
      email_cursor_pos_t *p = &c.pos[i];
      p->account_id = 1000000 + i;
      p->next_date = 1700000000;
      p->emitted = EMAIL_CURSOR_EMITTED_MAX;
      p->seen_count = EMAIL_CURSOR_SEEN_MAX;
      for (int k = 0; k < EMAIL_CURSOR_SEEN_MAX; k++) {
         memset(p->seen[k], 'a', EMAIL_CURSOR_ID_MAX);
         p->seen[k][0] = (char)('0' + k);
      }
   }
   char *b = email_cursor_encode(&c);
   TEST_ASSERT_NOT_NULL(b);
   TEST_ASSERT_TRUE(strlen(b) <= EMAIL_CURSOR_B64_MAX);
   email_cursor_t d;
   TEST_ASSERT_TRUE(email_cursor_decode(b, &d));
   TEST_ASSERT_EQUAL_INT(EMAIL_CURSOR_SEEN_MAX, d.pos[15].seen_count);
   free(b);
}

static void test_too_many_accounts_are_refused(void) {
   char json[4096];
   size_t pos = (size_t)snprintf(json, sizeof(json), "{\"v\":1,\"f\":\"0123456789abcdef\",\"a\":[");
   for (int i = 1; i <= EMAIL_CURSOR_ACCOUNTS + 1; i++)
      pos += (size_t)snprintf(json + pos, sizeof(json) - pos, "%s{\"id\":%d,\"k\":\"i\"}",
                              i > 1 ? "," : "", i);
   snprintf(json + pos, sizeof(json) - pos, "]}");
   TEST_ASSERT_FALSE(decodes(json));
}

static void test_filter_hash(void) {
   const int64_t a[] = { 3, 7, 9 };
   const int64_t b[] = { 9, 3, 7 };
   const int64_t c[] = { 3, 7 };
   const uint64_t h = email_cursor_filter_hash("email_list", a, 3, false, "inbox", "");
   TEST_ASSERT_EQUAL_UINT64(h, email_cursor_filter_hash("email_list", b, 3, false, "INBOX", ""));
   TEST_ASSERT_NOT_EQUAL(h, email_cursor_filter_hash("email_list", c, 2, false, "inbox", ""));
   TEST_ASSERT_NOT_EQUAL(h, email_cursor_filter_hash("email_list", NULL, 0, false, "inbox", ""));
   TEST_ASSERT_NOT_EQUAL(h, email_cursor_filter_hash("email_list", a, 3, true, "inbox", ""));
   TEST_ASSERT_NOT_EQUAL(h, email_cursor_filter_hash("email_search", a, 3, false, "inbox", ""));
   TEST_ASSERT_NOT_EQUAL(h, email_cursor_filter_hash("email_list", a, 3, false, "inbox", "x"));
}

/* =============================================================================
 * Gmail resume
 * ============================================================================= */

static void row(email_summary_t *r, const char *id, uint32_t uid, time_t date) {
   memset(r, 0, sizeof(*r));
   snprintf(r->message_id, sizeof(r->message_id), "%s", id);
   r->uid = uid;
   r->date = date;
}

static void test_gmail_filter_drops_what_was_emitted(void) {
   /* Fetched "dated at or before 300": three rows of that second, then older. */
   email_summary_t rows[6];
   row(&rows[0], "b9", 0, 301); /* newer than the bound: never kept */
   row(&rows[1], "a1", 0, 300);
   row(&rows[2], "a2", 0, 300);
   row(&rows[3], "a3", 0, 300);
   row(&rows[4], "a4", 0, 200);
   row(&rows[5], "a5", 0, 100);
   email_cursor_pos_t first = { .is_imap = false };
   int left = email_cursor_gmail_skip(&first);
   TEST_ASSERT_EQUAL_INT(6, email_cursor_gmail_filter(rows, 6, &first, &left)); /* all */

   /* a1 went out, and it's named. */
   email_summary_t r[6];
   memcpy(r, rows, sizeof(r));
   email_cursor_pos_t from = { .next_date = 300, .emitted = 1, .seen_count = 1 };
   snprintf(from.seen[0], sizeof(from.seen[0]), "a1");
   left = email_cursor_gmail_skip(&from);
   TEST_ASSERT_EQUAL_INT(4, email_cursor_gmail_filter(r, 6, &from, &left));
   TEST_ASSERT_EQUAL_STRING("a2", r[0].message_id);
   TEST_ASSERT_EQUAL_STRING("a5", r[3].message_id);

   /* Two went out, one named: the other is the first unnamed one of the second,
    * even when it comes in a later chunk. */
   email_cursor_pos_t two = { .next_date = 300, .emitted = 2, .seen_count = 1 };
   snprintf(two.seen[0], sizeof(two.seen[0]), "a1");
   left = email_cursor_gmail_skip(&two);
   memcpy(r, rows, sizeof(r));
   TEST_ASSERT_EQUAL_INT(0, email_cursor_gmail_filter(r, 2, &two, &left)); /* b9, a1 */
   TEST_ASSERT_EQUAL_INT(3, email_cursor_gmail_filter(r + 2, 4, &two, &left));
   TEST_ASSERT_EQUAL_STRING("a3", r[2].message_id);
}

/* =============================================================================
 * Paging simulation: IMAP mailboxes whose dates don't follow their UIDs
 * ============================================================================= */

#define SIM_MAX 200

typedef struct {
   int64_t id;
   int n;
   uint32_t uid[SIM_MAX];
   time_t date[SIM_MAX];
   int fail_pages; /* fails on the first this many pages, then answers */
} sim_box_t;

/* What a provider page from @p before (0 = top) returns: the @p limit highest
 * UIDs below it, in the server's (ascending) order. */
static int sim_fetch(const sim_box_t *b,
                     uint32_t before,
                     int limit,
                     email_summary_t *out,
                     bool *more,
                     uint32_t *next_before) {
   int idx[SIM_MAX], n = 0;
   for (int i = 0; i < b->n; i++) {
      if (before == 0 || b->uid[i] < before)
         idx[n++] = i;
   }
   /* Highest UIDs first. */
   for (int i = 0; i < n; i++)
      for (int j = i + 1; j < n; j++)
         if (b->uid[idx[j]] > b->uid[idx[i]]) {
            int t = idx[i];
            idx[i] = idx[j];
            idx[j] = t;
         }
   const int take = n < limit ? n : limit;
   for (int k = 0; k < take; k++) {
      const int i = idx[take - 1 - k]; /* ascending, as servers send */
      char id[32];
      snprintf(id, sizeof(id), "INBOX:%u", b->uid[i]);
      row(&out[k], id, b->uid[i], b->date[i]);
   }
   *more = n > take;
   *next_before = take > 0 ? b->uid[idx[take - 1]] : 0;
   return take;
}

static int by_uid_desc(const void *a, const void *b) {
   const uint32_t ua = ((const email_summary_t *)a)->uid, ub = ((const email_summary_t *)b)->uid;
   return ua < ub ? 1 : (ua > ub ? -1 : 0);
}

/* Pages through @p boxes with @p limit; @p seen counts each (box, uid) emitted. */
static int sim_page_all(sim_box_t *boxes, int nb, int limit, int seen[][SIM_MAX], int max_pages) {
   email_cursor_t cur;
   memset(&cur, 0, sizeof(cur));
   cur.count = nb;
   for (int i = 0; i < nb; i++)
      cur.pos[i] = (email_cursor_pos_t){ .account_id = boxes[i].id, .is_imap = true };

   int pages = 0;
   while (cur.count > 0 && pages < max_pages) {
      pages++;
      email_merge_in_t in[EMAIL_CURSOR_ACCOUNTS];
      email_summary_t rows[EMAIL_CURSOR_ACCOUNTS][64];
      int box_of[EMAIL_CURSOR_ACCOUNTS];
      for (int i = 0; i < cur.count; i++) {
         int b = 0;
         while (boxes[b].id != cur.pos[i].account_id)
            b++;
         box_of[i] = b;
         memset(&in[i], 0, sizeof(in[i]));
         in[i].account_id = cur.pos[i].account_id;
         in[i].is_imap = true;
         in[i].from = cur.pos[i];
         in[i].rows = rows[i];
         if (pages <= boxes[b].fail_pages) {
            in[i].err = EMAIL_ERR_UNREACHABLE;
            continue;
         }
         uint32_t nb_uid = 0;
         in[i].row_count = sim_fetch(&boxes[b], cur.pos[i].before_uid, limit, rows[i], &in[i].more,
                                     &nb_uid);
         qsort(rows[i], (size_t)in[i].row_count, sizeof(rows[i][0]), by_uid_desc);
         in[i].next_before_uid = nb_uid;
         in[i].next_uidvalidity = 1;
      }
      email_merge_pick_t picks[64];
      int pc = 0;
      email_cursor_t next;
      memset(&next, 0, sizeof(next));
      email_merge_page(in, cur.count, limit, picks, &pc, &next);
      TEST_ASSERT_TRUE(pc <= limit);
      /* Newest first across the page's picks of each account: a prefix of its UID order. */
      uint32_t last_uid[EMAIL_CURSOR_ACCOUNTS];
      for (int i = 0; i < cur.count; i++)
         last_uid[i] = UINT32_MAX;
      for (int k = 0; k < pc; k++) {
         const email_summary_t *r = &in[picks[k].account].rows[picks[k].row];
         TEST_ASSERT_TRUE(r->uid < last_uid[picks[k].account]);
         last_uid[picks[k].account] = r->uid;
         seen[box_of[picks[k].account]][r->uid]++;
      }
      /* Round-trip the cursor, as the client would. */
      char *b64 = next.count ? email_cursor_encode(&next) : NULL;
      memset(&cur, 0, sizeof(cur));
      if (b64) {
         TEST_ASSERT_TRUE(email_cursor_decode(b64, &cur));
         free(b64);
      }
   }
   return pages;
}

static void test_imap_paging_never_repeats_or_skips(void) {
   static sim_box_t boxes[3];
   memset(boxes, 0, sizeof(boxes));
   /* Dates deliberately out of UID order (mail moved in, clocks off). */
   unsigned seed = 7;
   for (int b = 0; b < 3; b++) {
      boxes[b].id = 10 + b;
      boxes[b].n = b == 2 ? 3 : 60 + b * 17; /* one small account */
      for (int i = 0; i < boxes[b].n; i++) {
         seed = seed * 1103515245u + 12345u;
         boxes[b].uid[i] = (uint32_t)(i * 2 + 5);
         boxes[b].date[i] = 1000000 + i * 60 + (time_t)((seed >> 16) % 3000) - 1500;
      }
   }
   static int seen[3][SIM_MAX];
   memset(seen, 0, sizeof(seen));
   sim_page_all(boxes, 3, 25, seen, 100);
   for (int b = 0; b < 3; b++)
      for (int i = 0; i < boxes[b].n; i++)
         TEST_ASSERT_EQUAL_INT_MESSAGE(1, seen[b][boxes[b].uid[i]], "each row exactly once");
}

static void test_a_failed_account_doesnt_wedge_the_others(void) {
   static sim_box_t boxes[2];
   memset(boxes, 0, sizeof(boxes));
   /* Account 1 is down for the first 6 pages, after the other one has run out. */
   boxes[0].id = 1;
   boxes[0].fail_pages = 6;
   boxes[0].n = 15;
   for (int i = 0; i < 15; i++) {
      boxes[0].uid[i] = (uint32_t)(i + 2);
      boxes[0].date[i] = 9000 + i;
   }
   boxes[1].id = 2;
   boxes[1].n = 40;
   for (int i = 0; i < 40; i++) {
      boxes[1].uid[i] = (uint32_t)(i + 2);
      boxes[1].date[i] = 5000 + i;
   }
   static int seen[2][SIM_MAX];
   memset(seen, 0, sizeof(seen));
   const int pages = sim_page_all(boxes, 2, 10, seen, 50);
   /* 4 pages of the healthy account, 2 more with only the failed one left (the cursor
    * keeps it), then its 15 rows once it answers. */
   TEST_ASSERT_EQUAL_INT(8, pages);
   for (int b = 0; b < 2; b++)
      for (int i = 0; i < boxes[b].n; i++)
         TEST_ASSERT_EQUAL_INT(1, seen[b][boxes[b].uid[i]]);
}

/* =============================================================================
 * Single pages
 * ============================================================================= */

static void test_an_account_still_fetching_that_runs_out_ends_the_page(void) {
   email_summary_t a[5], b[25];
   for (int i = 0; i < 5; i++)
      row(&a[i], "x", (uint32_t)(100 - i), 9000 - i); /* the newest mail */
   for (int i = 0; i < 25; i++)
      row(&b[i], "y", (uint32_t)(500 - i), 1000 - i);
   email_merge_in_t in[2] = {
      { .account_id = 1,
        .is_imap = true,
        .rows = a,
        .row_count = 5,
        .more = true,
        .from = { .account_id = 1, .is_imap = true } },
      { .account_id = 2,
        .is_imap = true,
        .rows = b,
        .row_count = 25,
        .more = true,
        .from = { .account_id = 2, .is_imap = true } },
   };
   email_merge_pick_t picks[25];
   int pc = 0;
   email_cursor_t next;
   memset(&next, 0, sizeof(next));
   TEST_ASSERT_TRUE(email_merge_page(in, 2, 25, picks, &pc, &next));
   TEST_ASSERT_EQUAL_INT(5, pc); /* account 1's next row might be newer than account 2's */
   TEST_ASSERT_EQUAL_INT(2, next.count);
   TEST_ASSERT_EQUAL_UINT32(96, next.pos[0].before_uid);
   TEST_ASSERT_EQUAL_UINT32(0, next.pos[1].before_uid); /* not reached: unchanged */
}

static void test_a_small_account_is_done_mid_page(void) {
   email_summary_t a[3], b[30];
   for (int i = 0; i < 3; i++)
      row(&a[i], "x", (uint32_t)(10 - i), 5000 - i * 10);
   for (int i = 0; i < 30; i++)
      row(&b[i], "y", (uint32_t)(900 - i), 5005 - i * 5);
   email_merge_in_t in[2] = {
      { .account_id = 1,
        .is_imap = true,
        .rows = a,
        .row_count = 3,
        .more = false,
        .from = { .account_id = 1, .is_imap = true } },
      { .account_id = 2,
        .is_imap = true,
        .rows = b,
        .row_count = 30,
        .more = true,
        .from = { .account_id = 2, .is_imap = true } },
   };
   email_merge_pick_t picks[25];
   int pc = 0;
   email_cursor_t next;
   memset(&next, 0, sizeof(next));
   TEST_ASSERT_TRUE(email_merge_page(in, 2, 25, picks, &pc, &next));
   TEST_ASSERT_EQUAL_INT(25, pc);
   int from_a = 0;
   for (int i = 0; i < pc; i++)
      from_a += picks[i].account == 0;
   TEST_ASSERT_EQUAL_INT(3, from_a);
   TEST_ASSERT_EQUAL_INT(1, next.count); /* account 1 is done */
   TEST_ASSERT_EQUAL_INT64(2, next.pos[0].account_id);
}

static void test_no_surviving_rows_advance_past_them(void) {
   email_merge_in_t in[1] = { { .account_id = 5,
                                .is_imap = true,
                                .row_count = 0,
                                .more = true,
                                .next_before_uid = 500,
                                .next_uidvalidity = 3,
                                .from = { .account_id = 5, .is_imap = true, .before_uid = 900 } } };
   email_merge_pick_t picks[10];
   int pc = 0;
   email_cursor_t next;
   memset(&next, 0, sizeof(next));
   TEST_ASSERT_TRUE(email_merge_page(in, 1, 10, picks, &pc, &next));
   TEST_ASSERT_EQUAL_INT(0, pc);
   TEST_ASSERT_EQUAL_UINT32(500, next.pos[0].before_uid);
   TEST_ASSERT_EQUAL_UINT32(3, next.pos[0].uidvalidity);
}

static void test_gmail_positions(void) {
   email_summary_t r[6];
   const time_t dates[6] = { 900, 899, 899, 899, 898, 897 };
   for (int i = 0; i < 6; i++) {
      char id[16];
      snprintf(id, sizeof(id), "a%d", i);
      row(&r[i], id, 0, dates[i]);
   }
   email_merge_in_t in[1] = { { .account_id = 8,
                                .is_imap = false,
                                .rows = r,
                                .row_count = 6,
                                .more = true,
                                .from = { .account_id = 8 } } };
   email_merge_pick_t picks[6];
   int pc = 0;
   email_cursor_t next;

   /* Stopped inside second 899: a1 already emitted at it. */
   memset(&next, 0, sizeof(next));
   email_merge_page(in, 1, 2, picks, &pc, &next);
   TEST_ASSERT_EQUAL_INT64(899, (int64_t)next.pos[0].next_date);
   TEST_ASSERT_EQUAL_INT(1, next.pos[0].emitted);
   TEST_ASSERT_EQUAL_INT(1, next.pos[0].seen_count);
   TEST_ASSERT_EQUAL_STRING("a1", next.pos[0].seen[0]);

   /* A page that started inside 899 carries its earlier emissions there. */
   in[0].from.next_date = 899;
   in[0].from.emitted = 1;
   in[0].from.seen_count = 1;
   snprintf(in[0].from.seen[0], sizeof(in[0].from.seen[0]), "ff");
   memset(&next, 0, sizeof(next));
   email_merge_page(in, 1, 3, picks, &pc, &next);
   TEST_ASSERT_EQUAL_INT(3, next.pos[0].emitted); /* ff, a1, a2 */
   TEST_ASSERT_EQUAL_INT(3, next.pos[0].seen_count);
   in[0].from = (email_cursor_pos_t){ .account_id = 8 };

   /* All of the fetch emitted, the provider has more: below the last row. */
   memset(&next, 0, sizeof(next));
   email_merge_page(in, 1, 6, picks, &pc, &next);
   TEST_ASSERT_EQUAL_INT64(897, (int64_t)next.pos[0].next_date);
   TEST_ASSERT_EQUAL_INT(1, next.pos[0].emitted);
   TEST_ASSERT_EQUAL_STRING("a5", next.pos[0].seen[0]);

   in[0].more = false;
   memset(&next, 0, sizeof(next));
   TEST_ASSERT_FALSE(email_merge_page(in, 1, 6, picks, &pc, &next));
   TEST_ASSERT_EQUAL_INT(0, next.count);
}

/* =============================================================================
 * Paging simulation: a Gmail mailbox that changes between pages
 * ============================================================================= */

#define GSIM_MAX 400

typedef struct {
   int n;
   char id[GSIM_MAX][16];
   time_t date[GSIM_MAX];
   bool gone[GSIM_MAX];
   int seq[GSIM_MAX]; /* order of arrival: the provider's order within a second */
} gsim_t;

static void gsim_add(gsim_t *g, time_t date) {
   const int i = g->n++;
   snprintf(g->id[i], sizeof(g->id[i]), "%x", 0x1000 + i);
   g->date[i] = date;
   g->seq[i] = i;
}

/* Gmail's list: newest first (newest arrival first within a second), dated at
 * or before @p bound (0 = none), the first @p want, offset @p skip. */
static int gsim_list(const gsim_t *g,
                     time_t bound,
                     int skip,
                     int want,
                     email_summary_t *out,
                     bool *more) {
   int idx[GSIM_MAX], n = 0;
   for (int i = 0; i < g->n; i++)
      if (!g->gone[i] && (bound == 0 || g->date[i] <= bound))
         idx[n++] = i;
   for (int i = 0; i < n; i++)
      for (int j = i + 1; j < n; j++) {
         const int a = idx[i], b = idx[j];
         if (g->date[b] > g->date[a] || (g->date[b] == g->date[a] && g->seq[b] > g->seq[a])) {
            idx[i] = b;
            idx[j] = a;
         }
      }
   int k = 0;
   for (int i = skip; i < n && k < want; i++, k++)
      row(&out[k], g->id[idx[i]], 0, g->date[idx[i]]);
   *more = skip + k < n;
   return k;
}

/* Pages through @p g as the panel does; @p change runs between pages. */
static void gsim_page_all(gsim_t *g,
                          const int *limits,
                          int n_limits,
                          void (*change)(gsim_t *, int page),
                          int *seen) {
   email_cursor_t cur;
   memset(&cur, 0, sizeof(cur));
   cur.count = 1;
   cur.pos[0] = (email_cursor_pos_t){ .account_id = 3 };
   for (int page = 0; cur.count > 0 && page < 200; page++) {
      const int limit = limits[page % n_limits];
      email_summary_t rows[64];
      /* A page plus a top-up, filtered as list_gmail does. */
      bool more = false;
      int have = 0;
      int left = email_cursor_gmail_skip(&cur.pos[0]);
      for (int p = 0, skip = 0; p < 3 && have < limit; p++) {
         const int want = limit + EMAIL_CURSOR_SEEN_MAX - have;
         const int got = gsim_list(g, cur.pos[0].next_date, skip, want, rows + have, &more);
         skip += got;
         have += email_cursor_gmail_filter(rows + have, got, &cur.pos[0], &left);
         if (!more)
            break;
      }
      email_merge_in_t in = { .account_id = 3,
                              .from = cur.pos[0],
                              .rows = rows,
                              .row_count = have,
                              .more = more };
      email_merge_pick_t picks[64];
      int pc = 0;
      email_cursor_t next;
      memset(&next, 0, sizeof(next));
      email_merge_page(&in, 1, limit, picks, &pc, &next);
      for (int k = 0; k < pc; k++) {
         int i = 0;
         while (strcmp(g->id[i], rows[picks[k].row].message_id) != 0)
            i++;
         seen[i]++;
      }
      char *b64 = next.count ? email_cursor_encode(&next) : NULL;
      memset(&cur, 0, sizeof(cur));
      if (b64) {
         TEST_ASSERT_TRUE(email_cursor_decode(b64, &cur));
         free(b64);
      }
      if (change)
         change(g, page);
   }
}

static gsim_t s_g;
static int s_seen[GSIM_MAX];

static void gsim_fill(void) {
   memset(&s_g, 0, sizeof(s_g));
   memset(s_seen, 0, sizeof(s_seen));
   for (int i = 0; i < 120; i++)
      gsim_add(&s_g, 10000 + i * 7);
   /* Five in one second, mid-mailbox. */
   for (int i = 0; i < 5; i++)
      gsim_add(&s_g, 10350);
}

/* Each message there from start to end, exactly once; none twice. */
static void gsim_check(int first_n) {
   for (int i = 0; i < s_g.n; i++) {
      if (i < first_n && !s_g.gone[i])
         TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_seen[i], "an unchanged message, exactly once");
      else
         TEST_ASSERT_TRUE(s_seen[i] <= 1);
   }
}

static void arrive(gsim_t *g, int page) {
   (void)page;
   gsim_add(g, 20000 + g->n); /* newer than everything: offsets shift, dates don't */
   gsim_add(g, 20000 + g->n);
}

static void read_some(gsim_t *g, int page) {
   /* Unread only, the user reading: messages leave, seen and not. */
   for (int i = 0, k = 0; i < g->n && k < 3; i++) {
      if (!g->gone[i] && s_seen[i] && (i + page) % 2 == 0) {
         g->gone[i] = true;
         k++;
      }
   }
}

static void test_gmail_paging_survives_arrivals(void) {
   gsim_fill();
   const int n0 = s_g.n;
   const int limits[] = { 10 };
   gsim_page_all(&s_g, limits, 1, arrive, s_seen);
   gsim_check(n0);
}

static void test_gmail_paging_survives_removals(void) {
   gsim_fill();
   const int n0 = s_g.n;
   const int limits[] = { 10 };
   gsim_page_all(&s_g, limits, 1, read_some, s_seen);
   gsim_check(n0);
}

static void test_gmail_paging_survives_a_changed_limit(void) {
   gsim_fill();
   const int n0 = s_g.n;
   const int limits[] = { 25, 7, 13, 3 };
   gsim_page_all(&s_g, limits, 4, NULL, s_seen);
   gsim_check(n0);
}

static void test_a_date_past_the_ceiling_is_refused(void) {
   char json[256];
   snprintf(json, sizeof(json),
            "{\"v\":1,\"f\":\"0123456789abcdef\",\"a\":[{\"id\":1,\"k\":\"g\",\"d\":%lld}]}",
            (long long)EMAIL_CURSOR_DATE_MAX);
   TEST_ASSERT_TRUE(decodes(json));
   snprintf(json, sizeof(json),
            "{\"v\":1,\"f\":\"0123456789abcdef\",\"a\":[{\"id\":1,\"k\":\"g\",\"d\":%lld}]}",
            (long long)EMAIL_CURSOR_DATE_MAX + 1);
   TEST_ASSERT_FALSE(decodes(json)); /* the fetch adds 1 to it */
   TEST_ASSERT_FALSE(decodes("{\"v\":1,\"f\":\"0123456789abcdef\",\"a\":[{\"id\":1,\"k\":\"g\","
                             "\"d\":9223372036854775807}]}"));
}

static void test_a_gmail_fetch_out_of_order_is_sorted_stably(void) {
   email_summary_t r[4];
   row(&r[0], "a", 0, 10);
   row(&r[1], "b", 0, 5);
   row(&r[2], "c", 0, 9);
   row(&r[3], "d", 0, 9);
   TEST_ASSERT_TRUE(email_cursor_gmail_order(r, 4));
   TEST_ASSERT_EQUAL_STRING("a", r[0].message_id);
   TEST_ASSERT_EQUAL_STRING("c", r[1].message_id); /* the second keeps its order */
   TEST_ASSERT_EQUAL_STRING("d", r[2].message_id);
   TEST_ASSERT_EQUAL_STRING("b", r[3].message_id);
   TEST_ASSERT_FALSE(email_cursor_gmail_order(r, 4)); /* in order now: left alone */
   TEST_ASSERT_FALSE(email_cursor_gmail_order(r, 0));
}

static void test_an_imap_position_is_bound_to_its_folder(void) {
   const uint32_t all = email_cursor_folder_hash("Archive");
   TEST_ASSERT_NOT_EQUAL(0, all);
   TEST_ASSERT_EQUAL_UINT32(all, email_cursor_folder_hash("ARCHIVE"));
   TEST_ASSERT_NOT_EQUAL(all, email_cursor_folder_hash("INBOX"));

   /* Carried through the merge and the cursor. */
   email_summary_t r[2];
   row(&r[0], "Archive:90", 90, 100);
   row(&r[1], "Archive:80", 80, 99);
   email_merge_in_t in[1] = { { .account_id = 3,
                                .is_imap = true,
                                .rows = r,
                                .row_count = 2,
                                .more = true,
                                .next_uidvalidity = 7,
                                .next_folder_hash = all,
                                .from = { .account_id = 3, .is_imap = true } } };
   email_merge_pick_t picks[1];
   int pc = 0;
   email_cursor_t next;
   memset(&next, 0, sizeof(next));
   TEST_ASSERT_TRUE(email_merge_page(in, 1, 1, picks, &pc, &next));
   TEST_ASSERT_EQUAL_UINT32(all, next.pos[0].folder_hash);
   char *b = email_cursor_encode(&next);
   email_cursor_t d;
   TEST_ASSERT_TRUE(email_cursor_decode(b, &d));
   free(b);
   TEST_ASSERT_EQUAL_UINT32(all, d.pos[0].folder_hash);

   /* A next page read from another folder is stale; the first page, or a
    * cursor that didn't record one, isn't checked. */
   TEST_ASSERT_TRUE(email_cursor_imap_folder_ok(&d.pos[0], "archive"));
   TEST_ASSERT_FALSE(email_cursor_imap_folder_ok(&d.pos[0], "INBOX"));
   email_cursor_pos_t top = { .account_id = 3, .is_imap = true, .folder_hash = all };
   TEST_ASSERT_TRUE(email_cursor_imap_folder_ok(&top, "INBOX"));
   email_cursor_pos_t old = { .account_id = 3, .is_imap = true, .before_uid = 50 };
   TEST_ASSERT_TRUE(email_cursor_imap_folder_ok(&old, "INBOX"));
   TEST_ASSERT_FALSE(decodes("{\"v\":1,\"f\":\"0123456789abcdef\",\"a\":[{\"id\":1,\"k\":\"i\","
                             "\"u\":9,\"f\":4294967296}]}"));
}

static void test_empty_gmail_pages_from_the_top_keep_the_position(void) {
   /* Pages with nothing in them but more past them: not the end. */
   email_merge_in_t in[1] = { { .account_id = 6,
                                .is_imap = false,
                                .row_count = 0,
                                .more = true,
                                .from = { .account_id = 6 } } };
   email_merge_pick_t picks[4];
   int pc = 0;
   email_cursor_t next;
   memset(&next, 0, sizeof(next));
   TEST_ASSERT_TRUE(email_merge_page(in, 1, 4, picks, &pc, &next));
   TEST_ASSERT_EQUAL_INT(0, pc);
   TEST_ASSERT_EQUAL_INT(1, next.count);
   TEST_ASSERT_EQUAL_INT64(0, (int64_t)next.pos[0].next_date);
   /* And with nothing more, it's done. */
   in[0].more = false;
   memset(&next, 0, sizeof(next));
   TEST_ASSERT_FALSE(email_merge_page(in, 1, 4, picks, &pc, &next));
}

static void test_gmail_paging_through_a_crowded_second(void) {
   memset(&s_g, 0, sizeof(s_g));
   memset(s_seen, 0, sizeof(s_seen));
   for (int i = 0; i < 30; i++)
      gsim_add(&s_g, 5000); /* more of one second than seen holds */
   for (int i = 0; i < 10; i++)
      gsim_add(&s_g, 4000 - i);
   const int limits[] = { 4 };
   gsim_page_all(&s_g, limits, 1, NULL, s_seen);
   gsim_check(s_g.n);
}

int main(void) {
   if (sodium_init() < 0)
      return 1;
   UNITY_BEGIN();
   RUN_TEST(test_round_trip);
   RUN_TEST(test_garbage_is_refused);
   RUN_TEST(test_too_many_accounts_are_refused);
   RUN_TEST(test_filter_hash);
   RUN_TEST(test_a_full_cursor_fits_its_limits);
   RUN_TEST(test_gmail_filter_drops_what_was_emitted);
   RUN_TEST(test_imap_paging_never_repeats_or_skips);
   RUN_TEST(test_a_failed_account_doesnt_wedge_the_others);
   RUN_TEST(test_an_account_still_fetching_that_runs_out_ends_the_page);
   RUN_TEST(test_a_small_account_is_done_mid_page);
   RUN_TEST(test_no_surviving_rows_advance_past_them);
   RUN_TEST(test_gmail_positions);
   RUN_TEST(test_gmail_paging_survives_arrivals);
   RUN_TEST(test_gmail_paging_survives_removals);
   RUN_TEST(test_gmail_paging_survives_a_changed_limit);
   RUN_TEST(test_gmail_paging_through_a_crowded_second);
   RUN_TEST(test_a_date_past_the_ceiling_is_refused);
   RUN_TEST(test_a_gmail_fetch_out_of_order_is_sorted_stably);
   RUN_TEST(test_an_imap_position_is_bound_to_its_folder);
   RUN_TEST(test_empty_gmail_pages_from_the_top_keep_the_position);
   return UNITY_END();
}
