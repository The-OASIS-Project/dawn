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
 * Messaging channels against a real database: which channel a message's
 * sender speaks for, /link (owner, wrong app, one account per person, a group
 * code used up when refused), and proving an SMS number with a texted code
 * (kept out of the SMS log, attempts and sends limited).  The engine's link
 * and channel files are linked as they are; only the driver registry and the
 * session slots are stand-ins.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#define MESSAGING_ENGINE_INTERNAL_ALLOWED

#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "core/rate_limiter.h"
#include "messaging/messaging_engine.h"
#include "messaging/messaging_engine_internal.h"
#include "unity.h"

/* --- stand-ins for the rest of the engine -------------------------------- */

atomic_bool s_initialized = true;
dawn_config_t g_config;
rate_limiter_t s_outbound_per_user_limiter;
static rate_limit_entry_t s_outbound_entries[8];

/* How many times the user's WebUI was told their channel list changed. */
static int s_channels_changed;

void webui_broadcast_messaging_channels_changed(int user_id,
                                                int64_t channel_id,
                                                const char *change,
                                                const char *link_code) {
   (void)user_id;
   (void)channel_id;
   (void)change;
   (void)link_code;
   __atomic_add_fetch(&s_channels_changed, 1, __ATOMIC_SEQ_CST);
}

void webui_broadcast_conversation_messages_appended(int user_id, int64_t conv_id) {
   (void)user_id;
   (void)conv_id;
}

int messaging_deliver(const messaging_driver_t *drv,
                      int user_id,
                      const char *provider_address,
                      const char *address_json,
                      const char *canonical_markdown) {
   return drv->send_text(user_id, provider_address, address_json, canonical_markdown);
}

void evict_session_slot(int64_t channel_id) {
   (void)channel_id;
}

bool mark_pending_reset_if_self(int64_t channel_id) {
   (void)channel_id;
   return false;
}

/* What the drivers were asked to send (the sends run on their own threads). */
static pthread_mutex_t s_sent_mutex = PTHREAD_MUTEX_INITIALIZER;
static int s_sent_count;
static char s_last_text[256];
static char s_last_log[256];

static void record(const char *text, const char *log_text) {
   pthread_mutex_lock(&s_sent_mutex);
   s_sent_count++;
   snprintf(s_last_text, sizeof(s_last_text), "%s", text ? text : "");
   snprintf(s_last_log, sizeof(s_last_log), "%s", log_text ? log_text : "");
   pthread_mutex_unlock(&s_sent_mutex);
}

static int fake_send(int user_id, const char *addr, const char *json, const char *text) {
   (void)user_id;
   (void)addr;
   (void)json;
   record(text, text);
   return 0;
}

static atomic_bool s_sms_fails =
    false; /* true: the phone service can't send; read by detached sends */

static int fake_send_unlogged(int user_id,
                              const char *addr,
                              const char *json,
                              const char *text,
                              const char *log_text) {
   (void)user_id;
   (void)addr;
   (void)json;
   record(text, log_text);
   return s_sms_fails ? 1 : 0;
}

static void fake_address_json(const char *addr, char *buf, size_t n) {
   snprintf(buf, n, "{\"id\":\"%s\"}", addr);
}

static const messaging_driver_t s_telegram = {
   .name = "telegram",
   .authenticates_sender = true,
   .out_format = MSG_FMT_PLAIN,
   .send_text = fake_send,
   .build_address_json = fake_address_json,
};

static const messaging_driver_t s_sms = {
   .name = "sms",
   .authenticates_sender = false,
   .out_format = MSG_FMT_PLAIN,
   .send_text = fake_send,
   .send_text_unlogged = fake_send_unlogged,
   .build_address_json = fake_address_json,
};

static bool s_sms_running = true; /* false: as with the phone service off */

const messaging_driver_t *find_driver(const char *name) {
   if (strcmp(name, "telegram") == 0) {
      return &s_telegram;
   }
   if (strcmp(name, "sms") == 0 && s_sms_running) {
      return &s_sms;
   }
   return NULL;
}

