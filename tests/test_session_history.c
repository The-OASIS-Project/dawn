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
 * Unit tests for session_history.c: which history a turn runs on, and the
 * history's conversation binding.  A turn must never run on, or write into,
 * another conversation's history.
 */

#include <json-c/json.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "core/session_manager.h"
#include "dawn_error.h"
#include "unity.h"

static session_t *s;

/* ---- stubs ---------------------------------------------------------------- */

struct json_object *llm_history_strip_provider_state(struct json_object *history) {
   struct json_object *copy = NULL;
   json_object_deep_copy(history, &copy, NULL);
   return copy;
}

/* The fake database: conversation id -> number of stored (non-system) messages. */
static int s_stored[100];
static int s_loader_calls;
static bool s_loader_fail;

static struct json_object *make_history(int64_t conv_id, int n) {
   struct json_object *arr = json_object_new_array();
   struct json_object *sys = json_object_new_object();
   json_object_object_add(sys, "role", json_object_new_string("system"));
   json_object_object_add(sys, "content", json_object_new_string("prompt"));
   json_object_array_add(arr, sys);
   for (int i = 0; i < n; i++) {
      struct json_object *m = json_object_new_object();
      char text[64];
      snprintf(text, sizeof(text), "conv %lld msg %d", (long long)conv_id, i);
      json_object_object_add(m, "role", json_object_new_string(i % 2 ? "assistant" : "user"));
      json_object_object_add(m, "content", json_object_new_string(text));
      json_object_object_add(m, "id", json_object_new_int64(conv_id * 1000 + i));
      json_object_array_add(arr, m);
   }
   return arr;
}

/* Each stored conversation's model (its "stored LLM settings"). */
static char s_conv_model[100][32];

static struct json_object *fake_loader(int user_id,
                                       int64_t conv_id,
                                       const session_llm_config_t *base,
                                       session_llm_config_t *cfg_out,
                                       bool *has_cfg_out) {
   (void)user_id;
   *has_cfg_out = false;
   s_loader_calls++;
   if (s_loader_fail || conv_id <= 0 || conv_id >= 100) {
      return NULL;
   }
   if (s_conv_model[conv_id][0]) {
      *cfg_out = *base;
      snprintf(cfg_out->model, sizeof(cfg_out->model), "%s", s_conv_model[conv_id]);
      *has_cfg_out = true;
   }
   return make_history(conv_id, s_stored[conv_id]);
}

/* session_manager.c is not linked: its turn-token and command-context
 * accessors, and session_get_llm_config's rule (the turn's settings only to a
 * thread carrying the turn's token). */
static __thread uint64_t tl_token;
uint64_t session_turn_token(void) {
   return tl_token;
}
void session_set_turn_token(uint64_t token) {
   tl_token = token;
}
session_t *session_get_command_context(void) {
   return NULL;
}
void session_update_system_prompt_locked(session_t *session, const char *system_prompt) {
   struct json_object *first = json_object_array_get_idx(session->conversation_history, 0);
   if (first) {
      json_object_object_add(first, "content", json_object_new_string(system_prompt));
   }
}
void session_get_llm_config(session_t *session, session_llm_config_t *config) {
   pthread_mutex_lock(&session->llm_config_mutex);
   if (session->turn_llm_config_set && tl_token != 0 && tl_token == session->turn_gen) {
      *config = session->turn_llm_config;
   } else {
      *config = session->llm_config;
   }
   pthread_mutex_unlock(&session->llm_config_mutex);
}

/* The model another thread sees for @p session, with @p token set on it. */
typedef struct {
   session_t *session;
   uint64_t token;
   char model[sizeof(((session_llm_config_t *)0)->model)];
} model_probe_t;

static void *probe_model(void *arg) {
   model_probe_t *p = (model_probe_t *)arg;
   session_set_turn_token(p->token);
   session_llm_config_t cfg;
   session_get_llm_config(p->session, &cfg);
   snprintf(p->model, sizeof(p->model), "%s", cfg.model);
   return NULL;
}

static const char *model_seen_with(uint64_t token) {
   static model_probe_t p;
   memset(&p, 0, sizeof(p));
   p.session = s;
   p.token = token;
   pthread_t t;
   pthread_create(&t, NULL, probe_model, &p);
   pthread_join(t, NULL);
   return p.model;
}

/* ---- fixture -------------------------------------------------------------- */

void setUp(void) {
   memset(s_stored, 0, sizeof(s_stored));
   memset(s_conv_model, 0, sizeof(s_conv_model));
   s_loader_calls = 0;
   s_loader_fail = false;
   s = calloc(1, sizeof(*s));
   pthread_mutex_init(&s->history_mutex, NULL);
   pthread_mutex_init(&s->llm_config_mutex, NULL);
   snprintf(s->llm_config.model, sizeof(s->llm_config.model), "viewed-model");
   s->session_id = 7;
   s->conversation_history = make_history(0, 0);
   session_set_history_loader(fake_loader);
}

void tearDown(void) {
   session_turn_end(s);
   atomic_store(&s->viewed_conversation_id, 0);
   json_object_put(s->conversation_history);
   pthread_mutex_destroy(&s->history_mutex);
   pthread_mutex_destroy(&s->llm_config_mutex);
   free(s);
   s = NULL;
   session_set_history_loader(NULL);
}

static int len(struct json_object *a) {
   return a ? (int)json_object_array_length(a) : -1;
}

static struct json_object *turn(void) {
   struct json_object *h = session_get_turn_history(s);
   json_object_put(h); /* the pin keeps it alive for the test */
   return h;
}

/* The conversation the client is showing. */
static void view(int64_t conv) {
   atomic_store(&s->viewed_conversation_id, conv);
}

static int64_t bound(void) {
   return atomic_load(&s->history_conversation_id);
}

/* Session holds conversation 3 (with n stored messages). */
static void load_live(int64_t conv, int n) {
   s_stored[conv] = n;
   session_replace_history(s, make_history(conv, n), conv);
}

/* ---- tests ---------------------------------------------------------------- */

void test_turn_on_loaded_conversation_uses_live_history(void) {
   load_live(3, 2);
   session_turn_begin(s, 3, 1);
   TEST_ASSERT_EQUAL_INT(0, s_loader_calls);
   TEST_ASSERT_TRUE(turn() == s->conversation_history);
   TEST_ASSERT_TRUE(session_add_turn_message(s, "user", "hi"));
   TEST_ASSERT_EQUAL_INT(4, len(s->conversation_history));
   TEST_ASSERT_EQUAL_INT64(3, bound());
}

