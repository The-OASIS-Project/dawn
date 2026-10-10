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
 * core_text_input_dispatch's gate on a turn's words: a turn with no words
 * runs only when it carries a prebuilt question (an image-only turn), and
 * then adds that question, saves its persisted form and calls the model.
 * Everything the dispatcher reaches is stubbed and recorded here.
 */

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_messages.h"
#include "core/conv_event.h"
#include "core/event_payload.h"
#include "core/session_history.h"
#include "core/session_manager.h"
#include "core/session_prefix.h"
#include "core/text_input_dispatch.h"
#include "llm/llm_history_kind.h"
#include "unity.h"

/* What the dispatcher did, as the stubs saw it. */
static int s_added_objects;
static int s_added_text;
static int s_llm_calls;
static char s_persisted[256];
static char s_persisted_ref[256];

/* ---- stubs ------------------------------------------------------------- */

int conv_db_add_message_ex(int64_t conv_id,
                           int user_id,
                           const char *role,
                           const char *content,
                           int64_t *msg_id_out) {
   (void)conv_id;
   (void)user_id;
   (void)role;
   snprintf(s_persisted, sizeof(s_persisted), "%s", content ? content : "");
   if (msg_id_out) {
      *msg_id_out = 7;
   }
   return AUTH_DB_SUCCESS;
}

int conv_db_add_row(int64_t conv_id, int user_id, const conv_message_row_t *row, int64_t *id_out) {
   (void)conv_id;
   (void)user_id;
   /* An ordinary question is the turn's saved row; a kinded one isn't counted. */
   const bool question = row && !row->kind;
   if (question) {
      snprintf(s_persisted, sizeof(s_persisted), "%s", row->content ? row->content : "");
      snprintf(s_persisted_ref, sizeof(s_persisted_ref), "%s",
               row->email_ref ? row->email_ref : "");
   }
   if (id_out) {
      *id_out = question ? 7 : 0;
   }
   return AUTH_DB_SUCCESS;
}

void conv_event_emit(int64_t conv_id, int user_id, const char *kind, char *payload_owned) {
   (void)conv_id;
   (void)user_id;
   (void)kind;
   free(payload_owned);
}

char *event_payload_status(bool generating) {
   (void)generating;
   return NULL;
}

void llm_history_set_kind(struct json_object *obj, message_kind_t kind) {
   (void)obj;
   (void)kind;
}

bool session_add_turn_message(session_t *session, const char *role, const char *content) {
   (void)session;
   (void)role;
   (void)content;
   s_added_text++;
   return true;
}

bool session_add_turn_message_object(session_t *session, struct json_object *message) {
   (void)session;
   s_added_objects++;
   json_object_put(message);
   return true;
}

void session_citation_stash_clear(session_t *session) {
   (void)session;
}

int session_dispatch_user_turn_ex(session_t *session,
                                  const char *user_turn_text,
                                  const char *turn_note) {
   (void)session;
   (void)user_turn_text;
   (void)turn_note;
   return 0;
}

char *session_llm_call_with_tts_no_add(session_t *session,
                                       const char *user_text,
                                       session_sentence_callback sentence_cb,
                                       void *userdata) {
   (void)session;
   (void)user_text;
   (void)sentence_cb;
   (void)userdata;
   s_llm_calls++;
   return strdup("a reply");
}

void session_prefix_question_saved(struct session *session,
                                   int64_t conv_id,
                                   int user_id,
                                   int64_t row_id) {
   (void)session;
   (void)conv_id;
   (void)user_id;
   (void)row_id;
}

void session_release_ref(struct session *session, struct json_object *obj) {
   (void)session;
   json_object_put(obj);
}

void session_stamp_last_message_id(session_t *session, const char *role, int64_t msg_id) {
   (void)session;
   (void)role;
   (void)msg_id;
}

void session_stamp_message_id(session_t *session, struct json_object *message, int64_t row_id) {
   (void)session;
   (void)message;
   (void)row_id;
}

void session_turn_mark_background(session_t *session) {
   (void)session;
}

static int s_from_visual_marks;
void session_turn_mark_from_visual(session_t *session) {
   (void)session;
   s_from_visual_marks++;
}
static int s_attach_rc;
static int s_attaches;
int session_turn_attach_email(session_t *session, const char *account, const char *message_id) {
   (void)session;
   (void)account;
   (void)message_id;
   s_attaches++;
   return s_attach_rc;
}

void session_turn_set_conversation(session_t *session, int64_t conv_id, bool may_load) {
   (void)session;
   (void)conv_id;
   (void)may_load;
}

void session_turn_set_pending(session_t *session, const char *role, const char *persist_text) {
   (void)session;
   (void)role;
   (void)persist_text;
}

/* ---- tests ------------------------------------------------------------- */

static session_t *s_session;

void setUp(void) {
   s_added_objects = 0;
   s_added_text = 0;
   s_llm_calls = 0;
   s_persisted[0] = '\0';
   s_persisted_ref[0] = '\0';
   s_session = calloc(1, sizeof(*s_session));
   TEST_ASSERT_NOT_NULL(s_session);
}