/* Wait for the n-th send (sends run detached). */
static void wait_sends(int n) {
   for (int i = 0; i < 200; i++) {
      pthread_mutex_lock(&s_sent_mutex);
      int c = s_sent_count;
      pthread_mutex_unlock(&s_sent_mutex);
      if (c >= n) {
         return;
      }
      usleep(5000);
   }
   TEST_FAIL_MESSAGE("expected send never happened");
}

/* --- helpers ------------------------------------------------------------- */

static int s_user_a;
static int s_user_b;

static int make_user(const char *name) {
   auth_db_create_user(name, "$argon2id$v=19$m=65536,t=3,p=4$c2FsdA$aGFzaA", false);
   auth_user_t u;
   memset(&u, 0, sizeof(u));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user(name, &u));
   return u.id;
}

static void code_for(int user, const char *hint, char code[MESSAGING_LINK_CODE_BUF_SIZE]) {
   TEST_ASSERT_EQUAL_INT(MESSAGING_SUCCESS, messaging_engine_generate_link_code(
                                                user, hint, code, MESSAGING_LINK_CODE_BUF_SIZE));
}

static int do_link(const char *provider,
                   const char *addr,
                   const char *sender,
                   messaging_chat_kind_t kind,
                   const char *code) {
   return handle_link_command(provider, addr, sender, kind, code);
}

static channel_resolve_t resolve(const char *provider,
                                 const char *addr,
                                 const char *sender,
                                 messaging_chat_kind_t kind,
                                 channel_ref_t *ref) {
   return resolve_inbound_channel(provider, addr, sender, kind, ref);
}

static int64_t channel_id_of(int user, const char *provider, const char *addr) {
   char sql[256];
   snprintf(sql, sizeof(sql),
            "SELECT id FROM messaging_channels WHERE user_id=%d AND provider='%s' AND "
            "provider_address='%s'",
            user, provider, addr);
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL));
   int64_t id = 0;
   if (sqlite3_step(st) == SQLITE_ROW) {
      id = sqlite3_column_int64(st, 0);
   }
   sqlite3_finalize(st);
   return id;
}

/* The 6 digits of the last code texted. */
static void last_code(char out[7]) {
   pthread_mutex_lock(&s_sent_mutex);
   const char *p = strstr(s_last_text, "DAWN code: ");
   TEST_ASSERT_NOT_NULL(p);
   snprintf(out, 7, "%s", p + strlen("DAWN code: "));
   pthread_mutex_unlock(&s_sent_mutex);
}

void setUp(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   rate_limiter_config_t cfg = { .max_count = 10, .window_sec = 60, .slot_count = 8 };
   rate_limiter_init(&s_outbound_per_user_limiter, s_outbound_entries, &cfg);
   s_user_a = make_user("user_a");
   s_user_b = make_user("user_b");
   s_sms_running = true;
   s_sms_fails = false;
   __atomic_store_n(&s_channels_changed, 0, __ATOMIC_SEQ_CST);
   pthread_mutex_lock(&s_sent_mutex);
   s_sent_count = 0;
   s_last_text[0] = '\0';
   s_last_log[0] = '\0';
   pthread_mutex_unlock(&s_sent_mutex);
}

void tearDown(void) {
   usleep(20000); /* let detached sends finish before the next test */
   auth_db_shutdown();
}

/* --- chat apps ----------------------------------------------------------- */

/* A group linked by A answers A only; B, in the same group, links their own
 * account and gets their own channel. */