void test_turn_for_other_conversation_runs_on_its_own_history(void) {
   load_live(3, 2); /* the user is viewing 3 */
   s_stored[5] = 4;
   session_turn_begin(s, 5, 1);
   TEST_ASSERT_EQUAL_INT(1, s_loader_calls);
   TEST_ASSERT_TRUE(turn() != s->conversation_history);
   TEST_ASSERT_EQUAL_INT(5, len(turn())); /* its own context: system + 4 */

   session_add_turn_message(s, "user", "for five");
   session_add_turn_message(s, "assistant", "reply for five");
   TEST_ASSERT_EQUAL_INT(7, len(turn()));
   /* The conversation being viewed is untouched, and still bound to it. */
   TEST_ASSERT_EQUAL_INT(3, len(s->conversation_history));
   TEST_ASSERT_EQUAL_INT64(3, bound());
   pthread_mutex_lock(&s->history_mutex);
   TEST_ASSERT_TRUE(session_turn_on_own_history_locked(s));
   pthread_mutex_unlock(&s->history_mutex);
}

void test_fresh_chat_adopts_new_conversation(void) {
   /* Fresh session (bound 0, system prompt only); conversation 8 has no messages. */
   session_turn_begin(s, 8, 1);
   TEST_ASSERT_TRUE(turn() == s->conversation_history);
   TEST_ASSERT_EQUAL_INT64(8, bound());
}

void test_fresh_chat_does_not_adopt_conversation_with_history(void) {
   /* "New chat" cleared the history, then a queued turn for 5 (which has
    * messages) dequeues: it must run on 5's context, not on the empty chat. */
   s_stored[5] = 2;
   session_turn_begin(s, 5, 1);
   TEST_ASSERT_TRUE(turn() != s->conversation_history);
   TEST_ASSERT_EQUAL_INT(3, len(turn()));
   TEST_ASSERT_EQUAL_INT64(0, bound());
   session_add_turn_message(s, "user", "q");
   TEST_ASSERT_EQUAL_INT(1, len(s->conversation_history)); /* new chat untouched */
}

void test_load_failure_runs_on_empty_private_history(void) {
   load_live(3, 2);
   s_loader_fail = true;
   session_turn_begin(s, 5, 1);
   TEST_ASSERT_TRUE(turn() != s->conversation_history);
   TEST_ASSERT_EQUAL_INT(0, len(turn()));
   session_add_turn_message(s, "user", "q");
   TEST_ASSERT_EQUAL_INT(3, len(s->conversation_history));
}

void test_late_conversation_on_fresh_chat_binds(void) {
   /* Fresh-chat first message: the turn starts before the row exists. */
   session_turn_begin(s, 0, 1);
   session_add_turn_message(s, "user", "first message");
   session_turn_set_conversation(s, 9, true);
   TEST_ASSERT_EQUAL_INT64(9, bound());
   TEST_ASSERT_TRUE(turn() == s->conversation_history);
}

void test_late_conversation_before_any_append_moves_to_its_own(void) {
   /* A voice turn resolves its conversation (5) while the session holds 3. */
   load_live(3, 2);
   s_stored[5] = 2;
   session_turn_begin(s, 0, 1);
   session_turn_set_conversation(s, 5, true);
   TEST_ASSERT_TRUE(turn() != s->conversation_history);
   TEST_ASSERT_EQUAL_INT(3, len(turn()));
   session_add_turn_message(s, "user", "for five");
   TEST_ASSERT_EQUAL_INT(3, len(s->conversation_history));
   TEST_ASSERT_EQUAL_INT64(3, bound());
}

void test_late_conversation_after_writing_elsewhere_marks_mixed(void) {
   load_live(3, 2);
   session_turn_begin(s, 0, 1);
   session_add_turn_message(s, "user", "written into three");
   session_turn_set_conversation(s, 5, true);
   TEST_ASSERT_EQUAL_INT64(SESSION_HISTORY_CONV_MIXED, bound());
}

void test_reply_mirrors_into_reloaded_live_history(void) {
   /* Turn for 5 runs privately; the user opens 5 meanwhile (reload from the DB,
    * which doesn't have the reply yet): the reply must reach that copy too. */
   load_live(3, 2);
   s_stored[5] = 2;
   session_turn_begin(s, 5, 1);
   session_add_turn_message(s, "user", "q");
   load_live(5, 3); /* reload includes the persisted user message */
   session_add_turn_message(s, "assistant", "answer");
   TEST_ASSERT_EQUAL_INT(5, len(s->conversation_history));
   struct json_object *last = json_object_array_get_idx(s->conversation_history, 4);
   struct json_object *c = NULL;
   json_object_object_get_ex(last, "content", &c);
   TEST_ASSERT_EQUAL_STRING("answer", json_object_get_string(c));
}

void test_assistant_id_stamps_only_final_entry(void) {
   load_live(3, 0);
   session_turn_begin(s, 3, 1);
   session_add_turn_message(s, "user", "q");
   session_add_turn_message(s, "assistant", "earlier reply");
   view(3);
   session_turn_end(s);
   session_turn_begin(s, 3, 1);
   session_add_turn_message(s, "user", "q2");
   /* The reply for q2 never reached history (cancelled): don't stamp the older one. */
   session_stamp_last_message_id(s, "assistant", 777);
   struct json_object *older = json_object_array_get_idx(s->conversation_history, 2);
   TEST_ASSERT_FALSE(json_object_object_get_ex(older, "id", NULL));
}

void test_reply_stamped_past_a_later_notice(void) {
   /* A message lands after the reply, before it is saved: the reply still gets
    * its row id. */
   load_live(3, 0);
   session_turn_begin(s, 3, 1);
   session_add_turn_message(s, "user", "q");
   session_add_turn_message(s, "assistant", "reply");
   session_add_message(s, "system", "incoming call");
   session_stamp_last_message_id(s, "assistant", 555);
   struct json_object *reply = json_object_array_get_idx(s->conversation_history, 2);
   struct json_object *id = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(reply, "id", &id));
   TEST_ASSERT_EQUAL_INT64(555, json_object_get_int64(id));
}

void test_turn_end_gives_reloaded_history_the_reply(void) {
   /* Turn for 5 on its own copy; after the reply was appended the user opened 5
    * (reload from the database, which lacks the reply): the reply reaches it. */
   load_live(3, 2);
   s_stored[5] = 2;
   session_turn_begin(s, 5, 1);
   session_add_turn_message(s, "user", "q");
   session_add_turn_message(s, "assistant", "answer");
   load_live(5, 3); /* has the user message, not the reply */
   view(5);
   session_turn_end(s);
   TEST_ASSERT_EQUAL_INT(5, len(s->conversation_history));
   struct json_object *last = json_object_array_get_idx(s->conversation_history, 4);
   struct json_object *c = NULL;
   json_object_object_get_ex(last, "content", &c);
   TEST_ASSERT_EQUAL_STRING("answer", json_object_get_string(c));
}

