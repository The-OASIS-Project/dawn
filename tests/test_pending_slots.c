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
 * Pending slots: one staged item of each kind per session; nothing staged in
 * another session is replaced or found; a confirm takes only the item it
 * names, in the user's next turn.
 */

#include <string.h>

#include "core/pending_slots.h"
#include "unity.h"

#define SLOTS 3
#define TTL 120
#define KIND_CALL 1
#define KIND_TEXT 2

typedef struct {
   pending_slot_t hdr;
   char number[16];
} item_t;

PENDING_ITEM_CHECK(item_t);
static item_t s_items[SLOTS];
PENDING_ARRAY_CHECK(s_items);
static const pending_slots_t s_slots = PENDING_SLOTS_TABLE(s_items, TTL);

static turn_origin_t origin(uint32_t session, uint64_t token, uint32_t number) {
   turn_origin_t o = { .session_id = session, .turn_token = token, .turn_number = number };
   return o;
}

/* Each stage() is its own turn (a new token), as separate user requests are. */
static uint64_t s_token;

void setUp(void) {
   memset(s_items, 0, sizeof(s_items));
   s_token = 100;
}

void tearDown(void) {
}

static item_t *stage(uint32_t session, int user, int kind, const char *number, time_t now) {
   const turn_origin_t o = origin(session, ++s_token, 1);
   item_t *it = (item_t *)pending_slots_stage(&s_slots, &o, user, kind, now, NULL);
   if (it) {
      strcpy(it->number, number);
   }
   return it;
}

/* Staging again in the same session replaces that session's item of the kind;
 * another kind gets its own slot. */
static void test_same_session_replaces(void) {
   item_t *a = stage(7, 1, KIND_CALL, "111", 1000);
   item_t *b = stage(7, 1, KIND_CALL, "222", 1001);
   TEST_ASSERT_EQUAL_PTR(a, b);
   TEST_ASSERT_EQUAL_STRING("222", b->number);
   item_t *c = stage(7, 1, KIND_TEXT, "333", 1002);
   TEST_ASSERT_NOT_EQUAL(b, c);
}

/* Another session's item is never replaced, and never found from here. */
static void test_other_session_untouched(void) {
   item_t *mine = stage(7, 1, KIND_CALL, "111", 1000);
   item_t *theirs = stage(8, 1, KIND_CALL, "999", 1001);
   TEST_ASSERT_NOT_EQUAL(mine, theirs);
   TEST_ASSERT_EQUAL_STRING("111", mine->number);

   const turn_origin_t now = origin(7, 500, 2);
   pending_slot_t *found = NULL;
   TEST_ASSERT_EQUAL_INT(PENDING_FOUND,
                         pending_slots_find(&s_slots, &now, 1, KIND_CALL, 0, 1002, &found));
   TEST_ASSERT_EQUAL_STRING("111", ((item_t *)found)->number);

   const turn_origin_t elsewhere = origin(9, 600, 2);
   TEST_ASSERT_EQUAL_INT(PENDING_NONE,
                         pending_slots_find(&s_slots, &elsewhere, 1, KIND_CALL, 0, 1002, &found));
   TEST_ASSERT_NULL(found);
}

/* Another user in the same session doesn't see the item either. */
static void test_other_user_not_found(void) {
   stage(7, 1, KIND_CALL, "111", 1000);
   const turn_origin_t now = origin(7, 500, 2);
   pending_slot_t *found = NULL;
   TEST_ASSERT_EQUAL_INT(PENDING_NONE,
                         pending_slots_find(&s_slots, &now, 2, KIND_CALL, 0, 1001, &found));
}

/* Full of other sessions' live items: refused, nothing evicted.  Once one
 * expires, its slot is reused. */
static void test_full_refuses_then_reuses_expired(void) {
   stage(1, 1, KIND_CALL, "a", 1000);
   stage(2, 1, KIND_CALL, "b", 1000);
   stage(3, 1, KIND_CALL, "c", 1050);
   TEST_ASSERT_NULL(stage(4, 1, KIND_CALL, "d", 1100));
   item_t *d = stage(4, 1, KIND_CALL, "d", 1000 + TTL + 1);
   TEST_ASSERT_NOT_NULL(d);
   TEST_ASSERT_EQUAL_STRING("d", d->number);
   TEST_ASSERT_EQUAL_STRING("c", s_items[2].number);
}

