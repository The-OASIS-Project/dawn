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
 * Unit tests for the append-only request (session_prefix.c): the first turn
 * freezes the system prompt, later changes are appended after the question,
 * the turn's context goes in front of it, and nothing already there changes.
 */

#include <json-c/json.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "auth/auth_db_conv_prefix.h"
#include "auth/auth_db_focus_handles.h"
#include "auth/auth_db_messages.h"
#include "auth/auth_db_withdraw.h"
#include "core/focus/focus_handles.h"
#include "core/prefix_in_force.h"
#include "core/session_manager.h"
#include "core/session_prefix.h"
#include "dawn_error.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_turn_blocks.h"
#include "unity.h"

/* ---- stubs for the session runtime ---- */
static const char *s_notice;
int session_effective_user_id(session_t *session) {
   (void)session;
   return 1;
}
char *session_take_new_notices_locked(session_t *session, int viewer_user_id) {
   (void)session;
   (void)viewer_user_id;
   char *out = s_notice ? strdup(s_notice) : NULL;
   s_notice = NULL; /* told once */
   return out;
}

#define FLAGS (JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE)

static session_t *s;

/* No turn runs on another thread here: a reference is let go at once. */
void session_release_ref_locked(session_t *session, struct json_object *obj) {
   (void)session;
   json_object_put(obj);
}

/* No turn reads the test's history on another thread unless a test says so. */
static bool s_turn_elsewhere;
bool session_turn_reads_elsewhere_locked(const session_t *session) {
   (void)session;
   return s_turn_elsewhere;
}

/* The session's own handle table (core/focus/focus_handles.c): one item a
 * test may give a handle in a history no conversation stored yet. */
static const char *s_table_item;
static int s_table_handle;
int focus_handles_withdrawn_locked(const session_t *session,
                                   int64_t conv_id,
                                   const char *const *item_ids,
                                   int n_ids,
                                   int *out,
                                   int cap) {
   (void)session;
   for (int i = 0; s_table_item && conv_id == 0 && cap > 0 && i < n_ids; i++) {
      if (strcmp(item_ids[i], s_table_item) == 0) {
         out[0] = s_table_handle;
         return 1;
      }
   }
   return 0;
}

/* The one live session, the user's. */
void session_manager_for_each_user_session(int user_id,
                                           void (*fn)(session_t *session, void *ctx),
                                           void *ctx) {
   (void)user_id;
   fn(s, ctx);
}

void setUp(void) {
   s = calloc(1, sizeof(*s));
   s->type = SESSION_TYPE_WEBUI; /* saves turn by turn (a voice surface saves whole) */
   pthread_mutex_init(&s->history_mutex, NULL);
   s->conversation_history = json_object_new_array();
   s_notice = NULL;
   s_turn_elsewhere = false;
   s_table_item = NULL;
}

void tearDown(void) {
   session_prefix_turn_free(s->prefix_turn);
   json_object_put(s->withdraw_pending);
   json_object_put(s->conversation_history);
   pthread_mutex_destroy(&s->history_mutex);
   free(s);
}

static void add(const char *role, const char *content) {
   struct json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string(role));
   json_object_object_add(m, "content", json_object_new_string(content));
   json_object_array_add(s->conversation_history, m);
}

static struct json_object *at(int i) {
   return json_object_array_get_idx(s->conversation_history, i);
}

static int count(void) {
   return (int)json_object_array_length(s->conversation_history);
}

static char *copy_of(const char *text) {
   return text ? strdup(text) : NULL;
}

/* A prompt whose system prompt is one section, "rules". */
static composed_prompt_t prompt(const char *stable,
                                const char *memory,
                                const char *directives,
                                const char *context) {
   composed_prompt_t cp = { .volatile_block = copy_of(context),
                            .memory_body = copy_of(memory),
                            .directives = copy_of(directives) };
   if (stable) {
      TEST_ASSERT_EQUAL_INT(0, prompt_sections_add(&cp, "rules", "your operating rules", stable));
      cp.stable_prefix = prompt_sections_join(&cp);
   }
   return cp;
}

