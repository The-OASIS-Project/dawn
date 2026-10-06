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
 * Images a tool returns (real auth_db in memory, real image store in a temp
 * directory): stored unbound and owner-only, bound with the tool row that
 * names them, reclaimed when no row does, rebuilt on reload from the row's
 * images as the live turn built them, saved by a voice save on its tool row,
 * and deleted with their conversation (or their user).
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "auth/auth_db_internal.h"
#undef AUTH_DB_INTERNAL_ALLOWED

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "auth/auth_db_messages.h"
#include "blob_store.h"
#include "config/dawn_config.h"
#include "core/conv_images.h"
#include "core/image_rehydrate.h"
#include "core/ocp_helpers.h"
#include "core/session_history.h"
#include "core/session_image_hold.h"
#include "core/session_manager.h"
#include "image_store.h"
#include "llm/llm_history_rows.h"
#include "llm/llm_tool_images.h"
#include "llm/llm_turn_blocks.h"
#include "memory/memory_extraction.h"
#include "memory/memory_history_loader.h"
#include "unity.h"

dawn_config_t g_config;

/* ---- The turn a tool runs in (session_manager / session_* stubs) ---- */

static session_t *s_ctx;
static int s_user;

session_t *session_get_command_context(void) {
   return s_ctx;
}
int session_effective_user_id(session_t *session) {
   /* s_ctx is no real session: only its user is read. */
   if (session != s_ctx && session && session->metrics.user_id > 0) {
      return (int)session->metrics.user_id;
   }
   return s_user;
}

/* The live sessions the unbound sweep asks (session_image_hold.c): an
 * interactive one and a job's. */
static session_t *s_live;
static session_t *s_job_live;
void session_manager_for_each_session_any(void (*fn)(session_t *session, void *ctx), void *ctx) {
   if (s_live) {
      fn(s_live, ctx);
   }
}
void job_manager_for_each_session(void (*fn)(session_t *session, void *ctx), void *ctx) {
   if (s_job_live) {
      fn(s_job_live, ctx);
   }
}

/* session_voice_save.c's neighbours: what it calls besides the database. */
char *get_command_prompt_dup(void) {
   return strdup("prompt");
}
void session_prefix_voice_save_locked(session_t *session) {
   (void)session;
}
bool session_prefix_is_frozen(struct json_object *history) {
   (void)history;
   return false;
}
void session_prefix_release_locked(session_t *session) {
   (void)session;
}
char *prefix_in_force_json(struct json_object *hist) {
   (void)hist;
   return NULL;
}
int focus_handles_save_locked(session_t *session, int64_t conv_id, int user_id) {
   (void)session;
   (void)conv_id;
   (void)user_id;
   return 0;
}
int tool_result_store_bind_locked(session_t *session,
                                  int64_t conv_id,
                                  uint64_t turn_token,
                                  bool with_ended) {
   (void)session;
   (void)conv_id;
   (void)turn_token;
   (void)with_ended;
   return AUTH_DB_SUCCESS;
}
int session_take_fact_sources_locked(session_t *session,
                                     session_fact_source_t out[SESSION_PENDING_FACT_SOURCES_MAX]) {
   (void)session;
   (void)out;
   return 0;
}
void session_record_fact_sources(const session_fact_source_t *facts,
                                 int count,
                                 int64_t conv_id,
                                 int owner_user_id) {
   (void)facts;
   (void)count;
   (void)conv_id;
   (void)owner_user_id;
}
void session_new_context_locked(session_t *session, const char *system_prompt) {
   (void)system_prompt;
   json_object_put(session->conversation_history);
   session->conversation_history = json_object_new_array();
}
void memory_extraction_build_fallback(session_t *session, memory_extraction_fallback_t *fb) {
   (void)session;
   (void)fb;
}
int memory_trigger_extraction(int user_id,
                              int64_t conversation_id,
                              const char *session_id_str,
                              struct json_object *conversation_history,
                              int message_count,
                              int duration_seconds,
                              const memory_extraction_fallback_t *fallback) {
   (void)user_id;
   (void)conversation_id;
   (void)session_id_str;
   (void)message_count;
   (void)duration_seconds;
   (void)fallback;
   json_object_put(conversation_history);
   return 0;
}

/* The model's window and its image limit (llm_context.c, llm_capabilities.c). */
int llm_context_get_size(llm_type_t type, cloud_provider_t provider, const char *model) {
   (void)type;
   (void)provider;
   (void)model;
   return 1000000;
}
static llm_image_limit_t s_limit = { 600, 24000000 };
bool llm_capabilities_image_limit(const char *key, llm_image_limit_t *out) {
   (void)key;
   *out = s_limit;
   return true;
}

/* ---- Fixture ---- */

static char s_tmpdir[256];

void setUp(void) {
   memset(&g_config, 0, sizeof(g_config));
   g_config.vision.max_image_size_kb = 4096;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("owner", "h", true));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("other", "h", false));
   snprintf(s_tmpdir, sizeof(s_tmpdir), "/tmp/dawn_toolimg_XXXXXX");
   TEST_ASSERT_NOT_NULL(mkdtemp(s_tmpdir));
   image_store_config_t cfg = { .max_size = 4 * 1024 * 1024,
                                .max_per_user = 100,
                                .data_dir = s_tmpdir };
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_init(&cfg));
   s_ctx = (session_t *)&s_user; /* any non-NULL session: only its user is read */
   s_user = 1;
   s_live = NULL;
   s_job_live = NULL;
}

void tearDown(void) {
   image_store_shutdown();
   blob_store_shutdown();
   auth_db_shutdown();
}

