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
#include "config/dawn_config.h"
#include "core/focus/focus_handles.h"
#include "core/prefix_in_force.h"
#include "core/session_compaction.h"
#include "core/session_manager.h"
#include "core/session_prefix.h"
#include "dawn_error.h"
#include "llm/llm_compaction.h"
#include "llm/llm_context.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_turn_blocks.h"
#include "memory/memory_history_loader.h"
#include "unity.h"

dawn_config_t g_config;

/* ---- stubs for the compaction's runtime (session_compaction.c) ---- */
static bool s_over;             /* the history reaches its threshold */
static const char *s_summary;   /* what the summarizer writes */
static char s_summarized[4096]; /* what it was given */
static int s_noted_tokens = -1; /* the tracked count after a compaction */
int llm_context_estimate_tokens(struct json_object *history) {
   return llm_compaction_estimate_range(history, 0, (int)json_object_array_length(history));
}
bool llm_context_over_threshold(uint32_t session_id,
                                struct json_object *history,
                                int extra_tokens,
                                llm_type_t type,
                                cloud_provider_t provider,
                                const char *model,
                                float threshold) {
   (void)session_id, (void)history, (void)extra_tokens, (void)type, (void)provider;
   (void)model, (void)threshold;
   return s_over;
}
void llm_context_note_compacted(uint32_t session_id, int tokens) {
   (void)session_id;
   s_noted_tokens = tokens;
}
char *llm_context_summarize(struct json_object *to_summarize,
                            int kept_tokens,
                            int window_tokens,
                            const llm_compaction_calibration_t *cal,
                            const char *tag,
                            const session_llm_config_t *config,
                            const atomic_bool *cancel,
                            llm_compaction_level_t *level_out) {
   (void)kept_tokens, (void)window_tokens, (void)cal, (void)tag, (void)config, (void)cancel;
   snprintf(s_summarized, sizeof(s_summarized), "%s",
            json_object_to_json_string_ext(to_summarize, JSON_C_TO_STRING_PLAIN));
   *level_out = LLM_COMPACT_NORMAL;
   return s_summary ? strdup(s_summary) : NULL;
}
int llm_resolve_config(const session_llm_config_t *session_config,
                       llm_resolved_config_t *resolved) {
   (void)session_config;
   memset(resolved, 0, sizeof(*resolved));
   resolved->type = LLM_CLOUD;
   resolved->cloud_provider = CLOUD_PROVIDER_CLAUDE;
   resolved->model = "m";
   return 0;
}
void session_get_llm_config(session_t *session, session_llm_config_t *config) {
   (void)session;
   memset(config, 0, sizeof(*config));
}
void session_set_command_context(session_t *session) {
   (void)session;
}
void session_set_llm_config_override(const session_t *session, const session_llm_config_t *c) {
   (void)session, (void)c;
}
int llm_is_interrupt_requested(void) {
   return 0;
}
void *llm_get_cancel_flag(void) {
   return NULL;
}
int llm_context_get_size(llm_type_t type, cloud_provider_t provider, const char *model) {
   (void)type, (void)provider, (void)model;
   return 200000;
}
void llm_context_calibration(uint32_t session_id,
                             llm_type_t type,
                             cloud_provider_t provider,
                             const char *model,
                             llm_compaction_calibration_t *out) {
   (void)session_id, (void)type, (void)provider, (void)model;
   memset(out, 0, sizeof(*out));
}
float llm_context_hard_threshold(void) {
   return 0.85f;
}
void llm_set_cancel_flag(void *flag) {
   (void)flag;
}
void llm_set_cancel_flag_ex(void *flag, bool honor_global) {
   (void)flag, (void)honor_global;
}
void session_retain(session_t *session) {
   (void)session;
}
void session_release(session_t *session) {
   (void)session;
}
void session_release_ref(session_t *session, struct json_object *obj) {
   (void)session;
   json_object_put(obj);
}
static int s_compaction_notices;
void session_compaction_client_notice(session_t *session,
                                      int64_t conversation_id,
                                      int tokens_before,
                                      int tokens_after,
                                      int messages_summarized,
                                      const char *summary,
                                      int level) {
   (void)session, (void)conversation_id, (void)tokens_before, (void)tokens_after;
   (void)messages_summarized, (void)summary, (void)level;
   s_compaction_notices++;
}
int text_to_speech(char *text) {
   (void)text;
   return 0;
}

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
   s_over = false;
   s_summary = NULL;
   s_summarized[0] = '\0';
   s_noted_tokens = -1;
   s_compaction_notices = 0;
}