/* A prompt of three sections. */
static composed_prompt_t sectioned(const char *persona, const char *rules, const char *user) {
   composed_prompt_t cp = { .directives = strdup("") };
   TEST_ASSERT_EQUAL_INT(0, prompt_sections_add(&cp, "persona", "your persona", persona));
   TEST_ASSERT_EQUAL_INT(0, prompt_sections_add(&cp, "rules", "your operating rules", rules));
   TEST_ASSERT_EQUAL_INT(0, prompt_sections_add(&cp, "user_context", "the user's context", user));
   cp.stable_prefix = prompt_sections_join(&cp);
   return cp;
}

static const char *content_of(int i) {
   return json_object_get_string(json_object_object_get(at(i), "content"));
}

/* An edit to one section appends that section, not the prompt; one the
 * prompt no longer has is withdrawn. */
static void test_a_change_appends_only_the_changed_section(void) {
   add("user", "Q1");
   composed_prompt_t cp = sectioned("PERSONA", "RULES", "Location: Here");
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
   TEST_ASSERT_EQUAL_STRING("PERSONA\n\nRULES\n\nLocation: Here", content_of(0));
   TEST_ASSERT_EQUAL_INT(2, count());

   add("assistant", "A1");
   add("user", "Q2");
   cp = sectioned("PERSONA", "RULES", "Location: There");
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
   TEST_ASSERT_EQUAL_INT(5, count());
   TEST_ASSERT_EQUAL_INT(MESSAGE_KIND_INSTRUCTION, llm_history_kind_of(at(4)));
   const char *change = content_of(4);
   TEST_ASSERT_NOT_NULL(strstr(change, "the user's context"));
   TEST_ASSERT_NOT_NULL(strstr(change, "Location: There"));
   TEST_ASSERT_NULL(strstr(change, "PERSONA"));
   TEST_ASSERT_NULL(strstr(change, "RULES"));

   /* The same again: nothing appended. */
   add("assistant", "A2");
   add("user", "Q3");
   cp = sectioned("PERSONA", "RULES", "Location: There");
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
   TEST_ASSERT_EQUAL_INT(7, count());

   /* The user cleared their settings: the section is withdrawn. */
   add("assistant", "A3");
   add("user", "Q4");
   cp = sectioned("PERSONA", "RULES", NULL);
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
   TEST_ASSERT_EQUAL_INT(10, count());
   TEST_ASSERT_NOT_NULL(strstr(content_of(9), "no longer applies"));
   TEST_ASSERT_NOT_NULL(strstr(content_of(9), "the user's context"));
}

/* A history frozen with no record (saved before sections were tracked): a
 * section whose text is in the frozen prompt is in force as is. */
static void test_a_history_with_no_record_keeps_what_it_has(void) {
   add("system", "PERSONA\n\nRULES v1");
   add("user", "Q1");
   session_prefix_apply_turn(s, NULL, NULL); /* frozen from what it has */
   TEST_ASSERT_EQUAL_INT(MESSAGE_KIND_PREFIX, llm_history_kind_of(at(0)));
   add("assistant", "A1");
   add("user", "Q2");
   composed_prompt_t cp = sectioned("PERSONA", "RULES v2", NULL);
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
   TEST_ASSERT_EQUAL_INT(5, count());
   TEST_ASSERT_NOT_NULL(strstr(content_of(4), "RULES v2"));
   TEST_ASSERT_NULL(strstr(content_of(4), "PERSONA"));
}

/* The directions in force are a record, not text read back: a surface whose
 * directions went away gets the line saying none apply, once. */
static void test_directions_are_tracked_by_the_record(void) {
   add("user", "Q1");
   composed_prompt_t cp = prompt("P", NULL, "Room=Kitchen.", NULL);
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
   TEST_ASSERT_EQUAL_INT(MESSAGE_KIND_DIRECTIVE, llm_history_kind_of(at(2)));
   add("assistant", "A1");
   add("user", "Q2");
   cp = prompt("P", NULL, "", NULL);
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
   TEST_ASSERT_EQUAL_INT(6, count());
   TEST_ASSERT_EQUAL_INT(MESSAGE_KIND_DIRECTIVE, llm_history_kind_of(at(5)));
   add("assistant", "A2");
   add("user", "Q3");
   cp = prompt("P", NULL, "", NULL);
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
   TEST_ASSERT_EQUAL_INT(8, count());
}