/* A PNG's bytes (its signature, then anything) as base64. */
static char *png_base64(const char *tag) {
   unsigned char bytes[64] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
   const size_t n = 8 + strlen(tag);
   memcpy(bytes + 8, tag, strlen(tag));
   return ocp_base64_encode(bytes, n);
}

/* A capture taken in the current turn (s_ctx, s_user). */
static void capture(tool_result_t *r, const char *tag) {
   memset(r, 0, sizeof(*r));
   snprintf(r->tool_call_id, sizeof(r->tool_call_id), "call_%s", tag);
   TEST_ASSERT_TRUE(llm_tool_images_ingest(png_base64(tag), r));
   snprintf(r->result, sizeof(r->result), "Image captured successfully.");
}

static image_retention_t retention_of(const char *id) {
   image_metadata_t m;
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_get_metadata(id, &m));
   return m.retention_policy;
}

static bool image_exists(const char *id) {
   image_metadata_t m;
   return image_store_get_metadata(id, &m) == IMAGE_STORE_SUCCESS;
}

static int64_t new_conv(int user) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(user, "t", &conv));
   return conv;
}

static char *images_json(const char *id) {
   char *out = malloc(64);
   snprintf(out, 64, "[\"%s\"]", id);
   return out;
}

/* An assistant call row and its result row (images: the result's). */
static void add_tool_turn_text(int64_t conv,
                               int user,
                               const char *call_id,
                               const char *text,
                               const char *images) {
   char calls[160];
   snprintf(calls, sizeof(calls),
            "[{\"id\":\"%s\",\"type\":\"function\",\"function\":{\"name\":\"viewing\","
            "\"arguments\":\"{}\"}}]",
            call_id);
   const conv_message_row_t a = { .role = "assistant", .content = "", .tool_calls = calls };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, user, &a, NULL));
   const conv_message_row_t t = { .role = "tool",
                                  .content = text,
                                  .tool_call_id = call_id,
                                  .images = images };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, user, &t, NULL));
}

static void add_tool_turn(int64_t conv, int user, const char *call_id, const char *images) {
   add_tool_turn_text(conv, user, call_id, "Image captured successfully.", images);
}

/* @p conv rebuilt as a model's request. */
static struct json_object *reload(int64_t conv, int user) {
   struct json_object *hist = memory_history_request_context(conv, user, 0, NULL, NULL, NULL);
   TEST_ASSERT_NOT_NULL(hist);
   return hist;
}

static struct json_object *find_tool(struct json_object *hist, const char *call_id) {
   for (size_t i = 0; i < json_object_array_length(hist); i++) {
      struct json_object *m = json_object_array_get_idx(hist, i);
      struct json_object *id = NULL;
      if (json_object_object_get_ex(m, "tool_call_id", &id) &&
          strcmp(json_object_get_string(id), call_id) == 0) {
         return m;
      }
   }
   return NULL;
}

static const char *json_text(struct json_object *obj) {
   return json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
}

/* ---- Tests ---- */

static void test_capture_stored_unbound_owner_only(void) {
   tool_result_t r;
   capture(&r, "a");
   TEST_ASSERT_TRUE(image_store_validate_id(r.vision_image_id));
   TEST_ASSERT_EQUAL_INT(1, r.vision_image_owner);
   image_metadata_t m;
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_get_metadata(r.vision_image_id, &m));
   TEST_ASSERT_EQUAL_INT(IMAGE_SOURCE_CAPTURE, m.source);
   TEST_ASSERT_EQUAL_INT(IMAGE_RETAIN_UNBOUND, m.retention_policy);
   TEST_ASSERT_EQUAL_STRING("image/png", m.mime_type); /* by its bytes */

   char path[IMAGE_PATH_MAX];
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS,
                         image_store_get_path(r.vision_image_id, 1, path, NULL));
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_FORBIDDEN,
                         image_store_get_path(r.vision_image_id, 2, path, NULL));
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_FORBIDDEN, /* the service token */
                         image_store_get_path(r.vision_image_id, 0, path, NULL));
   free(r.vision_image);
}

static void test_refused_and_guest_captures(void) {
   tool_result_t r;
   memset(&r, 0, sizeof(r));
   /* Not an image by its bytes, whatever it claims. */
   char *text = ocp_base64_encode((const unsigned char *)"GIF? no, plain text", 19);
   TEST_ASSERT_FALSE(llm_tool_images_ingest(text, &r));
   TEST_ASSERT_NULL(r.vision_image);

   /* Over the limit. */
   const size_t big = LLM_TOOL_IMAGE_MAX_BYTES + 1;
   unsigned char *bytes = calloc(1, big);
   memcpy(bytes, "\xff\xd8\xff", 3);
   char *b64 = ocp_base64_encode(bytes, big);
   free(bytes);
   TEST_ASSERT_FALSE(llm_tool_images_ingest(b64, &r));
   TEST_ASSERT_NULL(r.vision_image);
   TEST_ASSERT_NOT_NULL(strstr(r.result, "refused"));

   int count = -1;
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_count_user(1, &count));
   TEST_ASSERT_EQUAL_INT(0, count);

   /* A guest's capture: in memory only, shown without a stored id. */
   s_user = 0;
   TEST_ASSERT_TRUE(llm_tool_images_ingest(png_base64("g"), &r));
   TEST_ASSERT_EQUAL_STRING("", r.vision_image_id);
   TEST_ASSERT_NOT_NULL(r.vision_image);
   struct json_object *content = llm_tool_images_result_content(&r);
   TEST_ASSERT_TRUE(json_object_is_type(content, json_type_array));
   TEST_ASSERT_EQUAL_INT(2, (int)json_object_array_length(content));
   TEST_ASSERT_NULL(strstr(json_text(content), IMAGE_PART_ID_KEY));
   json_object_put(content);
   free(r.vision_image);
}

