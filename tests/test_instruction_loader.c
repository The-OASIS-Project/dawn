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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Unit tests for instruction_loader.c — file loading, path traversal
 * sanitization, module concatenation, and edge cases.
 *
 * Creates temporary instruction files in /tmp for testing, cleans up after.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tools/instruction_loader.h"
#include "unity.h"

/* ============================================================================
 * Test Fixture — create temp instruction files
 * ============================================================================ */

#define TEST_DIR "/tmp/dawn_test_instructions"
#define TOOL_NAME "test_tool"

static void write_file(const char *path, const char *content) {
   FILE *f = fopen(path, "w");
   if (f) {
      fputs(content, f);
      fclose(f);
   }
}

static void __attribute__((unused)) setup_test_files(void) {
   mkdir(TEST_DIR, 0755);
   mkdir(TEST_DIR "/" TOOL_NAME, 0755);

   write_file(TEST_DIR "/" TOOL_NAME "/_core.md", "# Core rules\nAlways be polite.\n");
   write_file(TEST_DIR "/" TOOL_NAME "/formal.md", "# Formal style\nUse titles.\n");
   write_file(TEST_DIR "/" TOOL_NAME "/casual.md", "# Casual style\nUse first names.\n");
   write_file(TEST_DIR "/" TOOL_NAME "/empty.md", "");
}

static void __attribute__((unused)) cleanup_test_files(void) {
   unlink(TEST_DIR "/" TOOL_NAME "/_core.md");
   unlink(TEST_DIR "/" TOOL_NAME "/formal.md");
   unlink(TEST_DIR "/" TOOL_NAME "/casual.md");
   unlink(TEST_DIR "/" TOOL_NAME "/empty.md");
   rmdir(TEST_DIR "/" TOOL_NAME);
   rmdir(TEST_DIR);
}

#define CWD_TEST_DIR "tool_instructions"
#define CWD_TOOL_NAME "test_loader_tmp"

static void setup_cwd_test_files(void) {
   mkdir(CWD_TEST_DIR, 0755); /* May already exist */
   mkdir(CWD_TEST_DIR "/" CWD_TOOL_NAME, 0755);

   write_file(CWD_TEST_DIR "/" CWD_TOOL_NAME "/_core.md", "# Core rules\nAlways be polite.\n");
   write_file(CWD_TEST_DIR "/" CWD_TOOL_NAME "/formal.md", "# Formal style\nUse titles.\n");
   write_file(CWD_TEST_DIR "/" CWD_TOOL_NAME "/casual.md", "# Casual style\nUse first names.\n");
   write_file(CWD_TEST_DIR "/" CWD_TOOL_NAME "/empty.md", "");
}

static void cleanup_cwd_test_files(void) {
   unlink(CWD_TEST_DIR "/" CWD_TOOL_NAME "/_core.md");
   unlink(CWD_TEST_DIR "/" CWD_TOOL_NAME "/formal.md");
   unlink(CWD_TEST_DIR "/" CWD_TOOL_NAME "/casual.md");
   unlink(CWD_TEST_DIR "/" CWD_TOOL_NAME "/empty.md");
   rmdir(CWD_TEST_DIR "/" CWD_TOOL_NAME);
   /* Don't rmdir CWD_TEST_DIR — may contain real tool instructions */
}

void setUp(void) {
}
void tearDown(void) {
}

/* ============================================================================
 * 1. Null/Empty Input Handling
 * ============================================================================ */

static void test_null_inputs(void) {
   char *output = NULL;

   TEST_ASSERT_NOT_EQUAL_MESSAGE(0, instruction_loader_load(NULL, "formal", &output),
                                 "NULL tool_name rejected");
   TEST_ASSERT_NULL_MESSAGE(output, "output is NULL on error");

   TEST_ASSERT_NOT_EQUAL_MESSAGE(0, instruction_loader_load("", "formal", &output),
                                 "empty tool_name rejected");
   TEST_ASSERT_NULL_MESSAGE(output, "output is NULL on empty tool_name");

   /* NULL modules with valid tool — should still load _core.md */
   int rc = instruction_loader_load(CWD_TOOL_NAME, NULL, &output);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "NULL modules loads _core.md only");
   TEST_ASSERT_NOT_NULL_MESSAGE(output, "output not NULL when _core.md exists");
   if (output) {
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "Core rules"), "_core.md content present");
      free(output);
      output = NULL;
   }

   /* Empty modules string */
   rc = instruction_loader_load(CWD_TOOL_NAME, "", &output);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "empty modules loads _core.md only");
   if (output) {
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "Core rules"),
                                   "_core.md content in empty modules");
      free(output);
      output = NULL;
   }
}

/* ============================================================================
 * 2. Single Module Loading
 * ============================================================================ */

static void test_single_module(void) {
   char *output = NULL;

   int rc = instruction_loader_load(CWD_TOOL_NAME, "formal", &output);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "single module loads successfully");
   TEST_ASSERT_NOT_NULL_MESSAGE(output, "output not NULL");
   if (output) {
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "Core rules"), "_core.md content present");
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "Formal style"), "formal.md content present");
      TEST_ASSERT_NULL_MESSAGE(strstr(output, "Casual style"), "casual.md NOT present");
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "---"), "separator between sections");
      free(output);
   }
}

/* ============================================================================
 * 3. Multiple Modules Loading
 * ============================================================================ */