static void test_group_owner_and_second_user(void) {
   char code[MESSAGING_LINK_CODE_BUF_SIZE];
   channel_ref_t ref;
   code_for(s_user_a, "telegram", code);
   TEST_ASSERT_EQUAL_INT(MESSAGING_SUCCESS,
                         do_link("telegram", "-500", "11", MESSAGING_CHAT_SHARED, code));

   TEST_ASSERT_EQUAL_INT(CHANNEL_RESOLVED,
                         resolve("telegram", "-500", "11", MESSAGING_CHAT_SHARED, &ref));
   TEST_ASSERT_EQUAL_INT(s_user_a, ref.user_id);
   TEST_ASSERT_EQUAL_INT(CHANNEL_NONE,
                         resolve("telegram", "-500", "22", MESSAGING_CHAT_SHARED, &ref));
   TEST_ASSERT_EQUAL_INT(CHANNEL_NONE,
                         resolve("telegram", "-500", NULL, MESSAGING_CHAT_SHARED, &ref));

   code_for(s_user_b, NULL, code);
   TEST_ASSERT_EQUAL_INT(MESSAGING_SUCCESS,
                         do_link("telegram", "-500", "22", MESSAGING_CHAT_SHARED, code));
   TEST_ASSERT_EQUAL_INT(CHANNEL_RESOLVED,
                         resolve("telegram", "-500", "22", MESSAGING_CHAT_SHARED, &ref));
   TEST_ASSERT_EQUAL_INT(s_user_b, ref.user_id);
   TEST_ASSERT_EQUAL_INT(CHANNEL_RESOLVED,
                         resolve("telegram", "-500", "11", MESSAGING_CHAT_SHARED, &ref));
   TEST_ASSERT_EQUAL_INT(s_user_a, ref.user_id);

   /* A provider that vouches for its sender keeps a public conversation. */
   int64_t conv = resolve_channel_conversation_id(&ref, "telegram", "-500", NULL);
   TEST_ASSERT_TRUE(conv > 0);
   bool is_private = true;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_is_private(conv, s_user_a, &is_private));
   TEST_ASSERT_FALSE(is_private);
}

/* The same person can't link one chat to a second DAWN account; in a group
 * the refused code is used up (others saw it). */
static void test_one_account_per_person(void) {
   char code[MESSAGING_LINK_CODE_BUF_SIZE];
   code_for(s_user_a, NULL, code);
   TEST_ASSERT_EQUAL_INT(MESSAGING_SUCCESS,
                         do_link("telegram", "-600", "11", MESSAGING_CHAT_SHARED, code));
   code_for(s_user_b, NULL, code);
   TEST_ASSERT_EQUAL_INT(MESSAGING_ALREADY_LINKED,
                         do_link("telegram", "-600", "11", MESSAGING_CHAT_SHARED, code));
   TEST_ASSERT_EQUAL_INT(MESSAGING_LINK_STATE_EXPIRED, messaging_engine_link_status(code));

   /* In a private chat the refused code stays usable. */
   code_for(s_user_a, NULL, code);
   TEST_ASSERT_EQUAL_INT(MESSAGING_SUCCESS,
                         do_link("telegram", "77", "77", MESSAGING_CHAT_ONE_TO_ONE, code));
   code_for(s_user_b, NULL, code);
   TEST_ASSERT_EQUAL_INT(MESSAGING_ALREADY_LINKED,
                         do_link("telegram", "77", "77", MESSAGING_CHAT_ONE_TO_ONE, code));
   TEST_ASSERT_EQUAL_INT(MESSAGING_LINK_STATE_PENDING, messaging_engine_link_status(code));
}

/* A code made for another app is refused (and in a group, used up). */
static void test_wrong_app(void) {
   char code[MESSAGING_LINK_CODE_BUF_SIZE];
   code_for(s_user_a, "sms", code);
   TEST_ASSERT_EQUAL_INT(MESSAGING_FAILURE,
                         do_link("telegram", "88", "88", MESSAGING_CHAT_ONE_TO_ONE, code));
   TEST_ASSERT_EQUAL_INT(MESSAGING_LINK_STATE_PENDING, messaging_engine_link_status(code));
   TEST_ASSERT_EQUAL_INT(MESSAGING_FAILURE,
                         do_link("telegram", "-700", "11", MESSAGING_CHAT_SHARED, code));
   TEST_ASSERT_EQUAL_INT(MESSAGING_LINK_STATE_EXPIRED, messaging_engine_link_status(code));
}

/* A one-to-one row with no owner (linked before owners were recorded) takes
 * its owner from its next message; a group row with no owner answers no one. */