static void test_bound_with_its_row_in_one_transaction(void) {
   tool_result_t r;
   capture(&r, "b");
   char *images = images_json(r.vision_image_id);

   /* A row that isn't saved (someone else's conversation) binds nothing. */
   const int64_t theirs = new_conv(2);
   const conv_message_row_t t = { .role = "tool",
                                  .content = "x",
                                  .tool_call_id = "c",
                                  .images = images };
   TEST_ASSERT_NOT_EQUAL(AUTH_DB_SUCCESS, conv_db_add_row(theirs, 1, &t, NULL));
   TEST_ASSERT_EQUAL_INT(IMAGE_RETAIN_UNBOUND, retention_of(r.vision_image_id));

   /* Saved: bound with it. */
   const int64_t conv = new_conv(1);
   add_tool_turn(conv, 1, "call_b", images);
   TEST_ASSERT_EQUAL_INT(IMAGE_RETAIN_PERMANENT, retention_of(r.vision_image_id));

   /* Images only go on a tool row. */
   const conv_message_row_t u = { .role = "user", .content = "hi", .images = images };
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, 1, &u, &id));
   sqlite3_stmt *st = NULL;
   sqlite3_prepare_v2(s_db.db, "SELECT images IS NULL FROM messages WHERE id = ?", -1, &st, NULL);
   sqlite3_bind_int64(st, 1, id);
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
   TEST_ASSERT_EQUAL_INT(1, sqlite3_column_int(st, 0));
   sqlite3_finalize(st);
   free(images);
   free(r.vision_image);
}

static void test_unbound_reclaimed_after_grace(void) {
   tool_result_t kept;
   tool_result_t dropped;
   capture(&kept, "k");
   capture(&dropped, "d");
   char *images = images_json(kept.vision_image_id);
   add_tool_turn(new_conv(1), 1, "call_k", images);
   free(images);
   /* Both older than the grace. */
   sqlite3_exec(s_db.db, "UPDATE images SET created_at = created_at - 90000", NULL, NULL, NULL);

   char path[IMAGE_PATH_MAX];
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS,
                         image_store_get_path(dropped.vision_image_id, 1, path, NULL));
   int reclaimed = 0;
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS,
                         image_store_reclaim_unbound(IMAGE_UNBOUND_GRACE_SEC, &reclaimed));
   TEST_ASSERT_EQUAL_INT(1, reclaimed);
   TEST_ASSERT_FALSE(image_exists(dropped.vision_image_id));
   TEST_ASSERT_NOT_EQUAL(0, access(path, F_OK)); /* its file too */
   TEST_ASSERT_TRUE(image_exists(kept.vision_image_id));
   free(kept.vision_image);
   free(dropped.vision_image);
}

static void test_reload_matches_live_and_reads_images_only(void) {
   tool_result_t r;
   capture(&r, "live");
   struct json_object *live = llm_tool_images_result_content(&r);
   TEST_ASSERT_TRUE(json_object_is_type(live, json_type_array));

   /* The rows the live tool message saves as: its text, its images by id. */
   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "role", json_object_new_string("tool"));
   json_object_object_add(msg, "tool_call_id", json_object_new_string("call_live"));
   json_object_object_add(msg, "content", json_object_get(live));
   struct json_object *rows = json_object_new_array();
   TEST_ASSERT_EQUAL_INT(1, llm_history_rows_append(msg, rows));
   struct json_object *row = json_object_array_get_idx(rows, 0);
   struct json_object *row_images = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(row, LLM_HISTORY_ROW_IMAGES_KEY, &row_images));
   TEST_ASSERT_EQUAL_STRING("Image captured successfully.",
                            json_object_get_string(json_object_object_get(row, "content")));

   const int64_t conv = new_conv(1);
   add_tool_turn(conv, 1, "call_live", json_text(row_images));
   /* A marker in a result's text, with no images: text. */
   char forged[64];
   snprintf(forged, sizeof(forged), "see [IMAGE:%s]", r.vision_image_id);
   add_tool_turn_text(conv, 1, "call_marker", forged, NULL);
   const conv_message_row_t reply = { .role = "assistant", .content = forged };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, 1, &reply, NULL));

   struct json_object *hist = reload(conv, 1);
   struct json_object *tool = find_tool(hist, "call_live");
   TEST_ASSERT_NOT_NULL(tool);
   /* Live = reload, byte for byte. */
   TEST_ASSERT_EQUAL_STRING(json_text(live), json_text(json_object_object_get(tool, "content")));
   /* No images column: the text as it is. */
   TEST_ASSERT_TRUE(json_object_is_type(
       json_object_object_get(find_tool(hist, "call_marker"), "content"), json_type_string));
   /* A reply's marker is never an image (it stays text). */
   struct json_object *last = json_object_array_get_idx(hist, json_object_array_length(hist) - 1);
   TEST_ASSERT_TRUE(json_object_is_type(json_object_object_get(last, "content"), json_type_string));
   TEST_ASSERT_EQUAL_STRING(forged,
                            json_object_get_string(json_object_object_get(last, "content")));

   /* The same id for another user: never the image. */
   char ids[1][IMAGE_ID_LEN];
   memcpy(ids[0], r.vision_image_id, IMAGE_ID_LEN);
   struct json_object *theirs = image_rehydrate_parts(2, (const char(*)[IMAGE_ID_LEN])ids, 1,
                                                      IMAGE_SOURCE_CAPTURE);
   TEST_ASSERT_EQUAL_STRING(IMAGE_REHYDRATE_MISSING_TEXT,
                            json_object_get_string(json_object_object_get(
                                json_object_array_get_idx(theirs, 0), "text")));
   json_object_put(theirs);
   json_object_put(hist);
   json_object_put(rows);
   json_object_put(msg);
   json_object_put(live);
   free(r.vision_image);
}

