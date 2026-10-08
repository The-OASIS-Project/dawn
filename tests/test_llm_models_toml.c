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
 * Unit tests for loading models.toml: each table from the disk copy when it has
 * it, else from the shipped copy compiled in (an older disk copy, such as a
 * service install keeps, can't drop a newer table).
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "llm/llm_models_toml.h"
#include "toml.h"
#include "unity.h"

/* ---- stubs ---- */
int config_file_readable(const char *path) {
   (void)path;
   return 0;
}

static char s_stale[64];

void setUp(void) {
}
void tearDown(void) {
}

static int context_window(toml_table_t *root, const char *provider, const char *model) {
   toml_table_t *tab = root ? toml_table_in(root, provider) : NULL;
   toml_datum_t d = tab ? toml_int_in(tab, model) : (toml_datum_t){ 0 };
   return d.ok ? (int)d.u.i : -1;
}

static void test_builtin_copy_is_complete(void) {
   llm_models_toml_t m;
   llm_models_toml_open_file(NULL, &m);
   TEST_ASSERT_NOT_NULL(m.builtin);
   TEST_ASSERT_NULL(m.disk);
   const char *tables[] = { "openai", "anthropic", "gemini", "cache_pricing", "thinking" };
   for (size_t i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
      TEST_ASSERT_EQUAL_PTR_MESSAGE(m.builtin, llm_models_toml_root_for(&m, tables[i]), tables[i]);
   }
   llm_models_toml_close(&m);
}

static void test_an_older_disk_copy_keeps_its_tables_and_gains_the_new_ones(void) {
   llm_models_toml_t m;
   llm_models_toml_open_file(s_stale, &m);
   TEST_ASSERT_NOT_NULL(m.disk);
   /* The disk copy's own table wins (the operator's edit)... */
   TEST_ASSERT_EQUAL_PTR(m.disk, llm_models_toml_root_for(&m, "anthropic"));
   TEST_ASSERT_EQUAL_INT(123456, context_window(llm_models_toml_root_for(&m, "anthropic"),
                                                "anthropic", "claude-opus-5"));
   /* ...and a table it predates comes from the built-in copy. */
   TEST_ASSERT_EQUAL_PTR(m.builtin, llm_models_toml_root_for(&m, "thinking"));
   TEST_ASSERT_EQUAL_PTR(m.builtin, llm_models_toml_root_for(&m, "cache_pricing"));
   TEST_ASSERT_NULL(llm_models_toml_root_for(&m, "no_such_table"));
   llm_models_toml_close(&m);
}

static void test_an_unparsable_disk_copy_falls_back_whole(void) {
   char bad[64];
   snprintf(bad, sizeof(bad), "/tmp/dawn_models_bad_%d.toml", (int)getpid());
   FILE *f = fopen(bad, "w");
   TEST_ASSERT_NOT_NULL(f);
   fputs("[anthropic\n\"claude\" = \n", f);
   fclose(f);
   llm_models_toml_t m;
   llm_models_toml_open_file(bad, &m);
   TEST_ASSERT_NULL(m.disk);
   TEST_ASSERT_EQUAL_PTR(m.builtin, llm_models_toml_root_for(&m, "anthropic"));
   llm_models_toml_close(&m);
   unlink(bad);
}

int main(void) {
   snprintf(s_stale, sizeof(s_stale), "/tmp/dawn_models_stale_%d.toml", (int)getpid());
   FILE *f = fopen(s_stale, "w");
   if (!f) {
      return 1;
   }
   /* A pre-reasoning-table models.toml, with an operator's edit. */
   fputs("[anthropic]\n\"claude-opus-5\" = 123456\n\"claude\" = 200000\n", f);
   fclose(f);

   UNITY_BEGIN();
   RUN_TEST(test_builtin_copy_is_complete);
   RUN_TEST(test_an_older_disk_copy_keeps_its_tables_and_gains_the_new_ones);
   RUN_TEST(test_an_unparsable_disk_copy_falls_back_whole);
   const int rc = UNITY_END();
   unlink(s_stale);
   return rc;
}