void tearDown(void) {
   free(s_session);
   s_session = NULL;
}

/* A question message with only an image part, as an image-only turn builds. */
static struct json_object *image_only_question(void) {
   struct json_object *msg = json_tokener_parse(
       "{\"role\":\"user\",\"content\":[{\"type\":\"image_url\","
       "\"image_url\":{\"url\":\"data:image/png;base64,AAAA\"}}]}");
   TEST_ASSERT_NOT_NULL(msg);
   return msg;
}

/* No words and no question: nothing happens. */
static void test_empty_text_without_a_question_is_refused(void) {
   const text_input_dispatch_opts_t opts = { .conversation_id = 3, .auth_user_id = 1 };
   TEST_ASSERT_NULL(core_text_input_dispatch(s_session, "", &opts));
   TEST_ASSERT_NULL(core_text_input_dispatch(s_session, "", NULL));
   TEST_ASSERT_EQUAL_INT(0, s_added_objects + s_added_text + s_llm_calls);
   TEST_ASSERT_EQUAL_STRING("", s_persisted);
}

/* No words but an image question: the question is added, its persisted form
 * saved, and the model called. */
static void test_image_only_turn_runs(void) {
   struct json_object *question = image_only_question();
   const text_input_dispatch_opts_t opts = { .conversation_id = 3,
                                             .auth_user_id = 1,
                                             .persist_content_override = "[IMAGE:img_aaaaaaaaaaaa]",
                                             .question_message = question };
   char *reply = core_text_input_dispatch(s_session, "", &opts);
   TEST_ASSERT_NOT_NULL(reply);
   TEST_ASSERT_EQUAL_INT(1, s_added_objects);
   TEST_ASSERT_EQUAL_INT(0, s_added_text);
   TEST_ASSERT_EQUAL_INT(1, s_llm_calls);
   TEST_ASSERT_EQUAL_STRING("[IMAGE:img_aaaaaaaaaaaa]", s_persisted);
   free(reply);
   json_object_put(question);
}

/* A visual's prompt marks its turn, so no confirm counts in it; a typed one
 * doesn't. */
static void test_from_visual_marks_the_turn(void) {
   s_from_visual_marks = 0;
   const text_input_dispatch_opts_t typed = { .conversation_id = 3, .auth_user_id = 1 };
   free(core_text_input_dispatch(s_session, "hello", &typed));
   TEST_ASSERT_EQUAL_INT(0, s_from_visual_marks);
   const text_input_dispatch_opts_t visual = { .conversation_id = 3,
                                               .auth_user_id = 1,
                                               .from_visual = true };
   free(core_text_input_dispatch(s_session, "yes", &visual));
   TEST_ASSERT_EQUAL_INT(1, s_from_visual_marks);
}

/* An attached email goes with the turn, or the turn doesn't run: nothing
 * added, saved or sent when the attach fails. */
static void test_a_failed_attach_refuses_the_turn(void) {
   s_attaches = 0;
   s_attach_rc = 1; /* FAILURE */
   const text_input_dispatch_opts_t opts = { .conversation_id = 3,
                                             .auth_user_id = 1,
                                             .email_account = "work",
                                             .email_message_id = "u42.7" };
   TEST_ASSERT_NULL(core_text_input_dispatch(s_session, "what is this?", &opts));
   TEST_ASSERT_EQUAL_INT(1, s_attaches);
   TEST_ASSERT_EQUAL_INT(0, s_added_objects + s_added_text + s_llm_calls);
   TEST_ASSERT_EQUAL_STRING("", s_persisted);
   s_attach_rc = 0;
   text_input_dispatch_opts_t saved = opts;
   saved.email_ref = "{\"account_id\":3}";
   free(core_text_input_dispatch(s_session, "what is this?", &saved));
   TEST_ASSERT_EQUAL_INT(2, s_attaches);
   TEST_ASSERT_EQUAL_INT(1, s_llm_calls);
   TEST_ASSERT_EQUAL_STRING("what is this?", s_persisted); /* the question, with its ref */
   TEST_ASSERT_EQUAL_STRING("{\"account_id\":3}", s_persisted_ref);
}

/* A NULL text is never a turn, question or not. */
static void test_null_text_is_refused(void) {
   struct json_object *question = image_only_question();
   const text_input_dispatch_opts_t opts = { .question_message = question };
   TEST_ASSERT_NULL(core_text_input_dispatch(s_session, NULL, &opts));
   TEST_ASSERT_EQUAL_INT(0, s_added_objects + s_llm_calls);
   json_object_put(question);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_empty_text_without_a_question_is_refused);
   RUN_TEST(test_image_only_turn_runs);
   RUN_TEST(test_null_text_is_refused);
   RUN_TEST(test_from_visual_marks_the_turn);
   RUN_TEST(test_a_failed_attach_refuses_the_turn);
   return UNITY_END();
}
