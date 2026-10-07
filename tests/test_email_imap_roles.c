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
 * Trash and Archive folders and MOVE / UIDPLUS, read from the replies of the
 * servers people use: marked folders win, a known name stands in when the
 * server marks none, and an account with neither gets no folder at all (the
 * move is refused rather than guessed).
 */

#include <string.h>

#include "tools/email_imap_roles.h"
#include "unity.h"

void setUp(void) {
}

void tearDown(void) {
}

static void test_capability(void) {
   email_imap_roles_t r;
   memset(&r, 0, sizeof(r));
   email_imap_roles_capability("* CAPABILITY IMAP4rev1 UNSELECT IDLE NAMESPACE QUOTA ID XLIST "
                               "CHILDREN X-GM-EXT-1 UIDPLUS COMPRESS=DEFLATE ENABLE MOVE "
                               "CONDSTORE ESEARCH UTF8=ACCEPT\r\nA1 OK done\r\n",
                               &r);
   TEST_ASSERT_TRUE(r.move);
   TEST_ASSERT_TRUE(r.uidplus);
   email_imap_roles_capability("* CAPABILITY IMAP4rev1 LITERAL+ IDLE\r\n", &r);
   TEST_ASSERT_FALSE(r.move);
   TEST_ASSERT_FALSE(r.uidplus);
   /* A response code counts; a word that only contains MOVE doesn't. */
   email_imap_roles_capability("A1 OK [CAPABILITY IMAP4rev1 X-MOVEX UIDPLUS] Logged in\r\n", &r);
   TEST_ASSERT_FALSE(r.move);
   TEST_ASSERT_TRUE(r.uidplus);
   email_imap_roles_capability("* CAPABILITY IMAP4rev1 NAMESPACE SPECIAL-USE X-GM-EXT-1\r\n", &r);
   TEST_ASSERT_TRUE(r.namespace_);
   TEST_ASSERT_TRUE(r.special_use);
   TEST_ASSERT_TRUE(r.gmail);
}

static void test_gmail(void) {
   email_imap_roles_t r;
   memset(&r, 0, sizeof(r));
   r.gmail = true;
   email_imap_roles_from_list("* LIST (\\HasNoChildren) \"/\" \"INBOX\"\r\n"
                              "* LIST (\\HasChildren \\Noselect) \"/\" \"[Gmail]\"\r\n"
                              "* LIST (\\All \\HasNoChildren) \"/\" \"[Gmail]/All Mail\"\r\n"
                              "* LIST (\\HasNoChildren \\Trash) \"/\" \"[Gmail]/Trash\"\r\n",
                              NULL, &r);
   TEST_ASSERT_EQUAL_STRING("[Gmail]/Trash", r.trash);
   TEST_ASSERT_EQUAL_STRING("[Gmail]/All Mail", r.archive);
   TEST_ASSERT_EQUAL_STRING("[Gmail]/All Mail", r.all);
}

static void test_outlook_marked(void) {
   email_imap_roles_t r;
   memset(&r, 0, sizeof(r));
   email_imap_roles_from_list("* LIST (\\HasNoChildren \\Trash) \"/\" \"Deleted Items\"\r\n"
                              "* LIST (\\HasNoChildren \\Archive) \"/\" Archive\r\n"
                              "* LIST (\\HasNoChildren) \"/\" Trash\r\n",
                              NULL, &r);
   /* The marked folder wins over a name match. */
   TEST_ASSERT_EQUAL_STRING("Deleted Items", r.trash);
   TEST_ASSERT_EQUAL_STRING("Archive", r.archive);
}

static void test_unmarked_by_name(void) {
   email_imap_roles_t r;
   memset(&r, 0, sizeof(r));
   /* iCloud-style names, Dovecot-style "INBOX." prefix, no attributes. */
   email_imap_roles_from_list("* LIST (\\HasNoChildren) \".\" \"INBOX.Deleted Messages\"\r\n"
                              "* LIST (\\HasNoChildren) \".\" \"INBOX.Archive\"\r\n",
                              NULL, &r);
   TEST_ASSERT_EQUAL_STRING("INBOX.Deleted Messages", r.trash);
   TEST_ASSERT_EQUAL_STRING("INBOX.Archive", r.archive);
   /* Better names win over worse ones, whatever the order. */
   email_imap_roles_from_list("* LIST () \"/\" Deleted\r\n* LIST () \"/\" Trash\r\n", NULL, &r);
   TEST_ASSERT_EQUAL_STRING("Trash", r.trash);
}

static void test_none_means_none(void) {
   email_imap_roles_t r;
   memset(&r, 0, sizeof(r));
   email_imap_roles_from_list("* LIST (\\HasNoChildren) \"/\" INBOX\r\n"
                              "* LIST (\\HasNoChildren) \"/\" Projects\r\n"
                              "* LIST (\\Noselect \\Trash) \"/\" Trash\r\n"
                              "* LIST (\\NonExistent) \"/\" Archive\r\n",
                              NULL, &r);
   TEST_ASSERT_EQUAL_STRING("", r.trash);
   TEST_ASSERT_EQUAL_STRING("", r.archive);
   /* A name that merely contains "Trash" isn't the trash. */
   email_imap_roles_from_list("* LIST () \"/\" \"Trash talk\"\r\n", NULL, &r);
   TEST_ASSERT_EQUAL_STRING("", r.trash);
}