void tearDown(void) {
   session_compaction_teardown(s);
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

/* ---- compaction at the seam (session_compaction.h) ---- */

/* One turn as a surface runs it: the question saved, the prompt applied, the
 * answer saved. */
static void run_turn(int64_t conv, const char *q, const char *a, const char *persona) {
   add("user", q);
   save_question(conv, count() - 1);
   composed_prompt_t cp = sectioned(persona, "R", "U");
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
   add("assistant", a);
   int64_t id = 0;
   const conv_message_row_t row = { .role = "assistant", .content = a };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, s_user, &row, &id));
   json_object_object_add(at(count() - 1), "id", json_object_new_int64(id));
}

/* What a provider is sent: the history without DAWN's keys or row ids. */
static char *wire_of(struct json_object *hist) {
   struct json_object *copy = llm_history_wire_copy(hist);
   TEST_ASSERT_NOT_NULL(copy);
   struct json_object *deep = NULL;
   TEST_ASSERT_EQUAL_INT(0, json_object_deep_copy(copy, &deep, NULL));
   json_object_put(copy);
   for (size_t i = 0; i < json_object_array_length(deep); i++) {
      json_object_object_del(json_object_array_get_idx(deep, i), "id");
   }
   char *out = strdup(json_object_to_json_string_ext(deep, FLAGS));
   json_object_put(deep);
   return out;
}

static int kind_count(message_kind_t kind) {
   int n = 0;
   for (int i = 0; i < count(); i++) {
      n += llm_history_kind_of(at(i)) == kind;
   }
   return n;
}

/* A summary applied at the next turn's seam: the summarized turns go, the
 * summary leads the first kept question, what was in force only in them is
 * sent again after the question, the compaction is saved with the turn, and
 * the conversation reloads as it was sent. */
static void test_a_compaction_applies_at_the_seam_and_reloads_the_same(void) {
   const int64_t conv = db_open_conv();
   run_turn(conv, "Q1", "A1", "P1");
   run_turn(conv, "Q2", "A2", "P2"); /* the persona changes: an instruction row */
   TEST_ASSERT_EQUAL_INT(1, kind_count(MESSAGE_KIND_INSTRUCTION));
   run_turn(conv, "Q3", "A3", "P2");
   run_turn(conv, "Q4", "A4", "P2");
   run_turn(conv, "Q5", "A5", "P2");

   add("user", "Q6");
   save_question(conv, count() - 1);
   s_over = true;
   s_summary = "They talked about Q1 and Q2.";
   session_compaction_prepare(s, 0);
   TEST_ASSERT_EQUAL_INT(COMPACTION_READY, atomic_load(&s->compaction.state));
   TEST_ASSERT_NOT_NULL(strstr(s_summarized, "Q1"));
   TEST_ASSERT_NULL(strstr(s_summarized, "TURN CONTEXT")); /* injected context isn't summarized */

   composed_prompt_t cp = sectioned("P2", "R", "U");
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);

   /* The summarized turns are gone; the summary leads the first kept question. */
   TEST_ASSERT_EQUAL_INT(COMPACTION_IDLE, atomic_load(&s->compaction.state));
   const char *wire = json_object_to_json_string(s->conversation_history);
   TEST_ASSERT_NULL(strstr(wire, "\"Q1\""));
   struct json_object *first = at(1);
   TEST_ASSERT_TRUE(llm_history_is_question(first));
   struct json_object *parts = json_object_object_get(first, "content");
   TEST_ASSERT_EQUAL_INT(MESSAGE_KIND_SUMMARY,
                         llm_history_kind_of(json_object_array_get_idx(parts, 0)));
   /* The persona change was in the summarized part: sent again after Q6. */
   struct json_object *last = at(count() - 1);
   TEST_ASSERT_EQUAL_INT(MESSAGE_KIND_INSTRUCTION, llm_history_kind_of(last));
   TEST_ASSERT_NOT_NULL(strstr(content_of(count() - 1), "P2"));
   TEST_ASSERT_TRUE(s_noted_tokens > 0);
   TEST_ASSERT_EQUAL_INT(1, s_compaction_notices); /* the client's marker */

   /* Saved with the turn: the summary, its node, the watermark. */
   conversation_t c = { 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_get(conv, s_user, &c));
   TEST_ASSERT_EQUAL_STRING("They talked about Q1 and Q2.", c.compaction_summary);
   const int64_t watermark = c.context_watermark_msg_id;
   TEST_ASSERT_TRUE(watermark > 0);
   summary_node_t node = { 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, summary_node_get_latest(conv, &node));
   TEST_ASSERT_EQUAL_INT64(watermark, node.msg_id_end);
   summary_node_free(&node);

   /* A reload is what was sent. */
   struct json_object *reloaded = memory_history_request_context(conv, s_user, watermark,
                                                                 c.compaction_summary, NULL, NULL);
   conv_free(&c);
   TEST_ASSERT_NOT_NULL(reloaded);
   char *live = wire_of(s->conversation_history);
   char *again = wire_of(reloaded);
   TEST_ASSERT_EQUAL_STRING(live, again);
   free(live);
   free(again);
   json_object_put(reloaded);
   db_close();
}