void test_turn_end_adopts_copy_when_live_holds_another(void) {
   /* A failed restore left 3 in the session history while 5 is on screen: the
    * turn's copy of 5 becomes the session history. */
   load_live(3, 2);
   s_stored[5] = 2;
   session_turn_begin(s, 5, 1);
   session_add_turn_message(s, "user", "q");
   view(5);
   session_turn_end(s);
   TEST_ASSERT_EQUAL_INT64(5, bound());
   TEST_ASSERT_EQUAL_INT(4, len(s->conversation_history));
}

void test_turn_settings_fixed_at_begin(void) {
   /* A sidebar load mid-turn changes the session's model, not the turn's. */
   load_live(3, 2);
   session_turn_begin(s, 3, 1);
   pthread_mutex_lock(&s->llm_config_mutex);
   snprintf(s->llm_config.model, sizeof(s->llm_config.model), "other-model");
   pthread_mutex_unlock(&s->llm_config_mutex);
   session_llm_config_t cfg;
   session_get_llm_config(s, &cfg);
   TEST_ASSERT_EQUAL_STRING("viewed-model", cfg.model);
}

void test_turn_settings_follow_the_token_to_other_threads(void) {
   load_live(3, 2);
   s_stored[5] = 1;
   snprintf(s_conv_model[5], sizeof(s_conv_model[5]), "five-model");
   session_turn_begin(s, 5, 1);
   /* A tool thread the turn spawned carries its token; any other thread doesn't. */
   TEST_ASSERT_EQUAL_STRING("five-model", model_seen_with(session_turn_token()));
   TEST_ASSERT_EQUAL_STRING("viewed-model", model_seen_with(0));
}

void test_load_without_stored_settings_uses_the_sessions(void) {
   /* Moving from 5 (stored model) to 6 (none) must not keep 5's model. */
   load_live(3, 2);
   s_stored[5] = 1;
   s_stored[6] = 1;
   snprintf(s_conv_model[5], sizeof(s_conv_model[5]), "five-model");
   session_turn_begin(s, 5, 1);
   pthread_mutex_lock(&s->history_mutex);
   s->turn_history_conv = 0; /* as if unresolved, so it can move */
   s->turn_pin_conv = 5;
   pthread_mutex_unlock(&s->history_mutex);
   session_turn_set_conversation(s, 6, true);
   session_llm_config_t cfg;
   session_get_llm_config(s, &cfg);
   TEST_ASSERT_EQUAL_STRING("viewed-model", cfg.model);
}

void test_off_thread_conversation_tags_the_stream(void) {
   load_live(3, 2);
   session_turn_begin(s, 0, 1);
   session_turn_set_conversation(s, 5, false);
   TEST_ASSERT_EQUAL_INT64(5, session_turn_conversation(s));
   TEST_ASSERT_EQUAL_INT64(5, atomic_load(&s->stream_conversation_id));
}

/* The handler creating a new chat's conversation: whether a running turn
 * adopted it (any claimed exchange is discarded). */
static bool adopt(int64_t conv) {
   char *user = NULL, *reply = NULL;
   bool adopted = false;
   (void)session_bind_created_conversation(s, conv, &user, &reply, &adopted);
   free(user);
   free(reply);
   return adopted;
}

static bool claim(int64_t conv, char **user, char **reply) {
   bool adopted = false;
   return session_bind_created_conversation(s, conv, user, reply, &adopted);
}

void test_adopt_only_a_waiting_turn(void) {
   session_turn_begin(s, 0, 1);
   TEST_ASSERT_FALSE(adopt(9)); /* a voice turn, say */
   session_turn_await_conversation(s);
   TEST_ASSERT_TRUE(adopt(9));
   TEST_ASSERT_EQUAL_INT64(9, session_turn_conversation(s));
   TEST_ASSERT_FALSE(adopt(10)); /* never clobbered */
}

void test_snapshot_is_a_copy_with_its_binding(void) {
   load_live(3, 2);
   int64_t conv = 0;
   int count = 0;
   struct json_object *snap = session_snapshot_history(s, &conv, &count);
   TEST_ASSERT_NOT_NULL(snap);
   TEST_ASSERT_TRUE(snap != s->conversation_history);
   TEST_ASSERT_EQUAL_INT64(3, conv);
   TEST_ASSERT_EQUAL_INT(3, count);
   json_object_put(snap);
}

void test_clear_unbinds(void) {
   load_live(3, 2);
   session_clear_history(s);
   TEST_ASSERT_EQUAL_INT64(0, bound());
   TEST_ASSERT_EQUAL_INT(0, len(s->conversation_history));
}

void test_failed_load_never_binds_fresh_history(void) {
   /* New chat (fresh, unbound) while a turn for 5 dequeues and its load fails:
    * the fresh history must not become 5's (its context would be lost for good). */
   s_stored[5] = 3;
   s_loader_fail = true;
   session_turn_begin(s, 5, 1);
   TEST_ASSERT_TRUE(turn() != s->conversation_history);
   TEST_ASSERT_EQUAL_INT64(0, bound());
}

void test_late_conversation_carries_the_turns_messages(void) {
   /* Turn began unresolved on 3's history, wrote its message, then resolved to 5
    * on its own thread: it moves to 5's history with its message, and 3's
    * history (which now holds a message from 5's turn) is marked mixed. */
   load_live(3, 2);
   s_stored[5] = 2;
   session_turn_begin(s, 0, 1);
   session_add_turn_message(s, "user", "for five");
   session_turn_set_conversation(s, 5, true);
   TEST_ASSERT_EQUAL_INT64(SESSION_HISTORY_CONV_MIXED, bound());
   TEST_ASSERT_TRUE(turn() != s->conversation_history);
   TEST_ASSERT_EQUAL_INT(4, len(turn())); /* system + 2 of five's + the carried message */
   struct json_object *last = json_object_array_get_idx(turn(), 3);
   struct json_object *c = NULL;
   json_object_object_get_ex(last, "content", &c);
   TEST_ASSERT_EQUAL_STRING("for five", json_object_get_string(c));
}

void test_late_conversation_off_thread_loads_nothing(void) {
   load_live(3, 2);
   s_stored[5] = 2;
   session_turn_begin(s, 0, 1);
   session_turn_set_conversation(s, 5, false); /* e.g. the WebSocket thread */
   TEST_ASSERT_EQUAL_INT(0, s_loader_calls);
   TEST_ASSERT_TRUE(turn() == s->conversation_history);
   /* The turn's own thread still resolves it. */
   session_turn_set_conversation(s, 5, true);
   TEST_ASSERT_EQUAL_INT(1, s_loader_calls);
   TEST_ASSERT_TRUE(turn() != s->conversation_history);
}