/* A turn context body, as the builder returns it (session_prefix frames it). */
#define CTX(t) "[system_time] " t "\n"

static void test_turns_append_and_never_rewrite(void) {
   add("system", "session start prompt");
   add("user", "Q1");
   composed_prompt_t cp = prompt("P", "MEM", "D", CTX("09:00"));
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);

   /* [prefix P][Q1 with memory + context][directive D] */
   TEST_ASSERT_EQUAL_INT(3, count());
   TEST_ASSERT_EQUAL_INT(MESSAGE_KIND_PREFIX, llm_history_kind_of(at(0)));
   TEST_ASSERT_EQUAL_STRING("P", json_object_get_string(json_object_object_get(at(0), "content")));
   /* Both blocks framed with the conversation's tag, which the prompt declares. */
   const char *tag = llm_history_tag(s->conversation_history);
   TEST_ASSERT_NOT_NULL(tag);
   TEST_ASSERT_EQUAL_INT(0, strncmp(tag, "dawn-", 5));
   char want[1024];
   snprintf(want, sizeof(want),
            "[{\"type\":\"text\",\"text\":\"--- USER MEMORY (%s) ---\\nMEM\\n"
            "--- END USER MEMORY (%s) ---\\n\",\"_kind\":\"memory\"},"
            "{\"type\":\"text\",\"text\":\"--- TURN CONTEXT (%s) ---\\n[system_time] 09:00\\n"
            "--- END TURN CONTEXT (%s) ---\\n\",\"_kind\":\"turn_context\"},"
            "{\"type\":\"text\",\"text\":\"Q1\"}]",
            tag, tag, tag, tag);
   TEST_ASSERT_EQUAL_STRING(
       want, json_object_to_json_string_ext(json_object_object_get(at(1), "content"), FLAGS));
   TEST_ASSERT_EQUAL_INT(MESSAGE_KIND_DIRECTIVE, llm_history_kind_of(at(2)));
   char *first = strdup(json_object_to_json_string_ext(s->conversation_history, FLAGS));

   /* Turn 2, nothing changed but the time: only the context is new. */
   add("assistant", "A1");
   add("user", "Q2");
   cp = prompt("P", "MEM", "D", CTX("09:05"));
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
   TEST_ASSERT_EQUAL_INT(5, count());
   struct json_object *q2 = json_object_object_get(at(4), "content");
   TEST_ASSERT_EQUAL_INT(2, (int)json_object_array_length(q2));
   TEST_ASSERT_EQUAL_INT(MESSAGE_KIND_TURN_CONTEXT,
                         llm_history_kind_of(json_object_array_get_idx(q2, 0)));

   /* What turn 1 sent is still there, byte for byte. */
   const char *now = json_object_to_json_string_ext(s->conversation_history, FLAGS);
   TEST_ASSERT_EQUAL_INT(0, strncmp(now, first, strlen(first) - 1));
   free(first);

   /* Turn 3: new instructions, no directions, new memory. */
   add("assistant", "A2");
   add("user", "Q3");
   cp = prompt("P2", "MEM2", "", CTX("09:10"));
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
   TEST_ASSERT_EQUAL_INT(9, count());
   TEST_ASSERT_EQUAL_STRING("P", json_object_get_string(json_object_object_get(at(0), "content")));
   TEST_ASSERT_EQUAL_INT(MESSAGE_KIND_INSTRUCTION, llm_history_kind_of(at(7)));
   TEST_ASSERT_NOT_NULL(
       strstr(json_object_get_string(json_object_object_get(at(7), "content")), "P2"));
   TEST_ASSERT_EQUAL_INT(MESSAGE_KIND_DIRECTIVE, llm_history_kind_of(at(8)));
   TEST_ASSERT_EQUAL_INT(MESSAGE_KIND_MEMORY, llm_history_kind_of(json_object_array_get_idx(
                                                  json_object_object_get(at(6), "content"), 0)));

   /* Turn 4: the same again: nothing appended but the context. */
   add("assistant", "A3");
   add("user", "Q4");
   cp = prompt("P2", "MEM2", "", CTX("09:15"));
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
   TEST_ASSERT_EQUAL_INT(11, count());
}