/* A summary of a range the history no longer holds as it was is dropped. */
static void test_a_stale_summary_is_dropped(void) {
   const int64_t conv = db_open_conv();
   for (int i = 0; i < 4; i++) {
      char q[8], a[8];
      snprintf(q, sizeof(q), "Q%d", i);
      snprintf(a, sizeof(a), "A%d", i);
      run_turn(conv, q, a, "P");
   }
   add("user", "Qn");
   save_question(conv, count() - 1);
   s_over = true;
   s_summary = "S";
   session_compaction_prepare(s, 0);
   TEST_ASSERT_EQUAL_INT(COMPACTION_READY, atomic_load(&s->compaction.state));
   /* The first message after the prompt changes under it. */
   json_object_array_del_idx(s->conversation_history, 1, 1);
   composed_prompt_t cp = sectioned("P", "R", "U");
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
   TEST_ASSERT_EQUAL_INT(COMPACTION_IDLE, atomic_load(&s->compaction.state));
   TEST_ASSERT_NULL(
       strstr(json_object_to_json_string(s->conversation_history), "CONVERSATION SUMMARY"));
   db_close();
}

/* A message saved as its own row, as a history stored before prefixes were. */
static void add_saved(int64_t conv, const char *role, const char *content) {
   add(role, content);
   int64_t id = 0;
   const conv_message_row_t row = { .role = role, .content = content };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, s_user, &row, &id));
   json_object_object_add(at(count() - 1), "id", json_object_new_int64(id));
}

/* A summary made before the seam that freezes the history (a conversation
 * stored before prefixes, reopened past its window) is applied at that seam:
 * the frozen array is the same history. */
static void test_a_summary_follows_its_history_when_frozen(void) {
   const int64_t conv = db_open_conv();
   add("system", "P");
   for (int i = 0; i < 5; i++) {
      char q[8], a[8];
      snprintf(q, sizeof(q), "Q%d", i);
      snprintf(a, sizeof(a), "A%d", i);
      add_saved(conv, "user", q);
      add_saved(conv, "assistant", a);
   }
   add_saved(conv, "user", "Qn");
   s_over = true;
   s_summary = "Earlier questions.";
   session_compaction_prepare(s, 0);
   TEST_ASSERT_EQUAL_INT(COMPACTION_READY, atomic_load(&s->compaction.state));
   TEST_ASSERT_FALSE(session_prefix_is_frozen(s->conversation_history));

   composed_prompt_t cp = sectioned("P", "R", "U");
   session_prefix_apply_turn(s, &cp, NULL);
   composed_prompt_free(&cp);
   TEST_ASSERT_TRUE(session_prefix_is_frozen(s->conversation_history));
   TEST_ASSERT_EQUAL_INT(COMPACTION_IDLE, atomic_load(&s->compaction.state));
   TEST_ASSERT_NOT_NULL(
       strstr(json_object_to_json_string(s->conversation_history), "Earlier questions."));
   TEST_ASSERT_NULL(strstr(json_object_to_json_string(s->conversation_history), "\"Q0\""));
   db_close();
}

/* A session going away starts no summary; a context replaced drops the one
 * waiting for it, and what a voice surface kept for its save. */