void test_live_swapped_before_resolution_is_not_trusted(void) {
   /* Turn began unresolved on private 3's history; the user opened 7 (history
    * replaced) and the turn then resolves to 7.  The array it pinned still holds
    * 3's messages, so it must move onto 7's own history. */
   load_live(3, 2);
   s_stored[7] = 1;
   session_turn_begin(s, 0, 1);
   load_live(7, 1);
   session_turn_set_conversation(s, 7, true);
   TEST_ASSERT_EQUAL_INT(2, len(turn())); /* 7's system + 1, none of 3's */
}

void test_set_conversation_outside_a_turn_is_a_noop(void) {
   load_live(3, 2);
   session_turn_set_conversation(s, 5, true);
   TEST_ASSERT_EQUAL_INT(0, s_loader_calls);
   TEST_ASSERT_NULL(s->turn_history);
   TEST_ASSERT_EQUAL_INT64(3, bound());
}

void test_turn_end_adopts_copy_over_unattributable_history(void) {
   /* The session history holds turns but no conversation (a turn whose
    * conversation was never created); a turn for 5, the conversation now on
    * screen, ran on its own copy: that copy becomes the session history. */
   session_turn_begin(s, 0, 1);
   session_add_turn_message(s, "user", "hello");
   session_turn_end(s);
   TEST_ASSERT_EQUAL_INT64(0, bound());
   s_stored[5] = 2;
   session_turn_begin(s, 5, 1);
   TEST_ASSERT_TRUE(turn() != s->conversation_history);
   session_add_turn_message(s, "user", "q");
   view(5);
   session_turn_end(s);
   TEST_ASSERT_EQUAL_INT64(5, bound());
   TEST_ASSERT_EQUAL_INT(4, len(s->conversation_history));
   /* The next turn for 5 needs no reload. */
   s_loader_calls = 0;
   session_turn_begin(s, 5, 1);
   TEST_ASSERT_EQUAL_INT(0, s_loader_calls);
}

void test_own_copy_turn_uses_its_conversations_model_on_its_thread(void) {
   load_live(3, 2);
   s_stored[5] = 1;
   snprintf(s_conv_model[5], sizeof(s_conv_model[5]), "five-model");
   session_turn_begin(s, 5, 1);
   session_llm_config_t cfg;
   session_get_llm_config(s, &cfg);
   TEST_ASSERT_EQUAL_STRING("five-model", cfg.model);
   session_turn_end(s);
   session_get_llm_config(s, &cfg);
   TEST_ASSERT_EQUAL_STRING("viewed-model", cfg.model);
}

void test_pending_user_waits_for_its_conversation(void) {
   session_turn_begin(s, 0, 1);
   session_turn_await_conversation(s);
   session_turn_set_pending(s, "user", "first [IMAGE:img_1]");
   int64_t conv = -1;
   TEST_ASSERT_NULL(session_turn_take_pending(s, "user", &conv)); /* no conversation yet */
   TEST_ASSERT_TRUE(adopt(9));
   char *t = session_turn_take_pending(s, "user", &conv);
   TEST_ASSERT_EQUAL_STRING("first [IMAGE:img_1]", t);
   TEST_ASSERT_EQUAL_INT64(9, conv);
   free(t);
   TEST_ASSERT_NULL(session_turn_take_pending(s, "user", &conv)); /* taken once */
}

static int64_t s_other_token;
static void *append_from_other_thread(void *arg) {
   (void)arg;
   session_set_turn_token(s_other_token);
   session_add_message(s, "system", "stray");
   return NULL;
}

void test_other_thread_never_writes_the_turns_history(void) {
   /* Only the turn writes the history it is serializing: a message from another
    * thread is refused, never slipped between a tool call and its result. */
   load_live(3, 2);
   session_turn_begin(s, 3, 1);
   session_add_turn_message(s, "user", "q");
   s_other_token = 0;
   pthread_t t;
   pthread_create(&t, NULL, append_from_other_thread, NULL);
   pthread_join(t, NULL);
   session_add_turn_message(s, "assistant", "a");
   view(3);
   session_turn_end(s);
   TEST_ASSERT_EQUAL_INT(5, len(s->conversation_history));
}

static char *render_for(int viewer) {
   pthread_mutex_lock(&s->history_mutex);
   char *out = session_render_notices_locked(s, viewer);
   pthread_mutex_unlock(&s->history_mutex);
   return out;
}

static char *render(void) {
   pthread_mutex_lock(&s->history_mutex);
   char *out = session_render_notices_locked(s, 0);
   pthread_mutex_unlock(&s->history_mutex);
   return out;
}

void test_notices_render_newest_kept_and_expire(void) {
   char *none = render();
   TEST_ASSERT_EQUAL_STRING("", none);
   free(none);

   session_post_notice(s, "[incoming call from Alice]");
   char *one = render();
   TEST_ASSERT_NOT_NULL(strstr(one, "(just now) [incoming call from Alice]"));
   free(one);

   /* Full: the oldest goes. */
   for (int i = 0; i < SESSION_NOTICES_MAX; i++) {
      char text[32];
      snprintf(text, sizeof(text), "event %d", i);
      session_post_notice(s, text);
   }
   TEST_ASSERT_EQUAL_INT(SESSION_NOTICES_MAX, s->notice_count);
   char *full = render();
   TEST_ASSERT_NULL(strstr(full, "Alice"));
   TEST_ASSERT_NOT_NULL(strstr(full, "event 0"));
   free(full);

   /* Older than the TTL: dropped when rendered. */
   s->notices[0].at -= SESSION_NOTICE_TTL_SEC;
   s->notices[1].at -= 5 * 60;
   char *aged = render();
   TEST_ASSERT_NULL(strstr(aged, "event 0"));
   TEST_ASSERT_NOT_NULL(strstr(aged, "(5 min ago) event 1"));
   TEST_ASSERT_EQUAL_INT(SESSION_NOTICES_MAX - 1, s->notice_count);
   free(aged);
}

void test_notices_show_only_the_household_and_the_viewers(void) {
   session_post_notice(s, "household event");
   session_post_notice_for(s, "alert for five", 5);
   session_post_notice_for(s, "alert for seven", 7);

   char *five = render_for(5);
   TEST_ASSERT_NOT_NULL(strstr(five, "household event"));
   TEST_ASSERT_NOT_NULL(strstr(five, "alert for five"));
   TEST_ASSERT_NULL(strstr(five, "alert for seven"));
   free(five);

   /* A guest surface (no user) sees only the household's. */
   char *guest = render_for(0);
   TEST_ASSERT_NOT_NULL(strstr(guest, "household event"));
   TEST_ASSERT_NULL(strstr(guest, "alert for"));
   free(guest);

   /* Nothing of the viewer's and no household events: nothing at all. */
   s->notice_count = 0;
   session_post_notice_for(s, "alert for seven", 7);
   char *none = render_for(5);
   TEST_ASSERT_EQUAL_STRING("", none);
   free(none);
   TEST_ASSERT_EQUAL_INT(1, s->notice_count); /* kept, not shown */
}