/* A history an older build saved: its per-turn system blocks go, and reasoning
 * bound to the prompt it had isn't replayed; its text stays. */
static void test_adopting_an_older_history(void) {
   add("system", "old stable");
   add("system", "old volatile");
   add("user", "Q1");
   struct json_object *a = json_object_new_object();
   json_object_object_add(a, "role", json_object_new_string("assistant"));
   json_object_object_add(a, "content", json_object_new_string("A1"));
   json_object_object_add(a, LLM_TURN_BLOCKS_KEY, json_object_new_array());
   json_object_array_add(s->conversation_history, a);
   add("user", "Q2");
   composed_prompt_t cp = prompt("P", NULL, "", CTX("10:00"));
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
   TEST_ASSERT_EQUAL_INT(4, count());
   TEST_ASSERT_EQUAL_INT(MESSAGE_KIND_PREFIX, llm_history_kind_of(at(0)));
   TEST_ASSERT_FALSE(json_object_object_get_ex(at(2), LLM_TURN_BLOCKS_KEY, NULL));
   TEST_ASSERT_EQUAL_STRING("A1", json_object_get_string(json_object_object_get(at(2), "content")));
}

/* No builder (a guest surface): the prompt it has is frozen, and the turn's
 * note and device events still reach it. */
static void test_guest_turn_keeps_its_prompt(void) {
   add("system", "guest prompt");
   add("user", "Q1");
   s_notice = "--- Recent device events ---\n- (09:00) Phone rang";
   session_prefix_apply_turn(s, NULL, "User is on SMS");
   TEST_ASSERT_EQUAL_INT(2, count());
   TEST_ASSERT_EQUAL_STRING("guest prompt",
                            json_object_get_string(json_object_object_get(at(0), "content")));
   const char *q = json_object_to_json_string_ext(at(1), FLAGS);
   TEST_ASSERT_NOT_NULL(strstr(q, "User is on SMS"));
   TEST_ASSERT_NOT_NULL(strstr(q, "Phone rang"));
   TEST_ASSERT_NOT_NULL(strstr(q, "--- END TURN CONTEXT (dawn-"));
}

/* A frozen tool's schema is recorded, and a change to it is recorded too (and
 * logged): the tools a conversation sends are rendered on every request. */
static void test_tool_schemas_are_recorded(void) {
   add("user", "Q1");
   composed_prompt_t cp = prompt("P", NULL, "", NULL);
   cp.tool_names = strdup("[\"a\",\"b\"]");
   cp.tool_schemas = strdup("{\"a\":\"h1\",\"b\":\"h2\",\"c\":\"h3\"}");
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
   char *rec = prefix_in_force_json(s->conversation_history);
   TEST_ASSERT_NOT_NULL(strstr(rec, "\"tool_schemas\":{\"a\":\"h1\",\"b\":\"h2\"}"));
   free(rec);

   add("assistant", "A1");
   add("user", "Q2");
   cp = prompt("P", NULL, "", NULL);
   cp.tool_names = strdup("[\"a\",\"b\"]");
   cp.tool_schemas = strdup("{\"a\":\"h1-changed\",\"b\":\"h2\"}");
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
   rec = prefix_in_force_json(s->conversation_history);
   TEST_ASSERT_NOT_NULL(strstr(rec, "\"a\":\"h1-changed\""));
   free(rec);
   TEST_ASSERT_EQUAL_INT(4, count()); /* no boundary, nothing appended */
}

/* A background job's report (untrusted) never shares a message with DAWN's
 * context: the context goes in a message of its own just before it. */