/* An expired item is reported and cleared. */
static void test_expired(void) {
   stage(7, 1, KIND_CALL, "111", 1000);
   const turn_origin_t now = origin(7, 500, 2);
   pending_slot_t *found = NULL;
   TEST_ASSERT_EQUAL_INT(PENDING_EXPIRED, pending_slots_find(&s_slots, &now, 1, KIND_CALL, 0,
                                                             1000 + TTL + 1, &found));
   TEST_ASSERT_NULL(found);
   TEST_ASSERT_EQUAL_INT(PENDING_NONE, pending_slots_find(&s_slots, &now, 1, KIND_CALL, 0,
                                                          1000 + TTL + 1, &found));
}

/* drop clears this session's item only; a stored origin carries no approval;
 * the staged origin is what turn_origin_check sees. */
static void test_drop_and_origin(void) {
   stage(7, 1, KIND_CALL, "111", 1000);
   item_t *theirs = stage(8, 1, KIND_CALL, "999", 1000);
   const turn_origin_t seven = origin(7, 107, 1);
   pending_slots_drop(&s_slots, &seven, 1, KIND_CALL);
   pending_slot_t *found = NULL;
   TEST_ASSERT_EQUAL_INT(PENDING_NONE,
                         pending_slots_find(&s_slots, &seven, 1, KIND_CALL, 0, 1001, &found));
   TEST_ASSERT_TRUE(theirs->hdr.active);

   turn_origin_t redeemed = origin(7, 200, 1);
   redeemed.code_redeemed = true;
   item_t *it = (item_t *)pending_slots_stage(&s_slots, &redeemed, 1, KIND_TEXT, 1002, NULL);
   TEST_ASSERT_FALSE(it->hdr.origin.code_redeemed);

   const turn_origin_t reply = origin(7, 300, 2);
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_OK, turn_origin_check(&it->hdr.origin, &reply));
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_SAME_TURN, turn_origin_check(&it->hdr.origin, &redeemed));
}

/* Each staged item gets a new id; a confirm naming the earlier item's id
 * finds the replacement is another item (and leaves it). */
static void test_item_id_binds_the_confirm(void) {
   item_t *first = stage(7, 1, KIND_CALL, "111", 1000);
   const uint32_t first_id = first->hdr.item_id;
   TEST_ASSERT_NOT_EQUAL(0, first_id);
   item_t *second = stage(7, 1, KIND_CALL, "999", 1001);
   TEST_ASSERT_NOT_EQUAL(first_id, second->hdr.item_id);

   const turn_origin_t reply = origin(7, 300, 2);
   item_t out;
   TEST_ASSERT_EQUAL_INT(PENDING_OTHER_ITEM,
                         pending_slots_take(&s_slots, &reply, 1, KIND_CALL, first_id, 1002, &out,
                                            sizeof(out), NULL));
   TEST_ASSERT_TRUE(second->hdr.active);
   TEST_ASSERT_EQUAL_INT(PENDING_FOUND,
                         pending_slots_take(&s_slots, &reply, 1, KIND_CALL, second->hdr.item_id,
                                            1002, &out, sizeof(out), NULL));
   TEST_ASSERT_EQUAL_STRING("999", out.number);
   TEST_ASSERT_FALSE(second->hdr.active);
}

/* take checks the turn: refused in the turn that staged it and after the
 * user moved on (the item stays); a code-redeemed confirm may come later. */
static void test_take_checks_the_turn(void) {
   item_t *it = stage(7, 1, KIND_CALL, "111", 1000); /* made at turn 1 */
   const uint32_t id = it->hdr.item_id;
   item_t out;
   turn_origin_rc_t orc = TURN_ORIGIN_OK;
   const turn_origin_t same = it->hdr.origin;
   TEST_ASSERT_EQUAL_INT(PENDING_NOT_NOW, pending_slots_take(&s_slots, &same, 1, KIND_CALL, id,
                                                             1001, &out, sizeof(out), &orc));
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_SAME_TURN, orc);
   turn_origin_t later = origin(7, 400, 3);
   TEST_ASSERT_EQUAL_INT(PENDING_NOT_NOW, pending_slots_take(&s_slots, &later, 1, KIND_CALL, id,
                                                             1001, &out, sizeof(out), &orc));
   TEST_ASSERT_EQUAL_INT(TURN_ORIGIN_NOT_NEXT, orc);
   TEST_ASSERT_TRUE(it->hdr.active);
   later.code_redeemed = true;
   TEST_ASSERT_EQUAL_INT(PENDING_FOUND, pending_slots_take(&s_slots, &later, 1, KIND_CALL, id, 1001,
                                                           &out, sizeof(out), &orc));
}