void test_full_notices_drop_a_previous_owners_first(void) {
   session_post_notice_for(s, "previous owner's", 7);
   for (int i = 1; i < SESSION_NOTICES_MAX; i++) {
      session_post_notice(s, "household");
   }
   TEST_ASSERT_EQUAL_INT(SESSION_NOTICES_MAX, s->notice_count);
   /* A notice for the surface's new user makes room from the old owner's,
    * even though it is not the oldest. */
   s->notices[0].at -= 60;
   session_post_notice_for(s, "new owner's", 5);
   char *out = render_for(5);
   TEST_ASSERT_NULL(strstr(out, "previous owner's"));
   TEST_ASSERT_NOT_NULL(strstr(out, "new owner's"));
   free(out);
   TEST_ASSERT_EQUAL_INT(SESSION_NOTICES_MAX, s->notice_count);
   int household = 0;
   for (int i = 0; i < s->notice_count; i++) {
      household += s->notices[i].user_id == 0;
   }
   TEST_ASSERT_EQUAL_INT(SESSION_NOTICES_MAX - 1, household); /* none of them lost */
}

void test_notice_truncated_on_a_character_boundary(void) {
   char text[SESSION_NOTICE_TEXT_MAX + 8];
   memset(text, 'a', sizeof(text) - 1);
   text[sizeof(text) - 1] = '\0';
   /* A 2-byte character straddling the cut. */
   text[SESSION_NOTICE_TEXT_MAX - 2] = (char)0xC3;
   text[SESSION_NOTICE_TEXT_MAX - 1] = (char)0xA9;
   session_post_notice(s, text);
   const size_t n = strlen(s->notices[0].text);
   TEST_ASSERT_TRUE(n < SESSION_NOTICE_TEXT_MAX);
   TEST_ASSERT_NOT_EQUAL((unsigned char)0xC3, (unsigned char)s->notices[0].text[n - 1]);
}

static void *end_turn_from_other_thread(void *arg) {
   (void)arg;
   session_turn_end(s);
   return NULL;
}

void test_only_the_turns_thread_ends_it(void) {
   load_live(3, 2);
   session_turn_begin(s, 3, 1);
   pthread_t t;
   pthread_create(&t, NULL, end_turn_from_other_thread, NULL);
   pthread_join(t, NULL);
   TEST_ASSERT_TRUE(s->turn_active);
}

void test_ended_turn_exchange_is_claimed_by_its_conversation(void) {
   /* The first message of a new chat, whose turn failed before the client
    * created the conversation. */
   session_turn_begin(s, 0, 1);
   session_turn_await_conversation(s);
   session_turn_set_pending(s, "user", "hello");
   session_turn_set_pending(s, "assistant", "hi there");
   session_turn_end(s);
   char *user = NULL, *reply = NULL;
   TEST_ASSERT_TRUE(claim(9, &user, &reply));
   TEST_ASSERT_EQUAL_STRING("hello", user);
   TEST_ASSERT_EQUAL_STRING("hi there", reply);
   free(user);
   free(reply);
   TEST_ASSERT_EQUAL_INT64(9, bound());
   TEST_ASSERT_FALSE(claim(9, &user, &reply)); /* once */
}

void test_turn_end_prefers_copy_over_reload_with_nothing_newer(void) {
   /* Turn for 5 on its own copy wrote a tool exchange and a reply; the user
    * opened 5 meanwhile (a reload from the database, which has none of it). */
   load_live(3, 2);
   s_stored[5] = 2;
   session_turn_begin(s, 5, 1);
   session_add_turn_message(s, "user", "q");
   session_add_turn_message(s, "assistant", "answer");
   load_live(5, 2);
   view(5);
   session_turn_end(s);
   TEST_ASSERT_EQUAL_INT(5, len(s->conversation_history)); /* system + 2 + q + answer */
}

void test_finish_returns_what_was_pending_when_conversation_arrives_late(void) {
   /* The conversation is created for the turn right before it ends: the turn
    * hands its pending messages back instead of dropping them. */
   session_turn_begin(s, 0, 1);
   session_turn_await_conversation(s);
   session_turn_set_pending(s, "user", "hello");
   session_turn_set_pending(s, "assistant", "hi");
   TEST_ASSERT_TRUE(adopt(9));
   session_turn_unsaved_t u;
   /* Handed back with the turn still open, so their ids stamp its history. */
   TEST_ASSERT_EQUAL_INT(SESSION_TURN_WRITE_UNSAVED, session_turn_finish(s, &u));
   TEST_ASSERT_TRUE(s->turn_active);
   TEST_ASSERT_EQUAL_INT64(9, u.conv);
   TEST_ASSERT_EQUAL_STRING("hello", u.user);
   TEST_ASSERT_EQUAL_STRING("hi", u.reply);
   free(u.user);
   free(u.reply);
   TEST_ASSERT_EQUAL_INT(SESSION_TURN_ENDED, session_turn_finish(s, &u));
   TEST_ASSERT_FALSE(s->turn_active);
   TEST_ASSERT_NULL(u.user);
   TEST_ASSERT_NULL(u.reply);
}

void test_new_context_drops_unclaimed_exchange(void) {
   session_turn_begin(s, 0, 1);
   session_turn_await_conversation(s);
   session_turn_set_pending(s, "user", "abandoned");
   session_turn_end(s);
   session_clear_history(s); /* "New chat" */
   char *user = NULL, *reply = NULL;
   TEST_ASSERT_FALSE(claim(9, &user, &reply));
}

void test_background_turn_is_not_user_originated(void) {
   session_turn_begin(s, 0, 1);
   TEST_ASSERT_TRUE(session_turn_user_originated(s));
   session_turn_mark_background(s);
   TEST_ASSERT_FALSE(session_turn_user_originated(s));
   session_turn_end(s);
   TEST_ASSERT_FALSE(session_turn_user_originated(s)); /* no turn: no one asked */
}

static bool s_other_originated;
static void *originated_on_other_thread(void *arg) {
   (void)arg;
   s_other_originated = session_turn_user_originated(s);
   return NULL;
}