static void test_multiple_modules(void) {
   char *output = NULL;

   int rc = instruction_loader_load(CWD_TOOL_NAME, "formal,casual", &output);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "multiple modules load successfully");
   TEST_ASSERT_NOT_NULL_MESSAGE(output, "output not NULL");
   if (output) {
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "Core rules"), "_core.md content present");
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "Formal style"), "formal.md content present");
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "Casual style"), "casual.md content present");
      free(output);
   }
}

/* ============================================================================
 * 4. Whitespace in Module List
 * ============================================================================ */

static void test_whitespace_handling(void) {
   char *output = NULL;

   int rc = instruction_loader_load(CWD_TOOL_NAME, " formal , casual ", &output);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "whitespace-padded modules load");
   TEST_ASSERT_NOT_NULL_MESSAGE(output, "output not NULL");
   if (output) {
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "Formal style"),
                                   "formal.md found despite whitespace");
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "Casual style"),
                                   "casual.md found despite whitespace");
      free(output);
   }
}

/* ============================================================================
 * 5. Nonexistent Module (graceful handling)
 * ============================================================================ */

static void test_missing_module(void) {
   char *output = NULL;

   /* One valid, one invalid */
   int rc = instruction_loader_load(CWD_TOOL_NAME, "formal,nonexistent", &output);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "partial load succeeds (valid + missing)");
   TEST_ASSERT_NOT_NULL_MESSAGE(output, "output not NULL");
   if (output) {
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "Formal style"), "valid module content present");
      free(output);
   }
}

/* ============================================================================
 * 6. Nonexistent Tool Directory
 * ============================================================================ */

static void test_missing_tool(void) {
   char *output = NULL;

   int rc = instruction_loader_load("no_such_tool_xyz", "formal", &output);
   TEST_ASSERT_NOT_EQUAL_MESSAGE(0, rc, "nonexistent tool returns error");
   TEST_ASSERT_NULL_MESSAGE(output, "output is NULL for missing tool");
}

/* ============================================================================
 * 7. Path Traversal Prevention
 * ============================================================================ */

static void test_path_traversal(void) {
   char *output = NULL;

   /* Tool name with path traversal */
   TEST_ASSERT_NOT_EQUAL_MESSAGE(0, instruction_loader_load("../etc", "passwd", &output),
                                 "tool_name with ../ rejected");
   TEST_ASSERT_NULL_MESSAGE(output, "no output on traversal attempt (tool)");

   TEST_ASSERT_NOT_EQUAL_MESSAGE(0, instruction_loader_load("foo/bar", "baz", &output),
                                 "tool_name with / rejected");

   /* Module name with path traversal */
   int rc = instruction_loader_load(CWD_TOOL_NAME, "../../etc/passwd", &output);
   /* Should still succeed (loads _core.md), but the malicious module is skipped */
   if (rc == 0 && output) {
      TEST_ASSERT_NULL_MESSAGE(strstr(output, "root:"), "no /etc/passwd content leaked");
      free(output);
      output = NULL;
   } else {
      /* If _core.md loaded, rc is 0. If not (no _core), could be error. Either is safe. */
      TEST_ASSERT_TRUE_MESSAGE(output == NULL || strstr(output, "root:") == NULL,
                               "path traversal in module blocked");
      free(output);
      output = NULL;
   }

   /* Module starting with dot */
   rc = instruction_loader_load(CWD_TOOL_NAME, ".hidden", &output);
   if (rc == 0 && output) {
      /* Should only have _core.md, not the .hidden module */
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "Core rules"),
                                   "only _core.md loaded, dot module skipped");
      free(output);
      output = NULL;
   }
}

/* ============================================================================
 * 8. Core-Only Loading (no modules)
 * ============================================================================ */

static void test_core_only(void) {
   char *output = NULL;

   int rc = instruction_loader_load(CWD_TOOL_NAME, "", &output);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "core-only load succeeds");
   TEST_ASSERT_NOT_NULL_MESSAGE(output, "output not NULL");
   if (output) {
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "Core rules"), "_core.md content present");
      TEST_ASSERT_NULL_MESSAGE(strstr(output, "Formal"), "no module content");
      TEST_ASSERT_NULL_MESSAGE(strstr(output, "Casual"), "no module content");
      free(output);
   }
}

/* ============================================================================
 * 9. Base Directory API
 * ============================================================================ */

static void test_base_dir_api(void) {
   const char *dir = instruction_loader_get_base_dir();
   TEST_ASSERT_NOT_NULL_MESSAGE(dir, "base dir not NULL");
   TEST_ASSERT_TRUE_MESSAGE(strlen(dir) > 0, "base dir not empty");
   TEST_ASSERT_EQUAL_STRING_MESSAGE(INSTRUCTION_LOADER_DEFAULT_DIR, dir,
                                    "base dir matches default");
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
   /* Set up test files in CWD-relative path */
   setup_cwd_test_files();

   UNITY_BEGIN();
   RUN_TEST(test_null_inputs);
   RUN_TEST(test_single_module);
   RUN_TEST(test_multiple_modules);
   RUN_TEST(test_whitespace_handling);
   RUN_TEST(test_missing_module);
   RUN_TEST(test_missing_tool);
   RUN_TEST(test_path_traversal);
   RUN_TEST(test_core_only);
   RUN_TEST(test_base_dir_api);
   int result = UNITY_END();

   /* Clean up */
   cleanup_cwd_test_files();

   return result;
}
