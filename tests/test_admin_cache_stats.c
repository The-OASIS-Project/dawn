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
 * Unit tests for the dawn-admin cache stats table.
 */

#define ADMIN_SOCKET_INTERNAL_ALLOWED

#include <string.h>

#include "auth/admin_socket_internal.h"
#include "dawn_error.h"
#include "llm/llm_interface.h"
#include "unity.h"

/* ---- stubs (the handler's I/O isn't under test) ---- */

int send_text_response(int client_fd, admin_resp_code_t code, const char *text) {
   (void)client_fd, (void)code, (void)text;
   return 0;
}
int auth_db_llm_usage_stats(int64_t since,
                            const char *provider,
                            llm_usage_stat_t *out,
                            int max,
                            int *count) {
   (void)since, (void)provider, (void)out, (void)max;
   *count = 0;
   return AUTH_DB_SUCCESS;
}
int cloud_provider_from_string(const char *str, llm_type_t *type, cloud_provider_t *provider) {
   if (!str || strcmp(str, "local") == 0) {
      *type = LLM_LOCAL;
      *provider = CLOUD_PROVIDER_NONE;
      return str ? SUCCESS : FAILURE;
   }
   *type = LLM_CLOUD;
   *provider = strcmp(str, "claude") == 0 ? CLOUD_PROVIDER_CLAUDE : CLOUD_PROVIDER_OPENAI;
   return SUCCESS;
}
/* Saved: a tenth of a read is paid, a write costs a quarter more. */
int llm_cache_saved_input_tokens(llm_type_t type,
                                 cloud_provider_t provider,
                                 const char *model,
                                 int cached_tokens,
                                 int cache_write_tokens) {
   (void)provider, (void)model;
   return type == LLM_LOCAL ? 0 : cached_tokens * 9 / 10 - cache_write_tokens / 4;
}

void setUp(void) {
}
void tearDown(void) {
}

static void test_stats_table(void) {
   llm_usage_stat_t rows[2] = {
      { .provider = "claude",
        .model = "claude-opus-5-5",
        .kind = "turn",
        .calls = 12,
        .prompt_tokens = 600000,
        .cache_read_tokens = 540000,
        .cache_write_tokens = 40000,
        .warm_misses = 1,
        .binding_drops = 0 },
      { .provider = "local",
        .model = "/var/lib/models/qwen.gguf",
        .kind = "tool_iter",
        .calls = 3,
        .prompt_tokens = 9000,
        .cache_read_tokens = 8000 },
   };
   char out[4096];
   admin_cache_format_stats(rows, 2, false, out, sizeof(out));
   TEST_ASSERT_NOT_NULL(strstr(out, "provider"));
   TEST_ASSERT_NOT_NULL(strstr(out, "claude-opus-5-5"));
   TEST_ASSERT_NOT_NULL(strstr(out, " 90%")); /* 540K of 600K */
   TEST_ASSERT_NOT_NULL(strstr(out, "476K")); /* 486K saved by reads, 10K paid for writes */
   TEST_ASSERT_NOT_NULL(strstr(out, " 88%"));
   TEST_ASSERT_NOT_NULL(strstr(out, "qwen.gguf")); /* a local model by its file name */
   TEST_ASSERT_NULL(strstr(out, "/var/lib"));
   /* A small buffer says how much it left out. */
   char small[460];
   admin_cache_format_stats(rows, 2, false, small, sizeof(small));
   TEST_ASSERT_NOT_NULL(strstr(small, "more"));
}

/* Groups the query left out are said, not dropped silently. */
static void test_stats_says_what_it_left_out(void) {
   llm_usage_stat_t row = { .provider = "claude", .model = "m", .kind = "turn", .calls = 1 };
   char out[4096];
   admin_cache_format_stats(&row, 1, true, out, sizeof(out));
   TEST_ASSERT_NOT_NULL(strstr(out, "more not shown"));
   admin_cache_format_stats(NULL, 0, false, out, sizeof(out));
   TEST_ASSERT_NOT_NULL(strstr(out, "no calls"));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_stats_table);
   RUN_TEST(test_stats_says_what_it_left_out);
   return UNITY_END();
}
