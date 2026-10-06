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
 * Who an action goes to (contact_resolve.c) against a real contacts table:
 * a whole name is certain; part of one, a sound-alike (spoken) or a name the
 * user never said is a question; several values ask which; numbers and
 * addresses are checked.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db_internal.h"
#include "memory/contacts_db.h"
#include "memory/memory_fact_search.h"
#include "tools/contact_resolve.h"
#include "unity.h"

static const char *DDL =
    "CREATE TABLE memory_entities (id INTEGER PRIMARY KEY AUTOINCREMENT, user_id INTEGER NOT "
    "NULL, name TEXT NOT NULL, entity_type TEXT NOT NULL, canonical_name TEXT NOT NULL, "
    "photo_id TEXT DEFAULT NULL, canonical_id INTEGER DEFAULT NULL, is_user_self INTEGER NOT "
    "NULL DEFAULT 0);"
    "CREATE TABLE contacts (id INTEGER PRIMARY KEY AUTOINCREMENT, user_id INTEGER NOT NULL, "
    "entity_id INTEGER NOT NULL, field_type TEXT NOT NULL, value TEXT NOT NULL, label TEXT "
    "DEFAULT '', created_at INTEGER NOT NULL DEFAULT 0);";

/* As auth_db_statements.c prepares it. */
static const char *SQL_LIST =
    "SELECT c.id, c.entity_id, e.name, e.canonical_name, c.field_type, c.value, c.label, "
    "e.photo_id FROM contacts c JOIN memory_entities e ON c.entity_id = e.id "
    "WHERE c.user_id = ? AND (? IS NULL OR c.field_type = ?) "
    "ORDER BY e.name LIMIT ? OFFSET ?";

static int64_t person(const char *name, const char *canonical, int64_t alias_of) {
   char sql[512];
   snprintf(sql, sizeof(sql),
            "INSERT INTO memory_entities (user_id, name, entity_type, canonical_name, "
            "canonical_id) VALUES (1, '%s', 'person', '%s', %s)",
            name, canonical, alias_of ? "?" : "NULL");
   sqlite3_stmt *st = NULL;
   sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL);
   if (alias_of) {
      sqlite3_bind_int64(st, 1, alias_of);
   }
   sqlite3_step(st);
   sqlite3_finalize(st);
   return sqlite3_last_insert_rowid(s_db.db);
}

static void contact(int64_t entity, const char *type, const char *value, const char *label) {
   char sql[512];
   snprintf(sql, sizeof(sql),
            "INSERT INTO contacts (user_id, entity_id, field_type, value, label) VALUES (1, "
            "%lld, '%s', '%s', '%s')",
            (long long)entity, type, value, label);
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(s_db.db, sql, NULL, NULL, NULL));
}

void setUp(void) {
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(":memory:", &s_db.db));
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(s_db.db, DDL, NULL, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(s_db.db, CONTACTS_FIND_SQL, -1,
                                                       &s_db.stmt_contacts_find, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_OK,
                         sqlite3_prepare_v2(s_db.db, SQL_LIST, -1, &s_db.stmt_contacts_list, NULL));
   s_db.initialized = true;

   contact(person("Chris Adams", "chris adams", 0), "phone", "+15550000001", "");
   contact(person("Christine Lee", "christine lee", 0), "phone", "+15550000002", "");
   contact(person("Cris Kemp", "cris kemp", 0), "phone", "+15550000003", "");
   const int64_t linda = person("Linda Kay", "linda kay", 0);
   contact(linda, "phone", "+15550000004", "");
   contact(linda, "email", "linda@example.com", "");
   person("Mom", "mom", linda);
   const int64_t bob = person("Bob Smith", "bob smith", 0);
   contact(bob, "phone", "+15550000005", "mobile");
   contact(bob, "phone", "+15550000006", "work");
   const int64_t jane = person("Jane Doe", "jane doe", 0);
   contact(jane, "phone", "+15550000007", "");
   person("wife", "wife", jane);
   contact(person("John Doe", "john doe", 0), "phone", "+15550000008", "");
   contact(person("Mary-Jane Watson", "mary-jane watson", 0), "phone", "+15550000010", "");
}

/* The user's own entity: never a sound-alike to ask about. */
static void make_self(const char *canonical) {
   char sql[256];
   snprintf(sql, sizeof(sql),
            "UPDATE memory_entities SET is_user_self = 1 WHERE canonical_name = '%s'", canonical);
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(s_db.db, sql, NULL, NULL, NULL));
}