static void test_quoted_names_and_nil_delimiter(void) {
   email_imap_roles_t r;
   memset(&r, 0, sizeof(r));
   email_imap_roles_from_list("* LIST (\\Trash) NIL \"My \\\"old\\\" bin\"\r\n", NULL, &r);
   TEST_ASSERT_EQUAL_STRING("My \"old\" bin", r.trash);
   /* A literal name isn't read (and isn't misread). */
   email_imap_roles_from_list("* LIST (\\Trash) \"/\" {5}\r\nTrash\r\n", NULL, &r);
   TEST_ASSERT_EQUAL_STRING("", r.trash);
}

static void test_quote_folder(void) {
   char out[64];
   TEST_ASSERT_TRUE(email_imap_quote_folder("Deleted Items", out, sizeof(out)));
   TEST_ASSERT_EQUAL_STRING("\"Deleted Items\"", out);
   TEST_ASSERT_TRUE(email_imap_quote_folder("a\"b\\c", out, sizeof(out)));
   TEST_ASSERT_EQUAL_STRING("\"a\\\"b\\\\c\"", out);
   TEST_ASSERT_FALSE(email_imap_quote_folder("a\r\nb", out, sizeof(out)));
   TEST_ASSERT_FALSE(email_imap_quote_folder("", out, sizeof(out)));
   TEST_ASSERT_FALSE(email_imap_quote_folder("abcdef", out, 6));
}

static void test_all_mail_only_on_gmail(void) {
   email_imap_roles_t r;
   memset(&r, 0, sizeof(r));
   /* Dovecot's virtual All marks \All but can't take a message. */
   email_imap_roles_from_list("* LIST (\\All) \"/\" virtual/All\r\n* LIST () \"/\" Archive\r\n",
                              NULL, &r);
   TEST_ASSERT_EQUAL_STRING("Archive", r.archive);
   /* Still the folder search looks through for "all". */
   TEST_ASSERT_EQUAL_STRING("virtual/All", r.all);
}

static void test_namespace(void) {
   email_imap_ns_t ns;
   email_imap_namespace_parse(
       "* NAMESPACE ((\"INBOX.\" \".\")) ((\"Other Users.\" \".\")) "
       "((\"Shared.\" \".\" \"X-PARAM\" (\"a\" \"b\"))(\"#public/\" \"/\"))\r\n"
       "A1 OK\r\n",
       &ns);
   TEST_ASSERT_EQUAL_STRING("INBOX.", ns.personal);
   TEST_ASSERT_EQUAL_CHAR('.', ns.delim);
   TEST_ASSERT_EQUAL_INT(3, ns.n_other);
   TEST_ASSERT_EQUAL_STRING("Other Users.", ns.other[0]);
   TEST_ASSERT_EQUAL_STRING("Shared.", ns.other[1]);
   TEST_ASSERT_EQUAL_STRING("#public/", ns.other[2]);
   email_imap_namespace_parse("* NAMESPACE ((\"\" \"/\")) NIL NIL\r\n", &ns);
   TEST_ASSERT_EQUAL_STRING("", ns.personal);
   TEST_ASSERT_EQUAL_INT(0, ns.n_other);
   /* Malformed: nothing taken. */
   email_imap_namespace_parse("* NAMESPACE ((\"x\" \"/\") NIL\r\n", &ns);
   TEST_ASSERT_EQUAL_STRING("", ns.personal);
   email_imap_namespace_parse(NULL, &ns);
   TEST_ASSERT_EQUAL_STRING("", ns.personal);
}

static void test_not_another_users_folder(void) {
   email_imap_ns_t ns;
   email_imap_namespace_parse("* NAMESPACE ((\"\" \"/\")) ((\"Other Users/\" \"/\")) "
                              "((\"Shared/\" \"/\"))\r\n",
                              &ns);
   email_imap_roles_t r;
   memset(&r, 0, sizeof(r));
   /* Marked or named, a folder someone else can read is never a target. */
   email_imap_roles_from_list("* LIST (\\Trash) \"/\" \"Other Users/bob/Trash\"\r\n"
                              "* LIST () \"/\" Shared/bob/Trash\r\n"
                              "* LIST (\\Archive) \"/\" Shared/team/Archive\r\n",
                              &ns, &r);
   TEST_ASSERT_EQUAL_STRING("", r.trash);
   TEST_ASSERT_EQUAL_STRING("", r.archive);
   /* A deep folder of a known name is the user's own project folder. */
   email_imap_roles_from_list("* LIST () \"/\" Work/2019/Archive\r\n", &ns, &r);
   TEST_ASSERT_EQUAL_STRING("", r.archive);
   /* Right under the personal prefix does count. */
   email_imap_namespace_parse("* NAMESPACE ((\"INBOX.\" \".\")) NIL NIL\r\n", &ns);
   email_imap_roles_from_list("* LIST () \".\" INBOX.Trash\r\n* LIST () \".\" INBOX.a.Archive\r\n",
                              &ns, &r);
   TEST_ASSERT_EQUAL_STRING("INBOX.Trash", r.trash);
   TEST_ASSERT_EQUAL_STRING("", r.archive);
}