static void test_an_envelope_gets_its_context_apart(void) {
   struct json_object *env = json_object_new_object();
   json_object_object_add(env, "role", json_object_new_string("user"));
   json_object_object_add(env, "content", json_object_new_string("[job] RESULT"));
   llm_history_set_kind(env, MESSAGE_KIND_ENVELOPE);
   json_object_array_add(s->conversation_history, env);
   composed_prompt_t cp = prompt("P", "MEM", "", CTX("09:00"));
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
   /* [prefix][context][envelope] */
   TEST_ASSERT_EQUAL_INT(3, count());
   TEST_ASSERT_TRUE(llm_history_is_context(at(1)));
   TEST_ASSERT_EQUAL_INT(MESSAGE_KIND_ENVELOPE, llm_history_kind_of(at(2)));
   TEST_ASSERT_EQUAL_STRING("[job] RESULT", content_of(2));
}

/* ---- saving a turn with its conversation (real auth_db) ---- */

static const char *TEST_DB = "/tmp/dawn_test_session_prefix.db";

typedef struct {
   int count;
   char roles[8][16];
   char kinds[8][16];
   int64_t context_of[8];
} saved_t;

static int collect_saved(const conversation_llm_row_t *row, void *ctx) {
   saved_t *out = ctx;
   if (out->count < 8) {
      snprintf(out->roles[out->count], sizeof(out->roles[0]), "%s", row->role);
      snprintf(out->kinds[out->count], sizeof(out->kinds[0]), "%s", row->kind ? row->kind : "-");
      out->context_of[out->count] = row->context_of;
   }
   out->count++;
   return 0;
}

static int s_user;

static int64_t db_open_conv(void) {
   unlink(TEST_DB);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(TEST_DB));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("alice", "h", true));
   auth_user_t u;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user("alice", &u));
   s_user = u.id;
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "c", &conv));
   return conv;
}

static void db_close(void) {
   auth_db_shutdown();
   unlink(TEST_DB);
}

/* Save the history message at @p index as a question row, as a surface does
 * (write, stamp, tell the seam). */
static int64_t save_question(int64_t conv, int index) {
   int64_t qid = 0;
   const conv_message_row_t q = { .role = "user", .content = llm_history_question_text(at(index)) };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, s_user, &q, &qid));
   json_object_object_add(at(index), "id", json_object_new_int64(qid));
   session_prefix_question_saved(s, conv, s_user, qid);
   return qid;
}

static void check_saved_turn(int64_t conv, int64_t qid) {
   saved_t rows = { 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_for_llm(conv, s_user, 0, collect_saved, &rows));
   TEST_ASSERT_EQUAL_INT(5, rows.count);
   TEST_ASSERT_EQUAL_STRING("-", rows.kinds[0]); /* the question */
   TEST_ASSERT_EQUAL_STRING("-", rows.kinds[1]); /* another writer's row */
   TEST_ASSERT_EQUAL_STRING("memory", rows.kinds[2]);
   TEST_ASSERT_EQUAL_STRING("turn_context", rows.kinds[3]);
   TEST_ASSERT_EQUAL_STRING("directive", rows.kinds[4]);
   TEST_ASSERT_EQUAL_STRING("system", rows.roles[4]);
   for (int i = 2; i < 5; i++) {
      TEST_ASSERT_EQUAL_INT64(qid, rows.context_of[i]); /* each names its question */
   }
   /* The directive message carries its row's id. */
   TEST_ASSERT_TRUE(json_object_object_get_ex(at(2), "id", NULL));

   conv_prefix_t p;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_prefix_get(conv, s_user, &p));
   TEST_ASSERT_EQUAL_STRING("P", p.prefix);
   TEST_ASSERT_EQUAL_STRING("[\"weather\"]", p.tools);
   conv_prefix_free(&p);
   TEST_ASSERT_NULL(s->prefix_turn); /* nothing left to save */
}

static void apply_first_turn(void) {
   composed_prompt_t cp = prompt("P", "MEM", "D", CTX("09:00"));
   cp.tool_names = strdup("[\"weather\"]");
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
}

/* The common order: the question is saved, then the prompt applied. */
static void test_a_turn_saves_when_its_prompt_is_applied(void) {
   const int64_t conv = db_open_conv();
   add("system", "start");
   add("user", "Q1");
   const int64_t qid = save_question(conv, 1);
   /* Another writer's row lands before the turn's context is saved. */
   const conv_message_row_t other = { .role = "assistant", .content = "elsewhere" };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, s_user, &other, NULL));
   TEST_ASSERT_EQUAL_INT64(qid, s->prefix_saved.row_id); /* waiting for the prompt */
   apply_first_turn();
   check_saved_turn(conv, qid);
   db_close();
}