void tearDown(void) {
   sqlite3_finalize(s_db.stmt_contacts_find);
   sqlite3_finalize(s_db.stmt_contacts_list);
   s_db.stmt_contacts_find = NULL;
   s_db.stmt_contacts_list = NULL;
   sqlite3_close(s_db.db);
   s_db.db = NULL;
   s_db.initialized = false;
}

/* Memory knows one thing: who "partner" is. */
int memory_search_execute(int user_id,
                          const char *query,
                          time_t since_ts,
                          memory_fact_t *out_facts,
                          float *out_scores,
                          int max,
                          int *out_count) {
   (void)user_id;
   (void)since_ts;
   *out_count = 0;
   if (max > 0 && strstr(query, "partner")) {
      memset(&out_facts[0], 0, sizeof(out_facts[0]));
      snprintf(out_facts[0].fact_text, sizeof(out_facts[0].fact_text),
               "Pat Doe is the user's partner.");
      out_scores[0] = 1.0f;
      *out_count = 1;
   }
   return 0;
}

static contact_resolve_t resolve(const char *input,
                                 bool spoken,
                                 const char *words,
                                 contact_field_t field) {
   const contact_resolve_opts_t opts = { .field = field, .spoken = spoken, .user_words = words };
   contact_resolve_t r;
   contact_resolve(1, input, &opts, &r);
   return r;
}

/* A whole name, or whole words of it, the user said: certain. */
static void test_unique(void) {
   contact_resolve_t r = resolve("Christine Lee", false, "call christine lee", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_UNIQUE, r.kind);
   TEST_ASSERT_EQUAL_STRING("+15550000002", r.value);
   r = resolve("chris", false, "Call Chris.", CONTACT_FIELD_PHONE); /* typed: no sound-alikes */
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_UNIQUE, r.kind);
   TEST_ASSERT_EQUAL_STRING("Chris Adams", r.name);
   r = resolve("mom", false, "call my mom", CONTACT_FIELD_PHONE); /* an alias */
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_UNIQUE, r.kind);
   TEST_ASSERT_EQUAL_STRING("Linda Kay", r.name);
   r = resolve("Linda", false, NULL, CONTACT_FIELD_EMAIL); /* no words: not checked */
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_UNIQUE, r.kind);
   TEST_ASSERT_EQUAL_STRING("linda@example.com", r.value);
}

/* Part of a name, or a name the user never said: is it this one?  A
 * sound-alike (spoken): which one? */
static void test_confirm(void) {
   contact_resolve_t r = resolve("christ", false, "call christ", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_CONFIRM, r.kind);
   TEST_ASSERT_EQUAL_INT(CONTACT_CONFIRM_PARTIAL, r.why);
   TEST_ASSERT_EQUAL_STRING("Christine Lee", r.name);

   r = resolve("chris", true, "call chris", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_AMBIGUOUS, r.kind);
   TEST_ASSERT_EQUAL_INT(CONTACT_CONFIRM_SOUNDS_LIKE, r.why);
   TEST_ASSERT_EQUAL_STRING("Chris Adams", r.candidates[0].name);
   bool cris_offered = false;
   for (int i = 1; i < r.candidate_count; i++) {
      cris_offered = cris_offered || strcmp(r.candidates[i].name, "Cris Kemp") == 0;
   }
   TEST_ASSERT_TRUE(cris_offered); /* every sound-alike, Christine too */
   /* The user's own contact is a contact like any other. */
   make_self("cris kemp");
   r = resolve("chris", true, "call chris", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_AMBIGUOUS, r.kind);
   /* The answer, by full name, settles it. */
   r = resolve("Cris Kemp", true, "cris kemp\ncall chris", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_UNIQUE, r.kind);
   /* A short form sounds like the long one; any name in the person's tree
    * counts ("Cris", the head over the entry "Cristopher Kemp"). */
   const int64_t cris = person("Cris", "cris", 0);
   contact(person("Cristopher Kemp", "cristopher kemp", cris), "phone", "+15550000014", "");
   r = resolve("Chris Adams", true, "call chris adams", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_UNIQUE, r.kind);
   /* Through the tree, "cris" is a whole word of Cristopher Kemp: as good a
    * match as Cris Kemp, so it asks which. */
   r = resolve("cris", false, "call cris", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_AMBIGUOUS, r.kind);
   TEST_ASSERT_EQUAL_INT(2, r.candidate_count);
   r = resolve("chris", true, "call chris", CONTACT_FIELD_PHONE);
   bool kristopher_offered = false;
   for (int i = 0; i < r.candidate_count; i++) {
      kristopher_offered = kristopher_offered ||
                           strcmp(r.candidates[i].name, "Cristopher Kemp") == 0;
   }
   TEST_ASSERT_TRUE(kristopher_offered);
   /* John and Jane (Doe) share a sound key, and no one confuses them. */
   r = resolve("John", true, "call john", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_UNIQUE, r.kind);

   /* The model's "correction", or a name read in an email. */
   r = resolve("Christine Lee", false, "call chris", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_CONFIRM, r.kind);
   TEST_ASSERT_EQUAL_INT(CONTACT_CONFIRM_NOT_SAID, r.why);
   char q[512];
   contact_resolve_question("Christine Lee", CONTACT_FIELD_PHONE, &r, q, sizeof(q));
   TEST_ASSERT_NOT_NULL(strstr(q, "isn't someone the user named"));
   /* Typed: a sound-alike of what was said isn't what was said. */
   r = resolve("Cris Kemp", false, "text chris", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_CONFIRM, r.kind);
   /* Nothing said at all (an image-only turn). */
   r = resolve("Christine Lee", false, "", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_CONFIRM, r.kind);
}

