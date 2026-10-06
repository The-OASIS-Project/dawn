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
 * Unit tests for calendar_pick: which writable calendar a name means.
 */

#include <stdio.h>
#include <string.h>

#include "tools/calendar_pick.h"
#include "unity.h"

static calendar_calendar_t s_cals[4];
static int s_count;

static void add(const char *name, const char *account, bool read_only) {
   calendar_calendar_t *c = &s_cals[s_count++];
   memset(c, 0, sizeof(*c));
   snprintf(c->display_name, sizeof(c->display_name), "%s", name);
   snprintf(c->account_name, sizeof(c->account_name), "%s", account);
   c->account_read_only = read_only;
}

void setUp(void) {
   s_count = 0;
}
void tearDown(void) {
}

/* Two writable calendars of one name: a question, and each one by its label. */
static void test_same_name_asks_and_labels_pick(void) {
   add("Home", "iCloud", false);
   add("Home", "Nextcloud", false);
   add("Work", "Google", false);
   int t = -1;
   TEST_ASSERT_EQUAL_INT(CALENDAR_PICK_NOT_FOUND,
                         calendar_pick_writable(s_cals, s_count, "home", &t));
   TEST_ASSERT_EQUAL_INT(CALENDAR_PICK_OK,
                         calendar_pick_writable(s_cals, s_count, "Home (nextcloud)", &t));
   TEST_ASSERT_EQUAL_INT(1, t);
   char label[CALENDAR_PICK_LABEL_MAX];
   calendar_pick_label(s_cals, s_count, 0, label, sizeof(label));
   TEST_ASSERT_EQUAL_STRING("Home (iCloud)", label);
   calendar_pick_label(s_cals, s_count, 2, label, sizeof(label));
   TEST_ASSERT_EQUAL_STRING("Work", label); /* a unique name stays plain */
}

/* Twins in one account are told apart by id; each label picks one. */
static void test_twins_in_one_account(void) {
   add("Home", "Google", false);
   add("Home", "Google", false);
   s_cals[0].id = 11;
   s_cals[1].id = 12;
   char label[CALENDAR_PICK_LABEL_MAX];
   calendar_pick_label(s_cals, s_count, 1, label, sizeof(label));
   TEST_ASSERT_EQUAL_STRING("Home (Google #12)", label);
   int t = -1;
   TEST_ASSERT_EQUAL_INT(CALENDAR_PICK_NOT_FOUND,
                         calendar_pick_writable(s_cals, s_count, "Home", &t));
   TEST_ASSERT_EQUAL_INT(CALENDAR_PICK_OK,
                         calendar_pick_writable(s_cals, s_count, "home (google #12)", &t));
   TEST_ASSERT_EQUAL_INT(1, t);
   TEST_ASSERT_TRUE(calendar_pick_names(s_cals, s_count, 0, "Home"));
   TEST_ASSERT_FALSE(calendar_pick_names(s_cals, s_count, 0, "Home (Google #12)"));
}

/* A read-only twin doesn't make a name ambiguous; naming only it is refused. */
static void test_read_only_twin(void) {
   add("Family", "Google", true);
   add("Family", "iCloud", false);
   int t = -1;
   TEST_ASSERT_EQUAL_INT(CALENDAR_PICK_OK, calendar_pick_writable(s_cals, s_count, "Family", &t));
   TEST_ASSERT_EQUAL_INT(1, t);
   TEST_ASSERT_EQUAL_INT(CALENDAR_PICK_READONLY,
                         calendar_pick_writable(s_cals, s_count, "Family (Google)", &t));
}

/* No name: the first writable one; a part of one name: only when unique. */
static void test_default_and_partial(void) {
   add("Holidays", "Google", true);
   add("Family Calendar", "iCloud", false);
   add("Work", "Google", false);
   int t = -1;
   TEST_ASSERT_EQUAL_INT(CALENDAR_PICK_OK, calendar_pick_writable(s_cals, s_count, NULL, &t));
   TEST_ASSERT_EQUAL_INT(1, t);
   TEST_ASSERT_EQUAL_INT(CALENDAR_PICK_OK, calendar_pick_writable(s_cals, s_count, "family", &t));
   TEST_ASSERT_EQUAL_INT(1, t);
   TEST_ASSERT_EQUAL_INT(CALENDAR_PICK_NOT_FOUND, calendar_pick_writable(s_cals, s_count, "r", &t));
   TEST_ASSERT_EQUAL_INT(CALENDAR_PICK_READONLY,
                         calendar_pick_writable(s_cals, s_count, "Holidays", &t));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_same_name_asks_and_labels_pick);
   RUN_TEST(test_twins_in_one_account);
   RUN_TEST(test_read_only_twin);
   RUN_TEST(test_default_and_partial);
   return UNITY_END();
}