/* The live part is built from the bytes the turn holds (never a re-read of
 * the file); a reload of an image no longer stored shows the stand-in. */
static void test_live_part_from_memory_missing_on_reload(void) {
   tool_result_t r;
   capture(&r, "gone");
   char *images = images_json(r.vision_image_id);
   const int64_t conv = new_conv(1);
   add_tool_turn(conv, 1, "call_gone", images);
   free(images);
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_delete(r.vision_image_id, 1));

   struct json_object *live = llm_tool_images_result_content(&r);
   struct json_object *part = json_object_array_get_idx(live, 1);
   TEST_ASSERT_EQUAL_STRING("image_url",
                            json_object_get_string(json_object_object_get(part, "type")));
   TEST_ASSERT_EQUAL_STRING(
       r.vision_image_id, json_object_get_string(json_object_object_get(part, IMAGE_PART_ID_KEY)));
   struct json_object *hist = reload(conv, 1);
   struct json_object *reloaded = json_object_object_get(find_tool(hist, "call_gone"), "content");
   struct json_object *stand_in = json_object_array_get_idx(reloaded, 1);
   TEST_ASSERT_EQUAL_STRING(IMAGE_REHYDRATE_MISSING_TEXT,
                            json_object_get_string(json_object_object_get(stand_in, "text")));
   /* Its rows keep the id, never the stand-in text. */
   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "role", json_object_new_string("tool"));
   json_object_object_add(msg, "tool_call_id", json_object_new_string("call_gone"));
   json_object_object_add(msg, "content", json_object_get(live));
   struct json_object *rows = json_object_new_array();
   llm_history_rows_append(msg, rows);
   TEST_ASSERT_NULL(strstr(json_text(rows), IMAGE_REHYDRATE_MISSING_TEXT));
   json_object_put(rows);
   json_object_put(msg);
   json_object_put(hist);
   json_object_put(live);
   free(r.vision_image);
}

static void test_claude_result_saves_images_by_id(void) {
   tool_result_t r;
   capture(&r, "claude");
   struct json_object *block = json_object_new_object();
   json_object_object_add(block, "type", json_object_new_string("tool_result"));
   json_object_object_add(block, "tool_use_id", json_object_new_string("toolu_1"));
   json_object_object_add(block, "content", llm_tool_images_result_content(&r));
   struct json_object *parts = json_object_new_array();
   json_object_array_add(parts, block);
   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "role", json_object_new_string("user"));
   json_object_object_add(msg, "content", parts);

   struct json_object *rows = json_object_new_array();
   TEST_ASSERT_EQUAL_INT(1, llm_history_rows_append(msg, rows));
   struct json_object *row = json_object_array_get_idx(rows, 0);
   TEST_ASSERT_EQUAL_STRING("tool", json_object_get_string(json_object_object_get(row, "role")));
   TEST_ASSERT_EQUAL_STRING("Image captured successfully.",
                            json_object_get_string(json_object_object_get(row, "content")));
   struct json_object *images = json_object_object_get(row, LLM_HISTORY_ROW_IMAGES_KEY);
   TEST_ASSERT_EQUAL_STRING(r.vision_image_id,
                            json_object_get_string(json_object_array_get_idx(images, 0)));
   /* DAWN's own keys never go on the wire, nested in a result's content too. */
   struct json_object *hist = json_object_new_array();
   json_object_array_add(hist, json_object_get(msg));
   struct json_object *wire = llm_history_wire_copy(hist);
   TEST_ASSERT_NOT_NULL(wire);
   TEST_ASSERT_NULL(strstr(json_text(wire), IMAGE_PART_ID_KEY));
   TEST_ASSERT_NOT_NULL(strstr(json_text(hist), IMAGE_PART_ID_KEY)); /* the history keeps it */
   json_object_put(wire);
   json_object_put(hist);
   json_object_put(rows);
   json_object_put(msg);
   free(r.vision_image);
}

/* A voice session, its history held in memory until it is saved. */
static session_t *voice_session(void) {
   session_t *s = calloc(1, sizeof(*s));
   pthread_mutex_init(&s->history_mutex, NULL);
   s->conversation_history = json_object_new_array();
   s->type = SESSION_TYPE_LOCAL;
   return s;
}

static void voice_free(session_t *s) {
   json_object_put(s->conversation_history);
   pthread_mutex_destroy(&s->history_mutex);
   free(s);
}

static void push(session_t *s, const char *role, struct json_object *content) {
   struct json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string(role));
   json_object_object_add(m, "content", content);
   json_object_array_add(s->conversation_history, m);
}

typedef struct {
   int tool_rows_with_images;
   int image_text_rows;
   char images[64];
} saved_t;

static int on_saved(const conversation_llm_row_t *row, void *ctx) {
   saved_t *s = ctx;
   if (row->content && strcmp(row->content, "[image]") == 0) {
      s->image_text_rows++;
   }
   if (strcmp(row->role, "tool") == 0 && row->images) {
      s->tool_rows_with_images++;
      snprintf(s->images, sizeof(s->images), "%s", row->images);
   }
   return 0;
}

