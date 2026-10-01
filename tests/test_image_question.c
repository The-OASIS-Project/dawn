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
 * An image question's history message (image_rehydrate_question), against a
 * real image store (in-memory SQLite + a temp directory): byte for byte what
 * a reload rebuilds, or refused (a missing or another user's image, an inline
 * data: image, too many images) with nothing built: never a degraded message.
 */

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "blob_store.h"
#include "core/image_rehydrate.h"
#include "dawn_error.h"
#include "image_store.h"
#include "unity.h"

static char s_tmpdir[256];

/* A well-formed id no image has. */
#define UNKNOWN_ID "img_000000000000"

void setUp(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("owner", "h", true));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("other", "h", false));
   snprintf(s_tmpdir, sizeof(s_tmpdir), "/tmp/dawn_imgq_XXXXXX");
   TEST_ASSERT_NOT_NULL(mkdtemp(s_tmpdir));
   image_store_config_t cfg = { .max_size = 1024 * 1024,
                                .max_per_user = 100,
                                .data_dir = s_tmpdir };
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_init(&cfg));
}

void tearDown(void) {
   image_store_shutdown();
   blob_store_shutdown();
   auth_db_shutdown();
}

/* An uploaded PNG of @p user's, its id in @p id. */
static void upload(int user, const char *tag, char id[IMAGE_ID_LEN]) {
   unsigned char bytes[64] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
   const size_t n = 8 + strlen(tag);
   memcpy(bytes + 8, tag, strlen(tag));
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_save(user, bytes, n, "image/png", id));
}

static const char *json_text(struct json_object *obj) {
   return json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
}

/* The question is the message a reload of its saved row rebuilds. */
static void test_question_matches_reload(void) {
   char ids[2][IMAGE_ID_LEN];
   upload(1, "a", ids[0]);
   upload(1, "b", ids[1]);
   char *content = image_marker_build_content("What are these?", ids, 2);
   TEST_ASSERT_NOT_NULL(content);

   struct json_object *q = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, image_rehydrate_question(1, content, ids, 2, &q));
   TEST_ASSERT_NOT_NULL(q);
   struct json_object *reload = image_rehydrate_message(1, "user", content);
   TEST_ASSERT_EQUAL_STRING(json_text(reload), json_text(q));
   const char *wire = json_text(q);
   TEST_ASSERT_NOT_NULL(strstr(wire, "data:image\\/png;base64,"));
   TEST_ASSERT_NULL(strstr(wire, IMAGE_REHYDRATE_MISSING_TEXT));

   json_object_put(reload);
   json_object_put(q);
   free(content);
}

/* An id no image has: refused, nothing built. */
static void test_unknown_id_refused(void) {
   char ids[1][IMAGE_ID_LEN] = { UNKNOWN_ID };
   char *content = image_marker_build_content("What is this?", ids, 1);
   struct json_object *q = (struct json_object *)1;
   TEST_ASSERT_EQUAL_INT(IMAGE_REHYDRATE_ERR_NOT_FOUND,
                         image_rehydrate_question(1, content, ids, 1, &q));
   TEST_ASSERT_NULL(q);
   free(content);
}