static void test_list_line_edges(void) {
   email_imap_roles_t r;
   memset(&r, 0, sizeof(r));
   /* An empty or unclosed delimiter isn't a LIST line we read. */
   email_imap_roles_from_list("* LIST (\\Trash) \"\" Trash\r\n* LIST (\\Trash) \"/ Bin\r\n", NULL,
                              &r);
   TEST_ASSERT_EQUAL_STRING("", r.trash);
   /* An escaped delimiter. */
   email_imap_roles_from_list("* LIST (\\Trash) \"\\\\\" Bin\r\n", NULL, &r);
   TEST_ASSERT_EQUAL_STRING("Bin", r.trash);
}

static void test_fetch_has_uid(void) {
   TEST_ASSERT_TRUE(email_imap_fetch_has_uid("* 12 FETCH (UID 4501)\r\n", 4501));
   TEST_ASSERT_TRUE(email_imap_fetch_has_uid("* 3 FETCH (FLAGS (\\Seen) UID 7)", 7));
   TEST_ASSERT_FALSE(email_imap_fetch_has_uid("* 12 FETCH (UID 45012)\r\n", 4501));
   TEST_ASSERT_FALSE(email_imap_fetch_has_uid("", 4501));
   TEST_ASSERT_FALSE(email_imap_fetch_has_uid(NULL, 4501));
   TEST_ASSERT_FALSE(email_imap_fetch_has_uid("A1 OK UID 4501\r\n", 4501));
   /* Not inside a quoted string (a Gmail label named "x UID 7"). */
   TEST_ASSERT_FALSE(
       email_imap_fetch_has_uid("* 1 FETCH (X-GM-LABELS (\"x UID 7\") UID 9)\r\n", 7));
   TEST_ASSERT_TRUE(email_imap_fetch_has_uid("* 1 FETCH (X-GM-LABELS (\"x UID 7\") UID 9)\r\n", 9));
}

static void test_inbox_children(void) {
   TEST_ASSERT_EQUAL_CHAR('.',
                          email_imap_inbox_child_delim(
                              "* LIST (\\HasChildren) \".\" INBOX\r\n* LIST () \".\" Junk\r\n"));
   TEST_ASSERT_EQUAL_CHAR('\0',
                          email_imap_inbox_child_delim("* LIST (\\HasNoChildren) \"/\" INBOX\r\n"));
   TEST_ASSERT_EQUAL_CHAR('\0', email_imap_inbox_child_delim("* LIST () \"/\" Trash\r\n"));
   TEST_ASSERT_EQUAL_CHAR('\0', email_imap_inbox_child_delim(NULL));
}

static void test_prefix_without_delimiter(void) {
   email_imap_ns_t ns;
   email_imap_namespace_parse("* NAMESPACE ((\"INBOX\" \".\")) NIL NIL\r\n", &ns);
   TEST_ASSERT_EQUAL_STRING("INBOX.", ns.personal);
   email_imap_roles_t r;
   memset(&r, 0, sizeof(r));
   email_imap_roles_from_list("* LIST () \".\" INBOX.Trash\r\n", &ns, &r);
   TEST_ASSERT_EQUAL_STRING("INBOX.Trash", r.trash);
   /* A delimiter of more than one character isn't a namespace. */
   email_imap_namespace_parse("* NAMESPACE ((\"INBOX.\" \"ab\")) NIL NIL\r\n", &ns);
   TEST_ASSERT_EQUAL_STRING("", ns.personal);
   /* An empty delimiter is no delimiter; the exclusions still hold. */
   email_imap_namespace_parse("* NAMESPACE ((\"\" \"\")) ((\"Other Users/\" \"\")) NIL\r\n", &ns);
   TEST_ASSERT_EQUAL_INT(1, ns.n_other);
   TEST_ASSERT_EQUAL_STRING("Other Users/", ns.other[0]);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_capability);
   RUN_TEST(test_gmail);
   RUN_TEST(test_outlook_marked);
   RUN_TEST(test_unmarked_by_name);
   RUN_TEST(test_none_means_none);
   RUN_TEST(test_quoted_names_and_nil_delimiter);
   RUN_TEST(test_quote_folder);
   RUN_TEST(test_all_mail_only_on_gmail);
   RUN_TEST(test_namespace);
   RUN_TEST(test_not_another_users_folder);
   RUN_TEST(test_list_line_edges);
   RUN_TEST(test_fetch_has_uid);
   RUN_TEST(test_inbox_children);
   RUN_TEST(test_prefix_without_delimiter);
   return UNITY_END();
}