static void test_unowned_rows(void) {
   char sql[512];
   snprintf(sql, sizeof(sql),
            "INSERT INTO messaging_channels (user_id, provider, provider_address, address_json, "
            "created_at, verified_at) VALUES (%d, 'telegram', '99', '{}', 1, 1), "
            "(%d, 'telegram', '-900', '{}', 1, 1)",
            s_user_a, s_user_a);
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(s_db.db, sql, NULL, NULL, NULL));
   channel_ref_t ref;
   TEST_ASSERT_EQUAL_INT(CHANNEL_RESOLVED,
                         resolve("telegram", "99", "99", MESSAGING_CHAT_ONE_TO_ONE, &ref));
   TEST_ASSERT_EQUAL_INT(CHANNEL_NONE,
                         resolve("telegram", "99", "98", MESSAGING_CHAT_ONE_TO_ONE, &ref));
   TEST_ASSERT_EQUAL_INT(CHANNEL_NONE,
                         resolve("telegram", "-900", "11", MESSAGING_CHAT_SHARED, &ref));
   /* No sender from a chat app: refused, nothing bound. */
   snprintf(sql, sizeof(sql),
            "INSERT INTO messaging_channels (user_id, provider, provider_address, address_json, "
            "created_at, verified_at) VALUES (%d, 'telegram', '55', '{}', 1, 1)",
            s_user_b);
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(s_db.db, sql, NULL, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(CHANNEL_NONE,
                         resolve("telegram", "55", NULL, MESSAGING_CHAT_ONE_TO_ONE, &ref));
}

/* --- SMS ----------------------------------------------------------------- */

/* /link from a number makes a pending channel and texts a code, kept out of
 * the log; the channel reaches nothing until the code is entered. */
static void test_sms_verify(void) {
   char code[MESSAGING_LINK_CODE_BUF_SIZE];
   channel_ref_t ref;
   code_for(s_user_a, "sms", code);
   TEST_ASSERT_EQUAL_INT(MESSAGING_SUCCESS,
                         do_link("sms", "+15550001111", NULL, MESSAGING_CHAT_ONE_TO_ONE, code));
   /* The panel is told at once: the pending channel and its code box appear
    * without a refresh. */
   TEST_ASSERT_EQUAL_INT(1, __atomic_load_n(&s_channels_changed, __ATOMIC_SEQ_CST));
   wait_sends(1);
   char digits[7];
   last_code(digits);
   pthread_mutex_lock(&s_sent_mutex);
   TEST_ASSERT_NULL(strstr(s_last_log, digits)); /* the code isn't what gets logged */
   pthread_mutex_unlock(&s_sent_mutex);

   TEST_ASSERT_EQUAL_INT(CHANNEL_NONE,
                         resolve("sms", "+15550001111", NULL, MESSAGING_CHAT_ONE_TO_ONE, &ref));
   int64_t id = channel_id_of(s_user_a, "sms", "+15550001111");
   TEST_ASSERT_EQUAL_INT(MESSAGING_UNKNOWN_CHANNEL,
                         messaging_engine_verify_channel(s_user_b, id, digits));
   char wrong[7];
   snprintf(wrong, sizeof(wrong), "%06d", (atoi(digits) + 1) % 1000000);
   TEST_ASSERT_EQUAL_INT(MESSAGING_BAD_CODE, messaging_engine_verify_channel(s_user_a, id, wrong));
   TEST_ASSERT_EQUAL_INT(MESSAGING_SUCCESS, messaging_engine_verify_channel(s_user_a, id, digits));
   TEST_ASSERT_EQUAL_INT(2, __atomic_load_n(&s_channels_changed, __ATOMIC_SEQ_CST));
   TEST_ASSERT_EQUAL_INT(CHANNEL_RESOLVED,
                         resolve("sms", "+15550001111", NULL, MESSAGING_CHAT_ONE_TO_ONE, &ref));
   TEST_ASSERT_EQUAL_INT(s_user_a, ref.user_id);
   TEST_ASSERT_FALSE(ref.authenticates_sender);

   /* Texts aren't learned: the channel's conversation is private, and made
    * private again if it was made public. */
   int64_t conv = resolve_channel_conversation_id(&ref, "sms", "+15550001111", NULL);
   TEST_ASSERT_TRUE(conv > 0);
   bool is_private = false;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_is_private(conv, s_user_a, &is_private));
   TEST_ASSERT_TRUE(is_private);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_set_private(conv, s_user_a, false));
   TEST_ASSERT_EQUAL_INT64(conv,
                           resolve_channel_conversation_id(&ref, "sms", "+15550001111", NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_is_private(conv, s_user_a, &is_private));
   TEST_ASSERT_TRUE(is_private);
}