/* A new chat's first turn: the prompt applied before its conversation exists,
 * the question saved once it does. */
static void test_a_turn_saves_when_its_question_is_saved(void) {
   const int64_t conv = db_open_conv();
   add("system", "start");
   add("user", "Q1");
   apply_first_turn();
   TEST_ASSERT_NOT_NULL(s->prefix_turn); /* waiting for the question */
   int64_t qid = 0;
   const conv_message_row_t q = { .role = "user", .content = "Q1" };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, s_user, &q, &qid));
   const conv_message_row_t other = { .role = "assistant", .content = "elsewhere" };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, s_user, &other, NULL));
   json_object_object_add(at(1), "id", json_object_new_int64(qid));
   session_prefix_question_saved(s, conv, s_user, qid);
   check_saved_turn(conv, qid);
   db_close();
}

/* A turn whose question was never saved is given up when a later one's is:
 * what it appended is saved with that one, naming no question, the prefix it
 * froze too, and the conversation leaves its earlier reasoning behind. */
static void test_an_unsaved_turn_is_given_up_for_the_next(void) {
   const int64_t conv = db_open_conv();
   add("system", "start");
   add("user", "Q1");
   apply_first_turn(); /* Q1 never saved */
   add("assistant", "A1");
   add("user", "Q2");
   composed_prompt_t cp = prompt("P", "MEM", "D", CTX("09:05"));
   cp.tool_names = strdup("[\"weather\"]");
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
   const int64_t qid = save_question(conv, 4);

   saved_t rows = { 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_for_llm(conv, s_user, 0, collect_saved, &rows));
   /* Q2, its context (the memory was Q1's), and turn 1's directive. */
   TEST_ASSERT_EQUAL_INT(3, rows.count);
   TEST_ASSERT_EQUAL_STRING("turn_context", rows.kinds[1]);
   TEST_ASSERT_EQUAL_INT64(qid, rows.context_of[1]);
   TEST_ASSERT_EQUAL_STRING("directive", rows.kinds[2]);
   TEST_ASSERT_EQUAL_INT64(0, rows.context_of[2]);
   conv_prefix_t p;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_prefix_get(conv, s_user, &p));
   TEST_ASSERT_EQUAL_STRING("P", p.prefix);
   TEST_ASSERT_EQUAL_INT64(qid, p.reasoning_floor_msg_id);
   conv_prefix_free(&p);
   TEST_ASSERT_NULL(s->prefix_turn);
   db_close();
}

/* An earlier turn's question saved after a later turn began (a new chat's
 * first exchange, adopted once its conversation exists): each record saves
 * with its own question. */
static void test_each_turn_saves_with_its_own_question(void) {
   const int64_t conv = db_open_conv();
   add("system", "start");
   add("user", "Q1");
   apply_first_turn();
   add("assistant", "A1");
   add("user", "Q2");
   composed_prompt_t cp = prompt("P", "MEM", "D2", CTX("09:05"));
   cp.tool_names = strdup("[\"weather\"]");
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
   const int64_t q1 = save_question(conv, 1);
   const int64_t q2 = save_question(conv, 4);

   saved_t rows = { 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_for_llm(conv, s_user, 0, collect_saved, &rows));
   /* Q1, its memory, context and directive; Q2, its context and directive. */
   TEST_ASSERT_EQUAL_INT(7, rows.count);
   for (int i = 1; i < 4; i++) {
      TEST_ASSERT_EQUAL_INT64(q1, rows.context_of[i]);
   }
   TEST_ASSERT_EQUAL_STRING("turn_context", rows.kinds[5]);
   TEST_ASSERT_EQUAL_INT64(q2, rows.context_of[5]);
   TEST_ASSERT_EQUAL_STRING("directive", rows.kinds[6]);
   TEST_ASSERT_EQUAL_INT64(q2, rows.context_of[6]);
   conv_prefix_t p;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_prefix_get(conv, s_user, &p));
   TEST_ASSERT_EQUAL_INT64(0, p.reasoning_floor_msg_id); /* nothing given up */
   conv_prefix_free(&p);
   TEST_ASSERT_NULL(s->prefix_turn);
   db_close();
}