static void test_a_closed_or_reset_session_keeps_no_summary(void) {
   const int64_t conv = db_open_conv();
   for (int i = 0; i < 4; i++) {
      char q[8], a[8];
      snprintf(q, sizeof(q), "Q%d", i);
      snprintf(a, sizeof(a), "A%d", i);
      run_turn(conv, q, a, "P");
   }
   add("user", "Qn");
   save_question(conv, count() - 1);
   s_over = true;
   s_summary = "S";

   session_compaction_prepare(s, 0);
   TEST_ASSERT_EQUAL_INT(COMPACTION_READY, atomic_load(&s->compaction.state));
   s->compaction.voice_removed = json_object_new_array();
   s->compaction.voice_summary = strdup("S");
   pthread_mutex_lock(&s->history_mutex);
   session_compaction_reset_locked(s);
   pthread_mutex_unlock(&s->history_mutex);
   TEST_ASSERT_EQUAL_INT(COMPACTION_IDLE, atomic_load(&s->compaction.state));
   TEST_ASSERT_NULL(s->compaction.hist);
   TEST_ASSERT_NULL(s->compaction.voice_removed);
   TEST_ASSERT_NULL(s->compaction.voice_summary);

   session_compaction_teardown(s);
   s_summarized[0] = '\0';
   session_compaction_prepare(s, 0);
   session_compaction_trigger(s, s->conversation_history, LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, "m");
   TEST_ASSERT_EQUAL_INT(COMPACTION_IDLE, atomic_load(&s->compaction.state));
   TEST_ASSERT_EQUAL_STRING("", s_summarized); /* nothing summarized */
   db_close();
}

/* A turn waiting on a summary still running stops waiting when it is itself
 * stopped, and the summary is let go (the turn that needed it is going). */
static void test_a_stopped_turn_stops_waiting(void) {
   const int64_t conv = db_open_conv();
   for (int i = 0; i < 4; i++) {
      char q[8], a[8];
      snprintf(q, sizeof(q), "Q%d", i);
      snprintf(a, sizeof(a), "A%d", i);
      run_turn(conv, q, a, "P");
   }
   s_over = true;
   /* A worker mid-summary, as prepare sees it. */
   atomic_store(&s->compaction.state, COMPACTION_RUNNING);
   atomic_store(&s->compaction.thread_active, true);
   atomic_store(&s->cancel_requested, true);
   session_compaction_prepare(s, 0); /* returns rather than waiting */
   TEST_ASSERT_TRUE(atomic_load(&s->compaction.cancel));
   TEST_ASSERT_EQUAL_INT(COMPACTION_RUNNING, atomic_load(&s->compaction.state));
   /* No real worker to join. */
   atomic_store(&s->compaction.thread_active, false);
   atomic_store(&s->compaction.state, COMPACTION_IDLE);
   atomic_store(&s->cancel_requested, false);
   db_close();
}

/* After a switch, a summary made to fit the model switched from is made again
 * for the one switched to. */
static void test_a_switch_summarizes_again(void) {
   const int64_t conv = db_open_conv();
   for (int i = 0; i < 4; i++) {
      char q[8], a[8];
      snprintf(q, sizeof(q), "Q%d", i);
      snprintf(a, sizeof(a), "A%d", i);
      run_turn(conv, q, a, "P");
   }
   add("user", "Qn");
   save_question(conv, count() - 1);
   s_over = true;
   s_summary = "For the old model.";
   session_compaction_prepare(s, 0);
   TEST_ASSERT_EQUAL_STRING("For the old model.", s->compaction.summary);

   session_llm_config_t from = { 0 };
   session_compaction_note_switch(s, &from);
   s_summary = "For the new model.";
   session_compaction_prepare(s, 0);
   TEST_ASSERT_EQUAL_INT(COMPACTION_READY, atomic_load(&s->compaction.state));
   TEST_ASSERT_EQUAL_STRING("For the new model.", s->compaction.summary);
   TEST_ASSERT_FALSE(atomic_load(&s->compaction.switched));
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
   RUN_TEST(test_a_compaction_applies_at_the_seam_and_reloads_the_same);
   RUN_TEST(test_a_stale_summary_is_dropped);
   RUN_TEST(test_a_summary_follows_its_history_when_frozen);
   RUN_TEST(test_a_closed_or_reset_session_keeps_no_summary);
   RUN_TEST(test_a_switch_summarizes_again);
   RUN_TEST(test_a_stopped_turn_stops_waiting);
   return UNITY_END();
}