/* A number's tries are counted per day, across codes: once spent, neither
 * the right code nor a new one gets through; and codes sent are limited. */
static void test_sms_limits(void) {
   char code[MESSAGING_LINK_CODE_BUF_SIZE];
   code_for(s_user_a, NULL, code);
   TEST_ASSERT_EQUAL_INT(MESSAGING_SUCCESS,
                         do_link("sms", "+15550002222", NULL, MESSAGING_CHAT_ONE_TO_ONE, code));
   wait_sends(1);
   char digits[7];
   last_code(digits);
   char wrong[7];
   snprintf(wrong, sizeof(wrong), "%06d", (atoi(digits) + 1) % 1000000);
   int64_t id = channel_id_of(s_user_a, "sms", "+15550002222");
   for (int i = 0; i < 10; i++) {
      TEST_ASSERT_EQUAL_INT(MESSAGING_BAD_CODE,
                            messaging_engine_verify_channel(s_user_a, id, wrong));
   }
   TEST_ASSERT_EQUAL_INT(MESSAGING_BAD_CODE, messaging_engine_verify_channel(s_user_a, id, digits));

   TEST_ASSERT_EQUAL_INT(MESSAGING_SUCCESS, messaging_engine_resend_verify_code(s_user_a, id));
   wait_sends(2);
   last_code(digits);
   TEST_ASSERT_EQUAL_INT(MESSAGING_BAD_CODE, messaging_engine_verify_channel(s_user_a, id, digits));
   TEST_ASSERT_EQUAL_INT(MESSAGING_SUCCESS, messaging_engine_resend_verify_code(s_user_a, id));
   TEST_ASSERT_EQUAL_INT(MESSAGING_RATE_LIMITED, messaging_engine_resend_verify_code(s_user_a, id));
   wait_sends(3);
}

/* Unlinking a number drops its proof: re-enabling it is refused, and linking
 * it again texts a new code. */
static void test_sms_unlink_drops_proof(void) {
   char code[MESSAGING_LINK_CODE_BUF_SIZE];
   code_for(s_user_a, NULL, code);
   TEST_ASSERT_EQUAL_INT(MESSAGING_SUCCESS,
                         do_link("sms", "+15550003333", NULL, MESSAGING_CHAT_ONE_TO_ONE, code));
   wait_sends(1);
   char digits[7];
   last_code(digits);
   int64_t id = channel_id_of(s_user_a, "sms", "+15550003333");
   TEST_ASSERT_EQUAL_INT(MESSAGING_SUCCESS, messaging_engine_verify_channel(s_user_a, id, digits));
   wait_sends(2);
   TEST_ASSERT_EQUAL_INT(MESSAGING_SUCCESS, messaging_engine_unlink_channel_by_id(s_user_a, id));
   TEST_ASSERT_EQUAL_INT(MESSAGING_NOT_VERIFIED,
                         messaging_engine_reenable_channel_by_id(s_user_a, id));
}

/* A refused code is used up where others can read it (a group; an SMS, which
 * crosses the phone service in the clear) and then reads as expired, not
 * linked. */
