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
 * Unit tests for the path checks in core/path_utils: the lexical traversal
 * test and the root containment test the library and web paths rely on.
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/path_utils.h"
#include "unity.h"

/* Room for the scratch root plus a short relative name */
#define TEST_PATH_MAX (PATH_MAX + 64)

static char s_root[PATH_MAX];

void setUp(void) {
}
void tearDown(void) {
}

void test_traversal_literal_components(void) {
   TEST_ASSERT_TRUE(path_has_traversal(".."));
   TEST_ASSERT_TRUE(path_has_traversal("../etc/passwd"));
   TEST_ASSERT_TRUE(path_has_traversal("/music/../etc"));
   TEST_ASSERT_TRUE(path_has_traversal("/music/.."));
}

void test_traversal_encoded(void) {
   TEST_ASSERT_TRUE(path_has_traversal("/library/parts/%2e%2e/x"));
   TEST_ASSERT_TRUE(path_has_traversal("/library/parts/%2E%2E/x"));
   TEST_ASSERT_TRUE(path_has_traversal("/a/%2e./b"));
   TEST_ASSERT_TRUE(path_has_traversal("/a/.%2e/b"));
   TEST_ASSERT_TRUE(path_has_traversal("/a/%252e%252e/b"));
}

void test_traversal_clean_paths(void) {
   TEST_ASSERT_FALSE(path_has_traversal(NULL));
   TEST_ASSERT_FALSE(path_has_traversal(""));
   TEST_ASSERT_FALSE(path_has_traversal("/music/Artist/Album/01 Song.flac"));
   TEST_ASSERT_FALSE(path_has_traversal("/music/file..flac")); /* dots inside a name */
   TEST_ASSERT_FALSE(path_has_traversal("/library/parts/862/1617031720/file.flac"));
}

void test_within_root(void) {
   char inside[TEST_PATH_MAX];
   char sibling[TEST_PATH_MAX];
   snprintf(inside, sizeof(inside), "%s/lib/track.flac", s_root);
   snprintf(sibling, sizeof(sibling), "%s/lib2/track.flac", s_root);
   char lib[TEST_PATH_MAX];
   snprintf(lib, sizeof(lib), "%s/lib", s_root);

   TEST_ASSERT_TRUE(path_is_within_root(inside, lib));
   /* A sibling whose name starts with the root's is outside it */
   TEST_ASSERT_FALSE(path_is_within_root(sibling, lib));
   /* A path that doesn't exist can't be resolved, so it isn't inside */
   char missing[TEST_PATH_MAX];
   snprintf(missing, sizeof(missing), "%s/lib/missing.flac", s_root);
   TEST_ASSERT_FALSE(path_is_within_root(missing, lib));
}

void test_within_root_symlink_escape(void) {
   char lib[TEST_PATH_MAX];
   char link[TEST_PATH_MAX];
   snprintf(lib, sizeof(lib), "%s/lib", s_root);
   snprintf(link, sizeof(link), "%s/lib/escape.flac", s_root);

   /* A link inside the root that points out of it is outside */
   TEST_ASSERT_EQUAL_INT(0, symlink("../lib2/track.flac", link));
   TEST_ASSERT_FALSE(path_is_within_root(link, lib));
   unlink(link);
}

/* A scratch tree: <root>/lib/track.flac and <root>/lib2/track.flac */
static int make_tree(void) {
   snprintf(s_root, sizeof(s_root), "/tmp/test_path_utils_XXXXXX");
   if (!mkdtemp(s_root)) {
      return 1;
   }
   const char *dirs[] = { "lib", "lib2" };
   for (size_t i = 0; i < 2; i++) {
      char d[TEST_PATH_MAX];
      char f[TEST_PATH_MAX];
      snprintf(d, sizeof(d), "%s/%s", s_root, dirs[i]);
      snprintf(f, sizeof(f), "%s/%s/track.flac", s_root, dirs[i]);
      if (mkdir(d, 0700) != 0) {
         return 1;
      }
      FILE *fp = fopen(f, "w");
      if (!fp) {
         return 1;
      }
      fclose(fp);
   }
   return 0;
}

static void remove_tree(void) {
   const char *dirs[] = { "lib", "lib2" };
   for (size_t i = 0; i < 2; i++) {
      char d[TEST_PATH_MAX];
      char f[TEST_PATH_MAX];
      snprintf(d, sizeof(d), "%s/%s", s_root, dirs[i]);
      snprintf(f, sizeof(f), "%s/%s/track.flac", s_root, dirs[i]);
      unlink(f);
      rmdir(d);
   }
   rmdir(s_root);
}

int main(void) {
   if (make_tree() != 0) {
      fprintf(stderr, "test_path_utils: couldn't create the scratch tree\n");
      return 1;
   }
   UNITY_BEGIN();
   RUN_TEST(test_traversal_literal_components);
   RUN_TEST(test_traversal_encoded);
   RUN_TEST(test_traversal_clean_paths);
   RUN_TEST(test_within_root);
   RUN_TEST(test_within_root_symlink_escape);
   int rc = UNITY_END();
   remove_tree();
   return rc;
}