static void test_voice_save_writes_tool_row_with_images(void) {
   tool_result_t r;
   capture(&r, "voice");
   session_t *s = voice_session();
   s_ctx = s;
   push(s, "system", json_object_new_string("prompt"));
   push(s, "user", json_object_new_string("what do you see?"));
   struct json_object *calls = json_tokener_parse(
       "[{\"id\":\"call_v\",\"type\":\"function\",\"function\":{\"name\":\"viewing\","
       "\"arguments\":\"{}\"}}]");
   push(s, "assistant", json_object_new_string(""));
   json_object_object_add(json_object_array_get_idx(s->conversation_history, 2), "tool_calls",
                          calls);
   snprintf(r.tool_call_id, sizeof(r.tool_call_id), "call_v");
   push(s, "tool", llm_tool_images_result_content(&r));
   json_object_object_add(json_object_array_get_idx(s->conversation_history, 3), "tool_call_id",
                          json_object_new_string("call_v"));
   push(s, "assistant", json_object_new_string("A desk."));

   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(0, session_save_voice_conversation(s, &conv));
   TEST_ASSERT_TRUE(conv > 0);
   saved_t saved = { 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_for_llm(conv, 1, 0, on_saved, &saved));
   TEST_ASSERT_EQUAL_INT(1, saved.tool_rows_with_images);
   TEST_ASSERT_EQUAL_INT(0, saved.image_text_rows);
   TEST_ASSERT_NOT_NULL(strstr(saved.images, r.vision_image_id));
   TEST_ASSERT_EQUAL_INT(IMAGE_RETAIN_PERMANENT, retention_of(r.vision_image_id));
   voice_free(s);
   free(r.vision_image);
}

/* An upload of @p user's (a JPEG). */
static void upload(int user, char id[IMAGE_ID_LEN]) {
   const unsigned char jpeg[] = { 0xff, 0xd8, 0xff, 0xe0, 'x' };
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS,
                         image_store_save(user, jpeg, sizeof(jpeg), "image/jpeg", id));
}

/* A question naming @p id, in @p conv. */
static void ask_with(int64_t conv, const char *id) {
   char text[128];
   snprintf(text, sizeof(text), "look\n[IMAGE:%s]", id);
   const conv_message_row_t q = { .role = "user", .content = text };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, 1, &q, NULL));
}

/* An image's file on disk. */
static bool file_exists(const char *id) {
   char path[IMAGE_PATH_MAX];
   return image_store_get_path(id, 1, path, NULL) == IMAGE_STORE_SUCCESS && access(path, F_OK) == 0;
}

/* A conversation delete takes the images only it names and has bound (its
 * captures, its questions' uploads), rows and files, in its transaction; an
 * image a reply only quotes, one another conversation names, and one a
 * running turn holds unbound all stay. */
static void test_delete_takes_only_what_the_conversation_owns(void) {
   tool_result_t cap;
   tool_result_t running;
   capture(&cap, "del");
   capture(&running, "run");
   char own[IMAGE_ID_LEN], quoted[IMAGE_ID_LEN], shared[IMAGE_ID_LEN];
   upload(1, own);
   upload(1, quoted);
   upload(1, shared);

   const int64_t conv = new_conv(1);
   const int64_t other = new_conv(1);
   ask_with(conv, own);
   ask_with(conv, shared);
   ask_with(other, shared);
   char *images = images_json(cap.vision_image_id);
   add_tool_turn(conv, 1, "call_del", images);
   free(images);
   char *held = images_json(running.vision_image_id);
   const conv_message_row_t later = { .role = "tool",
                                      .content = "x",
                                      .tool_call_id = "call_run",
                                      .images = held,
                                      .images_bind_later = true };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, 1, &later, NULL));
   char text[128];
   snprintf(text, sizeof(text), "as before [IMAGE:%s]", quoted);
   const conv_message_row_t reply = { .role = "assistant", .content = text };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, 1, &reply, NULL));
   TEST_ASSERT_TRUE(file_exists(cap.vision_image_id));

   /* Someone else's id can't delete it. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, conv_images_delete_conversation(conv, 2));
   /* Admin (any owner): the conversation's own go, as their owner's. */
   char cap_path[IMAGE_PATH_MAX];
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS,
                         image_store_get_path(cap.vision_image_id, 1, cap_path, NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_images_delete_conversation(conv, 0));
   TEST_ASSERT_FALSE(image_exists(cap.vision_image_id));
   TEST_ASSERT_NOT_EQUAL(0, access(cap_path, F_OK)); /* its file too */
   TEST_ASSERT_FALSE(image_exists(own));
   TEST_ASSERT_TRUE(image_exists(quoted));                  /* a reply can't own an image */
   TEST_ASSERT_TRUE(image_exists(shared));                  /* another conversation names it */
   TEST_ASSERT_TRUE(image_exists(running.vision_image_id)); /* unbound: the turn's */
   TEST_ASSERT_EQUAL_INT(IMAGE_RETAIN_UNBOUND, retention_of(running.vision_image_id));

   /* The other conversation is now the shared one's only: it goes with it. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_images_delete_conversation(other, 1));
   TEST_ASSERT_FALSE(image_exists(shared));

   /* An account delete purges the user's stores. */
   TEST_ASSERT_TRUE(conv_images_purge_user(1));
   TEST_ASSERT_FALSE(image_exists(quoted));
   TEST_ASSERT_FALSE(image_exists(running.vision_image_id));
   free(cap.vision_image);
   free(running.vision_image);
}

/* Rows stored before conversations recorded their images (an earlier v98
 * re-run here): the migration records them as the insert path does, and a
 * delete then takes what only that conversation names. */