void test_only_the_turns_own_code_is_user_originated(void) {
   session_turn_begin(s, 0, 1);
   pthread_t t;
   pthread_create(&t, NULL, originated_on_other_thread, NULL);
   pthread_join(t, NULL);
   TEST_ASSERT_FALSE(s_other_originated);
   TEST_ASSERT_TRUE(session_turn_user_originated(s));
}

void test_carried_messages_are_copies(void) {
   /* A message the turn wrote into another conversation's live history before
    * moving onto its own is copied, never shared between the two arrays. */
   load_live(3, 2);
   s_stored[5] = 2;
   session_turn_begin(s, 0, 1);
   session_add_turn_message(s, "user", "for five");
   struct json_object *original = json_object_array_get_idx(s->conversation_history, 3);
   session_turn_set_conversation(s, 5, true);
   struct json_object *carried = json_object_array_get_idx(turn(), 3);
   TEST_ASSERT_TRUE(carried != original);
}

void test_claimed_binding_gives_the_pin_its_identity(void) {
   /* An ended turn's exchange is claimed for 9 while a new turn waits on the
    * same live history: adopting 9 records it on the pin, not a move. */
   session_turn_begin(s, 0, 1);
   session_turn_await_conversation(s);
   session_add_turn_message(s, "user", "first");
   session_turn_set_pending(s, "user", "first");
   session_turn_end(s);
   session_turn_begin(s, 0, 1);
   session_turn_await_conversation(s);
   char *user = NULL, *reply = NULL;
   bool adopted = false;
   /* One step: claimed and adopted together.  The adopting turn writes the
    * claimed exchange, ahead of its own rows. */
   TEST_ASSERT_FALSE(session_bind_created_conversation(s, 9, &user, &reply, &adopted));
   TEST_ASSERT_TRUE(adopted);
   TEST_ASSERT_NULL(user);
   TEST_ASSERT_EQUAL_INT64(9, session_history_conversation_of(s, turn()));
   TEST_ASSERT_TRUE(turn() == s->conversation_history);
   int64_t conv = 0;
   TEST_ASSERT_TRUE(session_turn_take_prior(s, &conv, &user, &reply));
   TEST_ASSERT_EQUAL_INT64(9, conv);
   TEST_ASSERT_EQUAL_STRING("first", user);
   free(user);
   free(reply);
   session_stamp_claimed(s, 77, 0); /* this turn's own thread: stamped now */
   struct json_object *id = NULL;
   TEST_ASSERT_TRUE(
       json_object_object_get_ex(json_object_array_get_idx(s->conversation_history, 1), "id", &id));
   TEST_ASSERT_EQUAL_INT64(77, json_object_get_int64(id));
}

static void *stamp_claimed_from_other_thread(void *arg) {
   (void)arg;
   session_stamp_claimed(s, 201, 202);
   return NULL;
}

void test_claimed_rows_stamped_by_the_turn_reading_them(void) {
   /* The client's handler stamps a claimed exchange while another turn runs on
    * the same history: the turn stamps it at its end (it reads those messages
    * without the lock). */
   load_live(0, 0);
   session_turn_begin(s, 0, 1);
   session_turn_await_conversation(s);
   session_add_turn_message(s, "user", "first");
   session_turn_set_pending(s, "user", "first");
   session_turn_end(s);
   session_turn_begin(s, 0, 1); /* a voice turn: not waiting for a conversation */
   char *user = NULL, *reply = NULL;
   TEST_ASSERT_TRUE(claim(9, &user, &reply));
   free(user);
   free(reply);
   pthread_t t;
   pthread_create(&t, NULL, stamp_claimed_from_other_thread, NULL);
   pthread_join(t, NULL);
   struct json_object *first = json_object_array_get_idx(s->conversation_history, 1);
   TEST_ASSERT_FALSE(json_object_object_get_ex(first, "id", NULL)); /* not yet */
   session_turn_end(s);
   struct json_object *id = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(first, "id", &id));
   TEST_ASSERT_EQUAL_INT64(201, json_object_get_int64(id));
}

/* ---- facts saved before their conversation exists ---- */

static int s_recorded;
static int64_t s_recorded_conv;
static void record_fact(const session_fact_source_t *facts, int count, int64_t conv_id) {
   (void)facts;
   s_recorded += count;
   s_recorded_conv = conv_id;
}
static int defer_fact(int64_t fact_id) {
   int64_t conv = 0;
   return session_defer_fact_source(s, fact_id, 1, true, &conv);
}

static void reset_recorder(void) {
   s_recorded = 0;
   s_recorded_conv = 0;
   session_set_fact_source_hook(record_fact);
}

void test_new_chat_fact_recorded_when_turn_adopts_conversation(void) {
   reset_recorder();
   session_turn_begin(s, 0, 1);
   session_turn_await_conversation(s);
   TEST_ASSERT_EQUAL_INT(SESSION_FACT_SOURCE_QUEUED, defer_fact(41));
   TEST_ASSERT_TRUE(adopt(9));
   TEST_ASSERT_EQUAL_INT(1, s_recorded);
   TEST_ASSERT_EQUAL_INT64(9, s_recorded_conv);
}

void test_ended_turn_fact_recorded_when_exchange_claimed(void) {
   reset_recorder();
   session_turn_begin(s, 0, 1);
   session_turn_await_conversation(s);
   session_add_turn_message(s, "user", "remember my code");
   session_turn_set_pending(s, "user", "remember my code");
   TEST_ASSERT_EQUAL_INT(SESSION_FACT_SOURCE_QUEUED, defer_fact(42));
   session_turn_end(s);
   TEST_ASSERT_EQUAL_INT(0, s_recorded);
   char *user = NULL, *reply = NULL;
   TEST_ASSERT_TRUE(claim(9, &user, &reply));
   free(user);
   free(reply);
   TEST_ASSERT_EQUAL_INT(1, s_recorded);
   TEST_ASSERT_EQUAL_INT64(9, s_recorded_conv);
}

void test_fact_of_a_turn_without_a_conversation_is_not_left_behind(void) {
   /* A turn with no conversation of its own on a history that already is one
    * (a voice turn on the open conversation) ends without one: nothing to
    * record, and nothing left for a later conversation to take. */
   reset_recorder();
   load_live(3, 2);
   session_turn_begin(s, 0, 1);
   TEST_ASSERT_EQUAL_INT(SESSION_FACT_SOURCE_QUEUED, defer_fact(43));
   session_turn_end(s);
   TEST_ASSERT_EQUAL_INT(0, s->pending_fact_source_count);
}

