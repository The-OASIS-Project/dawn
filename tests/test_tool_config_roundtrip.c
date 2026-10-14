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
 * Tool-owned config sections survive a settings save: a settings save rewrites
 * dawn.toml, so the registry refuses a tool that parses a section it can't write
 * back, and what a tool's parser read, its writer writes.  Real registry and
 * TOML library; a fake tool.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "toml.h"
#include "tools/tool_registry.h"
#include "unity.h"

/* tool_registry.c's cache invalidation reaches into the LLM layer. */
void llm_tools_invalidate_cache(void) {
}

void invalidate_system_instructions(void) {
}

typedef struct {
   int level;
} fake_config_t;

static fake_config_t s_fake = { .level = 1 };

static void fake_parse(toml_table_t *table, void *config) {
   fake_config_t *cfg = (fake_config_t *)config;
   toml_datum_t level = table ? toml_int_in(table, "level") : (toml_datum_t){ 0 };
   if (level.ok)
      cfg->level = (int)level.u.i;
}

static void fake_write(void *fp, const void *config) {
   fprintf((FILE *)fp, "level = %d\n", ((const fake_config_t *)config)->level);
}

static char *fake_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   (void)value;
   *should_respond = 0;
   return NULL;
}

static tool_metadata_t fake_tool(const char *name, tool_config_writer_fn writer) {
   return (tool_metadata_t){
      .name = name,
      .device_string = name,
      .description = "Fake tool for the config round trip",
      .device_type = TOOL_DEVICE_TYPE_TRIGGER,
      .config = &s_fake,
      .config_size = sizeof(s_fake),
      .config_parser = fake_parse,
      .config_writer = writer,
      .config_section = "fake_tool",
      .callback = fake_callback,
   };
}

static char s_in[64];
static char s_out[64];

void setUp(void) {
   s_fake.level = 1;
   snprintf(s_in, sizeof(s_in), "/tmp/tool_cfg_rt_in_%d.toml", (int)getpid());
   snprintf(s_out, sizeof(s_out), "/tmp/tool_cfg_rt_out_%d.toml", (int)getpid());
   TEST_ASSERT_EQUAL_INT(0, tool_registry_init());
}

void tearDown(void) {
   tool_registry_shutdown();
   remove(s_in);
   remove(s_out);
}

/* A section the tool parses but can't write back would be deleted by the next
 * settings save, so the registry refuses the tool. */
static void test_parser_without_writer_is_refused(void) {
   static tool_metadata_t meta;
   meta = fake_tool("fake_tool", NULL);
   TEST_ASSERT_NOT_EQUAL(0, tool_registry_register(&meta));
   /* Also when the parser keeps its state elsewhere (no config pointer). */
   meta.config = NULL;
   meta.config_size = 0;
   TEST_ASSERT_NOT_EQUAL(0, tool_registry_register(&meta));
}

/* A section with only retired keys needs no parser or writer. */
static void test_retired_keys_only_section_registers(void) {
   static const char *const retired[] = { "enabled", NULL };
   static tool_metadata_t meta;
   meta = fake_tool("fake_tool", NULL);
   meta.config = NULL;
   meta.config_size = 0;
   meta.config_parser = NULL;
   meta.retired_keys = retired;
   TEST_ASSERT_EQUAL_INT(0, tool_registry_register(&meta));

   FILE *fp = fopen(s_in, "w");
   TEST_ASSERT_NOT_NULL(fp);
   fputs("[fake_tool]\nenabled = true\n", fp);
   fclose(fp);
   TEST_ASSERT_EQUAL_INT(0, tool_registry_parse_configs(s_in));
}

/* What the parser read from dawn.toml, the writer writes back. */
static void test_section_survives_a_save(void) {
   static tool_metadata_t meta;
   meta = fake_tool("fake_tool", fake_write);
   TEST_ASSERT_EQUAL_INT(0, tool_registry_register(&meta));

   FILE *fp = fopen(s_in, "w");
   TEST_ASSERT_NOT_NULL(fp);
   fputs("[fake_tool]\nlevel = 7\n", fp);
   fclose(fp);
   TEST_ASSERT_EQUAL_INT(0, tool_registry_parse_configs(s_in));

   fp = fopen(s_out, "w");
   TEST_ASSERT_NOT_NULL(fp);
   tool_registry_write_configs(fp);
   fclose(fp);

   fp = fopen(s_out, "r");
   TEST_ASSERT_NOT_NULL(fp);
   char err[128];
   toml_table_t *root = toml_parse_file(fp, err, sizeof(err));
   fclose(fp);
   TEST_ASSERT_NOT_NULL_MESSAGE(root, err);
   toml_datum_t level = toml_int_in(toml_table_in(root, "fake_tool"), "level");
   TEST_ASSERT_TRUE(level.ok);
   TEST_ASSERT_EQUAL_INT(7, (int)level.u.i);
   toml_free(root);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_parser_without_writer_is_refused);
   RUN_TEST(test_retired_keys_only_section_registers);
   RUN_TEST(test_section_survives_a_save);
   return UNITY_END();
}