/* Named another way: a word that alone picks the contact, an alias said, a
 * label, a hyphenated name. */
static void test_named(void) {
   contact_resolve_t r = resolve("Christine Lee", false, "call lee", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_UNIQUE, r.kind);
   /* Two Lees: "lee" names neither, so the model's pick asks. */
   contact(person("Bruce Lee", "bruce lee", 0), "phone", "+15550000011", "");
   r = resolve("Christine Lee", false, "text lee", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_CONFIRM, r.kind);

   r = resolve("Jane Doe", false, "text my wife I'm on my way", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_UNIQUE, r.kind);
   r = resolve("my wife", false, "text my wife", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_UNIQUE, r.kind);
   TEST_ASSERT_EQUAL_STRING("Jane Doe", r.name);

   r = resolve("Bob Smith mobile", false, "the mobile one\ncall bob smith", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_UNIQUE, r.kind);
   TEST_ASSERT_EQUAL_STRING("+15550000005", r.value);

   r = resolve("Mary", false, "call mary", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_UNIQUE, r.kind);
   r = resolve("mary jane", false, "call mary jane", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_UNIQUE, r.kind);

   /* A label picks one person's number, never another person. */
   contact(person("Bob", "bob", 0), "phone", "+15550000013", "work");
   r = resolve("Bob mobile", false, "text Bob on his mobile", CONTACT_FIELD_PHONE);
   TEST_ASSERT_TRUE(r.kind != CONTACT_RESOLVE_UNIQUE); /* Bob has no mobile: asks */

   /* Answering "which number?": the name and its label, or the number. */
   r = resolve("Bob Smith mobile", false, "his cell\ntext bob smith I'm late", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_UNIQUE, r.kind);
   r = resolve("+15550000006", false, "the work one\ntext bob smith", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_LITERAL, r.kind);
   TEST_ASSERT_EQUAL_STRING("Bob Smith", r.name);
}

/* Several values ask which; nothing close is NONE; near-misses are offered. */
static void test_ambiguous_and_none(void) {
   contact_resolve_t r = resolve("Bob Smith", false, "call bob smith", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_AMBIGUOUS, r.kind);
   TEST_ASSERT_EQUAL_INT(CONTACT_CONFIRM_SEVERAL_VALUES, r.why);
   TEST_ASSERT_EQUAL_INT(2, r.candidate_count);
   r = resolve("Zed", false, "call zed", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_NONE, r.kind);
   TEST_ASSERT_EQUAL_STRING("", r.memory);
   /* No contact by that name: what memory says comes along, in a live turn. */
   r = resolve("my partner", false, "text my partner", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_NONE, r.kind);
   TEST_ASSERT_NOT_NULL(strstr(r.memory, "Pat Doe is the user's partner."));
   char q[1024];
   contact_resolve_question("my partner", CONTACT_FIELD_PHONE, &r, q, sizeof(q));
   TEST_ASSERT_NOT_NULL(strstr(q, "Memory says:"));
   const contact_resolve_opts_t as_given = { .field = CONTACT_FIELD_PHONE };
   contact_resolve(1, "my partner", &as_given, &r);
   TEST_ASSERT_EQUAL_STRING("", r.memory);
   r = resolve("Cris Kempe", false, "call cris kempe", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_CONFIRM, r.kind); /* the one near-miss */
   TEST_ASSERT_EQUAL_INT(CONTACT_CONFIRM_CLOSEST, r.why);
   TEST_ASSERT_EQUAL_STRING("Cris Kemp", r.name);
}