/* Staging clears every expired item first, and replaces this session's own
 * expired one; drop with nothing staged is harmless. */
static void test_expired_swept_on_stage(void) {
   stage(1, 1, KIND_CALL, "a", 1000);
   stage(2, 1, KIND_CALL, "b", 1000);
   stage(1, 1, KIND_CALL, "c", 1000 + TTL + 5);
   TEST_ASSERT_FALSE(s_items[1].hdr.active);
   TEST_ASSERT_EQUAL_STRING("c", s_items[0].number);
   const turn_origin_t none = origin(9, 900, 1);
   pending_slots_drop(&s_slots, &none, 1, KIND_TEXT);
   TEST_ASSERT_TRUE(s_items[0].hdr.active);
}

/* A second item of one kind in the same turn is refused (the first stays);
 * the next turn may replace it.  A full table says so. */
static void test_twice_in_one_turn(void) {
   const turn_origin_t turn = origin(7, 500, 1);
   pending_stage_rc_t rc = PENDING_FULL;
   item_t *first = (item_t *)pending_slots_stage(&s_slots, &turn, 1, KIND_CALL, 1000, &rc);
   TEST_ASSERT_EQUAL_INT(PENDING_STAGED, rc);
   strcpy(first->number, "111");
   TEST_ASSERT_NULL(pending_slots_stage(&s_slots, &turn, 1, KIND_CALL, 1001, &rc));
   TEST_ASSERT_EQUAL_INT(PENDING_TWICE_IN_TURN, rc);
   TEST_ASSERT_EQUAL_STRING("111", first->number);
   TEST_ASSERT_NOT_NULL(pending_slots_stage(&s_slots, &turn, 1, KIND_TEXT, 1001, &rc));

   const turn_origin_t next = origin(7, 501, 2);
   TEST_ASSERT_NOT_NULL(pending_slots_stage(&s_slots, &next, 1, KIND_CALL, 1002, &rc));
   const turn_origin_t other = origin(8, 600, 1);
   TEST_ASSERT_NOT_NULL(pending_slots_stage(&s_slots, &other, 1, KIND_CALL, 1002, &rc));
   const turn_origin_t full = origin(9, 700, 1);
   TEST_ASSERT_NULL(pending_slots_stage(&s_slots, &full, 1, KIND_CALL, 1003, &rc));
   TEST_ASSERT_EQUAL_INT(PENDING_FULL, rc);
}

/* A drop doesn't clear an item the current turn staged: a failed preview in
 * the same turn can't make room for a second item. */
static void test_drop_keeps_this_turns_item(void) {
   const turn_origin_t turn = origin(7, 500, 1);
   item_t *first = (item_t *)pending_slots_stage(&s_slots, &turn, 1, KIND_CALL, 1000, NULL);
   TEST_ASSERT_NOT_NULL(first);
   pending_slots_drop(&s_slots, &turn, 1, KIND_CALL);
   TEST_ASSERT_TRUE(first->hdr.active);
   pending_stage_rc_t rc = PENDING_STAGED;
   TEST_ASSERT_NULL(pending_slots_stage(&s_slots, &turn, 1, KIND_CALL, 1001, &rc));
   TEST_ASSERT_EQUAL_INT(PENDING_TWICE_IN_TURN, rc);
   const turn_origin_t next = origin(7, 501, 2);
   pending_slots_drop(&s_slots, &next, 1, KIND_CALL);
   TEST_ASSERT_FALSE(first->hdr.active);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_same_session_replaces);
   RUN_TEST(test_other_session_untouched);
   RUN_TEST(test_other_user_not_found);
   RUN_TEST(test_full_refuses_then_reuses_expired);
   RUN_TEST(test_expired);
   RUN_TEST(test_drop_and_origin);
   RUN_TEST(test_item_id_binds_the_confirm);
   RUN_TEST(test_take_checks_the_turn);
   RUN_TEST(test_expired_swept_on_stage);
   RUN_TEST(test_twice_in_one_turn);
   RUN_TEST(test_drop_keeps_this_turns_item);
   return UNITY_END();
}