/* Run @p sql on the test database from a connection of its own: the item a
 * test forgets goes the way a delete trigger records it. */
static void db_exec(const char *sql) {
   sqlite3 *db = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(TEST_DB, &db));
   char *err = NULL;
   const int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
   TEST_ASSERT_EQUAL_INT_MESSAGE(SQLITE_OK, rc, err ? err : "");
   sqlite3_free(err);
   sqlite3_close(db);
}

static void forget_item(const char *item_id) {
   char sql[256];
   snprintf(sql, sizeof(sql), "INSERT INTO withdrawn_items (item_id, user_id) VALUES ('%s', %d)",
            item_id, s_user);
   db_exec(sql);
}

/* A turn built before the user forgot something and saved after: the item
 * and the USER MEMORY block leave its rows as they are saved, and its
 * conversation's reasoning waits for a turn built after the forgetting. */
static void test_a_late_save_withdraws_what_was_forgotten(void) {
   const int64_t conv = db_open_conv();
   conv_focus_handle_t item = { .source = "memory_fact", .item_id = "fact:7" };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_focus_handles_assign(conv, s_user, &item, 1));
   add("system", "start");
   add("user", "Q1");
   composed_prompt_t cp = prompt("P", "MEM secret", "",
                                 CTX("09:00") "[M1 memory_fact] fact secret\n");
   cp.built_at = (int64_t)time(NULL) - 5;
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
   /* Forgotten now, before the question is saved. */
   forget_item("fact:7");
   TEST_ASSERT_EQUAL_INT(SUCCESS, session_withdraw_forgotten(s_user, true));
   const int64_t qid = save_question(conv, 1);

   char sql[160];
   snprintf(sql, sizeof(sql),
            "SELECT 1 FROM messages WHERE conversation_id = %lld AND "
            "content LIKE '%%secret%%'",
            (long long)conv);
   sqlite3 *db = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(TEST_DB, &db));
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(db, sql, -1, &st, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_DONE, sqlite3_step(st)); /* no row keeps it */
   sqlite3_finalize(st);
   snprintf(sql, sizeof(sql), "SELECT reasoning_floor_pending FROM conversations WHERE id = %lld",
            (long long)conv);
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(db, sql, -1, &st, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
   TEST_ASSERT_TRUE(sqlite3_column_int64(st, 0) > 0); /* its reply's reasoning waits */
   sqlite3_finalize(st);
   sqlite3_close(db);
   (void)qid;
   db_close();
}

/* A forgotten item's line leaves a live history too (the history's
 * conversation named by the stored withdrawal), with its reasoning; while a
 * turn reads the history elsewhere it waits for the next turn. */
static void live_history_with_item(void) {
   struct json_object *q = json_tokener_parse(
       "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"--- TURN CONTEXT (t) ---\\n"
       "[M1 memory_fact] a secret\\n--- END TURN CONTEXT (t) ---\\n\",\"_kind\":\"turn_context\"},"
       "{\"type\":\"text\",\"text\":\"Q\"}]}");
   json_object_array_add(s->conversation_history, q);
   struct json_object *a = json_object_new_object();
   json_object_object_add(a, "role", json_object_new_string("assistant"));
   json_object_object_add(a, "content", json_object_new_string("A"));
   json_object_object_add(a, LLM_TURN_BLOCKS_KEY, json_object_new_array());
   json_object_array_add(s->conversation_history, a);
}