static void test_refused_code_used_up(void) {
   char code[MESSAGING_LINK_CODE_BUF_SIZE];
   code_for(s_user_a, "telegram", code);
   TEST_ASSERT_EQUAL_INT(MESSAGING_FAILURE,
                         do_link("sms", "+15550004444", NULL, MESSAGING_CHAT_ONE_TO_ONE, code));
   TEST_ASSERT_EQUAL_INT(MESSAGING_LINK_STATE_EXPIRED, messaging_engine_link_status(code));
   TEST_ASSERT_EQUAL_INT(MESSAGING_FAILURE,
                         do_link("telegram", "77", "77", MESSAGING_CHAT_ONE_TO_ONE, code));
}

/* A chat-app /link with no known sender (an anonymous admin, a forward) links
 * no one; in a group its code is used up. */
static void test_chat_link_without_sender(void) {
   char code[MESSAGING_LINK_CODE_BUF_SIZE];
   code_for(s_user_a, NULL, code);
   TEST_ASSERT_EQUAL_INT(MESSAGING_FAILURE,
                         do_link("telegram", "-800", NULL, MESSAGING_CHAT_SHARED, code));
   TEST_ASSERT_EQUAL_INT(MESSAGING_LINK_STATE_EXPIRED, messaging_engine_link_status(code));
}

static int64_t sends_of(int64_t id) {
   char sql[96];
   snprintf(sql, sizeof(sql), "SELECT verify_sends FROM messaging_channels WHERE id = %lld",
            (long long)id);
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL));
   int64_t n = -1;
   if (sqlite3_step(st) == SQLITE_ROW) {
      n = sqlite3_column_int64(st, 0);
   }
   sqlite3_finalize(st);
   return n;
}

/* Wait until the channel's send count reads n (a failed send is undone on
 * the send's own thread). */
static void wait_sends_of(int64_t id, int64_t n) {
   for (int i = 0; i < 200 && sends_of(id) != n; i++) {
      usleep(5000);
   }
   TEST_ASSERT_EQUAL_INT64(n, sends_of(id));
}

/* Without the SMS service a new code isn't issued: the current one stands and
 * no send is spent. */
static void test_resend_without_sms_service(void) {
   char code[MESSAGING_LINK_CODE_BUF_SIZE];
   code_for(s_user_a, NULL, code);
   TEST_ASSERT_EQUAL_INT(MESSAGING_SUCCESS,
                         do_link("sms", "+15550005555", NULL, MESSAGING_CHAT_ONE_TO_ONE, code));
   wait_sends(1);
   char digits[7];
   last_code(digits);
   int64_t id = channel_id_of(s_user_a, "sms", "+15550005555");
   TEST_ASSERT_EQUAL_INT64(1, sends_of(id));
   s_sms_running = false;
   TEST_ASSERT_EQUAL_INT(MESSAGING_DRIVER_NOT_REGISTERED,
                         messaging_engine_resend_verify_code(s_user_a, id));
   s_sms_running = true;
   TEST_ASSERT_EQUAL_INT64(1, sends_of(id));
   TEST_ASSERT_EQUAL_INT(MESSAGING_SUCCESS, messaging_engine_verify_channel(s_user_a, id, digits));
}

/* A code that couldn't be sent is voided, not just uncounted: resending while
 * texts fail can't buy fresh tries at codes no one received. */
static void test_failed_send_voids_code(void) {
   char code[MESSAGING_LINK_CODE_BUF_SIZE];
   code_for(s_user_a, NULL, code);
   s_sms_fails = true;
   TEST_ASSERT_EQUAL_INT(MESSAGING_SUCCESS,
                         do_link("sms", "+15550006666", NULL, MESSAGING_CHAT_ONE_TO_ONE, code));
   wait_sends(1);
   char digits[7];
   last_code(digits);
   int64_t id = channel_id_of(s_user_a, "sms", "+15550006666");
   wait_sends_of(id, 0);
   TEST_ASSERT_EQUAL_INT(MESSAGING_BAD_CODE, messaging_engine_verify_channel(s_user_a, id, digits));
   for (int i = 0; i < 4; i++) {
      TEST_ASSERT_EQUAL_INT(MESSAGING_SUCCESS, messaging_engine_resend_verify_code(s_user_a, id));
      wait_sends(2 + i);
      last_code(digits);
      wait_sends_of(id, 0);
      TEST_ASSERT_EQUAL_INT(MESSAGING_BAD_CODE,
                            messaging_engine_verify_channel(s_user_a, id, digits));
   }
}