void test_turn_moving_to_its_conversation_records_its_facts(void) {
   /* The live history holds an earlier exchange, so the turn moves onto its
    * own conversation rather than binding the live one: its facts go with it. */
   reset_recorder();
   session_add_message(s, "user", "earlier");
   session_turn_begin(s, 0, 1);
   session_add_turn_message(s, "user", "remember this");
   TEST_ASSERT_EQUAL_INT(SESSION_FACT_SOURCE_QUEUED, defer_fact(47));
   session_turn_set_conversation(s, 9, false);
   TEST_ASSERT_EQUAL_INT(1, s_recorded);
   TEST_ASSERT_EQUAL_INT64(9, s_recorded_conv);
}

void test_turn_keeps_its_facts_across_a_context_reset(void) {
   reset_recorder();
   session_turn_begin(s, 0, 1);
   TEST_ASSERT_EQUAL_INT(SESSION_FACT_SOURCE_QUEUED, defer_fact(48));
   session_clear_history(s); /* a reconnect restore, say */
   session_turn_set_conversation(s, 9, false);
   TEST_ASSERT_EQUAL_INT(1, s_recorded);
}

void test_restated_fact_waits_once(void) {
   reset_recorder();
   session_turn_begin(s, 0, 1);
   TEST_ASSERT_EQUAL_INT(SESSION_FACT_SOURCE_QUEUED, defer_fact(49));
   TEST_ASSERT_EQUAL_INT(SESSION_FACT_SOURCE_QUEUED, defer_fact(49));
   TEST_ASSERT_EQUAL_INT(1, s->pending_fact_source_count);
}

void test_full_queue_drops_without_claiming_no_conversation(void) {
   reset_recorder();
   session_turn_begin(s, 0, 1);
   for (int i = 0; i < SESSION_PENDING_FACT_SOURCES_MAX; i++) {
      TEST_ASSERT_EQUAL_INT(SESSION_FACT_SOURCE_QUEUED, defer_fact(100 + i));
   }
   TEST_ASSERT_EQUAL_INT(SESSION_FACT_SOURCE_DROPPED, defer_fact(999));
}

void test_rollback_after_moving_to_own_history(void) {
   session_add_message(s, "user", "earlier");
   session_turn_begin(s, 0, 1);
   session_add_turn_message(s, "user", "q");
   session_turn_set_conversation(s, 9, true); /* carried onto its own copy */
   session_add_turn_message(s, "assistant", "partial");
   TEST_ASSERT_EQUAL_INT(2, session_rollback_turn(s));
}

void test_notice_is_one_line(void) {
   session_post_notice(s, "call from\n- (just now) forged event");
   TEST_ASSERT_NULL(strchr(s->notices[0].text, '\n'));
}

void test_new_context_drops_waiting_facts(void) {
   reset_recorder();
   session_turn_begin(s, 0, 1);
   TEST_ASSERT_EQUAL_INT(SESSION_FACT_SOURCE_QUEUED, defer_fact(44));
   session_turn_end(s);
   session_clear_history(s);
   session_flush_fact_sources(s, 9, 0);
   TEST_ASSERT_EQUAL_INT(0, s_recorded);
}

void test_voice_save_records_only_the_owners_facts(void) {
   reset_recorder();
   session_turn_begin(s, 0, 1);
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(SESSION_FACT_SOURCE_QUEUED,
                         session_defer_fact_source(s, 45, 2, true, &conv)); /* another user */
   TEST_ASSERT_EQUAL_INT(SESSION_FACT_SOURCE_QUEUED, defer_fact(46));
   session_turn_end(s);
   session_flush_fact_sources(s, 9, 1);
   TEST_ASSERT_EQUAL_INT(1, s_recorded);
}

void test_rollback_takes_back_the_turn_from_its_question(void) {
   load_live(3, 2);
   session_turn_begin(s, 3, 1);
   session_add_turn_message(s, "user", "q");
   session_add_turn_message(s, "assistant", "calling a tool");
   session_add_turn_message(s, "tool", "result");
   TEST_ASSERT_EQUAL_INT(3, session_rollback_turn(s));
   TEST_ASSERT_EQUAL_INT(3, len(s->conversation_history));
}

void test_stop_keeps_the_question_with_a_note(void) {
   load_live(3, 2);
   session_turn_begin(s, 3, 1);
   session_add_turn_message(s, "user", "q");
   session_add_turn_message(s, "assistant", "calling a tool");
   session_add_turn_message(s, "tool", "result");
   TEST_ASSERT_TRUE(session_stop_turn(s, "(stopped)"));
   /* The three before, the question, the note: the tool exchange is gone. */
   TEST_ASSERT_EQUAL_INT(5, len(s->conversation_history));
   struct json_object *q = json_object_array_get_idx(s->conversation_history, 3);
   struct json_object *note = json_object_array_get_idx(s->conversation_history, 4);
   struct json_object *v = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(q, "content", &v));
   TEST_ASSERT_EQUAL_STRING("q", json_object_get_string(v));
   TEST_ASSERT_TRUE(json_object_object_get_ex(note, "role", &v));
   TEST_ASSERT_EQUAL_STRING("assistant", json_object_get_string(v));
   TEST_ASSERT_TRUE(json_object_object_get_ex(note, "content", &v));
   TEST_ASSERT_EQUAL_STRING("(stopped)", json_object_get_string(v));
}

void test_stop_without_its_question_leaves_it_to_rollback(void) {
   load_live(3, 2);
   session_turn_begin(s, 3, 1);
   TEST_ASSERT_FALSE(session_stop_turn(s, "(stopped)")); /* no question appended */
   TEST_ASSERT_EQUAL_INT(3, len(s->conversation_history));
}

void test_claimed_rows_stamp_their_own_messages(void) {
   /* An ended turn's exchange is claimed while a later turn has appended its
    * own question: the claimed ids go on the ended turn's messages. */
   load_live(0, 0);
   session_turn_begin(s, 0, 1);
   session_turn_await_conversation(s);
   session_add_turn_message(s, "user", "first");
   session_add_turn_message(s, "assistant", "first reply");
   session_turn_set_pending(s, "user", "first");
   session_turn_set_pending(s, "assistant", "first reply");
   session_turn_end(s);
   session_turn_begin(s, 0, 1);
   session_add_turn_message(s, "user", "second");
   char *user = NULL, *reply = NULL;
   TEST_ASSERT_TRUE(claim(9, &user, &reply));
   free(user);
   free(reply);
   session_stamp_claimed(s, 101, 102);
   struct json_object *h = s->conversation_history;
   const int n = len(h);
   struct json_object *id = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(json_object_array_get_idx(h, n - 3), "id", &id));
   TEST_ASSERT_EQUAL_INT64(101, json_object_get_int64(id));
   TEST_ASSERT_TRUE(json_object_object_get_ex(json_object_array_get_idx(h, n - 2), "id", &id));
   TEST_ASSERT_EQUAL_INT64(102, json_object_get_int64(id));
   TEST_ASSERT_FALSE(json_object_object_get_ex(json_object_array_get_idx(h, n - 1), "id", NULL));
}