/* Numbers and addresses: taken as given when the user gave them; one address
 * only. */
static void test_literals(void) {
   contact_resolve_t r = resolve("+1 555 000 0009", false, "call 555-000-0009 please",
                                 CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_LITERAL, r.kind);
   TEST_ASSERT_EQUAL_STRING("+15550000009", r.value);
   r = resolve("+15550000009", false, "yes", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_CONFIRM, r.kind);
   TEST_ASSERT_EQUAL_INT(CONTACT_CONFIRM_NOT_SAID, r.why);
   r = resolve("a@x.com, evil@y.com", false, "email a@x.com", CONTACT_FIELD_EMAIL);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_BAD_VALUE, r.kind);
   r = resolve("a@x.com", false, "Email A@X.com about it", CONTACT_FIELD_EMAIL);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_LITERAL, r.kind);
   /* Part of an address said isn't the address. */
   r = resolve("smith@gmail.com", false, "email bob.smith@gmail.com", CONTACT_FIELD_EMAIL);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_CONFIRM, r.kind);
   /* A number from the call log: its contact named as written; a sound-alike
    * of the name isn't enough here. */
   r = resolve("+15550000003", true, "call chris back", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_CONFIRM, r.kind);
   r = resolve("+15550000002", false, "call back christine lee", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_LITERAL, r.kind);
   TEST_ASSERT_EQUAL_STRING("Christine Lee", r.name);
   /* A different number than the one said. */
   r = resolve("+447911123456", false, "call +1 791 112 3456", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_CONFIRM, r.kind);
   r = resolve("+++", false, "call +++", CONTACT_FIELD_PHONE);
   TEST_ASSERT_EQUAL_INT(CONTACT_RESOLVE_BAD_VALUE, r.kind);
   TEST_ASSERT_TRUE(contact_email_is_single("bob.smith+tag@example.co.uk"));
   TEST_ASSERT_FALSE(contact_email_is_single("Bob <bob@example.com>"));
   TEST_ASSERT_FALSE(contact_email_is_single("bob@example"));
   TEST_ASSERT_FALSE(contact_email_is_single("a@b@c.com"));
}

/* A list too long for the question keeps whole entries and the instruction
 * after it (long addresses, five matches). */
static void test_long_candidate_list_keeps_the_ask(void) {
   contact_resolve_t r;
   memset(&r, 0, sizeof(r));
   r.kind = CONTACT_RESOLVE_AMBIGUOUS;
   r.candidate_count = CONTACT_RESOLVE_MAX_CANDIDATES;
   for (int i = 0; i < r.candidate_count; i++) {
      snprintf(r.candidates[i].name, sizeof(r.candidates[i].name), "Bob Number%d", i);
      memset(r.candidates[i].value, 'a', 240);
      snprintf(r.candidates[i].value + 240, 16, "@example.com");
   }
   char q[1024];
   contact_resolve_question("Bob", CONTACT_FIELD_EMAIL, &r, q, sizeof(q));
   TEST_ASSERT_NOT_NULL(strstr(q, "Ask the user which one"));
   TEST_ASSERT_NOT_NULL(strstr(q, "Bob Number0 (")); /* the first fits */
   /* Every entry shown is whole: each "(" has its ")". */
   int open = 0;
   for (const char *p = q; *p; p++) {
      open += *p == '(';
      open -= *p == ')';
   }
   TEST_ASSERT_EQUAL_INT(0, open);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_unique);
   RUN_TEST(test_confirm);
   RUN_TEST(test_named);
   RUN_TEST(test_ambiguous_and_none);
   RUN_TEST(test_literals);
   RUN_TEST(test_long_candidate_list_keeps_the_ask);
   return UNITY_END();
}