/* A number's daily codes are shared by every account linking it. */
static void test_number_cap_across_accounts(void) {
   char code[MESSAGING_LINK_CODE_BUF_SIZE];
   code_for(s_user_a, NULL, code);
   TEST_ASSERT_EQUAL_INT(MESSAGING_SUCCESS,
                         do_link("sms", "+15550007777", NULL, MESSAGING_CHAT_ONE_TO_ONE, code));
   int64_t a = channel_id_of(s_user_a, "sms", "+15550007777");
   TEST_ASSERT_EQUAL_INT(MESSAGING_SUCCESS, messaging_engine_resend_verify_code(s_user_a, a));
   TEST_ASSERT_EQUAL_INT(MESSAGING_SUCCESS, messaging_engine_resend_verify_code(s_user_a, a));
   code_for(s_user_b, NULL, code);
   TEST_ASSERT_EQUAL_INT(MESSAGING_SUCCESS,
                         do_link("sms", "+15550007777", NULL, MESSAGING_CHAT_ONE_TO_ONE, code));
   int64_t b = channel_id_of(s_user_b, "sms", "+15550007777");
   TEST_ASSERT_EQUAL_INT64(0, sends_of(b)); /* no code: the number's 3 are spent */
   TEST_ASSERT_EQUAL_INT(MESSAGING_RATE_LIMITED, messaging_engine_resend_verify_code(s_user_b, b));
   wait_sends(3);
}

/* The forms a link code arrives in. */
static void test_link_command_forms(void) {
   TEST_ASSERT_EQUAL_STRING("ABCD2345", messaging_link_command_args("/link ABCD2345"));
   TEST_ASSERT_EQUAL_STRING("abcd2345", messaging_link_command_args("  /Link abcd2345"));
   TEST_ASSERT_EQUAL_STRING("ABCD2345", messaging_link_command_args("/link@SomeBot ABCD2345"));
   TEST_ASSERT_EQUAL_STRING("ABCD2345", messaging_link_command_args("link ABCD2345"));
   TEST_ASSERT_EQUAL_STRING("ABCD2345", messaging_link_command_args("/link\nABCD2345"));
   TEST_ASSERT_NULL(messaging_link_command_args("link to the doc is below"));
   TEST_ASSERT_NULL(messaging_link_command_args("Link received")); /* 8 letters, not a code */
   TEST_ASSERT_NULL(messaging_link_command_args("link ABCD2345 please"));
   TEST_ASSERT_NULL(messaging_link_command_args("/linked ABCD2345"));
   TEST_ASSERT_NULL(messaging_link_command_args("hello"));
}

/* A user holds only a few live link codes. */
static void test_link_code_cap(void) {
   char code[MESSAGING_LINK_CODE_BUF_SIZE];
   for (int i = 0; i < 5; i++) {
      code_for(s_user_a, NULL, code);
   }
   TEST_ASSERT_EQUAL_INT(MESSAGING_RATE_LIMITED,
                         messaging_engine_generate_link_code(s_user_a, NULL, code, sizeof(code)));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_group_owner_and_second_user);
   RUN_TEST(test_one_account_per_person);
   RUN_TEST(test_wrong_app);
   RUN_TEST(test_unowned_rows);
   RUN_TEST(test_sms_verify);
   RUN_TEST(test_sms_limits);
   RUN_TEST(test_sms_unlink_drops_proof);
   RUN_TEST(test_refused_code_used_up);
   RUN_TEST(test_chat_link_without_sender);
   RUN_TEST(test_resend_without_sms_service);
   RUN_TEST(test_failed_send_voids_code);
   RUN_TEST(test_number_cap_across_accounts);
   RUN_TEST(test_link_command_forms);
   RUN_TEST(test_link_code_cap);
   return UNITY_END();
}