static void test_a_live_history_withdraws_too(void) {
   const int64_t conv = db_open_conv();
   atomic_store(&s->history_conversation_id, conv);
   live_history_with_item();
   conv_focus_handle_t item = { .source = "memory_fact", .item_id = "fact:999" };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_focus_handles_assign(conv, s_user, &item, 1));
   forget_item("fact:999");

   s_turn_elsewhere = true; /* a turn is reading it: this waits */
   TEST_ASSERT_EQUAL_INT(SUCCESS, session_withdraw_forgotten(s_user, false));
   TEST_ASSERT_NOT_NULL(strstr(json_object_to_json_string(at(0)), "a secret"));
   TEST_ASSERT_NOT_NULL(s->withdraw_pending);

   s_turn_elsewhere = false; /* the next turn applies it, and declares a boundary */
   add("user", "Q2");
   composed_prompt_t cp = prompt("P", NULL, "", NULL);
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
   TEST_ASSERT_NULL(s->withdraw_pending);
   TEST_ASSERT_NULL(strstr(json_object_to_json_string(s->conversation_history), "a secret"));
   TEST_ASSERT_FALSE(json_object_object_get_ex(at(2), LLM_TURN_BLOCKS_KEY, NULL));
   db_close();
}

/* A turn built after the user's memory was withdrawn carries the current
 * USER MEMORY block: it is saved as it was sent, and it settles the floor the
 * withdrawal left pending (in the same second as the withdrawal, too). */
static void test_a_turn_built_after_a_withdrawal_keeps_its_memory(void) {
   const int64_t conv = db_open_conv();
   TEST_ASSERT_EQUAL_INT(SUCCESS, session_withdraw_forgotten(s_user, true));
   add("system", "start");
   add("user", "Q1");
   composed_prompt_t cp = prompt("P", "MEM current", "", CTX("09:00"));
   cp.built_at = (int64_t)time(NULL);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_withdraw_seq(&cp.built_seq));
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
   save_question(conv, 1);

   sqlite3 *db = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(TEST_DB, &db));
   char sql[160];
   snprintf(sql, sizeof(sql),
            "SELECT COUNT(*) FROM messages WHERE conversation_id = %lld AND kind = 'memory' "
            "AND content LIKE '%%MEM current%%'",
            (long long)conv);
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(db, sql, -1, &st, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
   TEST_ASSERT_EQUAL_INT64(1, sqlite3_column_int64(st, 0)); /* kept */
   sqlite3_finalize(st);
   snprintf(sql, sizeof(sql), "SELECT reasoning_floor_pending FROM conversations WHERE id = %lld",
            (long long)conv);
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(db, sql, -1, &st, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
   TEST_ASSERT_EQUAL_INT64(0, sqlite3_column_int64(st, 0));
   sqlite3_finalize(st);
   sqlite3_close(db);
   db_close();
}

/* A history no conversation stored yet (a voice session's) withdraws a
 * forgotten item by its own handle table. */
static void test_an_unsaved_history_withdraws_by_its_table(void) {
   db_open_conv();
   live_history_with_item();
   s_table_item = "fact:42";
   s_table_handle = 1;
   focus_handles_t table = { .count = 1 }; /* its entry is the stub's */
   s->focus_handles = &table;
   forget_item("fact:42");
   TEST_ASSERT_EQUAL_INT(SUCCESS, session_withdraw_forgotten(s_user, false));
   TEST_ASSERT_NULL(strstr(json_object_to_json_string(s->conversation_history), "a secret"));
   s->focus_handles = NULL;
   db_close();
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_turns_append_and_never_rewrite);
   RUN_TEST(test_adopting_an_older_history);
   RUN_TEST(test_guest_turn_keeps_its_prompt);
   RUN_TEST(test_a_change_appends_only_the_changed_section);
   RUN_TEST(test_a_history_with_no_record_keeps_what_it_has);
   RUN_TEST(test_directions_are_tracked_by_the_record);
   RUN_TEST(test_tool_schemas_are_recorded);
   RUN_TEST(test_an_envelope_gets_its_context_apart);
   RUN_TEST(test_a_turn_saves_when_its_prompt_is_applied);
   RUN_TEST(test_a_turn_saves_when_its_question_is_saved);
   RUN_TEST(test_an_unsaved_turn_is_given_up_for_the_next);
   RUN_TEST(test_each_turn_saves_with_its_own_question);
   RUN_TEST(test_a_late_save_withdraws_what_was_forgotten);
   RUN_TEST(test_an_unsaved_history_withdraws_by_its_table);
   RUN_TEST(test_a_turn_built_after_a_withdrawal_keeps_its_memory);
   RUN_TEST(test_a_live_history_withdraws_too);
   return UNITY_END();
}