static void test_backfilled_records_drive_the_delete(void) {
   tool_result_t cap;
   capture(&cap, "old");
   char shared[IMAGE_ID_LEN], only[IMAGE_ID_LEN];
   upload(1, shared);
   upload(1, only);
   const int64_t conv = new_conv(1);
   const int64_t other = new_conv(1);
   ask_with(conv, shared);
   ask_with(conv, only);
   ask_with(other, shared);
   char *images = images_json(cap.vision_image_id);
   add_tool_turn(conv, 1, "call_old", images);
   free(images);
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(s_db.db, "DELETE FROM conversation_images", NULL,
                                                 NULL, NULL));

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_migrations_v98(s_db.db, NULL));
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK,
                         sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM conversation_images", -1,
                                            &st, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
   TEST_ASSERT_EQUAL_INT(4, sqlite3_column_int(st, 0));
   sqlite3_finalize(st);

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_images_delete_conversation(conv, 1));
   TEST_ASSERT_FALSE(image_exists(only));
   TEST_ASSERT_FALSE(image_exists(cap.vision_image_id));
   TEST_ASSERT_TRUE(image_exists(shared)); /* the other conversation names it */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_images_delete_conversation(other, 1));
   TEST_ASSERT_FALSE(image_exists(shared));
   free(cap.vision_image);
}

/* What the delete takes is chosen in its own transaction, so no capture is
 * left bound to a conversation that is gone: one bound before the delete
 * goes with it, and a row arriving after it can't be stored, so its capture
 * stays unbound for the grace sweep (the old collect-then-delete let a bind
 * land between the two and stranded it, permanent, with nothing naming it). */
static void test_delete_never_strands_a_capture(void) {
   tool_result_t before;
   tool_result_t after;
   capture(&before, "before");
   capture(&after, "after");
   const int64_t conv = new_conv(1);
   char *images = images_json(before.vision_image_id);
   add_tool_turn(conv, 1, "call_before", images);
   free(images);
   TEST_ASSERT_EQUAL_INT(IMAGE_RETAIN_PERMANENT, retention_of(before.vision_image_id));

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_images_delete_conversation(conv, 1));
   TEST_ASSERT_FALSE(image_exists(before.vision_image_id));

   /* The row naming the second capture comes after: refused, nothing bound. */
   char *late = images_json(after.vision_image_id);
   const conv_message_row_t t = { .role = "tool",
                                  .content = "x",
                                  .tool_call_id = "call_after",
                                  .images = late };
   TEST_ASSERT_NOT_EQUAL(AUTH_DB_SUCCESS, conv_db_add_row(conv, 1, &t, NULL));
   free(late);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, conv_db_bind_images(conv, 0, NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_bind_images(conv, 1, NULL));
   TEST_ASSERT_EQUAL_INT(IMAGE_RETAIN_UNBOUND, retention_of(after.vision_image_id));
   free(before.vision_image);
   free(after.vision_image);
}

/* A voice save rolled back: the captures it bound go back to unbound for its
 * retry; nothing a reply names, nor anything another conversation names, is
 * touched, and no file is removed. */
static void test_rollback_unbinds_only_its_captures(void) {
   tool_result_t mine;
   tool_result_t shared;
   capture(&mine, "mine");
   capture(&shared, "shared");
   char quoted[IMAGE_ID_LEN];
   upload(1, quoted);
   const int64_t conv = new_conv(1);
   const int64_t other = new_conv(1);
   char *a = images_json(mine.vision_image_id);
   char *b = images_json(shared.vision_image_id);
   add_tool_turn(conv, 1, "call_mine", a);
   add_tool_turn(conv, 1, "call_shared", b);
   add_tool_turn(other, 1, "call_shared2", b);
   free(a);
   free(b);
   char text[128];
   snprintf(text, sizeof(text), "as before [IMAGE:%s]", quoted);
   const conv_message_row_t reply = { .role = "assistant", .content = text };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, 1, &reply, NULL));

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_delete_ex(conv, 1, false, CONV_IMAGES_UNBIND, NULL));
   TEST_ASSERT_EQUAL_INT(IMAGE_RETAIN_UNBOUND, retention_of(mine.vision_image_id));
   TEST_ASSERT_TRUE(file_exists(mine.vision_image_id));
   TEST_ASSERT_EQUAL_INT(IMAGE_RETAIN_PERMANENT, retention_of(shared.vision_image_id));
   TEST_ASSERT_TRUE(image_exists(quoted));
   free(mine.vision_image);
   free(shared.vision_image);
}

/* A capture a row names that is no longer stored can't be bound: the bind
 * says so (a warning, never a failure: the save would fail on every retry). */
static void test_short_bind_is_reported(void) {
   tool_result_t kept;
   tool_result_t gone;
   capture(&kept, "kept");
   capture(&gone, "gone");
   const int64_t conv = new_conv(1);
   char images[96];
   snprintf(images, sizeof(images), "[\"%s\",\"%s\"]", kept.vision_image_id, gone.vision_image_id);
   const conv_message_row_t t = { .role = "tool",
                                  .content = "x",
                                  .tool_call_id = "call_two",
                                  .images = images,
                                  .images_bind_later = true };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, 1, &t, NULL));
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_delete(gone.vision_image_id, 1));
   int missing = -1;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_bind_images(conv, 1, &missing));
   TEST_ASSERT_EQUAL_INT(1, missing);
   TEST_ASSERT_EQUAL_INT(IMAGE_RETAIN_PERMANENT, retention_of(kept.vision_image_id));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_bind_images(conv, 1, &missing));
   TEST_ASSERT_EQUAL_INT(1, missing); /* still short, still a success */
   free(kept.vision_image);
   free(gone.vision_image);
}

/* An unbound capture past its grace that a live session's unsaved history
 * still names (a voice session that hasn't gone idle) is kept; once nothing
 * holds it, the sweep reclaims it. */