/* Another user's image (or none's, user 0): refused as if it didn't exist. */
static void test_foreign_id_refused(void) {
   char ids[1][IMAGE_ID_LEN];
   upload(2, "theirs", ids[0]);
   char *content = image_marker_build_content("What is this?", ids, 1);
   struct json_object *q = NULL;
   TEST_ASSERT_EQUAL_INT(IMAGE_REHYDRATE_ERR_NOT_FOUND,
                         image_rehydrate_question(1, content, ids, 1, &q));
   TEST_ASSERT_NULL(q);
   TEST_ASSERT_EQUAL_INT(IMAGE_REHYDRATE_ERR_NOT_FOUND,
                         image_rehydrate_question(0, content, ids, 1, &q));
   TEST_ASSERT_NULL(q);
   /* Its owner asks it fine. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, image_rehydrate_question(2, content, ids, 1, &q));
   json_object_put(q);
   free(content);
}

/* An image the question names but whose file is gone: refused (a reload
 * would show the stand-in; the question being asked never does). */
static void test_deleted_image_refused(void) {
   char ids[1][IMAGE_ID_LEN];
   upload(1, "gone", ids[0]);
   char *content = image_marker_build_content("What is this?", ids, 1);
   TEST_ASSERT_EQUAL_INT(IMAGE_STORE_SUCCESS, image_store_delete(ids[0], 1));
   struct json_object *q = NULL;
   TEST_ASSERT_EQUAL_INT(IMAGE_REHYDRATE_ERR_NOT_FOUND,
                         image_rehydrate_question(1, content, ids, 1, &q));
   TEST_ASSERT_NULL(q);
   free(content);
}

/* The question's images come only from its ids: an inline data: image in its
 * text fails it. */
static void test_inline_data_image_refused(void) {
   char ids[1][IMAGE_ID_LEN];
   upload(1, "ok", ids[0]);
   char *content = image_marker_build_content(
       "Look [IMAGE:data:image/png;base64,iVBORw0KGgo=] here", ids, 1);
   struct json_object *q = NULL;
   TEST_ASSERT_EQUAL_INT(IMAGE_REHYDRATE_ERR_NOT_FOUND,
                         image_rehydrate_question(1, content, ids, 1, &q));
   TEST_ASSERT_NULL(q);
   free(content);
}

/* A marker the user typed that names no image is text the question carries
 * as a reload does: the question still goes, byte for byte the reload. */
static void test_typed_marker_reads_as_reload(void) {
   char ids[1][IMAGE_ID_LEN];
   upload(1, "ok", ids[0]);
   char *content = image_marker_build_content("Was [IMAGE:" UNKNOWN_ID "] it?", ids, 1);
   struct json_object *q = NULL;
   TEST_ASSERT_EQUAL_INT(SUCCESS, image_rehydrate_question(1, content, ids, 1, &q));
   struct json_object *reload = image_rehydrate_message(1, "user", content);
   TEST_ASSERT_EQUAL_STRING(json_text(reload), json_text(q));
   TEST_ASSERT_NOT_NULL(strstr(json_text(q), IMAGE_REHYDRATE_MISSING_TEXT));
   json_object_put(reload);
   json_object_put(q);
   free(content);
}

/* Ids with no marker of theirs, a repeated id, and more ids than a message
 * can carry. */
static void test_ids_and_markers_agree(void) {
   char ids[2][IMAGE_ID_LEN];
   upload(1, "x", ids[0]);
   memcpy(ids[1], ids[0], IMAGE_ID_LEN);
   struct json_object *q = NULL;
   TEST_ASSERT_EQUAL_INT(IMAGE_REHYDRATE_ERR_NOT_FOUND,
                         image_rehydrate_question(1, "No markers", ids, 1, &q));
   TEST_ASSERT_NULL(q);

   char *content = image_marker_build_content("Twice", ids, 2);
   TEST_ASSERT_EQUAL_INT(SUCCESS, image_rehydrate_question(1, content, ids, 2, &q));
   json_object_put(q);
   free(content);

   static char many[IMAGE_REHYDRATE_MAX_IMAGES + 1][IMAGE_ID_LEN];
   for (int i = 0; i <= IMAGE_REHYDRATE_MAX_IMAGES; i++) {
      memcpy(many[i], ids[0], IMAGE_ID_LEN);
   }
   TEST_ASSERT_EQUAL_INT(IMAGE_REHYDRATE_ERR_LIMIT,
                         image_rehydrate_question(1, "Many", (const char(*)[IMAGE_ID_LEN])many,
                                                  IMAGE_REHYDRATE_MAX_IMAGES + 1, &q));
   TEST_ASSERT_NULL(q);

   TEST_ASSERT_EQUAL_INT(FAILURE, image_rehydrate_question(1, "No ids", ids, 0, &q));
   TEST_ASSERT_EQUAL_INT(FAILURE, image_rehydrate_question(1, NULL, ids, 1, &q));
}

/* A turn's images are its image_ids[] only: a stray base64 images[] is
 * ignored (never read, never rejected); a bad id or too many fail the turn. */
static void test_turn_ids_from_image_ids_only(void) {
   char ids[4][IMAGE_ID_LEN];
   int n = -1;
   struct json_object *p = json_tokener_parse(
       "{\"text\":\"hi\",\"images\":[{\"data\":\"!!not base64!!\",\"mime_type\":\"x\"}]}");
   TEST_ASSERT_EQUAL_INT(SUCCESS, image_turn_ids_parse(p, 4, ids, &n));
   TEST_ASSERT_EQUAL_INT(0, n);
   json_object_put(p);

   p = json_tokener_parse("{\"text\":\"hi\",\"images\":[{\"data\":\"x\"}],"
                          "\"image_ids\":[\"img_aaaaaaaaaaaa\",\"img_bbbbbbbbbbbb\"]}");
   TEST_ASSERT_EQUAL_INT(SUCCESS, image_turn_ids_parse(p, 4, ids, &n));
   TEST_ASSERT_EQUAL_INT(2, n);
   TEST_ASSERT_EQUAL_STRING("img_bbbbbbbbbbbb", ids[1]);
   json_object_put(p);

   p = json_tokener_parse("{\"image_ids\":[\"img_aaaaaaaaaaaa\",\"../etc/passwd\"]}");
   TEST_ASSERT_EQUAL_INT(IMAGE_REHYDRATE_ERR_NOT_FOUND, image_turn_ids_parse(p, 4, ids, &n));
   TEST_ASSERT_EQUAL_INT(0, n);
   json_object_put(p);

   p = json_tokener_parse("{\"image_ids\":[1]}");
   TEST_ASSERT_EQUAL_INT(IMAGE_REHYDRATE_ERR_NOT_FOUND, image_turn_ids_parse(p, 4, ids, &n));
   json_object_put(p);

   p = json_tokener_parse("{\"image_ids\":\"img_aaaaaaaaaaaa\"}");
   TEST_ASSERT_EQUAL_INT(IMAGE_REHYDRATE_ERR_NOT_FOUND, image_turn_ids_parse(p, 4, ids, &n));
   json_object_put(p);

   p = json_tokener_parse("{\"image_ids\":[\"img_aaaaaaaaaaaa\",\"img_aaaaaaaaaaab\","
                          "\"img_aaaaaaaaaaac\"]}");
   TEST_ASSERT_EQUAL_INT(IMAGE_REHYDRATE_ERR_LIMIT, image_turn_ids_parse(p, 2, ids, &n));
   TEST_ASSERT_EQUAL_INT(0, n);
   json_object_put(p);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_question_matches_reload);
   RUN_TEST(test_unknown_id_refused);
   RUN_TEST(test_foreign_id_refused);
   RUN_TEST(test_deleted_image_refused);
   RUN_TEST(test_inline_data_image_refused);
   RUN_TEST(test_typed_marker_reads_as_reload);
   RUN_TEST(test_ids_and_markers_agree);
   RUN_TEST(test_turn_ids_from_image_ids_only);
   return UNITY_END();
}