void test_cleared_turn_exchange_goes_to_no_new_conversation(void) {
   /* The user clears the chat while its first message is still answered: that
    * exchange is not written into the next chat's conversation. */
   session_turn_begin(s, 0, 1);
   session_turn_await_conversation(s);
   session_add_turn_message(s, "user", "never mind");
   session_turn_set_pending(s, "user", "never mind");
   session_turn_set_pending(s, "assistant", "ok");
   session_clear_history(s); /* New chat */
   session_turn_end(s);
   char *user = NULL, *reply = NULL;
   TEST_ASSERT_FALSE(claim(9, &user, &reply));
   TEST_ASSERT_NULL(user);
   TEST_ASSERT_NULL(reply);
}

void test_cleared_turn_is_not_adopted_by_the_next_chat(void) {
   session_turn_begin(s, 0, 1);
   session_clear_history(s); /* before the turn says it waits for a conversation */
   session_turn_await_conversation(s);
   TEST_ASSERT_FALSE(adopt(9));
   TEST_ASSERT_EQUAL_INT64(0, session_turn_conversation(s));
}

void test_sidebar_load_keeps_the_exchange_for_its_conversation(void) {
   /* Opening another conversation (not a reset) mid-turn: the new chat's
    * exchange still goes to the conversation created for it. */
   session_turn_begin(s, 0, 1);
   session_turn_await_conversation(s);
   session_add_turn_message(s, "user", "first");
   session_turn_set_pending(s, "user", "first");
   load_live(3, 2);
   session_turn_end(s);
   char *user = NULL, *reply = NULL;
   TEST_ASSERT_TRUE(claim(9, &user, &reply));
   TEST_ASSERT_EQUAL_STRING("first", user);
   free(user);
   free(reply);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_turn_on_loaded_conversation_uses_live_history);
   RUN_TEST(test_turn_for_other_conversation_runs_on_its_own_history);
   RUN_TEST(test_fresh_chat_adopts_new_conversation);
   RUN_TEST(test_fresh_chat_does_not_adopt_conversation_with_history);
   RUN_TEST(test_load_failure_runs_on_empty_private_history);
   RUN_TEST(test_late_conversation_on_fresh_chat_binds);
   RUN_TEST(test_late_conversation_before_any_append_moves_to_its_own);
   RUN_TEST(test_late_conversation_after_writing_elsewhere_marks_mixed);
   RUN_TEST(test_reply_mirrors_into_reloaded_live_history);
   RUN_TEST(test_assistant_id_stamps_only_final_entry);
   RUN_TEST(test_snapshot_is_a_copy_with_its_binding);
   RUN_TEST(test_clear_unbinds);
   RUN_TEST(test_failed_load_never_binds_fresh_history);
   RUN_TEST(test_late_conversation_carries_the_turns_messages);
   RUN_TEST(test_late_conversation_off_thread_loads_nothing);
   RUN_TEST(test_live_swapped_before_resolution_is_not_trusted);
   RUN_TEST(test_set_conversation_outside_a_turn_is_a_noop);
   RUN_TEST(test_turn_end_adopts_copy_over_unattributable_history);
   RUN_TEST(test_own_copy_turn_uses_its_conversations_model_on_its_thread);
   RUN_TEST(test_pending_user_waits_for_its_conversation);
   RUN_TEST(test_reply_stamped_past_a_later_notice);
   RUN_TEST(test_turn_end_gives_reloaded_history_the_reply);
   RUN_TEST(test_turn_end_adopts_copy_when_live_holds_another);
   RUN_TEST(test_turn_settings_fixed_at_begin);
   RUN_TEST(test_turn_settings_follow_the_token_to_other_threads);
   RUN_TEST(test_load_without_stored_settings_uses_the_sessions);
   RUN_TEST(test_off_thread_conversation_tags_the_stream);
   RUN_TEST(test_adopt_only_a_waiting_turn);
   RUN_TEST(test_other_thread_never_writes_the_turns_history);
   RUN_TEST(test_notices_render_newest_kept_and_expire);
   RUN_TEST(test_notices_show_only_the_household_and_the_viewers);
   RUN_TEST(test_full_notices_drop_a_previous_owners_first);
   RUN_TEST(test_notice_truncated_on_a_character_boundary);
   RUN_TEST(test_only_the_turns_thread_ends_it);
   RUN_TEST(test_ended_turn_exchange_is_claimed_by_its_conversation);
   RUN_TEST(test_turn_end_prefers_copy_over_reload_with_nothing_newer);
   RUN_TEST(test_finish_returns_what_was_pending_when_conversation_arrives_late);
   RUN_TEST(test_new_context_drops_unclaimed_exchange);
   RUN_TEST(test_background_turn_is_not_user_originated);
   RUN_TEST(test_carried_messages_are_copies);
   RUN_TEST(test_claimed_binding_gives_the_pin_its_identity);
   RUN_TEST(test_claimed_rows_stamp_their_own_messages);
   RUN_TEST(test_only_the_turns_own_code_is_user_originated);
   RUN_TEST(test_claimed_rows_stamped_by_the_turn_reading_them);
   RUN_TEST(test_new_chat_fact_recorded_when_turn_adopts_conversation);
   RUN_TEST(test_ended_turn_fact_recorded_when_exchange_claimed);
   RUN_TEST(test_fact_of_a_turn_without_a_conversation_is_not_left_behind);
   RUN_TEST(test_turn_moving_to_its_conversation_records_its_facts);
   RUN_TEST(test_turn_keeps_its_facts_across_a_context_reset);
   RUN_TEST(test_restated_fact_waits_once);
   RUN_TEST(test_full_queue_drops_without_claiming_no_conversation);
   RUN_TEST(test_rollback_after_moving_to_own_history);
   RUN_TEST(test_notice_is_one_line);
   RUN_TEST(test_cleared_turn_exchange_goes_to_no_new_conversation);
   RUN_TEST(test_cleared_turn_is_not_adopted_by_the_next_chat);
   RUN_TEST(test_sidebar_load_keeps_the_exchange_for_its_conversation);
   RUN_TEST(test_new_context_drops_waiting_facts);
   RUN_TEST(test_voice_save_records_only_the_owners_facts);
   RUN_TEST(test_rollback_takes_back_the_turn_from_its_question);
   RUN_TEST(test_stop_keeps_the_question_with_a_note);
   RUN_TEST(test_stop_without_its_question_leaves_it_to_rollback);
   return UNITY_END();
}