static void test_sweep_spares_what_a_live_session_holds(void) {
   tool_result_t r;
   capture(&r, "held");
   session_t *s = voice_session();
   push(s, "tool", llm_tool_images_result_content(&r));
   s_live = s;
   sqlite3_exec(s_db.db, "UPDATE images SET created_at = created_at - 90000", NULL, NULL, NULL);

   int reclaimed = -1;
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS,
                         image_store_reclaim_unbound(IMAGE_UNBOUND_GRACE_SEC, &reclaimed));
   TEST_ASSERT_EQUAL_INT(0, reclaimed);
   TEST_ASSERT_TRUE(image_exists(r.vision_image_id));

   /* Held through a compaction too (its rows wait for the voice save). */
   struct json_object *rows = json_object_new_array();
   struct json_object *row = json_object_new_object();
   char *ids = images_json(r.vision_image_id);
   json_object_object_add(row, LLM_HISTORY_ROW_IMAGES_KEY, json_tokener_parse(ids));
   free(ids);
   json_object_array_add(rows, row);
   s->compaction.voice_removed = json_object_new_array();
   json_object_array_add(s->compaction.voice_removed, rows);
   json_object_put(s->conversation_history);
   s->conversation_history = json_object_new_array();
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS,
                         image_store_reclaim_unbound(IMAGE_UNBOUND_GRACE_SEC, &reclaimed));
   TEST_ASSERT_EQUAL_INT(0, reclaimed);

   json_object_put(s->compaction.voice_removed);
   s->compaction.voice_removed = NULL;
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS,
                         image_store_reclaim_unbound(IMAGE_UNBOUND_GRACE_SEC, &reclaimed));
   TEST_ASSERT_EQUAL_INT(1, reclaimed);
   TEST_ASSERT_FALSE(image_exists(r.vision_image_id));
   s_live = NULL;
   voice_free(s);
   free(r.vision_image);
}

/* An unbound image of @p user's, created @p age seconds ago (rows only: the
 * sweep's unlink of a missing file is a no-op). */
static void unbound_row(const char *id, int user, int age) {
   char sql[256];
   snprintf(sql, sizeof(sql),
            "INSERT INTO images (id, user_id, source, retention_policy, mime_type, size, "
            "filename, created_at) VALUES ('%s', %d, 5, 3, 'image/png', 1, '%s.png', "
            "strftime('%%s','now') - %d)",
            id, user, id, age);
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(s_db.db, sql, NULL, NULL, NULL));
}

/* A history message naming @p id as a stored image part. */
static void name_image(session_t *s, const char *id) {
   struct json_object *part = json_object_new_object();
   json_object_object_add(part, IMAGE_PART_ID_KEY, json_object_new_string(id));
   struct json_object *content = json_object_new_array();
   json_object_array_add(content, part);
   push(s, "tool", content);
}

static int unbound_count(void) {
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(s_db.db,
                                                       "SELECT COUNT(*) FROM images WHERE "
                                                       "retention_policy = 3",
                                                       -1, &st, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
   const int n = sqlite3_column_int(st, 0);
   sqlite3_finalize(st);
   return n;
}

/* More held images than one batch, oldest first: the sweep walks past them
 * and reclaims the newer ones nothing holds. */
static void test_sweep_passes_a_batch_held_whole(void) {
   session_t *s = voice_session();
   char id[IMAGE_ID_LEN];
   for (int i = 0; i < 105; i++) {
      snprintf(id, sizeof(id), "img_h%011d", i);
      unbound_row(id, 1, 200000 - i);
      name_image(s, id);
   }
   for (int i = 0; i < 5; i++) {
      snprintf(id, sizeof(id), "img_n%011d", i);
      unbound_row(id, 1, 100000 - i);
   }
   s_live = s;
   int reclaimed = -1;
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS,
                         image_store_reclaim_unbound(IMAGE_UNBOUND_GRACE_SEC, &reclaimed));
   TEST_ASSERT_EQUAL_INT(5, reclaimed);
   TEST_ASSERT_EQUAL_INT(105, unbound_count());
   s_live = NULL;
   voice_free(s);
}

/* An image is held only by its owner's sessions (interactive or a job's): a
 * session of another user naming its id doesn't keep it. */
static void test_hold_is_the_owners_and_counts_jobs(void) {
   session_t *other = voice_session();
   other->metrics.user_id = 2;
   name_image(other, "img_owner1aaaaaa");
   session_t *job = voice_session();
   job->type = SESSION_TYPE_JOB;
   job->metrics.user_id = 1;
   name_image(job, "img_jobheldaaaaa");
   unbound_row("img_owner1aaaaaa", 1, 100000);
   unbound_row("img_jobheldaaaaa", 1, 100000);
   s_live = other;
   s_job_live = job;
   int reclaimed = -1;
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS,
                         image_store_reclaim_unbound(IMAGE_UNBOUND_GRACE_SEC, &reclaimed));
   TEST_ASSERT_EQUAL_INT(1, reclaimed);
   TEST_ASSERT_FALSE(image_exists("img_owner1aaaaaa")); /* user 2 can't hold user 1's */
   TEST_ASSERT_TRUE(image_exists("img_jobheldaaaaa"));  /* a job's history holds it */
   s_job_live = NULL;
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS,
                         image_store_reclaim_unbound(IMAGE_UNBOUND_GRACE_SEC, &reclaimed));
   TEST_ASSERT_EQUAL_INT(1, reclaimed);
   s_live = NULL;
   voice_free(other);
   voice_free(job);
}

/* A capture that would take the next request past the model's image limit
 * is refused in its turn (a loop has no seam), its stored image deleted; the
 * ones that fit are kept. */
static void test_capture_past_the_request_limit_refused(void) {
   struct json_object *hist = json_tokener_parse(
       "[{\"role\":\"tool\",\"tool_call_id\":\"c0\",\"content\":[{\"type\":\"text\",\"text\":"
       "\"x\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,AA==\"}}]}]");
   tool_result_list_t *results = calloc(1, sizeof(*results));
   capture(&results->results[0], "fits");
   capture(&results->results[1], "over");
   results->count = 2;
   char kept[IMAGE_ID_LEN], refused[IMAGE_ID_LEN];
   memcpy(kept, results->results[0].vision_image_id, IMAGE_ID_LEN);
   memcpy(refused, results->results[1].vision_image_id, IMAGE_ID_LEN);

   const llm_image_limit_t limit = { 2, 24000000 };
   TEST_ASSERT_EQUAL_INT(1, llm_tool_images_cap_batch(hist, results, &limit));
   TEST_ASSERT_NOT_NULL(results->results[0].vision_image);
   TEST_ASSERT_NULL(results->results[1].vision_image);
   TEST_ASSERT_EQUAL_STRING("", results->results[1].vision_image_id);
   TEST_ASSERT_EQUAL_STRING("Error: " LLM_TOOL_IMAGES_REFUSED_TEXT, results->results[1].result);
   TEST_ASSERT_TRUE(image_exists(kept));
   TEST_ASSERT_FALSE(image_exists(refused));
   /* Its content is its text alone. */
   struct json_object *content = llm_tool_images_result_content(&results->results[1]);
   TEST_ASSERT_TRUE(json_object_is_type(content, json_type_string));
   json_object_put(content);

   /* Bytes count too. */
   tool_result_list_t *more = calloc(1, sizeof(*more));
   capture(&more->results[0], "bytes");
   more->count = 1;
   const llm_image_limit_t tight = { 600, 40 };
   TEST_ASSERT_EQUAL_INT(1, llm_tool_images_cap_batch(hist, more, &tight));

   free(results->results[0].vision_image);
   free(results);
   free(more);
   json_object_put(hist);
}

/* A user's captures have their own cap: they never use up the room uploads
 * need, nor uploads theirs; past it a capture is refused (never an earlier
 * one evicted: a conversation names it). */
static void test_captures_capped_apart_from_uploads(void) {
   image_store_shutdown();
   image_store_config_t cfg = { .max_size = 4 * 1024 * 1024,
                                .max_per_user = 1,
                                .max_captures_per_user = 2,
                                .data_dir = s_tmpdir };
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_init(&cfg));

   char upload[IMAGE_ID_LEN];
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_save(1, "x", 1, "image/gif", upload));
   tool_result_t a, b, c;
   capture(&a, "one");
   capture(&b, "two");
   memset(&c, 0, sizeof(c));
   TEST_ASSERT_FALSE(llm_tool_images_ingest(png_base64("three"), &c));
   TEST_ASSERT_NOT_NULL(strstr(c.result, "captures are at their limit"));
   TEST_ASSERT_TRUE(image_exists(a.vision_image_id)); /* nothing evicted */
   char second[IMAGE_ID_LEN];
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_LIMIT_EXCEEDED,
                         image_store_save(1, "y", 1, "image/gif", second));
   free(a.vision_image);
   free(b.vision_image);
}

/* A capture cap that can't be checked refuses the capture (fails closed),
 * rather than counting it as zero. */
static void test_capture_cap_fails_closed(void) {
   image_store_shutdown();
   sqlite3_stmt *good = s_db.stmt_image_count_user_source;
   sqlite3_stmt *broken = NULL;
   /* Binds both parameters, and fails on every step (an integer overflow). */
   TEST_ASSERT_EQUAL_INT(
       SQLITE_OK,
       sqlite3_prepare_v2(s_db.db, "SELECT abs(-9223372036854775807 - 1 + ?1 * 0 + ?2 * 0)", -1,
                          &broken, NULL));
   s_db.stmt_image_count_user_source = broken;
   image_store_config_t cfg = { .max_size = 4 * 1024 * 1024,
                                .max_per_user = 100,
                                .data_dir = s_tmpdir };
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_init(&cfg));
   tool_result_t r;
   memset(&r, 0, sizeof(r));
   TEST_ASSERT_FALSE(llm_tool_images_ingest(png_base64("uncounted"), &r));
   TEST_ASSERT_NULL(r.vision_image);
   int count = -1;
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_count_user(1, &count));
   TEST_ASSERT_EQUAL_INT(0, count);
   /* An upload's cap subtracts the captures: it can't be checked either. */
   char id[IMAGE_ID_LEN];
   TEST_ASSERT_NOT_EQUAL(IMAGE_STORE_SUCCESS, image_store_save(1, "x", 1, "image/gif", id));
   s_db.stmt_image_count_user_source = good;
   sqlite3_finalize(broken);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_capture_stored_unbound_owner_only);
   RUN_TEST(test_refused_and_guest_captures);
   RUN_TEST(test_bound_with_its_row_in_one_transaction);
   RUN_TEST(test_unbound_reclaimed_after_grace);
   RUN_TEST(test_reload_matches_live_and_reads_images_only);
   RUN_TEST(test_live_part_from_memory_missing_on_reload);
   RUN_TEST(test_claude_result_saves_images_by_id);
   RUN_TEST(test_voice_save_writes_tool_row_with_images);
   RUN_TEST(test_delete_takes_only_what_the_conversation_owns);
   RUN_TEST(test_backfilled_records_drive_the_delete);
   RUN_TEST(test_delete_never_strands_a_capture);
   RUN_TEST(test_rollback_unbinds_only_its_captures);
   RUN_TEST(test_short_bind_is_reported);
   RUN_TEST(test_sweep_spares_what_a_live_session_holds);
   RUN_TEST(test_sweep_passes_a_batch_held_whole);
   RUN_TEST(test_hold_is_the_owners_and_counts_jobs);
   RUN_TEST(test_capture_cap_fails_closed);
   RUN_TEST(test_capture_past_the_request_limit_refused);
   RUN_TEST(test_captures_capped_apart_from_uploads);
   return UNITY_END();
}
